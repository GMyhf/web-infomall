/*
 * Focused regression tests for core validation, conversion, storage, and
 * corrupted-index handling.
 */

#include "common.h"
#include "indexer.h"
#include "parser.h"
#include "query.h"
#include "store.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

#include <fcntl.h>
#include <unistd.h>

namespace {

class TestContext {
public:
    void check(bool condition, const char* expression, int line) {
        checks_++;
        if (condition) return;
        failures_++;
        fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    }

    int finish() const {
        if (failures_ == 0) {
            printf("PASS: %d core checks\n", checks_);
            return 0;
        }
        fprintf(stderr, "FAIL: %d of %d core checks failed\n", failures_, checks_);
        return 1;
    }

private:
    int checks_ = 0;
    int failures_ = 0;
};

#define CHECK(ctx, expression) (ctx).check(static_cast<bool>(expression), #expression, __LINE__)

class TempDir {
public:
    TempDir() {
        char pattern[] = "/tmp/web-infomall-core-XXXXXX";
        char* created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("mkdtemp failed");
        path_ = created;
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

std::string make_incompressible_body(size_t size) {
    std::string body(size, '\0');
    uint32_t state = 0xC001D00Du;
    for (char& byte : body) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<char>(state & 0xFFu);
    }
    return body;
}

bool write_v2_shard(const std::string& path, const std::vector<HostBlock>& hosts,
                    const std::vector<UrlIndexEntry>& entries, const std::string& url_pool) {
    ShardFileHeader header = {};
    header.magic = SHARD_MAGIC;
    header.entry_count = static_cast<uint32_t>(entries.size());
    header.host_count = static_cast<uint32_t>(hosts.size());
    header.url_pool_size = static_cast<uint32_t>(url_pool.size());

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!hosts.empty())
        output.write(reinterpret_cast<const char*>(hosts.data()),
                     static_cast<std::streamsize>(hosts.size() * sizeof(HostBlock)));
    if (!entries.empty())
        output.write(reinterpret_cast<const char*>(entries.data()),
                     static_cast<std::streamsize>(entries.size() * sizeof(UrlIndexEntry)));
    output.write(url_pool.data(), static_cast<std::streamsize>(url_pool.size()));
    return output.good();
}

HostBlock make_host_block(const std::string& host, uint32_t first_entry, uint32_t entry_count) {
    HostBlock block = {};
    strncpy(block.host, host.c_str(), sizeof(block.host) - 1);
    block.first_entry = first_entry;
    block.entry_count = entry_count;
    return block;
}

UrlIndexEntry make_v2_entry(const std::string& url, uint32_t crawl_date,
                             uint32_t url_offset, uint16_t record_size = 0) {
    UrlIndexEntry entry = {};
    entry.url_hash = url_hash(url);
    entry.crawl_date = crawl_date;
    entry.record_size = record_size;
    entry.url_len = static_cast<uint16_t>(url.size());
    entry.url_offset = url_offset;
    return entry;
}

bool write_uncompressed_article(const std::string& path, const std::string& url,
                                uint32_t crawl_date, const std::string& title,
                                const std::string& body) {
    ArticleRecord record = {};
    record.magic = ARTICLE_MAGIC;
    record.crawl_date = crawl_date;
    record.url_len = static_cast<uint32_t>(url.size());
    record.title_len = static_cast<uint32_t>(title.size());
    record.body_compr_len = static_cast<uint32_t>(body.size());
    record.body_orig_len = static_cast<uint32_t>(body.size());
    record.record_size = ArticleRecord::HEADER_SIZE + record.url_len + record.title_len +
                         record.body_compr_len;
    record.mini_hash = mini_hash(url);

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&record), sizeof(record));
    output.write(url.data(), static_cast<std::streamsize>(url.size()));
    output.write(title.data(), static_cast<std::streamsize>(title.size()));
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    return output.good();
}

// True when a failed open() left nothing behind. Worth a helper because the
// interesting cases are the *late* failures: hosts/entries/url_pool are only
// assigned once the layout checks pass, so a shard rejected for bad ordering or
// an out-of-pool offset is the only kind that can leave them dangling into the
// mapping fail() has already munmap'd. Truncated prefixes fail earlier, before
// those fields are ever set, so they cannot exercise this on their own.
bool shard_state_cleared(const MappedShard& shard) {
    return shard.fd == -1 && shard.data == nullptr && shard.header == nullptr &&
           shard.hosts == nullptr && shard.entries == nullptr &&
           shard.url_pool == nullptr && shard.file_size == 0 && !shard.is_v2;
}

std::string read_file_bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool write_file_prefix(const std::string& path, const std::string& bytes, size_t size) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(size));
    return output.good();
}

void test_dates_and_tokenizer(TestContext& test) {
    CHECK(test, is_leap_year(2000));
    CHECK(test, !is_leap_year(1900));
    CHECK(test, is_leap_year(2024));
    CHECK(test, !is_leap_year(2023));

    CHECK(test, valid_crawl_date(20000229));
    CHECK(test, valid_crawl_date(20240229));
    CHECK(test, !valid_crawl_date(19000229));
    CHECK(test, !valid_crawl_date(20230229));
    CHECK(test, !valid_crawl_date(20240431));
    CHECK(test, !valid_crawl_date(20240001));
    CHECK(test, !valid_crawl_date(20241301));
    CHECK(test, !valid_crawl_date(20240100));
    CHECK(test, !valid_crawl_date(100000101));

    std::string malformed = "Alpha ";
    malformed.append("\xE4\xFF\xFF", 3);
    malformed += ' ';
    malformed.append("\xE9\x80\xFF", 3);
    malformed += " Omega";
    const std::vector<std::string> expected = {"alpha", "omega"};
    CHECK(test, tokenize_title(malformed) == expected);

    const std::string truncated("\xE4\xB8", 2);
    CHECK(test, tokenize_title(truncated).empty());

    const std::string valid_cjk("\xE4\xB8\xAD", 3);
    CHECK(test, tokenize_title(valid_cjk) == std::vector<std::string>{valid_cjk});
}

void test_gbk_error_recovery(TestContext& test) {
    GbToUtf8 converter;
    std::string input = "before";
    input.push_back(static_cast<char>(0xFF));
    input += "after";

    std::string expected = "before";
    expected.append("\xEF\xBF\xBD", 3);
    expected += "after";
    CHECK(test, converter.convert(input) == expected);
}

void test_truncated_parser_record(TestContext& test, const std::string& root) {
    std::string record = "id=1";
    record += '\x1e';
    record += "time=20240101";
    record += '\x1e';
    record += "url=http://example.test/truncated";
    record += '\x1e';
    record += "title=title";
    record += '\x1e';
    record += "body=body";

    const std::string truncated_path = root + "/truncated.dat";
    {
        std::ofstream output(truncated_path, std::ios::binary);
        output.write(record.data(), record.size());
    }
    ArticleParser truncated_parser;
    int truncated_count = truncated_parser.parse_file(
        truncated_path.c_str(), 0, [](const auto&) {});
    CHECK(test, truncated_count == 0);
    CHECK(test, truncated_parser.errors().size() == 1);

    const std::string complete_path = root + "/complete.dat";
    record += '\x1f';
    {
        std::ofstream output(complete_path, std::ios::binary);
        output.write(record.data(), record.size());
    }
    ArticleParser complete_parser;
    int complete_count = complete_parser.parse_file(
        complete_path.c_str(), 0, [](const auto&) {});
    CHECK(test, complete_count == 1);
    CHECK(test, complete_parser.errors().empty());

    const std::string malformed_path = root + "/malformed.dat";
    {
        std::ofstream output(malformed_path, std::ios::binary);
        const std::string malformed = std::string("garbage") + '\x1f';
        output.write(malformed.data(), malformed.size());
    }
    ArticleParser malformed_parser;
    int malformed_count = malformed_parser.parse_file(
        malformed_path.c_str(), 0, [](const auto&) {});
    CHECK(test, malformed_count == 0);
    CHECK(test, malformed_parser.errors().size() == 1);
}

void test_store_and_reader(TestContext& test, const std::string& root) {
    const std::string archive = root + "/archive";
    const std::string normal_url = "http://example.test/normal";
    const std::string normal_title = "Normal";
    const std::string normal_body = "normal body payload";
    const std::string large_url = "http://example.test/large";
    const std::string large_title = "Large";
    constexpr uint32_t target_record_size = 2u * 65536u + 17u;
    const size_t large_body_size = target_record_size - ArticleRecord::HEADER_SIZE
                                 - large_url.size() - large_title.size();
    const std::string large_body = make_incompressible_body(large_body_size);

    DataStore store(archive);
    const auto normal = store.write_article(
        normal_url, 20240229, normal_title, normal_body);
    const auto large = store.write_article(
        large_url, 20240229, large_title, large_body);
    CHECK(test, normal.ok);
    CHECK(test, large.ok);
    CHECK(test, large.size == target_record_size);
    CHECK(test, large.size > 65536u);
    CHECK(test, large.size % 65536u < ArticleRecord::HEADER_SIZE);
    CHECK(test, store.finish());

    const std::string data_root = archive + "/data";
    const std::string data_file = data_root + "/" + large.file_path;
    int fd = ::open(data_file.c_str(), O_RDONLY);
    CHECK(test, fd >= 0);
    if (fd >= 0) {
        ArticleRecord header = {};
        CHECK(test, pread(fd, &header, sizeof(header), large.offset)
                        == static_cast<ssize_t>(sizeof(header)));
        CHECK(test, header.record_size == target_record_size);
        CHECK(test, (header.flags & 1u) == 0);
        CHECK(test, header.crc32 != 0);
        ::close(fd);
    }

    ArticleReader reader(data_root);
    const auto normal_read = reader.read_article(
        normal.file_path, normal.offset, normal.size);
    CHECK(test, normal_read.valid);
    CHECK(test, normal_read.url == normal_url);
    CHECK(test, normal_read.title == normal_title);
    CHECK(test, normal_read.body == normal_body);
    CHECK(test, normal_read.date == 20240229);

    const uint16_t truncated_hint = static_cast<uint16_t>(large.size);
    CHECK(test, truncated_hint < ArticleRecord::HEADER_SIZE);
    const auto large_read = reader.read_article(
        large.file_path, large.offset, truncated_hint);
    CHECK(test, large_read.valid);
    CHECK(test, large_read.url == large_url);
    CHECK(test, large_read.title == large_title);
    CHECK(test, large_read.body == large_body);

    fd = ::open((data_root + "/" + normal.file_path).c_str(), O_RDWR);
    CHECK(test, fd >= 0);
    if (fd >= 0) {
        const off_t body_offset = static_cast<off_t>(normal.offset)
            + ArticleRecord::HEADER_SIZE + normal_url.size() + normal_title.size();
        unsigned char byte = 0;
        CHECK(test, pread(fd, &byte, 1, body_offset) == 1);
        byte ^= 0x01u;
        CHECK(test, pwrite(fd, &byte, 1, body_offset) == 1);
        ::close(fd);

        const auto corrupted = reader.read_article(
            normal.file_path, normal.offset, normal.size);
        CHECK(test, !corrupted.valid);
    }
}

void test_uncreatable_store(TestContext& test, const std::string& root) {
    const std::string blocker = root + "/not-a-directory";
    {
        std::ofstream output(blocker, std::ios::binary);
        output << 'x';
        CHECK(test, output.good());
    }

    DataStore store(blocker + "/archive");
    const auto location = store.write_article(
        "http://example.test/failure", 20240101, "title", "body");
    CHECK(test, !location.ok);
    CHECK(test, !store.finish());
}

void test_truncated_store_append(TestContext& test, const std::string& root) {
    const std::string archive = root + "/truncated-archive";
    DataStore initial(archive);
    auto first = initial.write_article(
        "http://example.test/first", 20240301, "first", "first body");
    CHECK(test, first.ok);
    CHECK(test, initial.finish());

    const std::string path = archive + "/data/" + first.file_path;
    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
    CHECK(test, fd >= 0);
    if (fd >= 0) {
        const char partial = 'x';
        CHECK(test, ::write(fd, &partial, 1) == 1);
        ::close(fd);
    }
    const auto size_before = std::filesystem::file_size(path);

    DataStore resumed(archive);
    auto second = resumed.write_article(
        "http://example.test/second", 20240302, "second", "second body");
    CHECK(test, !second.ok);
    CHECK(test, std::filesystem::file_size(path) == size_before);
}

void test_corrupt_shard(TestContext& test, const std::string& root) {
    const std::string path = root + "/corrupt.idx";
    ShardFileHeader header = {};
    header.magic = SHARD_MAGIC;
    header.entry_count = std::numeric_limits<uint32_t>::max();
    header.host_count = std::numeric_limits<uint32_t>::max();
    header.url_pool_size = std::numeric_limits<uint32_t>::max();

    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        CHECK(test, output.good());
    }

    MappedShard shard;
    CHECK(test, !shard.open(path.c_str()));
    CHECK(test, shard.fd == -1);
    CHECK(test, shard.data == nullptr);
}

void test_v2_shard_validation_and_legacy_dates(TestContext& test, const std::string& root) {
    const std::string fixture = root + "/v2-shard-fixture";
    std::filesystem::create_directories(fixture);
    const std::string alpha_url = "http://alpha.example.test/a";
    const std::string zeta_url = "http://zeta.example.test/z";
    const std::string pool = alpha_url + zeta_url;
    std::vector<HostBlock> hosts = {
        make_host_block("alpha.example.test", 0, 1),
        make_host_block("zeta.example.test", 1, 1),
    };
    std::vector<UrlIndexEntry> entries = {
        make_v2_entry(alpha_url, 20240101, 0),
        make_v2_entry(zeta_url, 20240102, static_cast<uint32_t>(alpha_url.size())),
    };

    const std::string valid_path = fixture + "/valid.idx";
    CHECK(test, write_v2_shard(valid_path, hosts, entries, pool));
    { MappedShard shard; CHECK(test, shard.open(valid_path.c_str())); }

    // A calendar-invalid legacy date is intentionally serviceable when every
    // binary-layout invariant remains valid. The query must preserve it rather
    // than silently restoring the old MappedShard::open rejection.
    const std::string archive = root + "/legacy-date-archive";
    const std::string data_dir = archive + "/data/200302";
    const std::string index_dir = archive + "/index";
    std::filesystem::create_directories(data_dir);
    std::filesystem::create_directories(index_dir);
    const std::string legacy_url = "http://legacy.example.test/page";
    const std::string title = "legacy title";
    const std::string body = "legacy body";
    const uint16_t record_size = static_cast<uint16_t>(ArticleRecord::HEADER_SIZE +
        legacy_url.size() + title.size() + body.size());
    CHECK(test, write_uncompressed_article(data_dir + "/data_0001.dat", legacy_url,
                                           20030229, title, body));
    std::vector<HostBlock> legacy_hosts = {make_host_block("legacy.example.test", 0, 1)};
    std::vector<UrlIndexEntry> legacy_entries = {
        make_v2_entry(legacy_url, 20030229, 0, record_size),
    };
    const std::string legacy_path = index_dir + "/url_" +
        (shard_for_host("legacy.example.test") < 10 ? "0" : "") +
        std::to_string(shard_for_host("legacy.example.test")) + ".idx";
    CHECK(test, write_v2_shard(legacy_path, legacy_hosts, legacy_entries, legacy_url));
    {
        MappedShard shard;
        const bool opened = shard.open(legacy_path.c_str());
        CHECK(test, opened);
        if (opened) CHECK(test, shard.entries[0].crawl_date == 20030229);
    }
    QueryEngine legacy_query(archive + "/data", index_dir);
    CHECK(test, legacy_query.init());
    const auto legacy_page = legacy_query.get_page(legacy_url);
    // `valid` defaults to true and means "integrity intact", not "found": every
    // early return in get_page yields {} with valid == true. Asserting it alone
    // stays green even when init() fails and nothing is looked up at all — it was
    // the one assertion here that survived restoring the date rejection while its
    // four neighbours went red. Check the identity the way every production caller
    // does, guarding on the URL first.
    CHECK(test, legacy_page.url == legacy_url);
    CHECK(test, legacy_page.valid);
    CHECK(test, legacy_page.date == 20030229);
    CHECK(test, legacy_page.title == title);
    const auto legacy_versions = legacy_query.get_versions(legacy_url);
    CHECK(test, legacy_versions.size() == 1 && legacy_versions[0].date == 20030229);

    // Each mutation starts from the same known-good layout, so a rejection is
    // evidence for that exact invariant rather than an incidental malformed file.
    auto bad_entries = entries;
    bad_entries[0].url_offset = static_cast<uint32_t>(pool.size());
    const std::string bad_offset = fixture + "/bad-offset.idx";
    CHECK(test, write_v2_shard(bad_offset, hosts, bad_entries, pool));
    { MappedShard shard; CHECK(test, !shard.open(bad_offset.c_str()));
      CHECK(test, shard_state_cleared(shard)); }

    auto unsorted_hosts = hosts;
    std::swap(unsorted_hosts[0], unsorted_hosts[1]);
    const std::string bad_hosts = fixture + "/bad-host-order.idx";
    CHECK(test, write_v2_shard(bad_hosts, unsorted_hosts, entries, pool));
    { MappedShard shard; CHECK(test, !shard.open(bad_hosts.c_str()));
      CHECK(test, shard_state_cleared(shard)); }

    const std::string first_url = "http://ordered.example.test/a";
    const std::string second_url = "http://ordered.example.test/b";
    std::vector<UrlIndexEntry> ordered_entries = {
        make_v2_entry(first_url, 20240101, 0),
        make_v2_entry(second_url, 20240102, static_cast<uint32_t>(first_url.size())),
    };
    if (ordered_entries[1].url_hash < ordered_entries[0].url_hash)
        std::swap(ordered_entries[0], ordered_entries[1]);
    const std::string ordered_pool = first_url + second_url;
    std::vector<HostBlock> ordered_hosts = {make_host_block("ordered.example.test", 0, 2)};
    const std::string ordered_path = fixture + "/ordered.idx";
    CHECK(test, write_v2_shard(ordered_path, ordered_hosts, ordered_entries, ordered_pool));
    { MappedShard shard; CHECK(test, shard.open(ordered_path.c_str())); }
    std::swap(ordered_entries[0], ordered_entries[1]);
    const std::string bad_order = fixture + "/bad-entry-order.idx";
    CHECK(test, write_v2_shard(bad_order, ordered_hosts, ordered_entries, ordered_pool));
    { MappedShard shard; CHECK(test, !shard.open(bad_order.c_str()));
      CHECK(test, shard_state_cleared(shard)); }

    auto bad_ranges = hosts;
    bad_ranges[0].first_entry = 2;
    const std::string bad_range = fixture + "/bad-host-range.idx";
    CHECK(test, write_v2_shard(bad_range, bad_ranges, entries, pool));
    { MappedShard shard; CHECK(test, !shard.open(bad_range.c_str()));
      CHECK(test, shard_state_cleared(shard)); }

    const std::string padded = fixture + "/padded.idx";
    CHECK(test, write_v2_shard(padded, hosts, entries, pool));
    const char trailing = 'x';
    std::ofstream append(padded, std::ios::binary | std::ios::app);
    append.write(&trailing, 1);
    append.close();
    { MappedShard shard; CHECK(test, !shard.open(padded.c_str()));
      CHECK(test, shard_state_cleared(shard)); }
}

void test_corrupt_input_red_team(TestContext& test, const std::string& root) {
    const std::string fixture = root + "/red-team";
    std::filesystem::create_directories(fixture);
    const std::string url = "http://red-team.example.test/page";
    const std::vector<HostBlock> hosts = {make_host_block("red-team.example.test", 0, 1)};
    const std::vector<UrlIndexEntry> entries = {make_v2_entry(url, 20240101, 0)};
    const std::string valid_path = fixture + "/valid.idx";
    CHECK(test, write_v2_shard(valid_path, hosts, entries, url));
    const std::string valid_bytes = read_file_bytes(valid_path);
    CHECK(test, !valid_bytes.empty());

    // Every proper prefix is a plausible interrupted write. Each must be
    // rejected without leaving a half-mapped shard behind for a caller to use.
    const std::string truncated_path = fixture + "/truncated.idx";
    for (size_t size = 0; size < valid_bytes.size(); size++) {
        CHECK(test, write_file_prefix(truncated_path, valid_bytes, size));
        MappedShard shard;
        CHECK(test, !shard.open(truncated_path.c_str()));
        // All eight fields fail() clears, not just the three obvious ones.
        // hosts/entries/url_pool are the ones that matter most: they point into
        // the mapping fail() has already munmap'd, so if a failed open forgets
        // them, a caller that ignores the return value dereferences freed pages.
        // Kept as one CHECK so strengthening this does not inflate the count.
        CHECK(test, shard_state_cleared(shard));
    }

    // Bit-level damage to the fields the parser relies on must also fail cleanly.
    auto bad_magic = valid_bytes;
    // Do not flip bit 0: SHARD_MAGIC ("IDX!") would become the supported v1
    // magic ("IDX "), an intentionally indistinguishable compatibility case.
    bad_magic[0] ^= 0x02;
    const std::string magic_path = fixture + "/bad-magic.idx";
    CHECK(test, write_file_prefix(magic_path, bad_magic, bad_magic.size()));
    { MappedShard shard; CHECK(test, !shard.open(magic_path.c_str())); }

    auto zero_url_length = entries;
    zero_url_length[0].url_len = 0;
    const std::string zero_url_path = fixture + "/zero-url-length.idx";
    CHECK(test, write_v2_shard(zero_url_path, hosts, zero_url_length, url));
    { MappedShard shard; CHECK(test, !shard.open(zero_url_path.c_str())); }

    const std::string data_dir = fixture + "/data";
    std::filesystem::create_directories(data_dir);
    ArticleReader reader(data_dir);

    ArticleRecord bad_magic_record = {};
    bad_magic_record.magic = 0;
    bad_magic_record.record_size = ArticleRecord::HEADER_SIZE;
    CHECK(test, write_file_prefix(data_dir + "/bad-magic.dat",
                                  std::string(reinterpret_cast<char*>(&bad_magic_record),
                                              sizeof(bad_magic_record)),
                                  sizeof(bad_magic_record)));
    CHECK(test, !reader.read_article("bad-magic.dat", 0, ArticleRecord::HEADER_SIZE).valid);

    ArticleRecord truncated_record = {};
    truncated_record.magic = ARTICLE_MAGIC;
    truncated_record.record_size = ArticleRecord::HEADER_SIZE + 8;
    const std::string truncated_bytes(reinterpret_cast<char*>(&truncated_record),
                                      sizeof(truncated_record));
    CHECK(test, write_file_prefix(data_dir + "/truncated-record.dat", truncated_bytes,
                                  truncated_bytes.size()));
    CHECK(test, !reader.read_article("truncated-record.dat", 0,
                                     ArticleRecord::HEADER_SIZE).valid);

    ArticleRecord mismatched_record = {};
    mismatched_record.magic = ARTICLE_MAGIC;
    mismatched_record.url_len = 1;
    mismatched_record.record_size = ArticleRecord::HEADER_SIZE;
    const std::string mismatched_bytes(reinterpret_cast<char*>(&mismatched_record),
                                       sizeof(mismatched_record));
    CHECK(test, write_file_prefix(data_dir + "/mismatched-record.dat", mismatched_bytes,
                                  mismatched_bytes.size()));
    CHECK(test, !reader.read_article("mismatched-record.dat", 0,
                                     ArticleRecord::HEADER_SIZE).valid);
}

void test_empty_shard_compatibility(TestContext& test, const std::string& root) {
    const std::string archive = root + "/empty-shard-archive";
    const std::string index_dir = archive + "/index";
    std::filesystem::create_directories(index_dir);
    const std::string path = index_dir + "/url_00.idx";

    ShardFileHeader header = {};
    header.magic = SHARD_MAGIC;
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        CHECK(test, output.good());
    }

    {
        MappedShard shard;
        CHECK(test, shard.open(path.c_str()));
        CHECK(test, shard.header->entry_count == 0);
    }

    QueryEngine query(archive + "/data", index_dir);
    CHECK(test, !query.init());
}

void test_query_index_roundtrip(TestContext& test, const std::string& root) {
    const std::string archive = root + "/query-archive";
    const std::string prefix(31, 'a');
    std::unordered_map<int, std::string> host_by_shard;
    std::string host_a, host_b;
    for (int i = 0; i < 1000 && host_b.empty(); i++) {
        std::string host = prefix + std::to_string(i) + ".example.test";
        int shard = shard_for_host(host);
        auto [it, inserted] = host_by_shard.emplace(shard, host);
        if (!inserted && it->second != host) {
            host_a = it->second;
            host_b = host;
        }
    }
    CHECK(test, !host_a.empty());
    CHECK(test, !host_b.empty());
    CHECK(test, host_a.substr(0, HOST_HASH_LEN - 1) == host_b.substr(0, HOST_HASH_LEN - 1));
    CHECK(test, shard_for_host(host_a) == shard_for_host(host_b));
    if (host_a.empty() || host_b.empty()) return;

    const std::string url_a = "http://" + host_a + "/nearest";
    const std::string url_b = "http://" + host_b + "/other";
    DataStore store(archive);
    Indexer indexer(archive + "/index");

    auto before = store.write_article(url_a, 20240131, "before", "before body");
    auto after = store.write_article(url_a, 20240210, "after", "after body");
    auto other = store.write_article(url_b, 20240205, "other", "other body");
    CHECK(test, before.ok && after.ok && other.ok);
    indexer.add_entry(url_a, 20240131, before.offset, before.size, before.file_seq);
    indexer.add_entry(url_a, 20240131, before.offset, before.size, before.file_seq);
    indexer.add_entry(url_a, 20240210, after.offset, after.size, after.file_seq);
    indexer.add_entry(url_b, 20240205, other.offset, other.size, other.file_seq);
    CHECK(test, store.finish());
    CHECK(test, indexer.build());
    CHECK(test, indexer.built_entries() == 3);

    QueryEngine query(archive + "/data", archive + "/index");
    CHECK(test, query.init());
    auto nearest = query.get_page(url_a, 20240201);
    CHECK(test, nearest.valid);
    CHECK(test, nearest.date == 20240131);
    CHECK(test, nearest.title == "before");
    auto second_host = query.get_page(url_b);
    CHECK(test, second_host.valid);
    CHECK(test, second_host.url == url_b);

    auto host_a_urls = query.get_host_urls(host_a, 10);
    auto host_b_urls = query.get_host_urls(host_b, 10);
    CHECK(test, host_a_urls.size() == 1 && host_a_urls[0].url == url_a);
    CHECK(test, host_b_urls.size() == 1 && host_b_urls[0].url == url_b);
    CHECK(test, query.get_versions(url_a).size() == 2);
    CHECK(test, query.get_host_summary(host_a).record_count == 2);

    auto matching_hosts = query.search_host_substring("example.test", 10);
    bool found_a = false, found_b = false;
    for (const auto& item : matching_hosts) {
        found_a = found_a || item.first == host_a;
        found_b = found_b || item.first == host_b;
    }
    CHECK(test, found_a);
    CHECK(test, found_b);

    uint32_t total = 0, hosts = 0, date_min = 0, date_max = 0;
    query.get_stats(total, hosts, date_min, date_max);
    CHECK(test, total == 3);
    CHECK(test, hosts == 2);
    CHECK(test, date_min == 20240131);
    CHECK(test, date_max == 20240210);
}

}  // namespace

int main() {
    TestContext test;
    try {
        TempDir temp;
        test_dates_and_tokenizer(test);
        test_gbk_error_recovery(test);
        test_truncated_parser_record(test, temp.path());
        test_store_and_reader(test, temp.path());
        test_uncreatable_store(test, temp.path());
        test_truncated_store_append(test, temp.path());
        test_corrupt_shard(test, temp.path());
        test_v2_shard_validation_and_legacy_dates(test, temp.path());
        test_corrupt_input_red_team(test, temp.path());
        test_empty_shard_compatibility(test, temp.path());
        test_query_index_roundtrip(test, temp.path());
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: unexpected exception: %s\n", error.what());
        return 1;
    }
    return test.finish();
}
