/*
 * query.cpp — QueryEngine: URL lookup, host search, prefix search.
 *
 * Uses mmap'd shard index files for zero-copy binary search.
 * All lookups are O(log N).  v2 shards embed URLs directly in the
 * index file (url_pool), eliminating data-file IO for searches.
 */

#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <map>
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
    const char* url_pool = nullptr;   // v2: embedded URL strings
    bool is_v2 = false;               // true if SHARD_MAGIC (v2), false if v1

    bool open(const char* path) {
        fd = ::open(path, O_RDONLY);
        if (fd < 0) return false;

        struct stat st;
        if (fstat(fd, &st) < 0) { close(fd); fd = -1; return false; }
        file_size = st.st_size;

        data = mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) { close(fd); fd = -1; return false; }

        header = static_cast<const ShardFileHeader*>(data);
        if (header->magic != SHARD_MAGIC && header->magic != SHARD_MAGIC_V1) {
            munmap(data, file_size); close(fd); fd = -1; data = nullptr;
            return false;
        }
        is_v2 = (header->magic == SHARD_MAGIC);
        hosts = reinterpret_cast<const HostBlock*>(header + 1);
        entries = reinterpret_cast<const UrlIndexEntry*>(hosts + header->host_count);

        // v2: URL pool follows entries array
        if (is_v2 && header->url_pool_size > 0) {
            url_pool = reinterpret_cast<const char*>(entries + header->entry_count);
        }
        return true;
    }

    ~MappedShard() {
        if (data && data != MAP_FAILED) munmap(data, file_size);
        if (fd >= 0) close(fd);
    }

    // Non-copyable, movable
    MappedShard() = default;
    MappedShard(const MappedShard&) = delete;
    MappedShard& operator=(const MappedShard&) = delete;
    MappedShard(MappedShard&& o) noexcept
        : fd(o.fd), file_size(o.file_size), data(o.data), header(o.header),
          hosts(o.hosts), entries(o.entries), url_pool(o.url_pool), is_v2(o.is_v2) {
        o.fd = -1; o.data = nullptr; o.url_pool = nullptr;
    }
};

// ── Article Reader (data-file access for full article bodies) ──

class ArticleReader {
    std::string data_dir_;
    std::unordered_map<std::string, FILE*> open_files_;

public:
    explicit ArticleReader(const std::string& data_dir) : data_dir_(data_dir) {}
    ~ArticleReader() {
        for (auto& kv : open_files_) fclose(kv.second);
    }
    ArticleReader(const ArticleReader&) = delete;

    FILE* open_file(const std::string& rel_path) {
        auto it = open_files_.find(rel_path);
        if (it != open_files_.end()) return it->second;
        std::string full = data_dir_ + "/" + rel_path;
        FILE* f = fopen(full.c_str(), "rb");
        if (f) open_files_[rel_path] = f;
        return f;
    }

    // Read full article (decompresses body if needed)
    struct Article {
        std::string url;
        std::string title;
        std::string body;
        uint32_t date;
    };

    Article read_article(const std::string& rel_path, int64_t offset, uint32_t size) {
        Article art = {};
        if (size < ArticleRecord::HEADER_SIZE) return art;

        std::vector<char> buf(size);
        FILE* f = open_file(rel_path);
        if (!f) return art;
        fseeko(f, offset, SEEK_SET);
        if (fread(buf.data(), 1, size, f) != size) return art;

        auto* rec = reinterpret_cast<const ArticleRecord*>(buf.data());
        if (rec->magic != ARTICLE_MAGIC) return art;

        art.url.assign(rec->url(), rec->url_len);
        art.title.assign(rec->title(), rec->title_len);
        art.date = rec->crawl_date;

        bool compressed = (rec->flags & 1);
        if (compressed && rec->body_compr_len > 0) {
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
    ArticleReader reader_;          // shared, lives for the process lifetime
    int shards_loaded_ = 0;

    // Reconstruct data-file path from crawl date
    static std::string data_path(uint32_t crawl_date) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%04u%02u/data_0001.dat",
                 crawl_date / 10000, (crawl_date / 100) % 100);
        return buf;
    }

public:
    QueryEngine(const std::string& data_dir, const std::string& index_dir)
        : data_dir_(data_dir), index_dir_(index_dir), reader_(data_dir) {}

    bool init() {
        srand(static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid()));
        for (int i = 0; i < NUM_SHARDS; i++) {
            char path[256];
            snprintf(path, sizeof(path), "%s/url_%02d.idx", index_dir_.c_str(), i);
            if (!shards_[i].open(path)) continue;
            shards_loaded_++;
        }
        printf("Loaded %d/%d shard index files\n", shards_loaded_, NUM_SHARDS);
        return shards_loaded_ > 0;
    }

    // ── URL resolve helpers ──────────────────────────────────

    // Get URL string for an entry (v2: from mmap pool; v1: from data file)
    std::string get_entry_url(const UrlIndexEntry& ent, int sid) {
        auto& shard = shards_[sid];
        if (shard.is_v2 && shard.url_pool) {
            return entry_url(ent, shard.url_pool);
        }
        // v1 fallback: read from data file
        auto buf = reader_.read_article(data_path(ent.crawl_date),
                                         ent.file_offset, ent.record_size);
        return buf.url;
    }

    // ── Single-page lookup ───────────────────────────────────

    ArticleReader::Article get_page(const std::string& url, uint32_t date = 0) {
        std::string host = extract_host(url);
        int sid = shard_for_host(host);
        if (sid < 0 || sid >= NUM_SHARDS) return {};

        auto& shard = shards_[sid];
        if (!shard.data) return {};

        auto* hb = find_host(shard.hosts, shard.header->host_count, host);
        if (!hb) return {};

        uint64_t hash = ::url_hash(url);
        const UrlIndexEntry* block = shard.entries + hb->first_entry;
        auto* first = find_first(block, hb->entry_count, hash);
        if (!first) return {};

        // Count versions and find best match
        uint32_t idx = static_cast<uint32_t>(first - block);
        uint32_t count = 0;
        while (idx + count < hb->entry_count && block[idx + count].url_hash == hash) count++;

        uint32_t best_idx = idx;
        if (date > 0 && count > 1) {
            uint32_t best_diff = UINT32_MAX;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t d = block[idx + i].crawl_date;
                uint32_t diff = (d > date) ? (d - date) : (date - d);
                if (diff < best_diff) { best_diff = diff; best_idx = idx + i; }
            }
        }

        auto& ent = block[best_idx];
        return reader_.read_article(data_path(ent.crawl_date), ent.file_offset, ent.record_size);
    }

    // ── Version listing ──────────────────────────────────────

    struct Version {
        uint32_t date;
        int record_count;
    };
    std::vector<Version> get_versions(const std::string& url) {
        std::vector<Version> vers;
        std::string host = extract_host(url);
        int sid = shard_for_host(host);
        if (sid < 0 || sid >= NUM_SHARDS) return vers;

        auto& shard = shards_[sid];
        if (!shard.data) return vers;

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
                   block[idx + count].crawl_date == v.date) count++;
            v.record_count = count;
            vers.push_back(v);
            idx += count;
        }
        return vers;
    }

    // ── Host search (substring) — scans all host blocks ──────

    std::vector<std::pair<std::string, uint32_t>> search_host_substring(
            const std::string& substr, int limit = 200) {
        std::vector<std::pair<std::string, uint32_t>> results;
        for (int sid = 0; sid < NUM_SHARDS; sid++) {
            auto& shard = shards_[sid];
            if (!shard.data) continue;
            for (uint32_t i = 0; i < shard.header->host_count &&
                 results.size() < static_cast<size_t>(limit); i++) {
                std::string host_name(shard.hosts[i].host,
                    strnlen(shard.hosts[i].host, HOST_HASH_LEN));
                if (host_name.find(substr) != std::string::npos) {
                    results.emplace_back(host_name, shard.hosts[i].entry_count);
                }
            }
        }
        return results;
    }

    // ── Host URL listing — reads URLs from mmap (v2) ─────────

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

        const UrlIndexEntry* block = shard.entries + hb->first_entry;
        std::string last_url;

        for (uint32_t i = 0; i < hb->entry_count && urls.size() < static_cast<size_t>(limit); i++) {
            auto& ent = block[i];
            std::string url;
            if (shard.is_v2 && shard.url_pool) {
                url = entry_url(ent, shard.url_pool);
            } else {
                // v1 fallback
                url = reader_.read_article(data_path(ent.crawl_date),
                                            ent.file_offset,
                                            std::min<uint32_t>(ent.record_size,
                                                     ArticleRecord::HEADER_SIZE + 1024u)).url;
            }
            if (!url.empty() && url != last_url) {
                urls.push_back({url, ent.crawl_date});
                last_url = url;
            }
        }
        return urls;
    }

    // ── URL prefix search — from mmap (v2), linear scan ──────

    std::vector<std::string> search_prefix(const std::string& prefix, int limit = 100) {
        std::vector<std::string> urls;

        for (int sid = 0; sid < NUM_SHARDS && urls.size() < static_cast<size_t>(limit); sid++) {
            auto& shard = shards_[sid];
            if (!shard.data) continue;

            // v2 fast path: inline URL pool, no data-file IO
            if (shard.is_v2 && shard.url_pool) {
                for (uint32_t i = 0; i < shard.header->entry_count &&
                     urls.size() < static_cast<size_t>(limit); i++) {
                    auto& ent = shard.entries[i];
                    if (entry_url_has_prefix(ent, shard.url_pool, prefix) ||
                        entry_host_contains(ent, shard.url_pool, prefix)) {
                        std::string u = entry_url(ent, shard.url_pool);
                        if (!u.empty() && (urls.empty() || u != urls.back())) {
                            urls.push_back(u);
                        }
                    }
                }
            } else {
                // v1 fallback: need data-file reads
                for (uint32_t i = 0; i < shard.header->entry_count &&
                     urls.size() < static_cast<size_t>(limit); i++) {
                    auto& ent = shard.entries[i];
                    auto art = reader_.read_article(data_path(ent.crawl_date),
                        ent.file_offset,
                        std::min<uint32_t>(ent.record_size, ArticleRecord::HEADER_SIZE + 1024u));
                    if (!art.url.empty() &&
                        (art.url.find(prefix) == 0 || extract_host(art.url).find(prefix) == 0)) {
                        if (urls.empty() || art.url != urls.back()) urls.push_back(art.url);
                    }
                }
            }
        }
        return urls;
    }

    // ── Stats ────────────────────────────────────────────────

    void get_stats(uint32_t& total_articles, uint32_t& total_urls,
                   uint32_t& date_min, uint32_t& date_max) {
        // Fast path: read from meta.dat
        char meta_path[256];
        snprintf(meta_path, sizeof(meta_path), "%s/meta.dat", index_dir_.c_str());
        FILE* mf = fopen(meta_path, "rb");
        if (mf) {
            ArchiveMeta meta;
            if (fread(&meta, sizeof(meta), 1, mf) == 1) {
                total_articles = meta.total_articles;
                total_urls = meta.total_urls;
                date_min = meta.date_min;
                date_max = meta.date_max;
                fclose(mf);
                if (total_urls > 0) return;
            } else {
                fclose(mf);
            }
        }
        // Slow path: compute from shard headers
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

    // ── Top hosts by entry count ───────────────────────────

    std::vector<std::pair<std::string, uint32_t>> get_top_hosts(int limit = 15) {
        std::vector<std::pair<std::string, uint32_t>> hosts;
        for (int sid = 0; sid < NUM_SHARDS; sid++) {
            auto& shard = shards_[sid];
            if (!shard.data) continue;
            for (uint32_t i = 0; i < shard.header->host_count; i++) {
                std::string name(shard.hosts[i].host, strnlen(shard.hosts[i].host, HOST_HASH_LEN));
                hosts.emplace_back(name, shard.hosts[i].entry_count);
            }
        }
        std::sort(hosts.begin(), hosts.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
        if (hosts.size() > static_cast<size_t>(limit))
            hosts.resize(limit);
        return hosts;
    }

    // ── Random URL ─────────────────────────────────────────

    std::string get_random_url() {
        // Pick a random shard, then random entry within it
        std::vector<int> valid_shards;
        for (int sid = 0; sid < NUM_SHARDS; sid++)
            if (shards_[sid].data && shards_[sid].header->entry_count > 0)
                valid_shards.push_back(sid);
        if (valid_shards.empty()) return "";

        int sid = valid_shards[rand() % valid_shards.size()];
        auto& shard = shards_[sid];
        uint32_t idx = rand() % shard.header->entry_count;
        auto& ent = shard.entries[idx];

        if (shard.is_v2 && shard.url_pool)
            return entry_url(ent, shard.url_pool);
        // v1 fallback
        return reader_.read_article(data_path(ent.crawl_date),
            ent.file_offset, std::min<uint32_t>(ent.record_size,
                ArticleRecord::HEADER_SIZE + 1024u)).url;
    }

    // ── Year distribution (cached) ─────────────────────────

    struct YearCount {
        uint32_t year;
        uint32_t count;
    };
    std::vector<YearCount> get_year_distribution() {
        if (!year_dist_cached_.empty()) return year_dist_cached_;

        std::map<uint32_t, uint32_t> ymap;
        for (int sid = 0; sid < NUM_SHARDS; sid++) {
            auto& shard = shards_[sid];
            if (!shard.data) continue;
            for (uint32_t i = 0; i < shard.header->entry_count; i++) {
                uint32_t y = shard.entries[i].crawl_date / 10000;
                ymap[y]++;
            }
        }
        for (auto& kv : ymap)
            year_dist_cached_.push_back({kv.first, kv.second});
        return year_dist_cached_;
    }

    // ── Today in history ───────────────────────────────────

    std::vector<std::string> get_today_in_history(uint32_t mmdd, int limit = 10) {
        std::vector<std::string> urls;
        if (mmdd < 101 || mmdd > 1231) return urls;

        // Scan: collect entries matching MMDD, preference newer years
        struct Candidate {
            std::string url;
            uint32_t date;
        };
        std::vector<Candidate> candidates;

        for (int sid = 0; sid < NUM_SHARDS && candidates.size() < 5000; sid++) {
            auto& shard = shards_[sid];
            if (!shard.data) continue;
            for (uint32_t i = 0; i < shard.header->entry_count &&
                 candidates.size() < 5000; i++) {
                auto& ent = shard.entries[i];
                if ((ent.crawl_date % 10000) == mmdd) {
                    std::string url;
                    if (shard.is_v2 && shard.url_pool)
                        url = entry_url(ent, shard.url_pool);
                    else {
                        url = reader_.read_article(data_path(ent.crawl_date),
                            ent.file_offset, std::min<uint32_t>(ent.record_size,
                                ArticleRecord::HEADER_SIZE + 1024u)).url;
                    }
                    if (!url.empty())
                        candidates.push_back({url, ent.crawl_date});
                }
            }
        }

        // Sort by date DESC (newer first), dedup URLs, take limit
        std::sort(candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) { return a.date > b.date; });

        std::string last;
        for (auto& c : candidates) {
            if (c.url == last) continue;
            last = c.url;
            urls.push_back(c.url);
            if (urls.size() >= static_cast<size_t>(limit)) break;
        }
        return urls;
    }

    // ── Browse by date ─────────────────────────────────────

    std::vector<UrlWithDate> get_by_date(uint32_t date, int limit = 200) {
        std::vector<UrlWithDate> results;
        for (int sid = 0; sid < NUM_SHARDS && results.size() < static_cast<size_t>(limit); sid++) {
            auto& shard = shards_[sid];
            if (!shard.data) continue;
            for (uint32_t i = 0; i < shard.header->entry_count &&
                 results.size() < static_cast<size_t>(limit); i++) {
                auto& ent = shard.entries[i];
                if (ent.crawl_date == date) {
                    std::string url;
                    if (shard.is_v2 && shard.url_pool)
                        url = entry_url(ent, shard.url_pool);
                    else {
                        url = reader_.read_article(data_path(ent.crawl_date),
                            ent.file_offset, std::min<uint32_t>(ent.record_size,
                                ArticleRecord::HEADER_SIZE + 1024u)).url;
                    }
                    if (!url.empty())
                        results.push_back({url, ent.crawl_date});
                }
            }
        }
        return results;
    }

    // ── Get article by exact URL + date (for diff) ─────────

    ArticleReader::Article get_page_by_date(const std::string& url, uint32_t date) {
        std::string host = extract_host(url);
        int sid = shard_for_host(host);
        if (sid < 0 || sid >= NUM_SHARDS) return {};

        auto& shard = shards_[sid];
        if (!shard.data) return {};

        auto* hb = find_host(shard.hosts, shard.header->host_count, host);
        if (!hb) return {};

        uint64_t hash = ::url_hash(url);
        const UrlIndexEntry* block = shard.entries + hb->first_entry;
        auto* first = find_first(block, hb->entry_count, hash);
        if (!first) return {};

        uint32_t idx = static_cast<uint32_t>(first - block);
        while (idx < hb->entry_count && block[idx].url_hash == hash) {
            if (block[idx].crawl_date == date) {
                auto& ent = block[idx];
                return reader_.read_article(data_path(ent.crawl_date),
                    ent.file_offset, ent.record_size);
            }
            idx++;
        }
        return {};
    }

private:
    mutable std::vector<YearCount> year_dist_cached_;
};
