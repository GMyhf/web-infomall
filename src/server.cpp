/*
 * server.cpp — Minimal HTTP replay server.
 *
 * Pure POSIX sockets, no external dependencies.
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <ctime>
#include <sys/time.h>

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
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':')
            os << c;
        else {
            os << '%' << std::hex << std::uppercase << (unsigned)(uint8_t)c;
        }
    }
    return os.str();
}

static std::string fmt_date(uint32_t d) {
    char buf[11];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", d / 10000, (d / 100) % 100, d % 100);
    return buf;
}

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
    ".search-bar{background:var(--surface);border:1px solid var(--line);border-radius:8px;box-shadow:var(--shadow);padding:14px;margin:22px 0}"
    ".search-bar form{display:flex;gap:10px;width:100%%;align-items:stretch}"
    ".search-bar input[type=text]{flex:1;min-width:0;padding:12px 14px;font-size:1rem;border:1px solid #cfc5b7;border-radius:6px;background:#fff;color:var(--ink);outline:none}"
    ".search-bar input[type=text]:focus{border-color:var(--brand);box-shadow:0 0 0 3px rgba(47,93,98,.16)}"
    ".search-bar button{padding:0 20px;min-height:46px;background:var(--brand);color:#fff;border:none;border-radius:6px;font-size:.96rem;font-weight:650;cursor:pointer;white-space:nowrap}"
    ".search-bar button:hover{background:var(--brand-2)}"
    ".result-item{background:var(--surface);padding:15px 18px;margin:10px 0;border-radius:8px;border:1px solid var(--line);box-shadow:0 1px 0 rgba(58,48,35,.04)}"
    ".result-item:hover{border-color:#c8b9a8;background:#fff}"
    ".result-item a{font-size:1rem;font-weight:620;overflow-wrap:anywhere}"
    ".result-item .meta,.meta{color:var(--muted);font-size:.86rem;margin-top:6px}"
    ".page-view{background:var(--surface);padding:26px;border-radius:8px;border:1px solid var(--line);box-shadow:var(--shadow);margin:18px 0}"
    ".page-view h2{margin-bottom:14px;overflow-wrap:anywhere}"
    ".page-meta{display:grid;gap:8px;color:var(--muted);font-size:.9rem;margin-bottom:18px;padding:14px;border:1px solid var(--line);border-radius:8px;background:var(--surface-2)}"
    ".page-meta div{overflow-wrap:anywhere}"
    ".page-body{line-height:1.85;white-space:pre-wrap;word-break:break-word;font-size:1rem;color:#2b3036;padding-top:4px}"
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
    "@media(max-width:720px){header{padding:16px}.header-inner{display:block}.system-badge{margin-top:10px}.container{padding:20px 14px 32px}.search-bar{padding:12px}.search-bar form{flex-direction:column}.search-bar button{width:100%%}.page-view{padding:18px}.stats{grid-template-columns:1fr 1fr}}"
    "@media(max-width:420px){.stats{grid-template-columns:1fr}header h1{font-size:1.08rem}}"
    "</style></head><body>"
    "<header><div class=\"header-inner\"><div><h1><a href=\"/\">Web InfoMall — 历史网页回放</a></h1>"
    "<p>中国网页信息博物馆 · Archive Replay</p></div><span class=\"system-badge\">C++ Replay System · Phase 2</span></div></header><div class=\"container\">";

static const char* PAGE_FOOTER =
    "</div><footer>Web InfoMall Archive Replay System · C++ Phase 2</footer></body></html>";

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

// ── HTTP Request Parser ───────────────────────────────────────

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
};

static HttpRequest parse_request(const char* data, size_t len) {
    HttpRequest req;
    const char* end = (const char*)memchr(data, '\n', len);
    if (!end) return req;

    std::string line(data, end - data);
    // Trim \r
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();

    // "GET /path?query HTTP/1.1"
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

// ── Page Builders ─────────────────────────────────────────────

static std::string build_home(QueryEngine& qe) {
    uint32_t total, urls, dmin, dmax;
    qe.get_stats(total, urls, dmin, dmax);

    char buf[16384];
    snprintf(buf, sizeof(buf), PAGE_HEADER, "Web InfoMall — 首页");
    std::string html(buf);

    // Search bar
    html += "<div class=\"search-bar\"><form action=\"/search\" method=\"get\">"
            "<input type=\"text\" name=\"q\" placeholder=\"输入 URL 或域名搜索...\" autofocus>"
            "<button type=\"submit\">搜索</button></form></div>";

    // Stats
    html += "<div class=\"stats\">";
    snprintf(buf, sizeof(buf),
        "<div class=\"stat-card\"><div class=\"number\">%u</div><div class=\"label\">已存档</div></div>"
        "<div class=\"stat-card\"><div class=\"number\">%u</div><div class=\"label\">域名</div></div>"
        "<div class=\"stat-card\"><div class=\"number\">%s — %s</div><div class=\"label\">时间范围</div></div>",
        total, urls, fmt_date(dmin).c_str(), fmt_date(dmax).c_str());
    html += buf;
    html += "</div>";

    // Help
    html += "<section class=\"help-section\"><h3>使用说明</h3><div class=\"result-item\">"
            "<p>输入 URL 地址或域名查看历史网页。例如：<code>sina.com.cn</code> 或 <code>http://www.pku.edu.cn</code></p>"
            "<p class=\"meta\">支持按域名浏览、URL 前缀搜索、以及查看同一URL的多个历史版本。</p>"
            "</div></section>";

    html += PAGE_FOOTER;
    return html;
}

static std::string build_search(QueryEngine& qe, const std::string& query) {
    std::string html;
    snprintf((char*)alloca(1000), sizeof(PAGE_HEADER),
        "搜索: %s", html_escape(query).c_str());
    // Use simple string concat
    char hdr[6000];
    snprintf(hdr, sizeof(hdr), PAGE_HEADER, "搜索结果");
    html = hdr;

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a></div>";
    html += "<div class=\"search-bar\"><form action=\"/search\" method=\"get\">"
            "<input type=\"text\" name=\"q\" value=\"" + html_escape(query) + "\">"
            "<button>搜索</button></form></div>";

    // Check if it's a specific URL
    if (query.find("http://") == 0 || query.find("https://") == 0) {
        // Redirect to replay
        html = ""; // Will be handled in main handler
        return html;
    }

    // Try exact host match first
    auto urls = qe.get_host_urls(query, 100);
    if (!urls.empty()) {
        char buf[256];
        snprintf(buf, sizeof(buf), "<h3>域名 <strong>%s</strong> 下有 %zu 个页面</h3>",
                 html_escape(query).c_str(), urls.size());
        html += buf;
        for (auto& u : urls) {
            html += "<div class=\"result-item\"><a href=\"/replay?url=" + url_encode(u.url) + "\">"
                    + html_escape(u.url) + "</a>";
            html += " <span class=\"meta\">(" + fmt_date(u.date) + ")</span>";
            html += "</div>";
        }
    } else {
        // Try host substring match (searches all host blocks)
        auto hosts = qe.search_host_substring(query, 100);
        if (!hosts.empty()) {
            char buf[256];
            snprintf(buf, sizeof(buf), "<h3>找到 %zu 个匹配 \"%s\" 的域名</h3>",
                     hosts.size(), html_escape(query).c_str());
            html += buf;
            for (auto& h : hosts) {
                html += "<div class=\"result-item\">"
                        "<a href=\"/search?q=" + url_encode(h.first) + "\">"
                        + html_escape(h.first) + "</a>"
                        "<span class=\"badge\">" + std::to_string(h.second) + " 页</span>"
                        "</div>";
            }
        } else {
            html += "<div class=\"notice\">未找到匹配 <strong>" + html_escape(query) + "</strong> 的结果。</div>";
        }
    }

    html += PAGE_FOOTER;
    return html;
}

static std::string build_replay(QueryEngine& qe, const std::string& url) {
    auto art = qe.get_page(url);
    if (art.url.empty()) {
        // Build a proper "not found" page instead of empty redirect
        char hdr[6000];
        snprintf(hdr, sizeof(hdr), PAGE_HEADER, "未找到");
        std::string html = hdr;
        html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a></div>";
        html += "<div class=\"notice\">未找到 URL: <strong>" + html_escape(url) + "</strong> 的存档。</div>";
        html += "<div class=\"search-bar\"><form action=\"/search\" method=\"get\">"
                "<input type=\"text\" name=\"q\" value=\"" + html_escape(url) + "\">"
                "<button>搜索</button></form></div>";
        html += PAGE_FOOTER;
        return html;
    }

    char buf[16000];
    snprintf(buf, sizeof(buf), PAGE_HEADER, html_escape(art.title).c_str());
    std::string html(buf);

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a>"
            "<a href=\"/calendar?url=" + url_encode(url) + "\">查看所有版本</a></div>";

    html += "<div class=\"page-view\"><h2>" + html_escape(art.title.empty() ? "(无标题)" : art.title) + "</h2>";
    html += "<div class=\"page-meta\">"
            "<div><strong>URL:</strong> " + html_escape(url) + "</div>"
            "<div><strong>时间:</strong> " + fmt_date(art.date) + "</div>"
            "<div><strong>站点:</strong> " + html_escape(extract_host(url)) + "</div>"
            "</div>";

    // Version count
    auto vers = qe.get_versions(url);
    if (vers.size() > 1) {
        snprintf(buf, sizeof(buf),
            "<div class=\"notice\">此 URL 共有 <strong>%zu</strong> 个历史版本。"
            "<a href=\"/calendar?url=%s\">查看 →</a></div>",
            vers.size(), url_encode(url).c_str());
        html += buf;
    }

    html += "<div class=\"page-body\">" + html_escape(art.body.empty() ? "(无内容)" : art.body) + "</div>";
    html += "</div>" + std::string(PAGE_FOOTER);
    return html;
}

static std::string build_calendar(QueryEngine& qe, const std::string& url) {
    auto vers = qe.get_versions(url);

    char buf[6000];
    snprintf(buf, sizeof(buf), PAGE_HEADER, "版本历史");
    std::string html(buf);

    html += "<div class=\"nav-links\"><a href=\"/\">返回首页</a>"
            "<a href=\"/replay?url=" + url_encode(url) + "\">查看最新版本</a></div>";

    html += "<h2>版本历史</h2>";
    html += "<div class=\"result-item\"><strong>URL:</strong> " + html_escape(url) + "<br>"
            "<strong>站点:</strong> " + html_escape(extract_host(url)) + "<br>"
            "<strong>版本数:</strong> " + std::to_string(vers.size()) + "</div>";

    if (!vers.empty()) {
        html += "<h3>所有版本</h3>";
        for (auto& v : vers) {
            html += "<div class=\"result-item\">"
                    "<a href=\"/replay?url=" + url_encode(url) + "&date=" + std::to_string(v.date) + "\">"
                    + fmt_date(v.date) + "</a>";
            if (v.record_count > 1) {
                html += " <span class=\"badge\">" + std::to_string(v.record_count) + " 条</span>";
            }
            html += "</div>";
        }
    }

    html += PAGE_FOOTER;
    return html;
}

// ── HTTP Response ─────────────────────────────────────────────

static void send_response(int fd, int code, const std::string& content_type, const std::string& body) {
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %d OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Server: WebInfoMall/2.0\r\n"
        "\r\n",
        code, content_type.c_str(), body.size());
    write(fd, hdr, strlen(hdr));
    write(fd, body.data(), body.size());
}

// ── Main Server Loop ──────────────────────────────────────────

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
    if (listen(sock, 10) < 0) {
        perror("listen"); close(sock); return 1;
    }

    printf("Server: http://localhost:%d\n", port);
    printf("Press Ctrl+C to stop.\n");

    while (true) {
        sockaddr_in client;
        socklen_t clen = sizeof(client);
        int csock = accept(sock, (sockaddr*)&client, &clen);
        if (csock < 0) { perror("accept"); continue; }

        char buf[8192];
        ssize_t n = read(csock, buf, sizeof(buf) - 1);
        if (n <= 0) { close(csock); continue; }
        buf[n] = '\0';

        auto req = parse_request(buf, n);
        struct timeval tv0, tv1;
        gettimeofday(&tv0, nullptr);
        // (timing printed at end of request)

        std::string response;
        std::string content_type = "text/html; charset=utf-8";

        if (req.path == "/") {
            response = build_home(qe);
        } else if (req.path == "/search") {
            std::string q = get_param(req.query, "q");
            if (q.empty()) {
                response = build_home(qe);
            } else if (q.find("http://") == 0 || q.find("https://") == 0) {
                // Redirect to replay
                char redir[512];
                snprintf(redir, sizeof(redir),
                    "HTTP/1.0 302 Found\r\nLocation: /replay?url=%s\r\n\r\n",
                    url_encode(q).c_str());
                write(csock, redir, strlen(redir));
                close(csock);
                continue;
            } else {
                response = build_search(qe, q);
            }
        } else if (req.path == "/replay") {
            std::string url = get_param(req.query, "url");
            if (url.empty()) {
                response = build_home(qe);
            } else {
                response = build_replay(qe, url);
            }
        } else if (req.path == "/calendar") {
            std::string url = get_param(req.query, "url");
            response = url.empty() ? build_home(qe) : build_calendar(qe, url);
        } else if (req.path == "/ping") {
            response = "pong";
            content_type = "text/plain";
        } else if (req.path == "/stats") {
            uint32_t total, urls, dmin, dmax;
            qe.get_stats(total, urls, dmin, dmax);
            char json[256];
            snprintf(json, sizeof(json),
                "{\"total\":%u,\"hosts\":%u,\"date_min\":%u,\"date_max\":%u}",
                total, urls, dmin, dmax);
            response = json;
            content_type = "application/json";
        } else {
            response = "<html><body><h1>404</h1></body></html>";
        }

        send_response(csock, 200, content_type, response);
        gettimeofday(&tv1, nullptr);
        double ms = (tv1.tv_sec - tv0.tv_sec) * 1000.0 + (tv1.tv_usec - tv0.tv_usec) / 1000.0;
        printf("[%s] %s?%s -> %d %.1fms\n", req.method.c_str(), req.path.c_str(), req.query.c_str(), 200, ms);
        close(csock);
    }

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
