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
    "@media(max-width:720px){header{padding:16px}.header-inner{display:block}.system-badge{margin-top:10px;white-space:normal}.container{padding:20px 14px 32px}.search-bar{padding:12px}.search-bar form{flex-direction:column}.search-bar button{width:100%%}.page-view{padding:20px;margin:14px 0}.page-view h2{font-size:1.32rem}.page-meta{grid-template-columns:1fr}.page-body{font-size:1rem;line-height:1.88}.stats{grid-template-columns:1fr 1fr}}"
    "@media(max-width:420px){.stats{grid-template-columns:1fr}header h1{font-size:1.08rem}.container{padding-left:12px;padding-right:12px}.result-item{padding:13px 14px}.quick-links a{max-width:100%%;overflow-wrap:anywhere}.page-view{padding:16px}.page-body{font-size:.98rem;line-height:1.82}}"
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

    html += "<section class=\"search-panel\"><div class=\"search-bar\"><form action=\"/search\" method=\"get\">"
            "<input type=\"text\" name=\"q\" placeholder=\"输入 URL 或域名搜索...\" autofocus>"
            "<button type=\"submit\">搜索</button></form></div>"
            "<p class=\"hint\">可输入完整 URL、域名，或域名片段。</p>"
            "<div class=\"quick-links\">"
            "<a href=\"/search?q=sina\">sina</a>"
            "<a href=\"/search?q=dailynews.sina.com.cn\">dailynews.sina.com.cn</a>"
            "<a href=\"/search?q=news.sina.com.cn\">news.sina.com.cn</a>"
            "</div></section>";

    html += "<div class=\"stats\">";
    snprintf(buf, sizeof(buf),
        "<div class=\"stat-card\"><div class=\"number\">%u</div><div class=\"label\">已存档</div></div>"
        "<div class=\"stat-card\"><div class=\"number\">%u</div><div class=\"label\">域名</div></div>"
        "<div class=\"stat-card\"><div class=\"number\">%s — %s</div><div class=\"label\">时间范围</div></div>",
        total, urls, fmt_date(dmin).c_str(), fmt_date(dmax).c_str());
    html += buf;
    html += "</div>";

    html += "<section class=\"help-section\"><h3>使用说明</h3><div class=\"result-item\">"
            "<p>输入 URL 地址或域名查看历史网页。例如：<code>sina.com.cn</code> 或 <code>http://www.pku.edu.cn</code></p>"
            "<p class=\"meta\">支持按域名浏览、URL 前缀搜索，以及查看同一 URL 的多个历史版本。</p>"
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
        snprintf(buf, sizeof(buf), "<h3 class=\"result-summary\">域名 <strong>%s</strong> 下有 %zu 个页面</h3>",
                 html_escape(query).c_str(), urls.size());
        html += buf;
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
                        "<a href=\"/search?q=" + url_encode(h.first) + "\">"
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

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a>"
            "<a href=\"/calendar?url=" + url_encode(url) + "\">查看所有版本</a></div>";

    html += "<div class=\"page-view\"><h2>"
            + html_escape(art.title.empty() ? "(无标题)" : art.title) + "</h2>";
    html += "<div class=\"page-meta\">"
            "<div class=\"meta-item\"><span class=\"meta-label\">URL</span>"
            "<span class=\"meta-value\">" + html_escape(url) + "</span></div>"
            "<div class=\"meta-item\"><span class=\"meta-label\">存档时间</span>"
            "<span class=\"meta-value\">" + fmt_date(art.date) + "</span></div>"
            "<div class=\"meta-item\"><span class=\"meta-label\">站点</span>"
            "<span class=\"meta-value\">" + html_escape(extract_host(url)) + "</span></div>"
            "</div>";

    auto vers = qe.get_versions(url);
    if (vers.size() > 1) {
        snprintf(buf, sizeof(buf),
            "<div class=\"notice\">此 URL 共有 <strong>%zu</strong> 个历史版本。"
            "<a href=\"/calendar?url=%s\">查看 →</a></div>",
            vers.size(), url_encode(url).c_str());
        html += buf;
    }

    html += "<div class=\"page-body\">"
            + html_escape(art.body.empty() ? "(无内容)" : art.body) + "</div>";
    html += "</div>" + std::string(PAGE_FOOTER);
    return html;
}

static std::string build_calendar(QueryEngine& qe, const std::string& url) {
    auto vers = qe.get_versions(url);

    char buf[32768];
    snprintf(buf, sizeof(buf), PAGE_HEADER, "版本历史");
    std::string html(buf);

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a>"
            "<a href=\"/replay?url=" + url_encode(url) + "\">查看最新版本</a></div>";

    html += "<h2>版本历史</h2>";
    html += "<div class=\"result-item\"><strong>URL:</strong> "
            + html_escape(url) + "<br><strong>站点:</strong> "
            + html_escape(extract_host(url)) + "<br><strong>版本数:</strong> "
            + std::to_string(vers.size()) + "</div>";

    if (!vers.empty()) {
        html += "<h3>所有版本</h3>";
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
    else if (req.path == "/ping") {
        send_response(csock, 200, "text/plain", "pong");
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
