/*
 * server.cpp — Multi-threaded HTTP replay server with gzip compression.
 *
 * Pure POSIX sockets + std::thread, no external dependencies.
 * Uses QueryEngine for all data access.
 *
 * Usage: ./serve <data_dir> <index_dir> [port]
 */

#include "common.h"
#include "query.cpp"
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
#include <sys/time.h>
#include <zlib.h>

// ── Concurrency ──────────────────────────────────────────────

constexpr int THREAD_POOL_SIZE = 4;
constexpr int LISTEN_BACKLOG   = 128;

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
            os << '%' << std::hex << std::uppercase
               << static_cast<unsigned int>(static_cast<uint8_t>(c));
        }
    }
    return os.str();
}

static std::string fmt_date(uint32_t d) {
    char buf[11];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", d / 10000, (d / 100) % 100, d % 100);
    return buf;
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
    ".site-tree .tree-dir{font-weight:650;color:#344054;font-size:.9rem;margin:6px 0 2px}"
    ".site-tree .tree-dir::before{content:\"📁 \";margin-right:2px}"
    ".site-tree .tree-file{margin-left:20px;font-size:.86rem;padding:2px 0}"
    ".site-tree .tree-file a{color:var(--brand)}"
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

static const char* PAGE_FOOTER =
    "</div><footer>Web InfoMall Archive Replay System · C++ Phase 2 v2</footer></body></html>";

// ── URL Decode ────────────────────────────────────────────────

static std::string url_decode(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int c;
            if (sscanf(s.c_str() + i + 1, "%2x", &c) == 1) {
                r += static_cast<char>(c);
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

// ── Gzip Compression ──────────────────────────────────────────

static bool gzip_compress(const std::string& input, std::string& output) {
    if (input.size() < 1024) return false; // Only compress if worth it

    z_stream zs = {};
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED,
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
    bool accepts_gzip = false;
    std::string etag_if_none_match;
};

static HttpRequest parse_request(const char* data, size_t len) {
    HttpRequest req;
    const char* cursor = data;
    const char* end = data + len;

    // Parse request line
    const char* nl = static_cast<const char*>(memchr(cursor, '\n', end - cursor));
    if (!nl) return req;
    std::string line(cursor, nl - cursor);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();

    size_t p1 = line.find(' ');
    if (p1 == std::string::npos) return req;
    size_t p2 = line.find(' ', p1 + 1);
    if (p2 == std::string::npos) p2 = line.size();

    req.method = line.substr(0, p1);
    std::string full_path = line.substr(p1 + 1, p2 - p1 - 1);
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

        // Check Accept-Encoding
        if (line.size() > 16 && strncasecmp(line.c_str(), "Accept-Encoding:", 16) == 0) {
            req.accepts_gzip = (line.find("gzip") != std::string::npos);
        }
        // Check If-None-Match
        if (line.size() > 14 && strncasecmp(line.c_str(), "If-None-Match:", 14) == 0) {
            size_t vpos = line.find('"');
            if (vpos != std::string::npos) {
                size_t vend = line.find('"', vpos + 1);
                if (vend != std::string::npos)
                    req.etag_if_none_match = line.substr(vpos + 1, vend - vpos - 1);
            }
        }
    }
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

// ── ETag Generation ───────────────────────────────────────────

static std::string make_etag(const std::string& url, uint32_t date) {
    char buf[64];
    uint64_t h = url_hash(url);
    snprintf(buf, sizeof(buf), "\"%llx-%u\"",
             static_cast<unsigned long long>(h), date);
    return buf;
}

// ── Page Builders ─────────────────────────────────────────────

static std::string build_home(QueryEngine& qe) {
    uint32_t total, urls, dmin, dmax;
    qe.get_stats(total, urls, dmin, dmax);

    char buf[16384];
    snprintf(buf, sizeof(buf), PAGE_HEADER, "Web InfoMall — 首页");
    std::string html(buf);

    // Search bar with date browse quick-link
    html += "<section class=\"search-panel\"><div class=\"search-bar\"><form action=\"/search\" method=\"get\">"
            "<input type=\"text\" name=\"q\" placeholder=\"输入 URL 或域名搜索...\" autofocus>"
            "<button type=\"submit\">搜索</button></form></div>"
            "<p class=\"hint\">可输入完整 URL、域名，或域名片段。| "
            "<a href=\"/random\">🎲 手气不错</a> | "
            "<a href=\"/browse\">📅 按日期浏览</a></p>"
            "<div class=\"quick-links\">"
            "<a href=\"/search?q=sina\">sina</a>"
            "<a href=\"/search?q=dailynews.sina.com.cn\">dailynews.sina.com.cn</a>"
            "<a href=\"/search?q=news.sina.com.cn\">news.sina.com.cn</a>"
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
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    uint32_t mmdd = (tm_now.tm_mon + 1) * 100 + tm_now.tm_mday;

    auto today_urls = qe.get_today_in_history(mmdd, 8);
    if (!today_urls.empty()) {
        html += "<section class=\"today-section\"><h3>📰 历史上的今天 (" +
                std::to_string(tm_now.tm_mon + 1) + "月" +
                std::to_string(tm_now.tm_mday) + "日)</h3>";
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
    char hdr[32768];
    snprintf(hdr, sizeof(hdr), PAGE_HEADER, "搜索结果");
    std::string html = hdr;

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a></div>";
    html += "<section class=\"search-panel\"><div class=\"search-bar\"><form action=\"/search\" method=\"get\">"
            "<input type=\"text\" name=\"q\" value=\"" + html_escape(query) + "\">"
            "<button>搜索</button></form></div>"
            "<p class=\"hint\">缩短关键词可以扩大匹配范围；输入完整 URL 会直接进入回放。</p></section>";

    // Try exact host match first
    auto urls = qe.get_host_urls(query, 100);
    if (!urls.empty()) {
        char buf[256];
        snprintf(buf, sizeof(buf), "<h3 class=\"result-summary\">域名 <strong><a href=\"/host?h=%s\">%s</a></strong> 下有 %zu 个页面</h3>",
                 url_encode(query).c_str(), html_escape(query).c_str(), urls.size());
        html += buf;
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
            char buf[256];
            snprintf(buf, sizeof(buf), "<h3 class=\"result-summary\">找到 %zu 个匹配 \"%s\" 的域名</h3>",
                     hosts.size(), html_escape(query).c_str());
            html += buf;
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

static std::string build_replay(QueryEngine& qe, const std::string& url) {
    auto art = qe.get_page(url);
    if (art.url.empty()) {
        char hdr[32768];
        snprintf(hdr, sizeof(hdr), PAGE_HEADER, "未找到");
        std::string html = hdr;
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

    char buf[16000];
    snprintf(buf, sizeof(buf), PAGE_HEADER, html_escape(art.title).c_str());
    std::string html(buf);

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
        snprintf(buf, sizeof(buf),
            "<div class=\"notice\">此 URL 共有 <strong>%zu</strong> 个历史版本。"
            "<a href=\"/calendar?url=%s\">查看所有版本 →</a></div>",
            vers.size(), url_encode(url).c_str());
        html += buf;
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

    html += "<div class=\"page-body\">"
            + html_escape(art.body.empty() ? "(无内容)" : art.body) + "</div>";

    // Metadata panel: technical details about the archived record
    html += "<details class=\"meta-panel\"><summary>📋 存档详情</summary><div class=\"meta-grid\">";
    snprintf(buf, sizeof(buf),
        "<div class=\"meta-cell\"><span class=\"mk\">存档日期</span><span class=\"mv\">%s</span></div>"
        "<div class=\"meta-cell\"><span class=\"mk\">站点域名</span><span class=\"mv\">%s</span></div>"
        "<div class=\"meta-cell\"><span class=\"mk\">URL 路径</span><span class=\"mv\">%s</span></div>",
        fmt_date(art.date).c_str(),
        html_escape(host).c_str(),
        html_escape(url.substr(url.find(host) + host.size())).c_str());
    html += buf;

    // Version stats
    snprintf(buf, sizeof(buf),
        "<div class=\"meta-cell\"><span class=\"mk\">历史版本数</span><span class=\"mv\">%zu</span></div>"
        "<div class=\"meta-cell\"><span class=\"mk\">正文长度</span><span class=\"mv\">%zu 字符</span></div>"
        "<div class=\"meta-cell\"><span class=\"mk\">编码转换</span><span class=\"mv\">GB2312 → UTF-8</span></div>",
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
    snprintf(buf, sizeof(buf), PAGE_HEADER, "版本历史");
    std::string html(buf);

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

    char buf[32768];
    std::string title = "域名: " + host;
    snprintf(buf, sizeof(buf), PAGE_HEADER, html_escape(title).c_str());
    std::string html(buf);

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

    // Compute stats
    uint32_t date_min = UINT32_MAX, date_max = 0;
    int unique_urls = 0;
    std::string last_url;
    std::map<uint32_t, int> year_counts;
    for (auto& u : urls) {
        if (u.date < date_min) date_min = u.date;
        if (u.date > date_max) date_max = u.date;
        if (u.url != last_url) { unique_urls++; last_url = u.url; }
        year_counts[u.date / 10000]++;
    }

    // Hero section
    html += "<div class=\"host-hero\"><h2>🌐 " + html_escape(host) + "</h2>";
    snprintf(buf, sizeof(buf),
        "<div class=\"host-url-count\">%d 个不重复 URL · %zu 条存档记录 · %s — %s</div>",
        unique_urls, urls.size(), fmt_date(date_min).c_str(), fmt_date(date_max).c_str());
    html += buf;

    // Stat cards
    html += "<div class=\"host-stats-row\">";
    snprintf(buf, sizeof(buf),
        "<div class=\"host-stat\"><div class=\"hs-num\">%d</div><div class=\"hs-lbl\">不重复 URL</div></div>"
        "<div class=\"host-stat\"><div class=\"hs-num\">%zu</div><div class=\"hs-lbl\">总存档数</div></div>"
        "<div class=\"host-stat\"><div class=\"hs-num\">%s</div><div class=\"hs-lbl\">最早</div></div>"
        "<div class=\"host-stat\"><div class=\"hs-num\">%s</div><div class=\"hs-lbl\">最新</div></div>"
        "<div class=\"host-stat\"><div class=\"hs-num\">%zu</div><div class=\"hs-lbl\">覆盖年份</div></div>",
        unique_urls, urls.size(), fmt_date(date_min).c_str(), fmt_date(date_max).c_str(),
        year_counts.size());
    html += buf;
    html += "</div>";

    // Year distribution mini-chart
    if (year_counts.size() >= 2) {
        int max_count = 0;
        for (auto& yc : year_counts) if (yc.second > max_count) max_count = yc.second;
        html += "<div style=\"margin-top:14px\"><span style=\"font-size:.82rem;color:#7b7166;font-weight:700\">按年份分布</span>";
        html += "<div class=\"year-chart\">";
        for (auto& yc : year_counts) {
            int pct = max_count > 0 ? (yc.second * 100 / max_count) : 0;
            if (pct < 3) pct = 3;
            snprintf(buf, sizeof(buf),
                "<div class=\"year-bar\" style=\"height:%d%%\">"
                "<span class=\"yb-tip\">%u: %d 篇</span></div>",
                pct, yc.first, yc.second);
            html += buf;
        }
        html += "</div></div>";
    }
    html += "</div>"; // host-hero

    // URL listing
    html += "<h3 style=\"margin-top:20px\">页面列表</h3>";
    last_url.clear();
    int shown = 0;
    for (auto& u : urls) {
        if (u.url == last_url) continue;  // dedup URLs, show first occurrence
        last_url = u.url;
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
    char buf[32768];
    std::string title = "站点地图: " + host;
    snprintf(buf, sizeof(buf), PAGE_HEADER, html_escape(title).c_str());
    std::string html(buf);

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a>"
            "<a href=\"/host?h=" + url_encode(host) + "\">域名概览</a></div>";

    auto urls = qe.get_host_urls(host, 1000);
    if (urls.empty()) {
        html += "<div class=\"notice\">未找到该域名的页面。</div>";
        html += PAGE_FOOTER;
        return html;
    }

    // Build path tree: prefix -> [(full_url, date)]
    std::map<std::string, std::vector<std::pair<std::string, uint32_t>>> tree;
    std::string prefix = "http://";
    if (urls[0].url.find("https://") == 0) prefix = "https://";
    std::string base = prefix + host;

    for (auto& u : urls) {
        // Group by path directory
        std::string path = u.url.substr(base.size());
        if (path.empty()) path = "/";
        size_t last_slash = path.rfind('/');
        std::string dir = base;
        if (last_slash != std::string::npos && last_slash > 0)
            dir = base + path.substr(0, last_slash + 1);
        else if (last_slash == 0)
            dir = base + "/";
        tree[dir].push_back({u.url, u.date});
    }

    snprintf(buf, sizeof(buf),
        "<h2>🗂️ 站点地图：%s</h2>"
        "<div class=\"result-item\"><strong>域名:</strong> %s<br>"
        "<strong>页面数:</strong> %zu<br>"
        "<strong>目录数:</strong> %zu</div>",
        html_escape(host).c_str(), html_escape(host).c_str(), urls.size(), tree.size());
    html += buf;

    // Render tree
    html += "<div class=\"site-tree\"><ul>";
    // Root level
    auto root_it = tree.find(base + "/");
    if (root_it == tree.end()) root_it = tree.find(base);

    std::set<std::string> shown;
    // Collect top-level directories
    std::set<std::string> top_dirs;
    for (auto& kv : tree) {
        std::string rel = kv.first.substr(base.size());
        if (rel.empty() || rel == "/") continue;
        size_t slash = rel.find('/');
        if (slash != std::string::npos && slash + 1 < rel.size())
            top_dirs.insert(base + "/" + rel.substr(0, slash + 1));
        else if (slash == std::string::npos)
            top_dirs.insert(base + "/" + rel + "/");
    }

    // Show root
    for (auto& kv : tree) {
        std::string rel = kv.first.substr(base.size());
        if (rel.empty() || rel == "/" || rel == "") {
            // Files at root
            for (auto& u : kv.second) {
                std::string fn = u.first.substr(base.size());
                html += "<li class=\"tree-file\"><a href=\"/replay?url=" + url_encode(u.first)
                        + "\">" + html_escape(fn.empty() ? "/" : fn) + "</a>"
                        "<span class=\"tree-count\">" + fmt_date(u.second) + "</span></li>";
            }
            break;
        }
    }

    // Show top-level directories
    for (auto& td : top_dirs) {
        std::string dirname = td.substr(base.size());
        size_t total = 0;
        for (auto& kv : tree) {
            if (kv.first.find(td) == 0 || kv.first == td)
                total += kv.second.size();
        }
        html += "<li class=\"tree-dir\">" + html_escape(dirname)
                + "<span class=\"tree-count\">(" + std::to_string(total) + " 个文件)</span></li>";
        // Show files in this directory
        for (auto& kv : tree) {
            if (kv.first == td || kv.first == td.substr(0, td.size() - 1)) {
                for (auto& u : kv.second) {
                    std::string fn = u.first.substr(td.size());
                    if (fn.find('/') == std::string::npos) {
                        html += "<li class=\"tree-file\"><a href=\"/replay?url="
                                + url_encode(u.first) + "\">" + html_escape(fn) + "</a>"
                                "<span class=\"tree-count\">" + fmt_date(u.second) + "</span></li>";
                    }
                }
            }
        }
    }

    html += "</ul></div>";
    html += PAGE_FOOTER;
    return html;
}

// ── Browse by date ──────────────────────────────────────────

static std::string build_browse(QueryEngine& qe, const std::string& date_str) {
    char buf[32768];
    snprintf(buf, sizeof(buf), PAGE_HEADER, "按日期浏览");
    std::string html(buf);

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a></div>";

    // Quick date buttons: compute available years and recent months
    uint32_t total, urls, dmin, dmax;
    qe.get_stats(total, urls, dmin, dmax);
    uint32_t min_year = dmin / 10000, max_year = dmax / 10000;

    html += "<h2>📅 按日期浏览</h2>";
    html += "<div class=\"date-browse-bar\"><form action=\"/browse\" method=\"get\">"
            "<label>选择日期：</label>"
            "<input type=\"date\" name=\"d\" value=\"" + html_escape(date_str) + "\">"
            "<button>浏览</button></form>";
    html += "<div class=\"date-quick-grid\"><span style=\"font-size:.82rem;color:var(--muted)\">快速跳转：</span>";

    // Year buttons
    for (uint32_t y = max_year; y >= min_year && y >= max_year - 12 && y >= min_year; y--) {
        html += "<a class=\"date-btn\" href=\"/browse?d=" + std::to_string(y) + "0101\">"
                + std::to_string(y) + "年</a>";
    }
    html += "</div></div>";

    if (!date_str.empty() && date_str.size() >= 8) {
        uint32_t date = static_cast<uint32_t>(atoi(date_str.c_str()));
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

    char buf[32768];
    snprintf(buf, sizeof(buf), PAGE_HEADER, "版本对比");
    std::string html(buf);

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a>"
            "<a href=\"/replay?url=" + url_encode(url) + "\">查看最新版本</a>"
            "<a href=\"/calendar?url=" + url_encode(url) + "\">版本历史</a></div>";

    html += "<h2>版本对比</h2>";
    html += "<div class=\"result-item\"><strong>URL:</strong> " + html_escape(url) + "<br>";

    if (art_a.url.empty() || art_b.url.empty()) {
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
            static int unchanged_count = 0;
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
    snprintf(buf, sizeof(buf), PAGE_HEADER, "统计信息");
    std::string html(buf);

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

static void send_response(int fd, int code, const std::string& content_type,
                           const std::string& body, bool gzip_ok = false,
                           const std::string& etag = "",
                           time_t last_modified = 0) {
    std::string response_body = body;
    bool is_gzipped = false;

    if (gzip_ok) {
        std::string compressed;
        if (gzip_compress(body, compressed)) {
            response_body = compressed;
            is_gzipped = true;
        }
    }

    char hdr[1024];
    int hdr_len;

    if (etag.empty() && last_modified == 0) {
        // Simple response
        hdr_len = snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 %d OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "Server: WebInfoMall/2.0\r\n"
            "%s"
            "\r\n",
            code, content_type.c_str(), response_body.size(),
            is_gzipped ? "Content-Encoding: gzip\r\n" : "");
    } else {
        // Response with caching headers
        hdr_len = snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 %d OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "Server: WebInfoMall/2.0\r\n"
            "%s"
            "%s"
            "%s"
            "Cache-Control: public, max-age=86400\r\n"
            "\r\n",
            code, content_type.c_str(), response_body.size(),
            is_gzipped ? "Content-Encoding: gzip\r\n" : "",
            etag.empty() ? "" : ("ETag: " + etag + "\r\n").c_str(),
            last_modified ? ("Last-Modified: " + http_date(last_modified) + "\r\n").c_str() : "");
    }

    write(fd, hdr, hdr_len);
    write(fd, response_body.data(), response_body.size());
}

static void send_304(int fd, const std::string& etag) {
    char hdr[256];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 304 Not Modified\r\n"
        "ETag: %s\r\n"
        "Cache-Control: public, max-age=86400\r\n"
        "Connection: close\r\n"
        "Server: WebInfoMall/2.0\r\n"
        "\r\n",
        etag.c_str());
    write(fd, hdr, hdr_len);
}

static void send_redirect(int fd, const std::string& location) {
    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 302 Found\r\n"
        "Location: %s\r\n"
        "Connection: close\r\n"
        "Server: WebInfoMall/2.0\r\n"
        "\r\n",
        location.c_str());
    write(fd, hdr, hdr_len);
}

// ── Request Handler ───────────────────────────────────────────

static void handle_request(QueryEngine& qe, int csock) {
    char buf[8192];
    ssize_t n = read(csock, buf, sizeof(buf) - 1);
    if (n <= 0) { close(csock); return; }
    buf[n] = '\0';

    auto req = parse_request(buf, n);
    struct timeval tv0, tv1;
    gettimeofday(&tv0, nullptr);

    std::string response;
    std::string content_type = "text/html; charset=utf-8";
    int code = 200;

    if (req.path == "/") {
        // ETag based on total article count (changes only on re-index)
        uint32_t total, urls, dmin, dmax;
        qe.get_stats(total, urls, dmin, dmax);
        std::string etag = "\"home-" + std::to_string(total) + "\"";
        if (!req.etag_if_none_match.empty() && req.etag_if_none_match == etag) {
            send_304(csock, etag);
            close(csock);
            return;
        }
        response = build_home(qe);
        send_response(csock, code, content_type, response, req.accepts_gzip);
    }
    else if (req.path == "/search") {
        std::string q = get_param(req.query, "q");
        if (q.empty()) {
            response = build_home(qe);
        } else if (q.find("http://") == 0 || q.find("https://") == 0) {
            send_redirect(csock, "/replay?url=" + url_encode(q));
            close(csock);
            return;
        } else {
            response = build_search(qe, q);
        }
        send_response(csock, code, content_type, response, req.accepts_gzip);
    }
    else if (req.path == "/replay") {
        std::string url = get_param(req.query, "url");
        if (url.empty()) {
            response = build_home(qe);
            send_response(csock, code, content_type, response, req.accepts_gzip);
        } else {
            auto art = qe.get_page(url);
            if (art.url.empty()) {
                response = build_replay(qe, url); // generates 404 page
                code = 404;
                send_response(csock, code, content_type, response, req.accepts_gzip);
            } else {
                std::string etag = make_etag(art.url, art.date);
                if (!req.etag_if_none_match.empty() && req.etag_if_none_match == etag) {
                    send_304(csock, etag);
                    close(csock);
                    return;
                }
                response = build_replay(qe, url);
                // Set Last-Modified to the crawl date
                time_t lm = 0;
                if (art.date >= 19910101) {
                    struct tm tm_val = {};
                    tm_val.tm_year = (art.date / 10000) - 1900;
                    tm_val.tm_mon = ((art.date / 100) % 100) - 1;
                    tm_val.tm_mday = art.date % 100;
                    lm = timegm(&tm_val);
                }
                send_response(csock, code, content_type, response,
                              req.accepts_gzip, etag, lm);
            }
        }
    }
    else if (req.path == "/calendar") {
        std::string url = get_param(req.query, "url");
        response = url.empty() ? build_home(qe) : build_calendar(qe, url);
        send_response(csock, code, content_type, response, req.accepts_gzip);
    }
    else if (req.path == "/host") {
        std::string host = get_param(req.query, "h");
        if (host.empty()) {
            send_redirect(csock, "/");
            close(csock);
            return;
        }
        response = build_host(qe, host);
        send_response(csock, code, content_type, response, req.accepts_gzip);
    }
    else if (req.path == "/random") {
        std::string url = qe.get_random_url();
        if (url.empty()) {
            send_redirect(csock, "/");
        } else {
            send_redirect(csock, "/replay?url=" + url_encode(url));
        }
        close(csock);
        return;
    }
    else if (req.path == "/sitemap") {
        std::string host = get_param(req.query, "h");
        if (host.empty()) {
            send_redirect(csock, "/");
            close(csock);
            return;
        }
        response = build_sitemap(qe, host);
        send_response(csock, code, content_type, response, req.accepts_gzip);
    }
    else if (req.path == "/browse") {
        std::string date_str = get_param(req.query, "d");
        response = build_browse(qe, date_str);
        send_response(csock, code, content_type, response, req.accepts_gzip);
    }
    else if (req.path == "/diff") {
        std::string url = get_param(req.query, "url");
        std::string a_str = get_param(req.query, "a");
        std::string b_str = get_param(req.query, "b");
        if (url.empty() || a_str.empty() || b_str.empty()) {
            send_redirect(csock, "/");
            close(csock);
            return;
        }
        uint32_t da = static_cast<uint32_t>(atoi(a_str.c_str()));
        uint32_t db = static_cast<uint32_t>(atoi(b_str.c_str()));
        response = build_diff(qe, url, da, db);
        send_response(csock, code, content_type, response, req.accepts_gzip);
    }
    else if (req.path == "/ping") {
        send_response(csock, 200, "text/plain", "pong");
    }
    else if (req.path == "/stats-page") {
        response = build_stats_page(qe);
        send_response(csock, code, content_type, response, req.accepts_gzip);
    }
    else if (req.path == "/stats") {
        uint32_t total, urls, dmin, dmax;
        qe.get_stats(total, urls, dmin, dmax);
        char json[512];
        snprintf(json, sizeof(json),
            "{\"total\":%u,\"hosts\":%u,\"date_min\":%u,\"date_max\":%u,"
            "\"server\":\"WebInfoMall/2.0\",\"threads\":%d}",
            total, urls, dmin, dmax, THREAD_POOL_SIZE);
        send_response(csock, 200, "application/json; charset=utf-8", json);
    }
    else {
        // Proper 404 page with navigation
        char hdr[16384];
        snprintf(hdr, sizeof(hdr), PAGE_HEADER, "404 Not Found");
        std::string html = hdr;
        html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a></div>";
        html += "<div class=\"notice\"><h2>404 — 页面不存在</h2>"
                "<p>您请求的页面 <code>" + html_escape(req.path) + "</code> 未找到。</p></div>";
        html += "<section class=\"search-panel\"><div class=\"search-bar\">"
                "<form action=\"/search\" method=\"get\">"
                "<input type=\"text\" name=\"q\" placeholder=\"搜索历史网页...\">"
                "<button>搜索</button></form></div></section>";
        html += PAGE_FOOTER;
        send_response(csock, 404, content_type, html, req.accepts_gzip);
    }

    gettimeofday(&tv1, nullptr);
    double ms = (tv1.tv_sec - tv0.tv_sec) * 1000.0 + (tv1.tv_usec - tv0.tv_usec) / 1000.0;
    printf("[%s] %s?%s -> %d %.1fms\n",
           req.method.c_str(), req.path.c_str(), req.query.c_str(), code, ms);
    close(csock);
}

// ── Thread Pool ───────────────────────────────────────────────

class ThreadPool {
    std::vector<std::thread> workers_;
    std::queue<int> queue_;               // client sockets
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    QueryEngine& qe_;

public:
    ThreadPool(QueryEngine& qe, int n_workers = THREAD_POOL_SIZE) : qe_(qe) {
        for (int i = 0; i < n_workers; i++) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    void enqueue(int csock) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push(csock);
        }
        cv_.notify_one();
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
            handle_request(qe_, csock);
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

    ThreadPool pool(qe);

    printf("Server: http://localhost:%d  (workers=%d)\n", port, THREAD_POOL_SIZE);
    printf("Press Ctrl+C to stop.\n");

    while (true) {
        sockaddr_in client;
        socklen_t clen = sizeof(client);
        int csock = accept(sock, (sockaddr*)&client, &clen);
        if (csock < 0) {
            perror("accept");
            continue;
        }
        pool.enqueue(csock);
    }

    pool.shutdown();
    close(sock);
    return 0;
}

// ── Entry ─────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <data_dir> <index_dir> [port]\n", argv[0]);
        fprintf(stderr, "  e.g. %s ../archive/data ../archive/index 8088\n", argv[0]);
        return 1;
    }
    std::string data_dir = argv[1];
    std::string index_dir = argv[2];
    int port = (argc > 3) ? atoi(argv[3]) : 8088;

    QueryEngine qe(data_dir, index_dir);
    if (!qe.init()) {
        fprintf(stderr, "ERROR: No index files found in %s\n", index_dir.c_str());
        fprintf(stderr, "Run ./load first to build the archive.\n");
        return 1;
    }

    return run_server(qe, port);
}
