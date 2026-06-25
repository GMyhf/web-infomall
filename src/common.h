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
#include <cctype>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <zlib.h>

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
    uint32_t mini_hash;      // FNV-1a folded to 32 bits (quick check, NOT MD5)
    uint32_t crc32;          // CRC-32 of entire record (0 = not computed, backward compat)

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

// URL hash — 64-bit FNV-1a (NOT MD5, despite historical naming elsewhere).
// Used as the primary sort/lookup key for UrlIndexEntry.
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

// Mini hash (FNV-1a ≫32 XOR lower 32 bits) — for quick record verification.
// NOTE: This is NOT MD5; it's the 64-bit FNV-1a hash folded to 32 bits.
inline uint32_t mini_hash(const std::string& url) {
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
    // Lowercase (safe for signed char by casting to unsigned char)
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

// Bounds-check an entry's slice against the URL pool. pool_size == 0 means
// "size unknown, skip check" (legacy callers); pass the real size to guard
// against a corrupted index causing an out-of-bounds read.
inline bool entry_in_pool(const UrlIndexEntry& e, uint32_t pool_size) {
    if (pool_size == 0) return true;  // unknown size — caller opted out
    // Use 64-bit math so url_offset + url_len cannot wrap.
    return static_cast<uint64_t>(e.url_offset) + e.url_len <= pool_size;
}

// Get URL string from v2 shard's URL pool
inline std::string entry_url(const UrlIndexEntry& e, const char* url_pool,
                             uint32_t pool_size = 0) {
    if (!url_pool || e.url_len == 0 || !entry_in_pool(e, pool_size)) return "";
    return std::string(url_pool + e.url_offset, e.url_len);
}

// Check if entry's URL starts with prefix (from v2 URL pool)
inline bool entry_url_has_prefix(const UrlIndexEntry& e, const char* url_pool,
                                  const std::string& prefix, uint32_t pool_size = 0) {
    if (!url_pool || e.url_len < prefix.size() || !entry_in_pool(e, pool_size)) return false;
    return memcmp(url_pool + e.url_offset, prefix.data(), prefix.size()) == 0;
}

// Check if entry's host (extracted from URL in pool) contains substr
// For search_host_substring: does URL's host part match?
inline bool entry_host_contains(const UrlIndexEntry& e, const char* url_pool,
                                 const std::string& substr, uint32_t pool_size = 0) {
    if (!url_pool || e.url_len == 0 || !entry_in_pool(e, pool_size)) return false;
    std::string_view url(url_pool + e.url_offset, e.url_len);
    // Extract host from URL
    auto pos = url.find("://");
    if (pos != std::string_view::npos) url.remove_prefix(pos + 3);
    auto end = url.find_first_of("/:?");
    std::string_view host = (end != std::string_view::npos) ? url.substr(0, end) : url;
    // Case-insensitive substring search
    auto it = std::search(host.begin(), host.end(), substr.begin(), substr.end(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a))
                                  == std::tolower(static_cast<unsigned char>(b)); });
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

// ── CRC-32 Helpers ───────────────────────────────────────────

// Compute CRC-32 over the record (excluding the crc32 field itself — set to 0)
inline uint32_t compute_record_crc32(const ArticleRecord* rec) {
    // CRC covers: header (with crc32=0) + url + title + compressed body
    uint32_t sz = rec->record_size;
    std::vector<char> copy(sz);
    memcpy(copy.data(), rec, sz);
    reinterpret_cast<ArticleRecord*>(copy.data())->crc32 = 0;
    return crc32(0, reinterpret_cast<const unsigned char*>(copy.data()), sz);
}

// ── Content-Type Inference ───────────────────────────────────

// Infer Content-Type from URL extension. Returns "text/html; charset=utf-8" as default.
inline std::string url_content_type(const std::string& url) {
    size_t dot = url.rfind('.');
    if (dot == std::string::npos) return "text/html; charset=utf-8";
    size_t q = url.find_first_of("?#", dot);
    std::string ext = url.substr(dot, q != std::string::npos ? q - dot : std::string::npos);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == ".html" || ext == ".htm" || ext == ".shtml" || ext == ".jsp" || ext == ".asp")
        return "text/html; charset=utf-8";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png")  return "image/png";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".css")  return "text/css";
    if (ext == ".js")   return "application/javascript";
    if (ext == ".xml")  return "application/xml";
    if (ext == ".json") return "application/json";
    if (ext == ".pdf")  return "application/pdf";
    if (ext == ".ico" || ext == ".svg")  return "image/svg+xml";
    if (ext == ".woff" || ext == ".woff2") return "font/woff2";
    if (ext == ".ttf")  return "font/ttf";
    if (ext == ".txt" || ext == ".text") return "text/plain; charset=utf-8";
    if (ext == ".mp3")  return "audio/mpeg";
    if (ext == ".mp4")  return "video/mp4";

    return "text/html; charset=utf-8";
}

// ── HTML Rewriting for Replay ─────────────────────────────────

// Rewrite absolute URLs through the archive proxy/replay endpoint.
//   <a href="http://..."> → <a href="/replay?url=http://...">
//   <img src="http://..."> → <img src="/proxy?url=http://...">
// Leaves relative URLs, anchors, javascript:, mailto: untouched.
enum class RewriteMode { REPLAY, PROXY };

inline std::string rewrite_url(const std::string& url, RewriteMode mode = RewriteMode::REPLAY) {
    if (url.empty() || url[0] == '#' || url.find("javascript:") == 0 ||
        url.find("mailto:") == 0 || url.find("data:") == 0) {
        return url;
    }
    if (url.find("://") == std::string::npos) return url; // relative — leave for now

    const char* prefix = (mode == RewriteMode::PROXY) ? "/proxy?url=" : "/replay?url=";
    return prefix + url;
}

// Rewrite HTML content: fix <a href>, <img src>, <link href>, <script src>, CSS url()
inline std::string rewrite_html_links(const std::string& html) {
    std::string result;
    result.reserve(html.size() * 11 / 10); // modest over-allocation
    size_t pos = 0;

    while (pos < html.size()) {
        // Find next tag or CSS url()
        size_t tag_open = html.find('<', pos);
        size_t css_url = html.find("url(", pos);

        if (tag_open == std::string::npos && css_url == std::string::npos) {
            result.append(html, pos, html.size() - pos);
            break;
        }

        // Process whichever comes first
        if (css_url != std::string::npos && (tag_open == std::string::npos || css_url < tag_open)) {
            // CSS url() pattern: url(http://...) or url('http://...') or url("http://...")
            result.append(html, pos, css_url - pos);
            size_t start = css_url + 4; // past "url("
            // Skip quotes
            char quote = 0;
            if (start < html.size() && (html[start] == '\'' || html[start] == '"')) {
                quote = html[start];
                start++;
            }
            size_t end = html.find(quote ? quote : ')', start);
            if (end == std::string::npos) {
                end = html.find(')', start);
            }
            if (end != std::string::npos) {
                std::string inner = html.substr(start, end - start);
                std::string rewritten = rewrite_url(inner, RewriteMode::PROXY);
                result += "url(";
                if (quote) result += quote;
                result += rewritten;
                if (quote) result += quote;
                result += ")";
                pos = end + 1;
            } else {
                result += "url(";
                pos = start;
            }
            continue;
        }

        // HTML tag
        result.append(html, pos, tag_open - pos);
        size_t tag_close = html.find('>', tag_open);
        if (tag_close == std::string::npos) {
            result.append(html, tag_open);
            break;
        }

        std::string_view tag(html.data() + tag_open, tag_close - tag_open + 1);
        bool is_anchor = (tag.size() > 1 && tag[1] == 'a');

        // Find href= or src= in the tag
        std::string rewritten_tag;
        rewritten_tag.reserve(tag.size() + 64);
        rewritten_tag += html[tag_open]; // '<'

        for (size_t i = tag_open + 1; i < tag_close; i++) {
            // Check for href="..."
            if (i + 5 < tag_close && strncasecmp(html.data() + i, "href=", 5) == 0) {
                rewritten_tag += "href=";
                i += 5;
                char q = html[i];
                if (q != '"' && q != '\'') { rewritten_tag += q; continue; }
                i++;
                size_t val_end = html.find(q, i);
                if (val_end == std::string::npos || val_end >= tag_close) {
                    rewritten_tag.append(html, i, tag_close - i);
                    break;
                }
                std::string val(html.data() + i, val_end - i);
                // Use REPLAY mode for <a> links, PROXY for everything else
                RewriteMode mode = is_anchor ? RewriteMode::REPLAY : RewriteMode::PROXY;
                rewritten_tag += rewrite_url(val, mode);
                i = val_end;
                continue;
            }
            // Check for src="..."
            if (i + 4 < tag_close && strncasecmp(html.data() + i, "src=", 4) == 0) {
                rewritten_tag += "src=";
                i += 4;
                char q = html[i];
                if (q != '"' && q != '\'') { rewritten_tag += q; continue; }
                i++;
                size_t val_end = html.find(q, i);
                if (val_end == std::string::npos || val_end >= tag_close) {
                    rewritten_tag.append(html, i, tag_close - i);
                    break;
                }
                std::string val(html.data() + i, val_end - i);
                rewritten_tag += rewrite_url(val, RewriteMode::PROXY);
                i = val_end;
                continue;
            }
            rewritten_tag += html[i];
        }
        rewritten_tag += '>';
        result += rewritten_tag;
        pos = tag_close + 1;
    }
    return result;
}

// ── Chinese Text Tokenizer (for title indexing) ───────────────

// Maximum title index entries per term (limit file size)
constexpr int TITLE_INDEX_TOP_K = 200;

struct TitlePosting {
    std::string url;
    uint32_t date;
    std::string title;
};

// Tokenize a title into search terms:
//   - Each CJK character (U+4E00–U+9FFF) is a unigram term
//   - Consecutive Latin/ASCII runs are split into words
//   - All terms are lowercased
//   - Terms shorter than 2 characters (after CJK extraction) are included
//     since a single CJK character is a valid Chinese word
//   - Pure punctuation terms are skipped
inline std::vector<std::string> tokenize_title(const std::string& title) {
    std::vector<std::string> terms;
    std::string latin_buf;

    for (size_t i = 0; i < title.size(); ) {
        unsigned char c = static_cast<unsigned char>(title[i]);
        // Detect CJK: U+4E00–U+9FFF in UTF-8 is 3 bytes: 0xE4 0xB8/0x80–0xE9 0xBF/0xBF
        if (c >= 0xE4 && i + 2 < title.size()) {
            unsigned char c2 = static_cast<unsigned char>(title[i+1]);
            unsigned char c3 = static_cast<unsigned char>(title[i+2]);
            if ((c == 0xE4 && c2 >= 0xB8) || (c == 0xE9 && c2 <= 0xBF) ||
                (c > 0xE4 && c < 0xE9) ||
                (c == 0xE4 && c2 == 0xB8 && c3 >= 0x80) ||
                (c == 0xE9 && c2 == 0xBF && c3 <= 0xBF)) {
                // Flush Latin buffer
                if (!latin_buf.empty()) {
                    if (latin_buf.size() >= 2) terms.push_back(latin_buf);
                    latin_buf.clear();
                }
                // Each CJK character is a term
                std::string cjk;
                cjk += title[i];
                cjk += title[i+1];
                cjk += title[i+2];
                // Skip pure punctuation CJK (CJK_Symbols range)
                terms.push_back(cjk);
                i += 3;
                continue;
            }
        }
        // Latin/ASCII or other
        if (isalnum(c) || c == '-' || c == '_') {
            latin_buf += static_cast<char>(std::tolower(c));
        } else {
            if (!latin_buf.empty()) {
                if (latin_buf.size() >= 2) terms.push_back(latin_buf);
                latin_buf.clear();
            }
        }
        i++;
    }
    // Flush remaining Latin buffer
    if (!latin_buf.empty() && latin_buf.size() >= 2) {
        terms.push_back(latin_buf);
    }

    // Remove duplicates (common with repeated chars in titles)
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    return terms;
}

// ── Title Index File Format ──────────────────────────────────
// title_idx.dat binary layout:
//   uint32_t  num_terms
//   [for each term]:
//     uint16_t  term_len
//     char      term[term_len]     (not null-terminated)
//     uint32_t  num_results        (capped at TITLE_INDEX_TOP_K)
//     [for each result]:
//       uint32_t  date
//       uint16_t  url_len
//       char      url[url_len]
//       uint16_t  title_len
//       char      title[title_len]

#endif // COMMON_H
