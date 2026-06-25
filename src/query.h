/*
 * query.h — QueryEngine: mmap'd shard index, URL lookup, host search.
 */

#ifndef QUERY_H
#define QUERY_H

#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <mutex>
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
    const char* url_pool = nullptr;
    bool is_v2 = false;

    bool open(const char* path);
    ~MappedShard();

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
    struct Article {
        std::string url;
        std::string title;
        std::string body;
        uint32_t date = 0;
        bool valid = true;   // false if CRC mismatch or decompression failed
    };

    explicit ArticleReader(const std::string& data_dir);
    ~ArticleReader();
    ArticleReader(const ArticleReader&) = delete;

    FILE* open_file(const std::string& rel_path);
    Article read_article(const std::string& rel_path, int64_t offset, uint32_t size);
};

// ── Query Engine ──────────────────────────────────────────────

class QueryEngine {
    std::string data_dir_;
    std::string index_dir_;
    MappedShard shards_[NUM_SHARDS];
    ArticleReader reader_;
    int shards_loaded_ = 0;

    // Precomputed data (loaded from disk at init)
    struct YearCount {
        uint32_t year;
        uint32_t count;
    };
    struct TodayEntry {
        uint16_t mmdd;
        std::vector<std::string> urls;
    };
    mutable std::vector<YearCount> year_dist_cached_;
    // Guards the lazy fill of year_dist_cached_ on the slow (no precompute) path,
    // since the server calls get_year_distribution() from multiple worker threads.
    mutable std::mutex year_dist_mtx_;
    std::vector<TodayEntry> today_data_;

    static std::string data_path(uint32_t crawl_date);
    void load_year_dist();
    void load_today();

public:
    struct UrlWithDate {
        std::string url;
        uint32_t date;
    };
    struct Version {
        uint32_t date;
        int record_count;
    };

    QueryEngine(const std::string& data_dir, const std::string& index_dir);

    bool init();

    // Single-page lookup
    ArticleReader::Article get_page(const std::string& url, uint32_t date = 0);

    // Version listing (sorted newest first)
    std::vector<Version> get_versions(const std::string& url);

    // Host substring search — scans all host blocks
    std::vector<std::pair<std::string, uint32_t>> search_host_substring(
            const std::string& substr, int limit = 200);

    // Host URL listing — reads URLs from mmap (v2)
    std::vector<UrlWithDate> get_host_urls(const std::string& host, int limit = 200);

    // URL prefix search — from mmap (v2), linear scan
    std::vector<std::string> search_prefix(const std::string& prefix, int limit = 100);

    // Stats
    void get_stats(uint32_t& total_articles, uint32_t& total_urls,
                   uint32_t& date_min, uint32_t& date_max);

    // Top hosts by entry count
    std::vector<std::pair<std::string, uint32_t>> get_top_hosts(int limit = 15);

    // Random URL
    std::string get_random_url();

    // Year distribution (precomputed, fallback to scan)
    std::vector<YearCount> get_year_distribution();

    // Today in history (precomputed, fallback to scan)
    std::vector<std::string> get_today_in_history(uint32_t mmdd, int limit = 10);

    // Browse by date
    std::vector<UrlWithDate> get_by_date(uint32_t date, int limit = 200);

    // Get article by exact URL + date (for diff)
    ArticleReader::Article get_page_by_date(const std::string& url, uint32_t date);

    // ── Title-based search (hot topic timeline) ──────────────
    // Search by title keywords, returns (url, date, title) sorted by date DESC
    std::vector<TitlePosting> search_by_title(const std::string& query, int limit = 50);

private:
    // Slow fallback methods when no precomputed data is available
    std::vector<std::string> get_today_in_history_slow(uint32_t mmdd, int limit);
    std::vector<YearCount> get_year_distribution_slow();
    std::string get_entry_url(const UrlIndexEntry& ent, int sid);

    // Title index (loaded from disk)
    // term → [TitlePosting]  sorted by date DESC, capped at TITLE_INDEX_TOP_K
    struct TitleTerm {
        std::string term;
        std::vector<TitlePosting> postings;
    };
    std::vector<TitleTerm> title_index_;
    void load_title_index();
    const std::vector<TitlePosting>* find_term(const std::string& term) const;
};

#endif // QUERY_H
