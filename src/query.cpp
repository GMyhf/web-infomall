/*
 * query.cpp — QueryEngine: URL lookup, host search, prefix search.
 *
 * Uses mmap'd shard index files for zero-copy binary search.
 * All lookups are O(log N).
 */

#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

// ── Mapped Shard File ─────────────────────────────────────────

struct MappedShard {
    int fd = -1;
    size_t file_size = 0;
    void* data = nullptr;
    const ShardFileHeader* header = nullptr;
    const HostBlock* hosts = nullptr;
    const UrlIndexEntry* entries = nullptr;

    bool open(const char* path) {
        fd = ::open(path, O_RDONLY);
        if (fd < 0) return false;

        struct stat st;
        if (fstat(fd, &st) < 0) { close(fd); fd = -1; return false; }
        file_size = st.st_size;

        data = mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) { close(fd); fd = -1; return false; }

        header = static_cast<const ShardFileHeader*>(data);
        if (header->magic != SHARD_MAGIC) {
            munmap(data, file_size); close(fd); fd = -1; data = nullptr;
            return false;
        }
        hosts = reinterpret_cast<const HostBlock*>(header + 1);
        entries = reinterpret_cast<const UrlIndexEntry*>(hosts + header->host_count);
        return true;
    }

    ~MappedShard() {
        if (data && data != MAP_FAILED) munmap(data, file_size);
        if (fd >= 0) close(fd);
    }

    // Non-copyable
    MappedShard() = default;
    MappedShard(const MappedShard&) = delete;
    MappedShard& operator=(const MappedShard&) = delete;
    MappedShard(MappedShard&& o) noexcept
        : fd(o.fd), file_size(o.file_size), data(o.data), header(o.header), hosts(o.hosts), entries(o.entries) {
        o.fd = -1; o.data = nullptr;
    }
};

// ── Article Reader ────────────────────────────────────────────

class ArticleReader {
    std::string data_dir_;
    std::unordered_map<std::string, FILE*> open_files_; // cached file handles

public:
    explicit ArticleReader(const std::string& data_dir) : data_dir_(data_dir) {}
    ~ArticleReader() {
        for (auto& kv : open_files_) fclose(kv.second);
    }
    // Non-copyable
    ArticleReader(const ArticleReader&) = delete;

    // Open a data file (cached)
    FILE* open_file(const std::string& rel_path) {
        auto it = open_files_.find(rel_path);
        if (it != open_files_.end()) return it->second;
        std::string full = data_dir_ + "/" + rel_path;
        FILE* f = fopen(full.c_str(), "rb");
        if (f) open_files_[rel_path] = f;
        return f;
    }

    // Read just the URL from an article record (header + URL field only, fast)
    std::string read_url(const std::string& rel_path, int64_t offset, uint32_t record_size) {
        FILE* f = open_file(rel_path);
        if (!f) return "";

        // Read header + URL (which immediately follows the header)
        // Read enough for max URL length (typically < 256 bytes)
        constexpr uint32_t read_size = ArticleRecord::HEADER_SIZE + 1024;
        char buf[read_size];

        fseeko(f, offset, SEEK_SET);
        size_t to_read = std::min(record_size, read_size);
        size_t n = fread(buf, 1, to_read, f);
        if (n < ArticleRecord::HEADER_SIZE) return "";

        auto* rec = reinterpret_cast<const ArticleRecord*>(buf);
        if (rec->magic != ARTICLE_MAGIC) return "";
        if (rec->url_len == 0 || rec->url_len > 2048) return "";

        std::string url(rec->url(), rec->url_len);
        return url;
    }

    // Read a full ArticleRecord from a data file
    std::vector<char> read_record(const std::string& rel_path, int64_t offset, uint32_t size) {
        FILE* f = open_file(rel_path);
        if (!f) return {};

        std::vector<char> buf(size);
        fseeko(f, offset, SEEK_SET);
        size_t n = fread(buf.data(), 1, size, f);

        if (n != size) return {};
        return buf;
    }

    // Read and decompress an article
    struct Article {
        std::string url;
        std::string title;
        std::string body;
        uint32_t date;
    };

    Article read_article(const std::string& rel_path, int64_t offset, uint32_t size) {
        Article art = {};
        auto buf = read_record(rel_path, offset, size);
        if (buf.size() < ArticleRecord::HEADER_SIZE) return art;

        auto* rec = reinterpret_cast<const ArticleRecord*>(buf.data());
        if (rec->magic != ARTICLE_MAGIC) return art;

        art.url.assign(rec->url(), rec->url_len);
        art.title.assign(rec->title(), rec->title_len);
        art.date = rec->crawl_date;

        bool compressed = (rec->flags & 1);
        if (compressed) {
            std::vector<char> decomp(rec->body_orig_len + 1);
            uLongf dest_len = rec->body_orig_len;
            int ret = ::uncompress(
                reinterpret_cast<Bytef*>(decomp.data()), &dest_len,
                reinterpret_cast<const Bytef*>(rec->body()), rec->body_compr_len);
            if (ret == Z_OK) {
                decomp.resize(dest_len);
                art.body.assign(decomp.data(), decomp.size());
            } else {
                art.body.assign(rec->body(), rec->body_compr_len);
            }
        } else {
            art.body.assign(rec->body(), rec->body_compr_len);
        }
        return art;
    }
};

// ── Query Engine ──────────────────────────────────────────────

class QueryEngine {
    std::string data_dir_;
    std::string index_dir_;
    MappedShard shards_[NUM_SHARDS];
    int shards_loaded_ = 0;

public:
    QueryEngine(const std::string& data_dir, const std::string& index_dir)
        : data_dir_(data_dir), index_dir_(index_dir) {}

    bool init() {
        for (int i = 0; i < NUM_SHARDS; i++) {
            char path[256];
            snprintf(path, sizeof(path), "%s/url_%02d.idx", index_dir_.c_str(), i);
            if (!shards_[i].open(path)) {
                // Empty shard — OK, just skip
                continue;
            }
            shards_loaded_++;
        }
        printf("Loaded %d/%d shard index files\n", shards_loaded_, NUM_SHARDS);
        return shards_loaded_ > 0;
    }

    // Get a single page by URL (optionally closest to date)
    ArticleReader::Article get_page(const std::string& url, uint32_t date = 0) {
        int sid = shard_for_url(url);
        if (sid < 0 || sid >= NUM_SHARDS) return {};

        auto& shard = shards_[sid];
        if (!shard.data) return {};

        // With (host, url_hash, date) sort: find host block first
        std::string host = extract_host(url);
        auto* hb = find_host(shard.hosts, shard.header->host_count, host);
        if (!hb) return {};

        uint64_t hash = ::url_hash(url);
        const UrlIndexEntry* block = shard.entries + hb->first_entry;
        auto* first = find_first(block, hb->entry_count, hash);
        if (!first) return {};

        // Find all versions (contiguous within host block)
        uint32_t idx = static_cast<uint32_t>(first - block);
        uint32_t count = 0;
        while (idx + count < hb->entry_count &&
               block[idx + count].url_hash == hash) {
            count++;
        }

        if (count == 0) return {};

        // If date specified, find closest
        uint32_t best_idx = idx;
        if (date > 0 && count > 1) {
            uint32_t best_diff = UINT32_MAX;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t d = block[idx + i].crawl_date;
                uint32_t diff = (d > date) ? (d - date) : (date - d);
                if (diff < best_diff) {
                    best_diff = diff;
                    best_idx = idx + i;
                }
            }
        }

        auto& ent = block[best_idx];
        // Reconstruct file path from shard/entry data
        // The data file is organized by month; we need to reconstruct the path
        char rel_path[128];
        uint32_t d = ent.crawl_date;
        snprintf(rel_path, sizeof(rel_path), "%04u%02u/data_0001.dat",
                 d / 10000, (d / 100) % 100);

        ArticleReader reader(data_dir_);
        return reader.read_article(rel_path, ent.file_offset, ent.record_size);
    }

    // Get version list for a URL
    struct Version {
        uint32_t date;
        int record_count;
    };
    std::vector<Version> get_versions(const std::string& url) {
        std::vector<Version> vers;
        int sid = shard_for_url(url);
        if (sid < 0 || sid >= NUM_SHARDS) return vers;

        auto& shard = shards_[sid];
        if (!shard.data) return vers;

        std::string host = extract_host(url);
        auto* hb = find_host(shard.hosts, shard.header->host_count, host);
        if (!hb) return vers;

        uint64_t hash = ::url_hash(url);
        const UrlIndexEntry* block = shard.entries + hb->first_entry;
        auto* first = find_first(block, hb->entry_count, hash);
        if (!first) return vers;

        uint32_t idx = static_cast<uint32_t>(first - block);
        while (idx < hb->entry_count && block[idx].url_hash == hash) {
            Version v;
            v.date = block[idx].crawl_date;
            uint32_t count = 1;
            while (idx + count < hb->entry_count &&
                   block[idx + count].url_hash == hash &&
                   block[idx + count].crawl_date == v.date) {
                count++;
            }
            v.record_count = count;
            vers.push_back(v);
            idx += count;
        }
        return vers;
    }

    // Search by host substring — scans all host blocks (fast, index-only)
    std::vector<std::pair<std::string, uint32_t>> search_host_substring(const std::string& substr, int limit = 200) {
        std::vector<std::pair<std::string, uint32_t>> results; // (host, count)
        for (int sid = 0; sid < NUM_SHARDS; sid++) {
            auto& shard = shards_[sid];
            if (!shard.data) continue;
            for (uint32_t i = 0; i < shard.header->host_count && results.size() < static_cast<size_t>(limit); i++) {
                std::string host_name(shard.hosts[i].host, strnlen(shard.hosts[i].host, HOST_HASH_LEN));
                if (host_name.find(substr) != std::string::npos) {
                    results.emplace_back(host_name, shard.hosts[i].entry_count);
                }
            }
        }
        return results;
    }

    // Search by host — returns URLs with dates
    struct UrlWithDate {
        std::string url;
        uint32_t date;
    };
    std::vector<UrlWithDate> get_host_urls(const std::string& host, int limit = 200) {
        std::vector<UrlWithDate> urls;
        int sid = shard_for_host(host);
        if (sid < 0 || sid >= NUM_SHARDS) return urls;

        auto& shard = shards_[sid];
        if (!shard.data) return urls;

        auto* hb = find_host(shard.hosts, shard.header->host_count, host);
        if (!hb) return urls;

        ArticleReader reader(data_dir_);
        for (uint32_t i = 0; i < hb->entry_count && urls.size() < static_cast<size_t>(limit); i++) {
            auto& ent = shard.entries[hb->first_entry + i];
            uint32_t d = ent.crawl_date;
            char rel_path[128];
            snprintf(rel_path, sizeof(rel_path), "%04u%02u/data_0001.dat",
                     d / 10000, (d / 100) % 100);

            std::string url = reader.read_url(rel_path, ent.file_offset, ent.record_size);
            if (!url.empty() && (urls.empty() || url != urls.back().url)) {
                urls.push_back({url, d});
            }
        }
        return urls;
    }

    // Search by URL prefix
    std::vector<std::string> search_prefix(const std::string& prefix, int limit = 100) {
        std::vector<std::string> urls;

        // Search all shards (prefix could match any host)
        ArticleReader reader(data_dir_);
        for (int sid = 0; sid < NUM_SHARDS && urls.size() < static_cast<size_t>(limit); sid++) {
            auto& shard = shards_[sid];
            if (!shard.data) continue;

            // Linear scan for prefix match (could be optimized with trie)
            for (uint32_t i = 0; i < shard.header->entry_count && urls.size() < static_cast<size_t>(limit); i++) {
                auto& ent = shard.entries[i];
                uint32_t d = ent.crawl_date;
                char rel_path[128];
                snprintf(rel_path, sizeof(rel_path), "%04u%02u/data_0001.dat",
                         d / 10000, (d / 100) % 100);

                // Read just the URL
                auto buf = reader.read_record(rel_path, ent.file_offset,
                    std::min(ent.record_size, ArticleRecord::HEADER_SIZE + 512u));
                if (buf.size() >= ArticleRecord::HEADER_SIZE) {
                    auto* rec = reinterpret_cast<const ArticleRecord*>(buf.data());
                    if (rec->magic == ARTICLE_MAGIC) {
                        std::string url(rec->url(), rec->url_len);
                        if (url.find(prefix) == 0 || extract_host(url).find(prefix) == 0) {
                            if (urls.empty() || url != urls.back()) {
                                urls.push_back(url);
                            }
                        }
                    }
                }
            }
        }
        return urls;
    }

    // Stats (cached from first call, pre-computed during index load)
    void get_stats(uint32_t& total_articles, uint32_t& total_urls,
                   uint32_t& date_min, uint32_t& date_max) {
        // Read from meta file if available (fast path)
        char meta_path[256];
        snprintf(meta_path, sizeof(meta_path), "%s/meta.dat", index_dir_.c_str());
        FILE* mf = fopen(meta_path, "rb");
        if (mf) {
            ArchiveMeta meta;
            if (fread(&meta, sizeof(meta), 1, mf) == 1) {
                total_articles = meta.total_articles;
                // If total_urls is 0 in meta, recompute from shards
                total_urls = meta.total_urls;
                date_min = meta.date_min;
                date_max = meta.date_max;
                fclose(mf);
                if (total_urls > 0) return;
                // Fall through to compute total_urls from shards
            } else {
                fclose(mf);
                // Fall through to compute from shards
            }
        }
        // Compute totals from shards (slow path, only if meta missing/incomplete)
        total_articles = 0; total_urls = 0;
        date_min = UINT32_MAX; date_max = 0;
        for (int sid = 0; sid < NUM_SHARDS; sid++) {
            auto& shard = shards_[sid];
            if (!shard.data) continue;
            total_articles += shard.header->entry_count;
            total_urls += shard.header->host_count;
            if (shard.header->entry_count > 0) {
                uint32_t d = shard.entries[0].crawl_date;
                if (d < date_min) date_min = d;
                d = shard.entries[shard.header->entry_count - 1].crawl_date;
                if (d > date_max) date_max = d;
            }
        }
    }
};
