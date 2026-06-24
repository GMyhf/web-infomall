/*
 * common.h — Core data structures for the Web InfoMall archive system.
 *
 * Follows the Depot TWebPageData pattern: fixed header + variable data buffer.
 * Simplified for news articles (no HTTP headers, just title + body).
 */

#ifndef COMMON_H
#define COMMON_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

// ── Constants ─────────────────────────────────────────────────

constexpr uint32_t ARTICLE_MAGIC   = 0x494E464F;  // "INFO"
constexpr uint32_t SHARD_MAGIC_V1  = 0x49445820;  // "IDX " (original format)
constexpr uint32_t SHARD_MAGIC     = 0x49445821;  // "IDX!" (v2: URL pool embedded)
constexpr int      NUM_SHARDS      = 37;           // Prime, ~Depot's 39
constexpr int      HOST_HASH_LEN   = 32;
constexpr int64_t  MAX_DAT_FILE    = 2LL * 1024 * 1024 * 1024; // 2GB
constexpr uint32_t MAX_URL_LEN     = 2048;         // Max URL length stored inline
constexpr uint32_t URL_PREFIX_LEN  = 64;           // For prefix search optimization

// ── Article Storage Format ────────────────────────────────────

#pragma pack(push, 1)
struct ArticleRecord {
    uint32_t magic;          // ARTICLE_MAGIC
    uint32_t flags;          // bit 0: compressed, bit 1: gb2312→utf8
    uint32_t crawl_date;     // YYYYMMDD
    uint32_t url_len;
    uint32_t title_len;
    uint32_t body_compr_len; // compressed body size (0 = uncompressed)
    uint32_t body_orig_len;  // original body size
    uint32_t record_size;    // total size including this header
    uint32_t mini_md5;       // first 4 bytes of URL MD5 (quick check)
    uint32_t reserved;

    // After header:
    //   char url[url_len]
    //   char title[title_len]
    //   char body[body_compr_len]

    const char* url()   const { return reinterpret_cast<const char*>(this + 1); }
    const char* title() const { return url() + url_len; }
    const char* body()  const { return title() + title_len; }
    uint32_t body_len() const { return body_compr_len; }

    static constexpr uint32_t HEADER_SIZE = 40;
};
#pragma pack(pop)

// ── URL Index Entry ───────────────────────────────────────────

#pragma pack(push, 1)
struct UrlIndexEntry {
    uint64_t url_hash;       // first 8 bytes of FNV-1a(URL) — sort key
    uint32_t crawl_date;     // YYYYMMDD — secondary sort (DESC)
    uint32_t file_offset;    // byte offset in the data file (fits ≤2GB)
    uint16_t record_size;    // record size in data file
    uint16_t url_len;        // URL length stored in string pool
    uint32_t url_offset;     // byte offset into URL string pool (at end of shard)
    uint32_t reserved;       // for future use
    // Total: 8 + 4 + 4 + 2 + 2 + 4 + 4 = 28 bytes (unchanged size)
    // In v2+ shard files, the URL is at: url_pool_base + url_offset
};
#pragma pack(pop)

// ── Host Block (embedded index) ───────────────────────────────

#pragma pack(push, 1)
struct HostBlock {
    char     host[HOST_HASH_LEN];  // hostname, \0-padded to 32 bytes
    uint32_t first_entry;          // index into UrlIndexEntry array
    uint32_t entry_count;          // number of entries for this host
    // Total: 32 + 4 + 4 = 40 bytes
};
#pragma pack(pop)

// ── Shard File Header ─────────────────────────────────────────

#pragma pack(push, 1)
struct ShardFileHeader {
    uint32_t magic;          // SHARD_MAGIC (v2) or SHARD_MAGIC_V1 (legacy)
    uint32_t entry_count;    // number of UrlIndexEntry records
    uint32_t host_count;     // number of HostBlock records
    uint32_t url_pool_size;  // size of URL string pool in bytes (0 in v1)
    // Followed by:
    //   HostBlock hosts[host_count]      (sorted by host)
    //   UrlIndexEntry entries[entry_count] (sorted by host, url_hash, crawl_date DESC)
    //   char url_pool[url_pool_size]      (concatenated null-terminated URLs)
};
#pragma pack(pop)

// ── Archive Metadata ──────────────────────────────────────────

struct ArchiveMeta {
    uint32_t total_articles;
    uint32_t total_urls;
    uint32_t date_min;
    uint32_t date_max;
};

// ── Hash Functions ────────────────────────────────────────────

// MD5-based URL hash (first 8 bytes of MD5 digest)
inline uint64_t url_hash(const std::string& url) {
    // FNV-1a 64-bit hash
    uint64_t fnv = 14695981039346656037ULL;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(url.data());
    size_t len = url.size();
    for (size_t i = 0; i < len; i++) {
        fnv ^= data[i];
        fnv *= 1099511628211ULL;
    }
    return fnv;
}

// Mini MD5 (first 4 bytes of MD5) — for quick record verification
inline uint32_t mini_md5(const std::string& url) {
    uint64_t h = url_hash(url);
    return static_cast<uint32_t>(h & 0xFFFFFFFF) ^ static_cast<uint32_t>(h >> 32);
}

// Host-based shard assignment (Depot's HashUrlFunciton pattern)
// XOR hostname characters, mod NUM_SHARDS
inline int shard_for_url(const std::string& url) {
    // Extract host
    const char* start = url.c_str();
    if (strncmp(start, "http://", 7) == 0) start += 7;
    else if (strncmp(start, "https://", 8) == 0) start += 8;
    const char* end = start;
    while (*end && *end != '/' && *end != ':' && *end != '?') end++;

    // XOR hostname bytes (Depot pattern)
    uint32_t h = 0;
    for (const char* p = start; p < end; p++) {
        h ^= static_cast<uint8_t>(*p);
        h = (h << 5) | (h >> 27);  // rotate
    }
    return static_cast<int>(h % NUM_SHARDS);
}

// Host-based shard assignment (direct host string)
inline int shard_for_host(const std::string& host) {
    uint32_t h = 0;
    for (size_t i = 0; i < host.size(); i++) {
        h ^= static_cast<uint8_t>(host[i]);
        h = (h << 5) | (h >> 27);
    }
    return static_cast<int>(h % NUM_SHARDS);
}

// ── URL Helpers ───────────────────────────────────────────────

inline std::string extract_host(const std::string& url) {
    const char* start = url.c_str();
    if (strncmp(start, "http://", 7) == 0) start += 7;
    else if (strncmp(start, "https://", 8) == 0) start += 8;
    const char* end = start;
    while (*end && *end != '/' && *end != ':' && *end != '?') end++;
    std::string host(start, end - start);
    // Lowercase
    std::transform(host.begin(), host.end(), host.begin(), ::tolower);
    return host;
}

// ── Comparators ───────────────────────────────────────────────

// Comparator for entries sorted by url_hash (used in binary search)
inline bool entry_by_url_hash(const UrlIndexEntry& a, const UrlIndexEntry& b) {
    return a.url_hash < b.url_hash;
}

inline bool host_cmp(const HostBlock& a, const HostBlock& b) {
    return strncmp(a.host, b.host, HOST_HASH_LEN) < 0;
}

// ── Binary Search Helpers ─────────────────────────────────────

// Find first entry with given url_hash (lower_bound on sorted array)
inline const UrlIndexEntry* find_first(const UrlIndexEntry* entries, uint32_t count, uint64_t hash) {
    int lo = 0, hi = static_cast<int>(count);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (entries[mid].url_hash < hash) lo = mid + 1;
        else hi = mid;
    }
    return (lo < static_cast<int>(count) && entries[lo].url_hash == hash) ? &entries[lo] : nullptr;
}

// Get URL string from v2 shard's URL pool
inline std::string entry_url(const UrlIndexEntry& e, const char* url_pool) {
    if (!url_pool || e.url_len == 0) return "";
    return std::string(url_pool + e.url_offset, e.url_len);
}

// Check if entry's URL starts with prefix (from v2 URL pool)
inline bool entry_url_has_prefix(const UrlIndexEntry& e, const char* url_pool,
                                  const std::string& prefix) {
    if (!url_pool || e.url_len < prefix.size()) return false;
    return memcmp(url_pool + e.url_offset, prefix.data(), prefix.size()) == 0;
}

// Check if entry's host (extracted from URL in pool) contains substr
// For search_host_substring: does URL's host part match?
inline bool entry_host_contains(const UrlIndexEntry& e, const char* url_pool,
                                 const std::string& substr) {
    if (!url_pool || e.url_len == 0) return false;
    std::string_view url(url_pool + e.url_offset, e.url_len);
    // Extract host from URL
    auto pos = url.find("://");
    if (pos != std::string_view::npos) url.remove_prefix(pos + 3);
    auto end = url.find_first_of("/:?");
    std::string_view host = (end != std::string_view::npos) ? url.substr(0, end) : url;
    // Case-insensitive substring search
    auto it = std::search(host.begin(), host.end(), substr.begin(), substr.end(),
        [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != host.end();
}

// Find HostBlock by hostname
inline const HostBlock* find_host(const HostBlock* hosts, uint32_t count, const std::string& host) {
    char key[HOST_HASH_LEN] = {};
    strncpy(key, host.c_str(), HOST_HASH_LEN - 1);
    int lo = 0, hi = static_cast<int>(count);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int cmp = strncmp(hosts[mid].host, key, HOST_HASH_LEN);
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo < static_cast<int>(count) && strncmp(hosts[lo].host, key, HOST_HASH_LEN) == 0)
        return &hosts[lo];
    return nullptr;
}

#endif // COMMON_H
