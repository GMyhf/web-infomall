/*
 * server.cpp — Multi-threaded HTTP replay server with gzip compression.
 *
 * Pure POSIX sockets + std::thread, no external dependencies.
 * Uses QueryEngine for all data access.
 *
 * Usage: ./serve <data_dir> <index_dir> [port]
 */

#include "common.h"
#include "query.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <functional>
#include <condition_variable>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <ctime>
#include <map>
#include <set>
#include <unordered_map>
#include <sys/time.h>
#include <zlib.h>
#include <signal.h>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <netinet/tcp.h>
#include <poll.h>
#include <iomanip>
#include <limits>

// ── Concurrency ──────────────────────────────────────────────

constexpr int THREAD_POOL_SIZE = 4;
constexpr int LISTEN_BACKLOG   = 128;
constexpr int ACCEPT_TIMEOUT_S = 1;
constexpr int KEEPALIVE_MAX_REQS = 10;
constexpr int KEEPALIVE_TIMEOUT_S = 5;
constexpr int RESPONSE_TIMEOUT_S = 30;
constexpr uint32_t MAX_REQUEST_SIZE = 32768;
constexpr size_t BUF_READ_SIZE = 4096;
constexpr size_t MAX_PENDING_CONNECTIONS = 256;

// ── Signal handling ──────────────────────────────────────────

static volatile sig_atomic_t server_running = 1;

static void handle_signal(int) {
    server_running = 0;
}

// ── Structured Logger ────────────────────────────────────────

enum LogLevel { LOG_DBG, LOG_INF, LOG_WRN, LOG_ERR };

struct Logger {
    static void log(LogLevel level, const char* fmt, ...) {
        static std::mutex log_mtx;
        std::lock_guard<std::mutex> lock(log_mtx);
        time_t now = time(nullptr);
        struct tm tm_val;
        localtime_r(&now, &tm_val);
        char tb[32];
        strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &tm_val);
        const char* ls[] = {"DBG", "INF", "WRN", "ERR"};
        FILE* out = (level >= LOG_WRN) ? stderr : stdout;
        fprintf(out, "[%s] [%s] ", tb, ls[level]);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(out, fmt, ap);
        va_end(ap);
        fprintf(out, "\n");
    }
};

#define LOG_DBG(...) Logger::log(LOG_DBG, __VA_ARGS__)
#define LOG_INF(...) Logger::log(LOG_INF, __VA_ARGS__)
#define LOG_WRN(...) Logger::log(LOG_WRN, __VA_ARGS__)
#define LOG_ERR(...) Logger::log(LOG_ERR, __VA_ARGS__)

// ── Simple Rate Limiter (per-IP, best-effort) ────────────────

struct RateLimiter {
    // Ring-buffer capacity must be >= max_reqs_, otherwise old timestamps get
    // overwritten before they expire and the sliding-window count goes wrong.
    static constexpr uint32_t WINDOW_SLOTS = 32;
    struct Bucket {
        time_t timestamps[WINDOW_SLOTS];
        uint32_t head;
        uint32_t count;
        time_t last_seen;
    };
    std::unordered_map<uint32_t, Bucket> buckets_;
    std::mutex mtx_;
    time_t window_ = 5;      // 5 second window
    uint32_t max_reqs_ = 30; // max requests per window (must be <= WINDOW_SLOTS)
    uint32_t checks_since_sweep_ = 0;
    static_assert(WINDOW_SLOTS >= 30, "WINDOW_SLOTS must hold a full window of requests");

    // Returns true if rate-limited (deny), false if allowed
    bool check(uint32_t ip) {
        time_t now = time(nullptr);
        std::lock_guard<std::mutex> lk(mtx_);

        // Periodically drop buckets idle past the window so the map can't
        // grow without bound (and new IPs always get tracked).
        if (++checks_since_sweep_ >= 4096) {
            checks_since_sweep_ = 0;
            for (auto it = buckets_.begin(); it != buckets_.end(); ) {
                if (now - it->second.last_seen > window_) it = buckets_.erase(it);
                else ++it;
            }
        }

        Bucket& b = buckets_[ip];  // creates zeroed bucket for new IPs
        b.last_seen = now;
        while (b.count > 0 &&
               now - b.timestamps[(b.head - b.count + WINDOW_SLOTS) % WINDOW_SLOTS] > window_)
            b.count--;
        if (b.count >= max_reqs_) return true;
        b.timestamps[b.head] = now;
        b.head = (b.head + 1) % WINDOW_SLOTS;
        b.count++;
        return false;
    }
};

// ── HTML Helpers ──────────────────────────────────────────────

static std::string html_escape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            default: r += c;
        }
    }
    return r;
}

static std::string url_encode(const std::string& s) {
    std::ostringstream os;
    for (char c : s) {
        if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == '/' || c == ':')
            os << c;
        else {
            os << '%' << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
               << static_cast<unsigned int>(static_cast<uint8_t>(c));
        }
    }
    return os.str();
}

static std::string fmt_date(uint32_t d) {
    if (!valid_crawl_date(d)) return "未知";
    char buf[16];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", d / 10000, (d / 100) % 100, d % 100);
    return buf;
}

static uint32_t local_today() {
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return static_cast<uint32_t>(tm_now.tm_year + 1900) * 10000u
         + static_cast<uint32_t>(tm_now.tm_mon + 1) * 100u
         + static_cast<uint32_t>(tm_now.tm_mday);
}

static std::string http_date(time_t t) {
    char buf[64];
    struct tm tm_val;
    gmtime_r(&t, &tm_val);
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_val);
    return buf;
}

// ── Page templates (CSS embedded) ────────────────────────────

static const char* PAGE_HEADER =
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>%s</title>"
    "<style>"
    ":root{--bg:#f6f4ef;--surface:#fffdf8;--surface-2:#fbf7ee;--ink:#1f2933;--muted:#667085;--line:#ded6c8;--brand:#2f5d62;--brand-2:#24484d;--accent:#b35f2a;--accent-soft:#f3dfc9;--info-soft:#e7f0ef;--shadow:0 10px 28px rgba(58,48,35,.08)}"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,\"Noto Sans SC\",sans-serif;background:var(--bg);color:var(--ink);line-height:1.5}"
    "body:before{content:\"\";position:fixed;inset:0;pointer-events:none;background:linear-gradient(180deg,rgba(255,253,248,.95),rgba(246,244,239,.72) 260px,rgba(246,244,239,0));z-index:-1}"
    "header{background:var(--brand);color:#fff;border-bottom:4px solid var(--accent);padding:18px 24px}"
    ".header-inner{max-width:1120px;margin:0 auto;display:flex;align-items:center;justify-content:space-between;gap:18px}"
    "header h1{font-size:1.25rem;font-weight:750;letter-spacing:0}header h1 a{color:#fff;text-decoration:none}"
    "header p{font-size:.86rem;color:#d7e5e2;margin-top:3px}"
    ".system-badge{display:inline-flex;align-items:center;gap:6px;white-space:nowrap;border:1px solid rgba(255,255,255,.26);background:rgba(255,255,255,.12);color:#edf7f5;border-radius:999px;padding:6px 10px;font-size:.78rem}"
    ".container{max-width:1120px;margin:0 auto;padding:28px 24px 40px}"
    "h2{font-size:1.45rem;line-height:1.25}h3{font-size:1rem;color:#344054;margin:18px 0 10px}"
    "a{color:var(--brand);text-decoration:none}a:hover{text-decoration:underline}"
    "code{background:var(--surface-2);border:1px solid var(--line);border-radius:4px;padding:1px 5px;color:#5c3f20}"
    ".search-panel{margin:22px 0}"
    ".search-panel .hint{color:var(--muted);font-size:.9rem;margin-top:-10px;margin-bottom:12px}"
    ".search-bar{background:var(--surface);border:1px solid var(--line);border-radius:8px;box-shadow:var(--shadow);padding:14px;margin:22px 0 12px}"
    ".search-bar form{display:flex;gap:10px;width:100%%;align-items:stretch}"
    ".search-bar input[type=text]{flex:1;min-width:0;padding:12px 14px;font-size:1rem;border:1px solid #cfc5b7;border-radius:6px;background:#fff;color:var(--ink);outline:none;transition:border-color .15s ease,box-shadow .15s ease,background .15s ease}"
    ".search-bar input[type=text]:focus{border-color:var(--brand);box-shadow:0 0 0 3px rgba(47,93,98,.18);background:#fffefb}"
    ".search-bar button{padding:0 20px;min-height:46px;background:var(--brand);color:#fff;border:none;border-radius:6px;font-size:.96rem;font-weight:650;cursor:pointer;white-space:nowrap}"
    ".search-bar button:hover{background:var(--brand-2)}"
    ".quick-links{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}"
    ".quick-links a{display:inline-flex;align-items:center;border:1px solid var(--line);background:var(--surface-2);border-radius:999px;padding:5px 10px;font-size:.84rem;color:#4e5c60}"
    ".quick-links a:hover{background:var(--info-soft);text-decoration:none;color:var(--brand-2)}"
    ".result-summary{color:#344054;font-size:1rem;margin:18px 0 10px}"
    ".result-item{background:var(--surface);padding:15px 18px;margin:10px 0;border-radius:8px;border:1px solid var(--line);box-shadow:0 1px 0 rgba(58,48,35,.04)}"
    ".result-item:hover{border-color:#c8b9a8;background:#fff}"
    ".result-item a{font-size:1rem;font-weight:620;overflow-wrap:anywhere}"
    ".result-item .meta,.meta{color:var(--muted);font-size:.86rem;margin-top:6px}"
    ".page-view{background:var(--surface);padding:30px;border-radius:8px;border:1px solid var(--line);box-shadow:var(--shadow);margin:18px auto;max-width:900px}"
    ".page-view h2{margin-bottom:16px;overflow-wrap:anywhere;font-size:1.55rem;line-height:1.35}"
    ".page-meta{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px;color:var(--muted);font-size:.9rem;margin-bottom:24px;padding:14px;border:1px solid var(--line);border-radius:8px;background:var(--surface-2)}"
    ".meta-item{min-width:0}.meta-label{display:block;color:#7b7166;font-size:.76rem;font-weight:700;margin-bottom:3px}.meta-value{display:block;color:#344054;overflow-wrap:anywhere}"
    ".page-body{max-width:760px;margin:0 auto;line-height:1.95;white-space:pre-wrap;word-break:break-word;overflow-wrap:anywhere;font-size:1.05rem;color:#2b3036;letter-spacing:0;text-align:left}"
    ".notice{background:#fff7e8;border:1px solid #e7be82;padding:13px 15px;border-radius:8px;margin:14px 0;color:#7a4316;font-size:.92rem}"
    ".notice a{color:#7a4316;font-weight:650}"
    ".stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin:16px 0 22px}"
    ".stat-card{background:var(--surface);padding:16px;border-radius:8px;border:1px solid var(--line);box-shadow:0 1px 0 rgba(58,48,35,.04)}"
    ".stat-card .number{font-size:1.35rem;line-height:1.2;font-weight:760;color:var(--brand);overflow-wrap:anywhere}"
    ".stat-card .label{font-size:.8rem;color:var(--muted);margin-top:5px}"
    ".nav-links{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin:4px 0 16px;color:#9b8c7b}"
    ".nav-links a{display:inline-flex;align-items:center;border:1px solid var(--line);background:var(--surface);border-radius:6px;padding:7px 10px;color:var(--brand);font-size:.9rem;font-weight:620}"
    ".nav-links a:hover{background:var(--info-soft);text-decoration:none}"
    "footer{text-align:center;padding:32px;color:#8b8175;font-size:.82rem}"
    ".badge{display:inline-flex;align-items:center;padding:3px 8px;background:var(--info-soft);color:var(--brand-2);border:1px solid #c9dcd8;border-radius:999px;font-size:.78rem;font-weight:650;margin-left:6px;vertical-align:middle}"
    /* Timeline (calendar page) */
    ".timeline{position:relative;padding:24px 0 16px 40px;margin:20px 0}"
    ".timeline::before{content:\"\";position:absolute;left:16px;top:0;bottom:0;width:3px;background:linear-gradient(180deg,var(--brand),#8db5b9 50%%,#d4cfc2)}"
    ".tl-item{position:relative;padding:10px 0 10px 28px;margin-bottom:4px;border-radius:6px;transition:background .15s}"
    ".tl-item:hover{background:var(--surface-2)}"
    ".tl-item::before{content:\"\";position:absolute;left:-27px;top:18px;width:12px;height:12px;border-radius:50%%;background:var(--brand);border:2px solid #fff;box-shadow:0 0 0 2px var(--brand)}"
    ".tl-item:first-child::before{background:var(--accent);box-shadow:0 0 0 2px var(--accent)}"
    ".tl-item a{font-weight:650;font-size:1.02rem}"
    ".tl-item .tl-date{display:inline-block;min-width:90px;color:var(--brand-2);font-weight:700;font-size:.92rem}"
    ".tl-item .tl-count{color:var(--muted);font-size:.82rem;margin-left:6px}"
    ".tl-year-marker{position:relative;padding:8px 0 4px 28px;margin-top:8px}"
    ".tl-year-marker::before{content:\"\";position:absolute;left:-31px;top:12px;width:20px;height:20px;border-radius:50%%;background:var(--accent-soft);border:3px solid var(--accent)}"
    ".tl-year-marker .tl-year{font-weight:750;color:var(--accent);font-size:1.05rem}"
    /* Metadata panel (replay page) */
    ".meta-panel{border:1px solid var(--line);border-radius:8px;margin:20px 0;overflow:hidden}"
    ".meta-panel summary{padding:12px 18px;background:var(--surface-2);cursor:pointer;font-size:.9rem;font-weight:650;color:var(--muted);user-select:none;list-style:none}"
    ".meta-panel summary::-webkit-details-marker{display:none}"
    ".meta-panel summary::before{content:\"▸ \";display:inline-block;transition:transform .2s;margin-right:4px}"
    ".meta-panel[open] summary::before{transform:rotate(90deg)}"
    ".meta-panel .meta-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px;padding:16px 18px}"
    ".meta-panel .meta-cell{font-size:.85rem}.meta-cell .mk{color:#7b7166;font-size:.74rem;font-weight:700;display:block}.meta-cell .mv{color:#344054;margin-top:2px}"
    /* Recommendations (replay page) */
    ".rec-section{margin-top:32px;padding-top:20px;border-top:1px solid var(--line)}"
    ".rec-section h3{font-size:.95rem;color:#344054;margin-bottom:12px}"
    ".rec-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:10px}"
    ".rec-card{background:var(--surface);padding:12px 14px;border-radius:6px;border:1px solid var(--line);transition:border-color .15s,background .15s}"
    ".rec-card:hover{border-color:var(--brand);background:#fff}"
    ".rec-card a{font-size:.88rem;font-weight:600;display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".rec-card .rec-date{font-size:.78rem;color:var(--muted);margin-top:3px}"
    /* Top domains leaderboard */
    ".top-domains{display:grid;grid-template-columns:1fr;gap:4px;counter-reset:rank}"
    ".td-row{display:flex;align-items:center;gap:8px;padding:6px 10px;border-radius:6px;font-size:.9rem;transition:background .12s;counter-increment:rank}"
    ".td-row:hover{background:var(--surface-2)}"
    ".td-row::before{content:counter(rank);display:inline-flex;align-items:center;justify-content:center;width:22px;height:22px;border-radius:50%%;background:var(--brand);color:#fff;font-size:.72rem;font-weight:750;flex-shrink:0}"
    ".td-row:nth-child(1)::before{background:var(--accent)}"
    ".td-row:nth-child(2)::before{background:#c0852c}"
    ".td-row:nth-child(3)::before{background:#6b8e6b}"
    ".td-row a{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-weight:600}"
    ".td-row .td-count{color:var(--muted);font-size:.78rem;white-space:nowrap}"
    /* Year chart on home */
    ".home-year-chart{display:flex;gap:3px;align-items:flex-end;height:60px;margin-top:8px;padding:0 2px}"
    ".home-year-chart .hy-bar{flex:1;min-width:3px;background:var(--brand);border-radius:2px 2px 0 0;opacity:.5;transition:opacity .15s;position:relative}"
    ".home-year-chart .hy-bar:hover{opacity:1}"
    ".home-year-chart .hy-bar .hy-tip{display:none;position:absolute;bottom:calc(100%%+6px);left:50%%;transform:translateX(-50%%);background:#333;color:#fff;padding:2px 6px;border-radius:3px;font-size:.68rem;white-space:nowrap;z-index:1}"
    ".home-year-chart .hy-bar:hover .hy-tip{display:block}"
    ".hy-labels{display:flex;gap:3px;padding:2px 2px 0;font-size:.62rem;color:var(--muted)}"
    ".hy-labels span{flex:1;min-width:3px;text-align:center;overflow:hidden}"
    /* Today in history */
    ".today-section{margin-top:24px;padding-top:20px;border-top:1px solid var(--line)}"
    ".today-section h3{font-size:.95rem;color:#344054;margin-bottom:8px}"
    ".today-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:8px}"
    ".today-card{background:var(--surface);padding:10px 13px;border-radius:6px;border:1px solid var(--line);font-size:.85rem;transition:border-color .12s,background .12s}"
    ".today-card:hover{border-color:var(--brand);background:#fff}"
    ".today-card a{font-weight:600;display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".today-card .today-date{font-size:.74rem;color:var(--muted);margin-top:2px}"
    /* Date browse page */
    ".date-browse-bar{background:var(--surface);border:1px solid var(--line);border-radius:8px;box-shadow:var(--shadow);padding:18px;margin:16px 0}"
    ".date-browse-bar form{display:flex;gap:10px;align-items:center;flex-wrap:wrap}"
    ".date-browse-bar label{font-weight:650;color:#344054;font-size:.92rem}"
    ".date-browse-bar input[type=date]{padding:10px 12px;font-size:.95rem;border:1px solid #cfc5b7;border-radius:6px}"
    ".date-btn{padding:8px 6px;background:var(--surface);border:1px solid var(--line);border-radius:6px;color:var(--brand);font-size:.8rem;font-weight:600;cursor:pointer;transition:all .12s}"
    ".date-btn:hover{background:var(--info-soft);border-color:var(--brand)}"
    ".date-quick-grid{display:flex;flex-wrap:wrap;gap:4px;margin-top:8px}"
    /* Sitemap tree */
    ".site-tree{background:var(--surface);border:1px solid var(--line);border-radius:8px;padding:16px 18px;margin:16px 0}"
    ".site-tree ul{list-style:none;padding:0;margin:0}"
    ".site-tree li{padding:2px 0;position:relative}"
    ".site-tree .tree-dir{font-weight:650;color:#344054;font-size:.9rem;margin:6px 0 2px;overflow-wrap:anywhere;word-break:break-word}"
    ".site-tree .tree-dir::before{content:\"📁 \";margin-right:2px}"
    ".site-tree .tree-file{margin-left:20px;font-size:.86rem;padding:2px 0}"
    ".site-tree .tree-file a{color:var(--brand);overflow-wrap:anywhere;word-break:break-word}"
    ".site-tree .tree-count{color:var(--muted);font-size:.78rem;margin-left:4px}"
    /* Diff view */
    ".diff-section{margin-top:20px;border-top:1px solid var(--line);padding-top:16px}"
    ".diff-section h3{font-size:.95rem;color:#344054;margin-bottom:10px}"
    ".diff-bar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:12px}"
    ".diff-bar a{padding:6px 12px;font-size:.84rem;border-radius:6px;text-decoration:none;font-weight:650;transition:all .12s}"
    ".diff-bar a.diff-prev{border:1px solid var(--line);background:var(--surface-2);color:var(--muted)}"
    ".diff-bar a.diff-prev:hover{background:var(--info-soft);color:var(--brand)}"
    ".diff-bar a.diff-next{border:1px solid var(--line);background:var(--surface-2);color:var(--muted)}"
    ".diff-bar a.diff-next:hover{background:var(--info-soft);color:var(--brand)}"
    ".diff-added{background:#e6f7e6;border-left:3px solid #4caf50;padding:8px 12px;margin:4px 0;border-radius:0 6px 6px 0}"
    ".diff-removed{background:#fde8e8;border-left:3px solid #e57373;padding:8px 12px;margin:4px 0;border-radius:0 6px 6px 0;text-decoration:line-through;color:#8b5050}"
    ".diff-unchanged{padding:8px 12px;margin:4px 0;color:var(--muted);font-size:.9rem}"
    ".diff-label{display:inline-block;font-size:.72rem;font-weight:750;padding:1px 6px;border-radius:3px;margin-right:6px}"
    ".diff-added .diff-label{background:#c8e6c9;color:#2e7d32}"
    ".diff-removed .diff-label{background:#ffcdd2;color:#c62828}"
    ".host-hero{background:var(--surface);padding:24px 28px;border-radius:10px;border:1px solid var(--line);box-shadow:var(--shadow);margin:16px 0}"
    ".host-hero h2{font-size:1.6rem;margin-bottom:4px;color:var(--brand);overflow-wrap:anywhere}"
    ".host-hero .host-url-count{color:var(--muted);font-size:.9rem;margin-top:6px}"
    ".host-stats-row{display:flex;gap:24px;flex-wrap:wrap;margin-top:16px}"
    ".host-stat{text-align:center;min-width:90px}"
    ".host-stat .hs-num{font-size:1.5rem;font-weight:760;color:var(--brand)}"
    ".host-stat .hs-lbl{font-size:.78rem;color:var(--muted);margin-top:2px}"
    ".year-chart{display:flex;gap:2px;align-items:flex-end;height:48px;margin-top:16px;padding:0 2px}"
    ".year-bar{flex:1;min-width:4px;background:var(--brand);border-radius:2px 2px 0 0;opacity:.55;transition:opacity .15s;position:relative}"
    ".year-bar:hover{opacity:1}"
    ".year-bar .yb-tip{display:none;position:absolute;bottom:calc(100%% + 6px);left:50%%;transform:translateX(-50%%);background:#333;color:#fff;padding:3px 7px;border-radius:4px;font-size:.72rem;white-space:nowrap;z-index:1}"
    ".year-bar:hover .yb-tip{display:block}"
    "@media(max-width:720px){header{padding:16px}.header-inner{display:block}.system-badge{margin-top:10px;white-space:normal}.container{padding:20px 14px 32px}.search-bar{padding:12px}.search-bar form{flex-direction:column}.search-bar button{width:100%%}.page-view{padding:20px;margin:14px 0}.page-view h2{font-size:1.32rem}.page-meta{grid-template-columns:1fr}.page-body{font-size:1rem;line-height:1.88}.stats{grid-template-columns:1fr 1fr}.rec-grid{grid-template-columns:1fr}.timeline{padding-left:32px}.timeline::before{left:12px}.tl-item::before{left:-23px;width:10px;height:10px}.host-hero{padding:18px}}"
    "@media(max-width:420px){.stats{grid-template-columns:1fr}header h1{font-size:1.08rem}.container{padding-left:12px;padding-right:12px}.result-item{padding:13px 14px}.quick-links a{max-width:100%%;overflow-wrap:anywhere}.page-view{padding:16px}.page-body{font-size:.98rem;line-height:1.82}.host-stats-row{gap:12px}.host-stat{min-width:60px}}"
    "</style></head><body>"
    "<header><div class=\"header-inner\"><div><h1><a href=\"/\">Web InfoMall — 历史网页回放</a></h1>"
    "<p>中国网页信息博物馆 · Archive Replay</p></div><span class=\"system-badge\">v2 · Threaded</span></div></header><div class=\"container\">";

static std::string page_header(const std::string& title) {
    std::string escaped_title = html_escape(title);
    int needed = snprintf(nullptr, 0, PAGE_HEADER, escaped_title.c_str());
    if (needed < 0) return "";
    std::string result(static_cast<size_t>(needed) + 1, '\0');
    snprintf(result.data(), result.size(), PAGE_HEADER, escaped_title.c_str());
    result.resize(static_cast<size_t>(needed));
    return result;
}

static const char* PAGE_FOOTER =
    "</div><footer>Web InfoMall Archive Replay System · C++ Phase 2 v2</footer></body></html>";

// ── URL Decode ────────────────────────────────────────────────

static std::string url_decode(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex_value = [](unsigned char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex_value(static_cast<unsigned char>(s[i + 1]));
            int lo = hex_value(static_cast<unsigned char>(s[i + 2]));
            if (hi >= 0 && lo >= 0) {
                r += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        } else if (s[i] == '+') {
            r += ' ';
            continue;
        }
        r += s[i];
    }
    return r;
}

// ── Socket write helper ──────────────────────────────────────

static thread_local bool response_write_failed = false;

// A peer can keep accepting tiny amounts of data often enough to reset
// SO_SNDTIMEO forever. Use non-blocking sends plus one absolute deadline for
// the whole write so slow readers cannot occupy every worker indefinitely.
static bool write_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(RESPONSE_TIMEOUT_S);
    auto fail = []() {
        response_write_failed = true;
        return false;
    };
    while (len > 0) {
        if (std::chrono::steady_clock::now() >= deadline) return fail();
        ssize_t n = send(fd, p, len, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return fail();

            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) return fail();
            long long timeout_ms = std::min<long long>(
                remaining.count(), std::numeric_limits<int>::max());
            struct pollfd pfd = {fd, POLLOUT, 0};
            int ready = poll(&pfd, 1, static_cast<int>(std::max<long long>(1, timeout_ms)));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return fail();
            continue;
        }
        if (n == 0) return fail();
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

static int wait_readable_until(
        int fd, const std::chrono::steady_clock::time_point& deadline) {
    while (server_running) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return 0;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        long long timeout_ms = std::max<long long>(1, remaining.count());
        timeout_ms = std::min<long long>(timeout_ms, std::numeric_limits<int>::max());

        struct pollfd pfd = {fd, POLLIN, 0};
        int ready = poll(&pfd, 1, static_cast<int>(timeout_ms));
        if (ready < 0 && errno == EINTR) continue;
        return ready;
    }
    return -1;
}

// ── Gzip Compression ──────────────────────────────────────────

static bool gzip_compress(const std::string& input, std::string& output) {
    if (input.size() < 1024) return false; // Only compress if worth it

    z_stream zs = {};
    // Level 6: near-identical ratio to 9 on HTML at a fraction of the CPU —
    // compression runs synchronously on worker threads.
    if (deflateInit2(&zs, 6, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    size_t bound = deflateBound(&zs, input.size());
    output.resize(bound);

    zs.next_out = reinterpret_cast<Bytef*>(output.data());
    zs.avail_out = static_cast<uInt>(output.size());

    int ret = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);

    if (ret != Z_STREAM_END) return false;
    output.resize(zs.total_out);
    return output.size() < input.size() * 0.95; // Must save at least 5%
}

// ── HTTP Request Parser ───────────────────────────────────────

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string version;
    bool accepts_gzip = false;
    bool connection_keep_alive = false;
    bool connection_close = false;
    bool has_message_body = false;
    bool valid = false;
    std::string etag_if_none_match;
};

static std::string trim_ascii(std::string value) {
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) begin++;
    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) end--;
    return value.substr(begin, end - begin);
}

static std::string lowercase_header(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static bool header_has_token(const std::string& value, const std::string& wanted) {
    std::string lower = lowercase_header(value);
    size_t pos = 0;
    while (pos <= lower.size()) {
        size_t comma = lower.find(',', pos);
        if (comma == std::string::npos) comma = lower.size();
        std::string token = trim_ascii(lower.substr(pos, comma - pos));
        size_t semicolon = token.find(';');
        if (semicolon != std::string::npos) token.resize(semicolon);
        if (trim_ascii(token) == wanted) return true;
        if (comma == lower.size()) break;
        pos = comma + 1;
    }
    return false;
}

static bool accepts_gzip_encoding(const std::string& value) {
    std::string lower = lowercase_header(value);
    bool saw_gzip = false;
    bool gzip_enabled = false;
    bool saw_wildcard = false;
    bool wildcard_enabled = false;
    size_t pos = 0;
    while (pos <= lower.size()) {
        size_t comma = lower.find(',', pos);
        if (comma == std::string::npos) comma = lower.size();
        std::string item = trim_ascii(lower.substr(pos, comma - pos));
        size_t semicolon = item.find(';');
        std::string coding = trim_ascii(item.substr(0, semicolon));
        bool disabled = false;
        if (semicolon != std::string::npos) {
            std::string params = item.substr(semicolon + 1);
            size_t q = params.find("q=");
            if (q != std::string::npos) {
                char* end = nullptr;
                double quality = strtod(params.c_str() + q + 2, &end);
                disabled = end == params.c_str() + q + 2 || quality <= 0.0;
            }
        }
        if (coding == "gzip") {
            saw_gzip = true;
            gzip_enabled = !disabled;
        } else if (coding == "*") {
            saw_wildcard = true;
            wildcard_enabled = !disabled;
        }
        if (comma == lower.size()) break;
        pos = comma + 1;
    }
    return saw_gzip ? gzip_enabled : (saw_wildcard && wildcard_enabled);
}

static std::string weak_etag_opaque(std::string tag) {
    tag = trim_ascii(std::move(tag));
    if (tag.size() >= 2 && tag[0] == 'W' && tag[1] == '/')
        tag = trim_ascii(tag.substr(2));
    if (tag.size() < 2 || tag.front() != '"' || tag.back() != '"') return "";
    return tag.substr(1, tag.size() - 2);
}

static bool if_none_match_matches(const std::string& value, const std::string& etag) {
    std::string wanted = weak_etag_opaque(etag);
    if (wanted.empty()) return false;

    size_t pos = 0;
    while (pos < value.size()) {
        size_t end = pos;
        bool quoted = false;
        while (end < value.size()) {
            if (value[end] == '"') quoted = !quoted;
            if (value[end] == ',' && !quoted) break;
            end++;
        }
        std::string candidate = trim_ascii(value.substr(pos, end - pos));
        if (candidate == "*" || weak_etag_opaque(candidate) == wanted) return true;
        if (end == value.size()) break;
        pos = end + 1;
    }
    return false;
}

static HttpRequest parse_request(const char* data, size_t len) {
    HttpRequest req;
    bool saw_host = false;
    bool saw_content_length = false;
    bool saw_transfer_encoding = false;
    std::string accept_encoding;
    const char* cursor = data;
    const char* end = data + len;

    // Parse request line
    const char* nl = static_cast<const char*>(memchr(cursor, '\n', end - cursor));
    if (!nl) return req;
    std::string line(cursor, nl - cursor);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();

    std::istringstream request_line(line);
    std::string full_path, extra;
    if (!(request_line >> req.method >> full_path >> req.version) || request_line >> extra)
        return req;
    if ((req.version != "HTTP/1.1" && req.version != "HTTP/1.0") ||
        full_path.empty() || full_path[0] != '/') return req;
    for (unsigned char c : full_path)
        if (c <= 0x20 || c == 0x7F) return req;
    size_t q = full_path.find('?');
    if (q != std::string::npos) {
        req.path = full_path.substr(0, q);
        req.query = full_path.substr(q + 1);
    } else {
        req.path = full_path;
    }

    // Parse remaining headers
    cursor = nl + 1;
    while (cursor < end) {
        nl = static_cast<const char*>(memchr(cursor, '\n', end - cursor));
        if (!nl) break;
        line.assign(cursor, nl - cursor);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        cursor = nl + 1;
        if (line.empty()) break; // end of headers

        size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return req;
        std::string name = lowercase_header(line.substr(0, colon));
        if (!std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '_';
            })) return req;
        std::string value = trim_ascii(line.substr(colon + 1));
        if (name == "host") {
            if (saw_host || value.empty()) return req;
            saw_host = true;
        } else if (name == "accept-encoding") {
            if (!accept_encoding.empty()) accept_encoding += ',';
            accept_encoding += value;
        } else if (name == "if-none-match") {
            if (!req.etag_if_none_match.empty()) req.etag_if_none_match += ',';
            req.etag_if_none_match += value;
        }
        else if (name == "connection") {
            req.connection_keep_alive = req.connection_keep_alive ||
                                        header_has_token(value, "keep-alive");
            req.connection_close = req.connection_close || header_has_token(value, "close");
        } else if (name == "content-length") {
            if (saw_content_length || saw_transfer_encoding) return req;
            if (value.empty() ||
                !std::all_of(value.begin(), value.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) return req;
            char* endptr = nullptr;
            errno = 0;
            unsigned long long body_len = strtoull(value.c_str(), &endptr, 10);
            if (errno != 0 || !endptr || *endptr != '\0') return req;
            saw_content_length = true;
            req.has_message_body = body_len != 0;
        } else if (name == "transfer-encoding") {
            if (saw_transfer_encoding || saw_content_length || value.empty()) return req;
            saw_transfer_encoding = true;
            req.has_message_body = true;
        }
    }
    if (req.version == "HTTP/1.1" && !saw_host) return req;
    req.accepts_gzip = accepts_gzip_encoding(accept_encoding);
    req.valid = true;
    return req;
}

static std::string get_param(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t eq = query.find('=', pos);
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        if (eq != std::string::npos && eq < amp) {
            std::string k = query.substr(pos, eq - pos);
            std::string v = query.substr(eq + 1, amp - eq - 1);
            if (k == key) return url_decode(v);
        }
        pos = amp + 1;
    }
    return "";
}

static bool parse_date_value(const std::string& value, uint32_t& date, bool allow_empty = false) {
    if (value.empty()) {
        date = 0;
        return allow_empty;
    }
    if (value.size() != 8 ||
        !std::all_of(value.begin(), value.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; })) return false;
    unsigned long parsed = strtoul(value.c_str(), nullptr, 10);
    if (parsed > UINT32_MAX || !valid_crawl_date(static_cast<uint32_t>(parsed))) return false;
    date = static_cast<uint32_t>(parsed);
    return true;
}

static bool valid_host_param(const std::string& host) {
    if (host.empty() || host.size() > MAX_URL_LEN) return false;
    for (unsigned char c : host) {
        if (c <= 0x20 || c == 0x7F || c == '/' || c == ':' || c == '?' ||
            c == '#' || c == '@') return false;
    }
    return true;
}

// ── ETag Generation ───────────────────────────────────────────

static std::string make_content_etag(const std::string& prefix,
                                     const std::string& content) {
    char buf[160];
    uint64_t content_hash = url_hash(content);
    snprintf(buf, sizeof(buf), "\"%s-%llx-%zu\"", prefix.c_str(),
             static_cast<unsigned long long>(content_hash), content.size());
    return buf;
}

static std::string replay_etag_prefix(const std::string& url, uint32_t date) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%llx-%u",
             static_cast<unsigned long long>(url_hash(url)), date);
    return buf;
}

// ── Page Builders ─────────────────────────────────────────────

static std::string build_home(QueryEngine& qe, uint32_t today = 0) {
    uint32_t total, urls, dmin, dmax;
    qe.get_stats(total, urls, dmin, dmax);
    if (today == 0) today = local_today();

    char buf[16384];
    std::string html = page_header("Web InfoMall — 首页");

    // Search bar with date browse quick-link
    html += "<section class=\"search-panel\"><div class=\"search-bar\"><form action=\"/search\" method=\"get\">"
            "<input type=\"text\" name=\"q\" placeholder=\"输入 URL 或域名搜索...\" autofocus>"
            "<button type=\"submit\">搜索</button></form></div>"
            "<p class=\"hint\">可输入完整 URL、域名，或域名片段。| "
            "<a href=\"/random\">🎲 手气不错</a> | "
            "<a href=\"/browse\">📅 按日期浏览</a> | "
            "<a href=\"/topic\">🔥 热点事件回溯</a></p>"
            "<div class=\"quick-links\">"
            "<a href=\"/search?q=sina\">sina</a>"
            "<a href=\"/search?q=dailynews.sina.com.cn\">dailynews.sina.com.cn</a>"
            "<a href=\"/search?q=news.sina.com.cn\">news.sina.com.cn</a>"
            "<a href=\"/topic?q=北京\">🔥 北京</a>"
            "<a href=\"/topic?q=奥运\">🔥 奥运</a>"
            "<a href=\"/topic?q=非典\">🔥 非典</a>"
            "</div></section>";

    // Stats row + year chart
    html += "<div class=\"stats\">";
    snprintf(buf, sizeof(buf),
        "<div class=\"stat-card\"><div class=\"number\">%u</div><div class=\"label\">已存档</div></div>"
        "<div class=\"stat-card\"><div class=\"number\">%u</div><div class=\"label\">域名</div></div>"
        "<div class=\"stat-card\"><div class=\"number\">%s — %s</div><div class=\"label\">时间范围</div></div>"
        "<div class=\"stat-card\"><a href=\"/random\" style=\"font-size:1.5rem;text-decoration:none\">🎲</a><div class=\"label\">随机浏览</div></div>",
        total, urls, fmt_date(dmin).c_str(), fmt_date(dmax).c_str());
    html += buf;
    html += "</div>";

    // Year distribution chart
    auto year_dist = qe.get_year_distribution();
    if (year_dist.size() >= 2) {
        uint32_t max_count = 0;
        for (auto& y : year_dist) if (y.count > max_count) max_count = y.count;
        html += "<h3 style=\"font-size:.9rem;color:#7b7166;margin:12px 0 4px\">📊 按年份存档量</h3>";
        html += "<div class=\"home-year-chart\">";
        for (auto& y : year_dist) {
            int pct = max_count > 0 ? (y.count * 100 / max_count) : 0;
            if (pct < 4) pct = 4;
            snprintf(buf, sizeof(buf),
                "<div class=\"hy-bar\" style=\"height:%d%%\">"
                "<span class=\"hy-tip\">%u: %u 篇</span></div>", pct, y.year, y.count);
            html += buf;
        }
        html += "</div><div class=\"hy-labels\">";
        uint32_t step = year_dist.size() > 20 ? year_dist.size() / 20 : 1;
        for (size_t i = 0; i < year_dist.size(); i++) {
            if (i % step == 0 || i == year_dist.size() - 1)
                html += "<span>" + std::to_string(year_dist[i].year) + "</span>";
            else
                html += "<span></span>";
        }
        html += "</div>";
    }

    // Top domains leaderboard
    auto top_hosts = qe.get_top_hosts(12);
    if (!top_hosts.empty()) {
        html += "<h3 style=\"font-size:.9rem;color:#7b7166;margin:20px 0 6px\">🏆 域名排行榜</h3>";
        html += "<div class=\"top-domains\">";
        for (auto& h : top_hosts) {
            html += "<div class=\"td-row\"><a href=\"/host?h=" + url_encode(h.first) + "\">"
                    + html_escape(h.first) + "</a>"
                    "<span class=\"td-count\">" + std::to_string(h.second) + " 页</span></div>";
        }
        html += "</div>";
        html += "<div style=\"margin-top:6px;font-size:.82rem;color:var(--muted)\">"
                "<a href=\"/stats-page\">查看完整统计 →</a></div>";
    }

    // Today in history
    uint32_t mmdd = today % 10000;

    auto today_urls = qe.get_today_in_history(mmdd, 8);
    if (!today_urls.empty()) {
        html += "<section class=\"today-section\"><h3>📰 历史上的今天 (" +
                std::to_string(mmdd / 100) + "月" +
                std::to_string(mmdd % 100) + "日)</h3>";
        html += "<div class=\"today-grid\">";
        for (auto& url : today_urls) {
            std::string host = extract_host(url);
            html += "<div class=\"today-card\"><a href=\"/replay?url=" + url_encode(url) + "\">"
                    + html_escape(url) + "</a>"
                    "<div class=\"today-date\">🏠 " + html_escape(host) + "</div></div>";
        }
        html += "</div></section>";
    }

    // Help
    html += "<section class=\"help-section\" style=\"margin-top:20px\">"
            "<h3 style=\"font-size:.9rem;color:#7b7166\">ℹ️ 使用说明</h3><div class=\"result-item\">"
            "<p>输入 URL 地址或域名查看历史网页。例如：<code>sina.com.cn</code> 或 <code>http://www.pku.edu.cn</code></p>"
            "<p class=\"meta\">支持按域名浏览、URL 前缀搜索、查看同一 URL 的多个历史版本、版本间差异对比。</p>"
            "</div></section>";

    html += PAGE_FOOTER;
    return html;
}

static std::string build_search(QueryEngine& qe, const std::string& query) {
    std::string html = page_header("搜索结果");

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a></div>";
    html += "<section class=\"search-panel\"><div class=\"search-bar\"><form action=\"/search\" method=\"get\">"
            "<input type=\"text\" name=\"q\" value=\"" + html_escape(query) + "\">"
            "<button>搜索</button></form></div>"
            "<p class=\"hint\">缩短关键词可以扩大匹配范围；输入完整 URL 会直接进入回放。</p></section>";

    if (has_http_scheme(query)) {
        auto matches = qe.search_prefix(query, 100);
        if (matches.empty()) {
            html += "<div class=\"notice\"><strong>未找到精确存档或 URL 前缀。</strong><br>"
                    "可以删除路径末尾部分后重试。</div>";
        } else {
            html += "<h3 class=\"result-summary\">找到 " + std::to_string(matches.size())
                    + " 个 URL 前缀匹配</h3>";
            for (const auto& url : matches) {
                html += "<div class=\"result-item\"><a href=\"/replay?url="
                        + url_encode(url) + "\">" + html_escape(url) + "</a></div>";
            }
        }
        html += PAGE_FOOTER;
        return html;
    }

    // Try exact host match first
    auto urls = qe.get_host_urls(query, 100);
    if (!urls.empty()) {
        html += "<h3 class=\"result-summary\">域名 <strong><a href=\"/host?h="
                + url_encode(query) + "\">" + html_escape(query)
                + "</a></strong> 下有 " + std::to_string(urls.size()) + " 个页面</h3>";
        // "Browse all" link
        html += "<div class=\"nav-links\" style=\"margin-bottom:12px\">"
                "<a href=\"/host?h=" + url_encode(query) + "\">查看域名概览 →</a></div>";
        for (auto& u : urls) {
            html += "<div class=\"result-item\"><a href=\"/replay?url=" + url_encode(u.url) + "\">"
                    + html_escape(u.url) + "</a>";
            html += " <span class=\"meta\">(" + fmt_date(u.date) + ")</span></div>";
        }
    } else {
        auto hosts = qe.search_host_substring(query, 100);
        if (!hosts.empty()) {
            html += "<h3 class=\"result-summary\">找到 " + std::to_string(hosts.size())
                    + " 个匹配 \"" + html_escape(query) + "\" 的域名</h3>";
            for (auto& h : hosts) {
                html += "<div class=\"result-item\">"
                        "<a href=\"/host?h=" + url_encode(h.first) + "\">"
                        + html_escape(h.first) + "</a>"
                        "<span class=\"badge\">" + std::to_string(h.second) + " 页</span></div>";
            }
        } else {
            html += "<div class=\"notice\"><strong>未找到匹配结果。</strong><br>"
                    "请尝试更短的域名片段，或输入完整 URL 后直接回放。</div>";
        }
    }

    html += PAGE_FOOTER;
    return html;
}

static std::string build_replay(QueryEngine& qe, const std::string& url,
                                 ArticleReader::Article art) {
    if (art.url.empty()) {
        std::string html = page_header("未找到");
        html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a></div>";
        html += "<div class=\"notice\"><strong>未找到存档。</strong><br>URL: "
                + html_escape(url) + "</div>";
        html += "<section class=\"search-panel\"><div class=\"search-bar\">"
                "<form action=\"/search\" method=\"get\">"
                "<input type=\"text\" name=\"q\" value=\"" + html_escape(url) + "\">"
                "<button>搜索</button></form></div>"
                "<p class=\"hint\">可以删除路径末尾部分，只保留域名或较短 URL 前缀再试。</p></section>";
        html += PAGE_FOOTER;
        return html;
    }
    if (!art.valid) {
        std::string html = page_header("数据损坏");
        html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a></div>";
        html += "<div class=\"notice\"><strong>存档记录校验失败（CRC 不匹配或解压失败），无法可靠展示。</strong><br>URL: "
                + html_escape(url) + "</div>";
        html += PAGE_FOOTER;
        return html;
    }

    char buf[16000];
    std::string html = page_header(art.title);

    std::string host = extract_host(url);
    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a>"
            "<a href=\"/calendar?url=" + url_encode(url) + "\">查看所有版本</a>"
            "<a href=\"/host?h=" + url_encode(host) + "\">此域名下其他页面</a></div>";

    html += "<div class=\"page-view\"><h2>"
            + html_escape(art.title.empty() ? "(无标题)" : art.title) + "</h2>";
    html += "<div class=\"page-meta\">"
            "<div class=\"meta-item\"><span class=\"meta-label\">URL</span>"
            "<span class=\"meta-value\"><a href=\"" + html_escape(url) + "\">"
            + html_escape(url) + "</a></span></div>"
            "<div class=\"meta-item\"><span class=\"meta-label\">存档时间</span>"
            "<span class=\"meta-value\">" + fmt_date(art.date) + "</span></div>"
            "<div class=\"meta-item\"><span class=\"meta-label\">站点</span>"
            "<span class=\"meta-value\"><a href=\"/host?h=" + url_encode(host) + "\">"
            + html_escape(host) + "</a></span></div>"
            "</div>";

    auto vers = qe.get_versions(url);
    if (vers.size() > 1) {
        html += "<div class=\"notice\">此 URL 共有 <strong>"
                + std::to_string(vers.size()) + "</strong> 个历史版本。"
                "<a href=\"/calendar?url=" + url_encode(url)
                + "\">查看所有版本 →</a></div>";
    }
    // Diff links to adjacent versions
    if (vers.size() >= 2 && art.date > 0) {
        uint32_t prev_date = 0, next_date = 0;
        for (size_t i = 0; i < vers.size(); i++) {
            if (vers[i].date == art.date) {
                if (i + 1 < vers.size()) prev_date = vers[i + 1].date;
                if (i > 0) next_date = vers[i - 1].date;
                break;
            }
        }
        html += "<div class=\"diff-bar\">";
        if (prev_date)
            html += "<a class=\"diff-prev\" href=\"/diff?url=" + url_encode(url) + "&a="
                    + std::to_string(prev_date) + "&b=" + std::to_string(art.date) + "\">"
                    "📝 与上一版本 ("
                    + fmt_date(prev_date) + ") 对比</a>";
        if (next_date)
            html += "<a class=\"diff-next\" href=\"/diff?url=" + url_encode(url) + "&a="
                    + std::to_string(art.date) + "&b=" + std::to_string(next_date) + "\">"
                    "📝 与下一版本 ("
                    + fmt_date(next_date) + ") 对比</a>";
        html += "</div>";
    }

    // ── Auto-link bare URLs in body text ─────────────────────
    std::string body_html;
    {
        // Bind by reference — art.body can be multi-MB; copying it here was pure waste.
        static const std::string kEmptyBody = "(无内容)";
        const std::string& raw_body = art.body.empty() ? kEmptyBody : art.body;
        body_html.reserve(raw_body.size() * 12 / 10);
        size_t pos = 0;
        // Simple URL auto-linking: http:// and https:// patterns
        while (pos < raw_body.size()) {
            auto url_start = raw_body.find("http://", pos);
            auto https_start = raw_body.find("https://", pos);
            size_t found = std::string::npos;
            if (url_start != std::string::npos && (https_start == std::string::npos || url_start < https_start))
                found = url_start;
            else if (https_start != std::string::npos)
                found = https_start;

            if (found == std::string::npos || found > raw_body.size()) {
                body_html.append(html_escape(raw_body.substr(pos)));
                break;
            }
            body_html.append(html_escape(raw_body.substr(pos, found - pos)));
            // Find end of URL (space, punctuation, or end)
            size_t url_end = found;
            while (url_end < raw_body.size() && raw_body[url_end] != ' ' &&
                   raw_body[url_end] != '\n' && raw_body[url_end] != '\t' &&
                   raw_body[url_end] != '\r') url_end++;
            std::string url = raw_body.substr(found, url_end - found);
            body_html += "<a href=\"/replay?url=" + url_encode(url) + "\">"
                       + html_escape(url) + "</a>";
            pos = url_end;
        }
    }

    // Archive replay banner — injected above the page-body
    html += "<div style=\"background:var(--info-soft);border:1px solid #c9dcd8;border-radius:8px;padding:12px 16px;margin:12px 0;display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px\">"
            "<span style=\"font-size:.9rem;color:var(--brand-2)\">"
            "📅 存档日期: <strong>" + fmt_date(art.date) + "</strong>"
            " · 🏠 " + html_escape(host) + "</span>"
            "<span style=\"display:flex;gap:6px;flex-wrap:wrap\">";
    if (vers.size() > 1) {
        html += "<a href=\"/calendar?url=" + url_encode(url) + "\" style=\"padding:4px 10px;background:var(--brand);color:#fff;border-radius:4px;font-size:.82rem;font-weight:650;text-decoration:none\">📋 全部 " + std::to_string(vers.size()) + " 个版本</a>";
    }
    html += "<a href=\"/replay?url=" + url_encode(url) + "&date=" + std::to_string(art.date) + "\" style=\"padding:4px 10px;background:var(--surface-2);border:1px solid var(--line);border-radius:4px;font-size:.82rem;color:var(--muted);text-decoration:none\">🔗 永久链接</a>"
            "<a href=\"/proxy?url=" + url_encode(url) + "&date=" + std::to_string(art.date) + "\" style=\"padding:4px 10px;background:var(--surface-2);border:1px solid var(--line);border-radius:4px;font-size:.82rem;color:var(--muted);text-decoration:none\">📄 原始内容</a>"
            "</span></div>";

    // Pre-grow once for the (potentially large) body plus trailing markup,
    // avoiding repeated reallocations of `html` as it's appended below.
    html.reserve(html.size() + body_html.size() + 8192);
    html += "<div class=\"page-body\">" + body_html + "</div>";

    // Metadata panel: technical details about the archived record
    html += "<details class=\"meta-panel\"><summary>📋 存档详情</summary><div class=\"meta-grid\">";
    // Host is lowercased by extract_host but URL keeps original case. If find()
    // misses, show the full URL rather than applying arithmetic to npos.
    size_t host_pos = url.find(host);
    std::string display_path = host_pos != std::string::npos
        ? url.substr(host_pos + host.size()) : url;
    html += "<div class=\"meta-cell\"><span class=\"mk\">存档日期</span><span class=\"mv\">"
            + fmt_date(art.date) + "</span></div>"
            "<div class=\"meta-cell\"><span class=\"mk\">站点域名</span><span class=\"mv\">"
            + html_escape(host) + "</span></div>"
            "<div class=\"meta-cell\"><span class=\"mk\">URL 路径</span><span class=\"mv\">"
            + html_escape(display_path) + "</span></div>";

    // Version stats
    snprintf(buf, sizeof(buf),
        "<div class=\"meta-cell\"><span class=\"mk\">历史版本数</span><span class=\"mv\">%zu</span></div>"
        "<div class=\"meta-cell\"><span class=\"mk\">正文长度</span><span class=\"mv\">%zu 字符</span></div>"
        "<div class=\"meta-cell\"><span class=\"mk\">编码转换</span><span class=\"mv\">GB18030 → UTF-8</span></div>",
        vers.size(), art.body.size());
    html += buf;
    html += "</div></details>";

    html += "</div>"; // close page-view

    // Same-domain recommendations
    auto host_urls = qe.get_host_urls(host, 9);
    int rec_count = 0;
    html += "<section class=\"rec-section\"><h3>📂 「" + html_escape(host)
            + "」下的其他页面</h3><div class=\"rec-grid\">";
    for (auto& hu : host_urls) {
        if (hu.url == url) continue;  // skip current page
        if (++rec_count > 8) break;
        html += "<div class=\"rec-card\"><a href=\"/replay?url=" + url_encode(hu.url) + "\">"
                + html_escape(hu.url) + "</a>"
                "<div class=\"rec-date\">" + fmt_date(hu.date) + "</div></div>";
    }
    if (rec_count == 0) {
        html += "<div class=\"meta\">此域名下暂无其他页面。</div>";
    }
    html += "</div></section>";

    html += PAGE_FOOTER;
    return html;
}

static std::string build_calendar(QueryEngine& qe, const std::string& url) {
    auto vers = qe.get_versions(url);
    std::string host = extract_host(url);

    char buf[32768];
    std::string html = page_header("版本历史");

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a>"
            "<a href=\"/replay?url=" + url_encode(url) + "\">查看最新版本</a>"
            "<a href=\"/host?h=" + url_encode(host) + "\">域名概览</a></div>";

    html += "<h2>版本历史</h2>";
    html += "<div class=\"result-item\"><strong>URL:</strong> "
            + html_escape(url) + "<br><strong>站点:</strong> "
            "<a href=\"/host?h=" + url_encode(host) + "\">" + html_escape(host) + "</a>"
            + "<br><strong>版本数:</strong> " + std::to_string(vers.size()) + "</div>";

    if (!vers.empty()) {
        // Compute time span stats
        uint32_t earliest = vers.back().date;
        uint32_t latest = vers.front().date;
        uint32_t span_years = (latest / 10000) - (earliest / 10000);

        // Stats row
        snprintf(buf, sizeof(buf),
            "<div class=\"stats\" style=\"margin:16px 0\">"
            "<div class=\"stat-card\"><div class=\"number\">%zu</div><div class=\"label\">历史版本</div></div>"
            "<div class=\"stat-card\"><div class=\"number\">%s</div><div class=\"label\">最早存档</div></div>"
            "<div class=\"stat-card\"><div class=\"number\">%s</div><div class=\"label\">最新存档</div></div>"
            "<div class=\"stat-card\"><div class=\"number\">%u 年</div><div class=\"label\">时间跨度</div></div>"
            "</div>",
            vers.size(), fmt_date(earliest).c_str(), fmt_date(latest).c_str(), span_years);
        html += buf;

        // CSS timeline
        html += "<h3>存档时间线</h3><div class=\"timeline\">";
        uint32_t last_year = 0;
        for (auto& v : vers) {
            uint32_t year = v.date / 10000;
            // Year marker
            if (year != last_year) {
                last_year = year;
                snprintf(buf, sizeof(buf),
                    "<div class=\"tl-year-marker\"><span class=\"tl-year\">%u 年</span></div>", year);
                html += buf;
            }
            html += "<div class=\"tl-item\">"
                    "<a href=\"/replay?url=" + url_encode(url) + "&date="
                    + std::to_string(v.date) + "\">"
                    "<span class=\"tl-date\">" + fmt_date(v.date) + "</span></a>";
            if (v.record_count > 1)
                html += "<span class=\"tl-count\">(" + std::to_string(v.record_count) + " 条记录)</span>";
            html += "</div>";
        }
        html += "</div>";

        // Compact table view as secondary option
        html += "<h3 style=\"margin-top:24px\">列表视图</h3>";
        for (auto& v : vers) {
            html += "<div class=\"result-item\">"
                    "<a href=\"/replay?url=" + url_encode(url) + "&date="
                    + std::to_string(v.date) + "\">" + fmt_date(v.date) + "</a>";
            if (v.record_count > 1)
                html += " <span class=\"badge\">" + std::to_string(v.record_count) + " 条</span>";
            html += "</div>";
        }
    }

    html += PAGE_FOOTER;
    return html;
}

// ── Host Overview ──────────────────────────────────────────────

static std::string build_host(QueryEngine& qe, const std::string& host) {
    auto urls = qe.get_host_urls(host, 500);
    auto summary = qe.get_host_summary(host);

    char buf[32768];
    std::string title = "域名: " + host;
    std::string html = page_header(title);

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a>"
            "<a href=\"/search?q=" + url_encode(host) + "\">搜索此域名</a>"
            "<a href=\"/sitemap?h=" + url_encode(host) + "\">站点地图</a></div>";

    if (urls.empty()) {
        html += "<div class=\"notice\"><strong>未找到域名。</strong><br>"
                "域名 <code>" + html_escape(host) + "</code> 在归档中不存在。<br>"
                "请检查拼写，或尝试搜索域名片段。</div>";
        html += "<section class=\"search-panel\"><div class=\"search-bar\">"
                "<form action=\"/search\" method=\"get\">"
                "<input type=\"text\" name=\"q\" value=\"" + html_escape(host) + "\">"
                "<button>搜索</button></form></div></section>";
        html += PAGE_FOOTER;
        return html;
    }

    // Hero section
    html += "<div class=\"host-hero\"><h2>🌐 " + html_escape(host) + "</h2>";
    snprintf(buf, sizeof(buf),
        "<div class=\"host-url-count\">%llu 个不重复 URL · %llu 条存档记录 · %s — %s</div>",
        static_cast<unsigned long long>(summary.unique_url_count),
        static_cast<unsigned long long>(summary.record_count),
        fmt_date(summary.date_min).c_str(), fmt_date(summary.date_max).c_str());
    html += buf;

    // Stat cards
    html += "<div class=\"host-stats-row\">";
    snprintf(buf, sizeof(buf),
        "<div class=\"host-stat\"><div class=\"hs-num\">%llu</div><div class=\"hs-lbl\">不重复 URL</div></div>"
        "<div class=\"host-stat\"><div class=\"hs-num\">%llu</div><div class=\"hs-lbl\">总存档数</div></div>"
        "<div class=\"host-stat\"><div class=\"hs-num\">%s</div><div class=\"hs-lbl\">最早</div></div>"
        "<div class=\"host-stat\"><div class=\"hs-num\">%s</div><div class=\"hs-lbl\">最新</div></div>"
        "<div class=\"host-stat\"><div class=\"hs-num\">%zu</div><div class=\"hs-lbl\">覆盖年份</div></div>",
        static_cast<unsigned long long>(summary.unique_url_count),
        static_cast<unsigned long long>(summary.record_count),
        fmt_date(summary.date_min).c_str(), fmt_date(summary.date_max).c_str(),
        summary.year_counts.size());
    html += buf;
    html += "</div>";

    // Year distribution mini-chart
    if (summary.year_counts.size() >= 2) {
        uint64_t max_count = 0;
        for (auto& yc : summary.year_counts) if (yc.second > max_count) max_count = yc.second;
        html += "<div style=\"margin-top:14px\"><span style=\"font-size:.82rem;color:#7b7166;font-weight:700\">按年份分布</span>";
        html += "<div class=\"year-chart\">";
        for (auto& yc : summary.year_counts) {
            int pct = max_count > 0 ? static_cast<int>(yc.second * 100 / max_count) : 0;
            if (pct < 3) pct = 3;
            snprintf(buf, sizeof(buf),
                "<div class=\"year-bar\" style=\"height:%d%%\">"
                "<span class=\"yb-tip\">%u: %llu 篇</span></div>",
                pct, yc.first, static_cast<unsigned long long>(yc.second));
            html += buf;
        }
        html += "</div></div>";
    }
    html += "</div>"; // host-hero

    // URL listing
    html += "<h3 style=\"margin-top:20px\">页面列表</h3>";
    int shown = 0;
    for (auto& u : urls) {
        if (++shown > 200) {
            html += "<div class=\"result-item\"><span class=\"meta\">... 还有更多页面，请使用搜索功能缩小范围。</span></div>";
            break;
        }
        html += "<div class=\"result-item\"><a href=\"/replay?url=" + url_encode(u.url) + "\">"
                + html_escape(u.url) + "</a>";
        html += " <span class=\"meta\">" + fmt_date(u.date) + "</span></div>";
    }

    html += PAGE_FOOTER;
    return html;
}

// ── Sitemap (path tree) ─────────────────────────────────────

static std::string build_sitemap(QueryEngine& qe, const std::string& host) {
    std::string title = "站点地图: " + host;
    std::string html = page_header(title);

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a>"
            "<a href=\"/host?h=" + url_encode(host) + "\">域名概览</a></div>";

    auto urls = qe.get_host_urls(host, 1000);
    if (urls.empty()) {
        html += "<div class=\"notice\">未找到该域名的页面。</div>";
        html += PAGE_FOOTER;
        return html;
    }

    // Build a path tree independently of scheme. A host can legitimately have
    // both http and https captures, so deriving every path from the first URL's
    // scheme can underflow substr() and terminate the process.
    std::map<std::string, std::vector<std::pair<std::string, uint32_t>>> tree;
    for (auto& u : urls) {
        size_t scheme = u.url.find("://");
        size_t path_start = scheme == std::string::npos
            ? std::string::npos : u.url.find('/', scheme + 3);
        std::string path = path_start == std::string::npos ? "/" : u.url.substr(path_start);
        size_t last_slash = path.rfind('/');
        std::string dir = last_slash == std::string::npos
            ? "/" : path.substr(0, last_slash + 1);
        tree[dir].push_back({u.url, u.date});
    }

    std::string escaped_host = html_escape(host);
    html += "<h2>🗂️ 站点地图：" + escaped_host + "</h2>"
            "<div class=\"result-item\"><strong>域名:</strong> " + escaped_host + "<br>"
            "<strong>页面数:</strong> " + std::to_string(urls.size()) + "<br>"
            "<strong>目录数:</strong> " + std::to_string(tree.size()) + "</div>";

    // Render directories and their immediate captures.
    html += "<div class=\"site-tree\"><ul>";
    for (auto& kv : tree) {
        html += "<li class=\"tree-dir\">" + html_escape(kv.first)
                + "<span class=\"tree-count\">(" + std::to_string(kv.second.size())
                + " 个文件)</span></li>";
        for (auto& u : kv.second) {
            size_t scheme = u.first.find("://");
            size_t path_start = scheme == std::string::npos
                ? std::string::npos : u.first.find('/', scheme + 3);
            std::string path = path_start == std::string::npos ? "/" : u.first.substr(path_start);
            html += "<li class=\"tree-file\"><a href=\"/replay?url="
                    + url_encode(u.first) + "\">" + html_escape(path) + "</a>"
                    "<span class=\"tree-count\">" + fmt_date(u.second) + "</span></li>";
        }
    }

    html += "</ul></div>";
    html += PAGE_FOOTER;
    return html;
}

// ── Browse by date ──────────────────────────────────────────

static std::string build_browse(QueryEngine& qe, const std::string& date_str) {
    char buf[32768];
    std::string html = page_header("按日期浏览");

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a></div>";

    // Quick date buttons: compute available years and recent months
    uint32_t total, urls, dmin, dmax;
    qe.get_stats(total, urls, dmin, dmax);
    uint32_t min_year = dmin / 10000, max_year = dmax / 10000;

    // <input type=date> submits YYYY-MM-DD while the year quick-links use
    // YYYYMMDD — normalize both to 8 digits before comparing crawl dates.
    std::string digits;
    for (char c : date_str)
        if (c >= '0' && c <= '9') digits += c;

    // The date input only displays its value in YYYY-MM-DD form.
    std::string input_value;
    if (digits.size() == 8)
        input_value = digits.substr(0, 4) + "-" + digits.substr(4, 2) + "-" + digits.substr(6, 2);

    html += "<h2>📅 按日期浏览</h2>";
    html += "<div class=\"date-browse-bar\"><form action=\"/browse\" method=\"get\">"
            "<label>选择日期：</label>"
            "<input type=\"date\" name=\"d\" value=\"" + html_escape(input_value) + "\">"
            "<button>浏览</button></form>";
    html += "<div class=\"date-quick-grid\"><span style=\"font-size:.82rem;color:var(--muted)\">快速跳转：</span>";

    // Year buttons
    for (uint32_t y = max_year; y >= min_year && y >= max_year - 12 && y >= min_year; y--) {
        html += "<a class=\"date-btn\" href=\"/browse?d=" + std::to_string(y) + "0101\">"
                + std::to_string(y) + "年</a>";
    }
    html += "</div></div>";

    if (digits.size() == 8) {
        uint32_t date = static_cast<uint32_t>(atoi(digits.c_str()));
        auto results = qe.get_by_date(date, 500);

        if (results.empty()) {
            html += "<div class=\"notice\">该日期 (" + fmt_date(date)
                    + ") 没有找到存档页面。请尝试其他日期。</div>";
        } else {
            snprintf(buf, sizeof(buf), "<h3 class=\"result-summary\">%s — 共 %zu 个页面</h3>",
                     fmt_date(date).c_str(), results.size());
            html += buf;

            // Group by host
            std::string last_host;
            for (auto& r : results) {
                std::string h = extract_host(r.url);
                if (h != last_host) {
                    if (last_host.size())
                        html += "</div>"; // close previous group
                    html += "<h4 style=\"margin:12px 0 6px;color:#344054\">🏠 "
                            "<a href=\"/host?h=" + url_encode(h) + "\">"
                            + html_escape(h) + "</a></h4><div>";
                    last_host = h;
                }
                html += "<div class=\"result-item\"><a href=\"/replay?url="
                        + url_encode(r.url) + "\">" + html_escape(r.url) + "</a></div>";
            }
            if (!last_host.empty()) html += "</div>";
        }
    } else {
        html += "<div class=\"notice\">请选择或输入日期来浏览当天存档的所有页面。</div>";
    }

    html += PAGE_FOOTER;
    return html;
}

// ── Version Diff ────────────────────────────────────────────

// Simple line-level diff: split text by paragraphs, compare
struct DiffChunk {
    enum Type { UNCHANGED, ADDED, REMOVED };
    Type type;
    std::string text;
};

static std::vector<DiffChunk> simple_diff(const std::string& old_text,
                                           const std::string& new_text) {
    std::vector<DiffChunk> result;

    // Split into paragraphs
    auto split = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t nl = s.find('\n', pos);
            if (nl == std::string::npos) nl = s.size();
            std::string line = s.substr(pos, nl - pos);
            if (!line.empty())
                lines.push_back(line);
            pos = nl + 1;
        }
        if (lines.empty() && !s.empty()) lines.push_back(s);
        return lines;
    };

    auto old_lines = split(old_text);
    auto new_lines = split(new_text);

    // Simple LCS-based diff on paragraphs
    size_t oi = 0, ni = 0;
    while (oi < old_lines.size() || ni < new_lines.size()) {
        if (oi < old_lines.size() && ni < new_lines.size() &&
            old_lines[oi] == new_lines[ni]) {
            result.push_back({DiffChunk::UNCHANGED, old_lines[oi]});
            oi++; ni++;
        } else {
            // Look ahead for match
            bool found = false;
            for (size_t ahead = 1; ahead < 6 && ni + ahead < new_lines.size(); ahead++) {
                if (oi < old_lines.size() && new_lines[ni + ahead] == old_lines[oi]) {
                    for (size_t k = 0; k < ahead; k++)
                        result.push_back({DiffChunk::ADDED, new_lines[ni + k]});
                    ni += ahead;
                    found = true;
                    break;
                }
            }
            if (!found) {
                for (size_t ahead = 1; ahead < 6 && oi + ahead < old_lines.size(); ahead++) {
                    if (ni < new_lines.size() && old_lines[oi + ahead] == new_lines[ni]) {
                        for (size_t k = 0; k < ahead; k++)
                            result.push_back({DiffChunk::REMOVED, old_lines[oi + k]});
                        oi += ahead;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                if (oi < old_lines.size())
                    result.push_back({DiffChunk::REMOVED, old_lines[oi++]});
                if (ni < new_lines.size())
                    result.push_back({DiffChunk::ADDED, new_lines[ni++]});
            }
        }
    }
    return result;
}

static std::string build_diff(QueryEngine& qe, const std::string& url,
                               uint32_t date_a, uint32_t date_b) {
    auto art_a = qe.get_page_by_date(url, date_a);
    auto art_b = qe.get_page_by_date(url, date_b);

    std::string html = page_header("版本对比");

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a>"
            "<a href=\"/replay?url=" + url_encode(url) + "\">查看最新版本</a>"
            "<a href=\"/calendar?url=" + url_encode(url) + "\">版本历史</a></div>";

    html += "<h2>版本对比</h2>";
    html += "<div class=\"result-item\"><strong>URL:</strong> " + html_escape(url) + "<br>";

    if (art_a.url.empty() || art_b.url.empty() || !art_a.valid || !art_b.valid) {
        html += "<div class=\"notice\">无法加载对比所需的两个版本。</div>";
        html += PAGE_FOOTER;
        return html;
    }

    html += "<strong>版本 A:</strong> " + fmt_date(date_a) + " · "
            "<strong>版本 B:</strong> " + fmt_date(date_b) + "</div>";

    // Version navigation
    auto vers = qe.get_versions(url);
    html += "<div class=\"diff-bar\">";
    // Find previous version before date_a
    uint32_t prev_date = 0, next_date = 0;
    for (size_t i = 0; i < vers.size(); i++) {
        if (vers[i].date == date_a && i + 1 < vers.size()) prev_date = vers[i + 1].date;
        if (vers[i].date == date_b && i > 0) next_date = vers[i - 1].date;
    }
    if (prev_date)
        html += "<a class=\"diff-prev\" href=\"/diff?url=" + url_encode(url)
                + "&a=" + std::to_string(prev_date) + "&b=" + std::to_string(date_a) + "\">← 更早版本对比</a>";
    if (next_date)
        html += "<a class=\"diff-next\" href=\"/diff?url=" + url_encode(url)
                + "&a=" + std::to_string(date_b) + "&b=" + std::to_string(next_date) + "\">更新版本对比 →</a>";
    html += "</div>";

    // Execute diff
    auto chunks = simple_diff(art_a.body, art_b.body);

    html += "<div class=\"diff-section\"><h3>正文差异</h3>";
    int total_changes = 0;
    int unchanged_count = 0;
    for (auto& c : chunks) {
        if (c.type == DiffChunk::ADDED) {
            html += "<div class=\"diff-added\"><span class=\"diff-label\">+ 新增</span>"
                    + html_escape(c.text) + "</div>";
            total_changes++;
        } else if (c.type == DiffChunk::REMOVED) {
            html += "<div class=\"diff-removed\"><span class=\"diff-label\">- 删除</span>"
                    + html_escape(c.text) + "</div>";
            total_changes++;
        } else {
            // Show only some unchanged context
            unchanged_count++;
            if (unchanged_count <= 3) {
                html += "<div class=\"diff-unchanged\">" + html_escape(c.text) + "</div>";
            } else if (unchanged_count == 4) {
                html += "<div class=\"diff-unchanged\">... (省略无变化段落) ...</div>";
            }
        }
    }
    if (total_changes == 0)
        html += "<div class=\"notice\">两个版本正文内容完全相同。</div>";
    html += "</div>";

    // Title diff
    if (art_a.title != art_b.title) {
        html += "<div class=\"diff-section\"><h3>标题变化</h3>";
        html += "<div class=\"diff-removed\"><span class=\"diff-label\">- 旧标题</span>"
                + html_escape(art_a.title) + "</div>";
        html += "<div class=\"diff-added\"><span class=\"diff-label\">+ 新标题</span>"
                + html_escape(art_b.title) + "</div>";
        html += "</div>";
    }

    html += PAGE_FOOTER;
    return html;
}

// ── Stats Page (HTML) ───────────────────────────────────────

static std::string build_stats_page(QueryEngine& qe) {
    uint32_t total, urls, dmin, dmax;
    qe.get_stats(total, urls, dmin, dmax);

    char buf[32768];
    std::string html = page_header("统计信息");

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a></div>";
    html += "<h2>📊 归档统计</h2>";

    html += "<div class=\"stats\">";
    snprintf(buf, sizeof(buf),
        "<div class=\"stat-card\"><div class=\"number\">%u</div><div class=\"label\">总存档数</div></div>"
        "<div class=\"stat-card\"><div class=\"number\">%u</div><div class=\"label\">域名数</div></div>"
        "<div class=\"stat-card\"><div class=\"number\">%s</div><div class=\"label\">最早存档</div></div>"
        "<div class=\"stat-card\"><div class=\"number\">%s</div><div class=\"label\">最新存档</div></div>",
        total, urls, fmt_date(dmin).c_str(), fmt_date(dmax).c_str());
    html += buf;
    html += "</div>";

    // Year distribution
    auto yd = qe.get_year_distribution();
    if (!yd.empty()) {
        html += "<h3>按年份存档量</h3>";
        html += "<table style=\"width:100%;border-collapse:collapse;margin:12px 0\">";
        html += "<tr style=\"border-bottom:2px solid var(--line);color:var(--muted)\">"
                "<th style=\"text-align:left;padding:8px\">年份</th>"
                "<th style=\"text-align:right;padding:8px\">存档数</th>"
                "<th style=\"text-align:right;padding:8px\">占比</th>"
                "<th style=\"text-align:left;padding:8px\"></th></tr>";
        for (auto& y : yd) {
            float pct = total > 0 ? y.count * 100.0f / total : 0;
            int bar_w = static_cast<int>(pct * 3); // 0-300px max
            snprintf(buf, sizeof(buf),
                "<tr style=\"border-bottom:1px solid var(--line)\">"
                "<td style=\"padding:6px 8px;font-weight:650\">%u</td>"
                "<td style=\"padding:6px 8px;text-align:right\">%u</td>"
                "<td style=\"padding:6px 8px;text-align:right\">%.1f%%</td>"
                "<td style=\"padding:6px 8px\"><div style=\"background:var(--brand);height:14px;width:%dpx;border-radius:3px;opacity:.6\"></div></td>"
                "</tr>",
                y.year, y.count, pct, bar_w > 0 ? bar_w : 2);
            html += buf;
        }
        html += "</table>";
    }

    // All hosts listing
    auto hosts = qe.get_top_hosts(1000);
    if (!hosts.empty()) {
        html += "<h3 style=\"margin-top:24px\">全部域名 (" + std::to_string(hosts.size()) + ")</h3>";
        html += "<div class=\"top-domains\">";
        int rank = 0;
        for (auto& h : hosts) {
            rank++;
            if (rank > 50) {
                html += "<div style=\"padding:8px;color:var(--muted)\">... 还有 "
                        + std::to_string(hosts.size() - 50) + " 个域名</div>";
                break;
            }
            html += "<div class=\"td-row\"><a href=\"/host?h=" + url_encode(h.first) + "\">"
                    + html_escape(h.first) + "</a>"
                    "<span class=\"td-count\">" + std::to_string(h.second) + " 页</span></div>";
        }
        html += "</div>";
    }

    // Server info
    html += "<h3 style=\"margin-top:24px\">服务器信息</h3>";
    html += "<div class=\"result-item\">";
    snprintf(buf, sizeof(buf),
        "<div class=\"meta-grid\" style=\"display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:8px\">"
        "<div><strong>软件版本</strong><br>WebInfoMall 2.0</div>"
        "<div><strong>工作线程</strong><br>%d</div>"
        "<div><strong>索引格式</strong><br>v2 (URL池)</div>"
        "<div><strong>分片数</strong><br>%d</div>"
        "<div><strong>压缩</strong><br>gzip / zlib</div>"
        "<div><strong>缓存</strong><br>ETag + 304</div>"
        "</div>",
        THREAD_POOL_SIZE, NUM_SHARDS);
    html += buf;
    html += "</div>";

    html += PAGE_FOOTER;
    return html;
}

// ── HTTP Response ─────────────────────────────────────────────

static const char* status_reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 304: return "Not Modified";
        case 302: return "Found";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Content Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

static void send_encoded_response(int fd, int code, const std::string& content_type,
                                  const std::string& body, bool is_gzipped,
                                  const std::string& etag = "",
                                  time_t last_modified = 0,
                                  bool keep_alive = false,
                                  const std::string& extra_headers = "") {
    std::string hdr;
    hdr.reserve(512 + extra_headers.size());
    char line[256];
    snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", code, status_reason(code));
    hdr += line;
    hdr += "Content-Type: " + content_type + "\r\n";
    snprintf(line, sizeof(line), "Content-Length: %zu\r\n", body.size());
    hdr += line;
    hdr += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    hdr += "Server: WebInfoMall/2.0\r\n";
    hdr += "X-Content-Type-Options: nosniff\r\n";
    hdr += "Referrer-Policy: no-referrer\r\n";
    if (content_type.find("text/html") == 0 &&
        extra_headers.find("Content-Security-Policy:") == std::string::npos) {
        hdr += "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; "
               "img-src 'self' data:; form-action 'self'; base-uri 'none'; frame-ancestors 'none'\r\n";
        hdr += "X-Frame-Options: DENY\r\n";
    }
    if (is_gzipped) hdr += "Content-Encoding: gzip\r\n";
    hdr += "Vary: Accept-Encoding\r\n";
    if (!etag.empty()) hdr += "ETag: " + etag + "\r\n";
    if (last_modified) hdr += "Last-Modified: " + http_date(last_modified) + "\r\n";
    if (!etag.empty() || last_modified)
        hdr += "Cache-Control: public, max-age=86400\r\n";
    hdr += extra_headers;
    hdr += "\r\n";

    if (!write_all(fd, hdr.data(), hdr.size())) return;
    write_all(fd, body.data(), body.size());
}

static void send_response(int fd, int code, const std::string& content_type,
                          const std::string& body, bool gzip_ok = false,
                          const std::string& etag = "",
                          time_t last_modified = 0,
                          bool keep_alive = false,
                          const std::string& extra_headers = "") {
    std::string compressed;
    bool is_gzipped = gzip_ok && gzip_compress(body, compressed);
    send_encoded_response(fd, code, content_type,
                          is_gzipped ? compressed : body, is_gzipped,
                          etag, last_modified, keep_alive, extra_headers);
}

static void send_status_response(int fd, int code, const char* status_text,
                                  const std::string& content_type,
                                  const std::string& body) {
    (void)status_text;
    send_response(fd, code, content_type, body);
}

static void send_304(int fd, const std::string& etag, bool keep_alive = false) {
    std::string hdr = "HTTP/1.1 304 Not Modified\r\n"
        "ETag: " + etag + "\r\n"
        "Cache-Control: public, max-age=86400\r\n" +
        (keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n") +
        "Server: WebInfoMall/2.0\r\n"
        "Vary: Accept-Encoding\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n";
    write_all(fd, hdr.data(), hdr.size());
}

static int send_cacheable_response(int fd, int code, const std::string& content_type,
                                   const std::string& body, bool gzip_ok,
                                   const std::string& etag_prefix,
                                   const std::string& if_none_match,
                                   time_t last_modified, bool keep_alive) {
    std::string compressed;
    bool is_gzipped = gzip_ok && gzip_compress(body, compressed);
    const std::string& encoded_body = is_gzipped ? compressed : body;
    std::string etag = make_content_etag(
        etag_prefix + (is_gzipped ? "-gzip" : ""), encoded_body);
    if (if_none_match_matches(if_none_match, etag)) {
        send_304(fd, etag, keep_alive);
        return 304;
    }
    send_encoded_response(fd, code, content_type, encoded_body, is_gzipped,
                          etag, last_modified, keep_alive);
    return code;
}

static void send_redirect(int fd, const std::string& location, bool keep_alive = false) {
    if (location.find_first_of("\r\n") != std::string::npos) {
        send_status_response(fd, 500, "Internal Server Error", "text/plain", "Invalid redirect\n");
        return;
    }
    send_response(fd, 302, "text/plain; charset=utf-8", "", false, "", 0,
                  keep_alive, "Location: " + location + "\r\n");
}

// ── Topic / Hot Event Timeline ──────────────────────────────

static std::string build_topic(QueryEngine& qe, const std::string& query) {
    char buf[65536];
    std::string stitle = "热点事件: " + query;
    std::string html = page_header(stitle);

    html += R"(<div class="nav-links"><a href="/">返回首页</a></div>)";
    if (query.empty()) {
        html += R"(<h2>热点事件回溯</h2>)"
                R"(<section class="search-panel"><div class="search-bar">)"
                R"(<form action="/topic" method="get">)"
                R"(<input type="text" name="q" autofocus>)"
                R"(<button>搜索</button></form></div></section>)";
        html += PAGE_FOOTER;
        return html;
    }

    auto results = qe.search_by_title(query, 100);
    html += R"(<h2>🔥 热点事件回溯: <strong>)" + html_escape(query) + R"(</strong></h2>)";

    if (results.empty()) {
        html += R"(<div class="notice">未找到包含 <strong>)" + html_escape(query) + R"(</strong> 的标题。</div>)";
        html += R"(<section class="search-panel"><div class="search-bar">)"
                R"(<form action="/topic" method="get">)"
                R"(<input type="text" name="q" value=")" + html_escape(query) + R"(">)"
                R"(<button>搜索</button></form></div></section>)";
        html += PAGE_FOOTER;
        return html;
    }

    std::map<uint32_t, int> date_counts;
    std::set<std::string> sources;
    uint32_t date_min = 99999999, date_max = 0;
    for (auto& r : results) {
        date_counts[r.date / 100]++;
        sources.insert(extract_host(r.url));
        if (r.date < date_min) date_min = r.date;
        if (r.date > date_max) date_max = r.date;
    }

    snprintf(buf, sizeof(buf),
        R"(<div class="stats">)"
        R"(<div class="stat-card"><div class="number">%zu</div><div class="label">相关报道</div></div>)"
        R"(<div class="stat-card"><div class="number">%zu</div><div class="label">来源站点</div></div>)"
        R"(<div class="stat-card"><div class="number">%s - %s</div><div class="label">时间范围</div></div>)"
        R"(</div>)",
        results.size(), sources.size(),
        fmt_date(date_min).c_str(), fmt_date(date_max).c_str());
    html += buf;

    if (date_counts.size() >= 2) {
        int max_count = 0;
        for (auto& dc : date_counts) if (dc.second > max_count) max_count = dc.second;
        html += R"(<h3 style="font-size:.9rem;color:#7b7166;margin:12px 0 4px">📊 报道强度</h3>)";
        html += R"(<div class="home-year-chart">)";
        for (auto& dc : date_counts) {
            int pct = max_count > 0 ? (dc.second * 100 / max_count) : 0;
            if (pct < 4) pct = 4;
            uint32_t year = dc.first / 100;
            uint32_t month = dc.first % 100;
            snprintf(buf, sizeof(buf),
                R"(<div class="hy-bar" style="height:%d%%">)"
                R"(<span class="hy-tip">%04u-%02u: %d 篇</span></div>)", pct, year, month, dc.second);
            html += buf;
        }
        html += "</div>";
    }

    html += R"(<h3 style="margin:20px 0 10px">📰 按时间线</h3>)";
    html += R"(<div class="timeline">)";

    std::string last_month;
    for (auto& r : results) {
        uint32_t ym = r.date / 100;
        std::string ym_str = fmt_date(ym * 100 + 1).substr(0, 7);
        if (ym_str != last_month) {
            last_month = ym_str;
            snprintf(buf, sizeof(buf),
                R"(<div class="tl-year-marker"><span class="tl-year">%s</span></div>)", ym_str.c_str());
            html += buf;
        }
        std::string host = extract_host(r.url);
        html += R"(<div class="tl-item">)"
                R"(<a href="/replay?url=)" + url_encode(r.url) + R"(">)"
                R"(<span class="tl-date">)" + fmt_date(r.date) + R"(</span></a>)"
                R"(<span style="color:var(--muted);font-size:.82rem">🏠 )" + html_escape(host) + "</span>";
        if (!r.title.empty()) {
            html += R"(<br><span style="font-size:.9rem;color:#344054">)" + html_escape(r.title) + "</span>";
        }
        html += "</div>";
    }
    if (!results.empty()) html += "</div>";

    html += R"(<h3 style="margin-top:24px">来源分布</h3><div class="top-domains">)";
    std::map<std::string, int> host_counts;
    for (auto& r : results) host_counts[extract_host(r.url)]++;
    for (auto& [host, count] : host_counts) {
        html += R"(<div class="td-row"><a href="/host?h=)" + url_encode(host) + R"(">)"
                + html_escape(host) + "</a>"
                R"(<span class="td-count">)" + std::to_string(count) + R"( 篇</span></div>)";
    }
    html += "</div>";

    html += PAGE_FOOTER;
    return html;
}

// ── Proxy Handler ─────────────────────────────────────────────

static int handle_proxy(QueryEngine& qe, int csock, const std::string& url,
                        uint32_t date, bool gzip_ok, bool keep_alive) {
    auto art = qe.get_page(url, date);
    if (art.url.empty() || !art.valid) {
        std::string msg = "404 Not Found: " + url;
        send_response(csock, 404, "text/plain; charset=utf-8", msg,
                      false, "", 0, keep_alive);
        return 404;
    }
    std::string ct = url_content_type(url);
    bool use_gzip = gzip_ok && ct.find("image/") != 0 && ct.find("font/") != 0;
    // Raw archived content is untrusted HTML from 1991–2017 crawls: serve it
    // sandboxed so embedded <script> can't run in this site's origin, and
    // forbid MIME sniffing.
    std::string extra =
        "Content-Security-Policy: sandbox; default-src 'none'; style-src 'unsafe-inline'; "
        "img-src 'self' data:; media-src 'self'; font-src 'self'\r\n"
        "X-Content-Type-Options: nosniff\r\n";
    send_response(csock, 200, ct, art.body, use_gzip, "", 0, keep_alive, extra);
    return 200;
}

// ── Request Handler ───────────────────────────────────────────

// Returns true if the caller should keep the connection alive.
// Never closes csock — the worker loop owns the socket's lifetime, so a
// return of false means "done with this connection", not "already closed".
// (Closing here and again in the worker double-closed the fd, which could
// tear down an unrelated connection that had reused the same fd number.)
static bool handle_request(QueryEngine& qe, int csock, RateLimiter& limiter,
                           std::string& connection_buffer, bool allow_keepalive) {
    response_write_failed = false;
    // Keep bytes beyond the current header for the next request. This makes
    // HTTP/1.1 pipelining deterministic instead of silently discarding whatever
    // happened to arrive in the same read().
    auto header_deadline = std::chrono::steady_clock::now()
                         + std::chrono::seconds(KEEPALIVE_TIMEOUT_S);
    size_t header_end = connection_buffer.find("\r\n\r\n");
    while (header_end == std::string::npos) {
        if (connection_buffer.size() >= MAX_REQUEST_SIZE) {
            LOG_WRN("Request too large (%zu bytes), closing", connection_buffer.size());
            send_status_response(csock, 413, "Content Too Large",
                                 "text/plain; charset=utf-8", "Request headers too large\n");
            return false;
        }
        int ready = wait_readable_until(csock, header_deadline);
        if (ready == 0) {
            send_status_response(csock, 408, "Request Timeout",
                                 "text/plain; charset=utf-8", "Request headers timed out\n");
            return false;
        }
        if (ready < 0) return false;
        char tmp[BUF_READ_SIZE];
        ssize_t n = read(csock, tmp, sizeof(tmp));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        connection_buffer.append(tmp, static_cast<size_t>(n));
        header_end = connection_buffer.find("\r\n\r\n");
        if (header_end != std::string::npos && header_end + 4 > MAX_REQUEST_SIZE) {
            send_status_response(csock, 413, "Content Too Large",
                                 "text/plain; charset=utf-8", "Request headers too large\n");
            return false;
        }
    }

    header_end += 4;
    std::string req_data = connection_buffer.substr(0, header_end);
    connection_buffer.erase(0, header_end);
    auto req = parse_request(req_data.data(), req_data.size());
    if (!req.valid) {
        send_status_response(csock, 400, "Bad Request",
                             "text/plain; charset=utf-8", "400 Bad Request\n");
        return false;
    }
    if (req.method != "GET") {
        send_response(csock, 405, "text/plain; charset=utf-8", "405 Method Not Allowed\n",
                      false, "", 0, false, "Allow: GET\r\n");
        return false;
    }
    if (req.has_message_body) {
        send_status_response(csock, 400, "Bad Request",
                             "text/plain; charset=utf-8", "Request bodies are not supported\n");
        return false;
    }

    // Rate limiting
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(csock, (sockaddr*)&peer, &plen) == 0) {
        if (limiter.check(peer.sin_addr.s_addr)) {
            char peer_ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
            LOG_WRN("Rate limit exceeded for %s", peer_ip);
            send_response(csock, 429, "text/plain; charset=utf-8",
                          "429 Too Many Requests\n", false, "", 0, false,
                          "Retry-After: 5\r\n");
            return false;
        }
    }

    struct timeval tv0, tv1;
    gettimeofday(&tv0, nullptr);

    bool protocol_keepalive = req.version == "HTTP/1.1"
        ? !req.connection_close : req.connection_keep_alive;
    bool wants_keepalive = allow_keepalive && protocol_keepalive;

    std::string response;
    std::string content_type = "text/html; charset=utf-8";
    int code = 200;

    if (req.path == "/") {
        uint32_t total, urls, dmin, dmax;
        qe.get_stats(total, urls, dmin, dmax);
        uint32_t today = local_today();
        response = build_home(qe, today);
        code = send_cacheable_response(
            csock, code, content_type, response, req.accepts_gzip,
            "home-" + std::to_string(total) + "-" + std::to_string(today),
            req.etag_if_none_match, 0, wants_keepalive);
    }
    else if (req.path == "/search") {
        std::string q = get_param(req.query, "q");
        if (q.size() > MAX_URL_LEN) {
            send_status_response(csock, 400, "Bad Request", "text/plain; charset=utf-8",
                                 "Search query is too long\n");
            return false;
        }
        if (q.empty()) {
            response = build_home(qe);
        } else if (has_http_scheme(q)) {
            auto exact = qe.get_page(q);
            if (!exact.url.empty()) {
                send_redirect(csock, "/replay?url=" + url_encode(q), wants_keepalive);
                code = 302;
                goto done;
            }
            response = build_search(qe, q);
        } else {
            response = build_search(qe, q);
        }
        send_response(csock, code, content_type, response, req.accepts_gzip, "", 0, wants_keepalive);
    }
    else if (req.path == "/replay") {
        std::string url = get_param(req.query, "url");
        if (url.empty()) {
            response = build_home(qe);
            send_response(csock, code, content_type, response, req.accepts_gzip, "", 0, wants_keepalive);
        } else {
            if (!valid_archive_url(url)) {
                send_status_response(csock, 400, "Bad Request", "text/plain; charset=utf-8",
                                     "Invalid archive URL\n");
                return false;
            }
            // Version selector from calendar/timeline links (0 = latest)
            uint32_t req_date = 0;
            if (!parse_date_value(get_param(req.query, "date"), req_date, true)) {
                send_status_response(csock, 400, "Bad Request", "text/plain; charset=utf-8",
                                     "Invalid date; expected YYYYMMDD\n");
                return false;
            }
            auto art = qe.get_page(url, req_date);
            if (art.url.empty() || !art.valid) {
                // Not found, or record exists but failed CRC/decompression.
                // Either way the page cannot be served — treat as 404 ("record
                // exists but is broken"). build_replay renders the right notice;
                // never cache corrupt content.
                response = build_replay(qe, url, std::move(art));
                code = 404;
                send_response(csock, code, content_type, response, req.accepts_gzip, "", 0, wants_keepalive);
            } else {
                std::string article_url = art.url;
                uint32_t article_date = art.date;
                response = build_replay(qe, url, std::move(art));
                time_t lm = 0;
                if (valid_crawl_date(article_date)) {
                    struct tm tm_val = {};
                    tm_val.tm_year = (article_date / 10000) - 1900;
                    tm_val.tm_mon = ((article_date / 100) % 100) - 1;
                    tm_val.tm_mday = article_date % 100;
                    lm = timegm(&tm_val);
                }
                code = send_cacheable_response(
                    csock, code, content_type, response, req.accepts_gzip,
                    replay_etag_prefix(article_url, article_date),
                    req.etag_if_none_match, lm, wants_keepalive);
            }
        }
    }
    else if (req.path == "/topic") {
        std::string q = get_param(req.query, "q");
        if (q.size() > MAX_URL_LEN) {
            send_status_response(csock, 400, "Bad Request", "text/plain; charset=utf-8",
                                 "Topic query is too long\n");
            return false;
        }
        response = build_topic(qe, q);
        send_response(csock, 200, content_type, response, req.accepts_gzip, "", 0, wants_keepalive);
    }
    else if (req.path == "/proxy") {
        std::string url = get_param(req.query, "url");
        if (url.empty()) {
            send_redirect(csock, "/", wants_keepalive);
            code = 302;
            goto done;
        }
        if (!valid_archive_url(url)) {
            send_status_response(csock, 400, "Bad Request", "text/plain; charset=utf-8",
                                 "Invalid archive URL\n");
            return false;
        }
        uint32_t req_date = 0;
        if (!parse_date_value(get_param(req.query, "date"), req_date, true)) {
            send_status_response(csock, 400, "Bad Request", "text/plain; charset=utf-8",
                                 "Invalid date; expected YYYYMMDD\n");
            return false;
        }
        code = handle_proxy(qe, csock, url, req_date, req.accepts_gzip, wants_keepalive);
        // handle_proxy does NOT close the socket if keep_alive
    }
    else if (req.path == "/calendar") {
        std::string url = get_param(req.query, "url");
        if (!url.empty() && !valid_archive_url(url)) {
            send_status_response(csock, 400, "Bad Request", "text/plain; charset=utf-8",
                                 "Invalid archive URL\n");
            return false;
        }
        response = url.empty() ? build_home(qe) : build_calendar(qe, url);
        send_response(csock, code, content_type, response, req.accepts_gzip, "", 0, wants_keepalive);
    }
    else if (req.path == "/host") {
        std::string host = get_param(req.query, "h");
        if (host.empty()) {
            send_redirect(csock, "/", wants_keepalive);
            code = 302;
            goto done;
        }
        if (!valid_host_param(host)) {
            send_status_response(csock, 400, "Bad Request", "text/plain; charset=utf-8",
                                 "Invalid host\n");
            return false;
        }
        response = build_host(qe, host);
        send_response(csock, code, content_type, response, req.accepts_gzip, "", 0, wants_keepalive);
    }
    else if (req.path == "/random") {
        std::string url = qe.get_random_url();
        if (url.empty()) {
            send_redirect(csock, "/", wants_keepalive);
        } else {
            send_redirect(csock, "/replay?url=" + url_encode(url), wants_keepalive);
        }
        code = 302;
        goto done;
    }
    else if (req.path == "/sitemap") {
        std::string host = get_param(req.query, "h");
        if (host.empty()) {
            send_redirect(csock, "/", wants_keepalive);
            code = 302;
            goto done;
        }
        if (!valid_host_param(host)) {
            send_status_response(csock, 400, "Bad Request", "text/plain; charset=utf-8",
                                 "Invalid host\n");
            return false;
        }
        response = build_sitemap(qe, host);
        send_response(csock, code, content_type, response, req.accepts_gzip, "", 0, wants_keepalive);
    }
    else if (req.path == "/browse") {
        std::string date_str = get_param(req.query, "d");
        if (!date_str.empty()) {
            std::string normalized = date_str;
            if (date_str.size() == 10 && date_str[4] == '-' && date_str[7] == '-')
                normalized = date_str.substr(0, 4) + date_str.substr(5, 2) + date_str.substr(8, 2);
            uint32_t browse_date = 0;
            if (!parse_date_value(normalized, browse_date)) {
                send_status_response(csock, 400, "Bad Request", "text/plain; charset=utf-8",
                                     "Invalid date; expected YYYYMMDD or YYYY-MM-DD\n");
                return false;
            }
            date_str = normalized;
        }
        response = build_browse(qe, date_str);
        send_response(csock, code, content_type, response, req.accepts_gzip, "", 0, wants_keepalive);
    }
    else if (req.path == "/diff") {
        std::string url = get_param(req.query, "url");
        std::string a_str = get_param(req.query, "a");
        std::string b_str = get_param(req.query, "b");
        if (url.empty() || a_str.empty() || b_str.empty()) {
            send_redirect(csock, "/", wants_keepalive);
            code = 302;
            goto done;
        }
        if (!valid_archive_url(url)) {
            send_status_response(csock, 400, "Bad Request", "text/plain; charset=utf-8",
                                 "Invalid archive URL\n");
            return false;
        }
        uint32_t da = 0, db = 0;
        if (!parse_date_value(a_str, da) || !parse_date_value(b_str, db)) {
            send_status_response(csock, 400, "Bad Request", "text/plain; charset=utf-8",
                                 "Invalid date; expected YYYYMMDD\n");
            return false;
        }
        response = build_diff(qe, url, da, db);
        send_response(csock, code, content_type, response, req.accepts_gzip, "", 0, wants_keepalive);
    }
    else if (req.path == "/ping") {
        send_response(csock, 200, "text/plain", "pong", false, "", 0, wants_keepalive);
    }
    else if (req.path == "/stats-page") {
        response = build_stats_page(qe);
        send_response(csock, code, content_type, response, req.accepts_gzip, "", 0, wants_keepalive);
    }
    else if (req.path == "/stats") {
        uint32_t total, urls, dmin, dmax;
        qe.get_stats(total, urls, dmin, dmax);
        char json[512];
        snprintf(json, sizeof(json),
            "{\"total\":%u,\"hosts\":%u,\"date_min\":%u,\"date_max\":%u,"
            "\"server\":\"WebInfoMall/2.0\",\"threads\":%d}",
            total, urls, dmin, dmax, THREAD_POOL_SIZE);
        send_response(csock, 200, "application/json; charset=utf-8", json,
                      false, "", 0, wants_keepalive);
    }
    else {
        // Proper 404 page with navigation
        std::string html = page_header("404 Not Found");
        html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a></div>";
        html += "<div class=\"notice\"><h2>404 — 页面不存在</h2>"
                "<p>您请求的页面 <code>" + html_escape(req.path) + "</code> 未找到。</p></div>";
        html += "<section class=\"search-panel\"><div class=\"search-bar\">"
                "<form action=\"/search\" method=\"get\">"
                "<input type=\"text\" name=\"q\" placeholder=\"搜索历史网页...\">"
                "<button>搜索</button></form></div></section>";
        html += PAGE_FOOTER;
        code = 404;
        send_response(csock, code, content_type, html, req.accepts_gzip, "", 0, wants_keepalive);
    }

done:
    gettimeofday(&tv1, nullptr);
    double ms = (tv1.tv_sec - tv0.tv_sec) * 1000.0 + (tv1.tv_usec - tv0.tv_usec) / 1000.0;
    LOG_INF("%s %s?%s -> %d (%.1fms)",
            req.method.c_str(), req.path.c_str(), req.query.c_str(), code, ms);

    return wants_keepalive && !response_write_failed;
}

// ── Thread Pool ───────────────────────────────────────────────

class ThreadPool {
    std::vector<std::thread> workers_;
    std::queue<int> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    QueryEngine& qe_;
    RateLimiter& limiter_;

public:
    ThreadPool(QueryEngine& qe, RateLimiter& limiter, int n_workers = THREAD_POOL_SIZE)
        : qe_(qe), limiter_(limiter) {
        for (int i = 0; i < n_workers; i++) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    bool enqueue(int csock) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_ || queue_.size() >= MAX_PENDING_CONNECTIONS) return false;
            queue_.push(csock);
        }
        cv_.notify_one();
        return true;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

private:
    void worker_loop() {
        while (true) {
            int csock = -1;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                csock = queue_.front();
                queue_.pop();
            }

            try {
                std::string connection_buffer;
                connection_buffer.reserve(4096);
                // Handle requests with a connection-level buffer so pipelined
                // bytes survive between iterations.
                for (int ka_count = 0; ka_count < KEEPALIVE_MAX_REQS; ka_count++) {
                    bool keep_alive = handle_request(
                        qe_, csock, limiter_, connection_buffer,
                        ka_count + 1 < KEEPALIVE_MAX_REQS);
                    if (!keep_alive) break;
                }
            } catch (const std::exception& e) {
                LOG_ERR("Unhandled request exception: %s", e.what());
            } catch (...) {
                LOG_ERR("Unhandled non-standard request exception");
            }
            close(csock);
        }
    }
};

// ── Main Server ───────────────────────────────────────────────

static int run_server(QueryEngine& qe, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(sock); return 1;
    }
    if (listen(sock, LISTEN_BACKLOG) < 0) {
        perror("listen"); close(sock); return 1;
    }

    // Register signal handlers for graceful shutdown
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    // A client that disconnects mid-response would otherwise raise SIGPIPE
    // on write() and kill the whole process.
    signal(SIGPIPE, SIG_IGN);

    // Set accept timeout so we can poll the shutdown flag
    struct timeval tv = {ACCEPT_TIMEOUT_S, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    RateLimiter limiter;
    ThreadPool pool(qe, limiter);

    printf("Server: http://localhost:%d  (workers=%d)\n", port, THREAD_POOL_SIZE);
    printf("Press Ctrl+C to stop.\n");

    while (server_running) {
        sockaddr_in client;
        socklen_t clen = sizeof(client);
        int csock = accept(sock, (sockaddr*)&client, &clen);
        if (csock < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                // Timeout or signal — recheck server_running
                continue;
            }
            perror("accept");
            continue;
        }
        // Apply timeouts before a worker sees the socket. Without an initial
        // timeout, four slow clients can occupy the entire pool indefinitely.
        struct timeval client_timeout = {KEEPALIVE_TIMEOUT_S, 0};
        setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &client_timeout, sizeof(client_timeout));
        setsockopt(csock, SOL_SOCKET, SO_SNDTIMEO, &client_timeout, sizeof(client_timeout));
        if (!pool.enqueue(csock)) {
            send_status_response(csock, 503, "Service Unavailable",
                                 "text/plain; charset=utf-8", "Server is busy\n");
            close(csock);
        }
    }

    printf("\nShutting down...\n");
    pool.shutdown();
    close(sock);
    printf("Server stopped.\n");
    return 0;
}

// ── Entry ─────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <data_dir> <index_dir> [port]\n", argv[0]);
        fprintf(stderr, "  e.g. %s ../archive/data ../archive/index 8088\n", argv[0]);
        return 1;
    }
    std::string data_dir = argv[1];
    std::string index_dir = argv[2];
    char* port_end = nullptr;
    long parsed_port = argc > 3 ? strtol(argv[3], &port_end, 10) : 8088;
    if ((argc > 3 && (!port_end || *port_end != '\0')) ||
        parsed_port < 1 || parsed_port > 65535) {
        fprintf(stderr, "ERROR: port must be an integer from 1 to 65535\n");
        return 1;
    }
    int port = static_cast<int>(parsed_port);

    QueryEngine qe(data_dir, index_dir);
    if (!qe.init()) {
        fprintf(stderr, "ERROR: No complete, valid index set found in %s\n", index_dir.c_str());
        fprintf(stderr, "Run ./load or ./verify before starting the server.\n");
        return 1;
    }

    return run_server(qe, port);
}
