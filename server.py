"""
Web replay server — Wayback Machine-like interface for archived web pages.

Pure Python standard library implementation (no external dependencies).

Usage:
    python3 server.py                  # Start on http://localhost:5000
    python3 server.py --port 8080      # Custom port
    python3 server.py --db archive.db  # Custom database path

NOTE: This is Phase 1 (Python prototype). For production, use the
C++ Phase 2 server in src/:  ./src/serve <data_dir> <index_dir> [port]
"""

import warnings
warnings.warn("Phase 1 (Python) is deprecated. Use the C++ Phase 2 system in src/ for production.", DeprecationWarning, stacklevel=2)

import argparse
import os
import sys
import re
import json
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from store import ArchiveStore
from parser import extract_url_host


# ── HTML Templates ─────────────────────────────────────────────

BASE_HEADER = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
  * {{ box-sizing: border-box; margin: 0; padding: 0; }}
  body {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
         background: #f5f5f5; color: #222; }}
  header {{ background: #1a1a2e; color: #fff; padding: 16px 24px; }}
  header h1 {{ font-size: 1.3em; }}
  header h1 a {{ color: #fff; text-decoration: none; }}
  header p {{ font-size: 0.85em; color: #aaa; margin-top: 4px; }}
  .container {{ max-width: 1000px; margin: 0 auto; padding: 24px; }}
  .search-bar {{ display: flex; gap: 8px; margin: 24px 0; }}
  .search-bar input[type="text"] {{ flex: 1; padding: 12px 16px; font-size: 1em;
      border: 2px solid #ddd; border-radius: 8px; }}
  .search-bar input[type="date"] {{ padding: 12px 16px; font-size: 1em;
      border: 2px solid #ddd; border-radius: 8px; width: 180px; }}
  .search-bar button {{ padding: 12px 24px; background: #2563eb; color: #fff;
      border: none; border-radius: 8px; font-size: 1em; cursor: pointer; }}
  .search-bar button:hover {{ background: #1d4ed8; }}
  .result-item {{ background: #fff; padding: 16px 20px; margin: 8px 0;
      border-radius: 8px; border: 1px solid #e5e7eb; }}
  .result-item a {{ color: #2563eb; text-decoration: none; font-size: 1.05em; }}
  .result-item a:hover {{ text-decoration: underline; }}
  .result-item .meta {{ color: #6b7280; font-size: 0.85em; margin-top: 4px; }}
  .calendar {{ display: flex; flex-wrap: wrap; gap: 20px; margin: 16px 0; }}
  .calendar-year {{ background: #fff; padding: 16px; border-radius: 8px;
      border: 1px solid #e5e7eb; min-width: 200px; }}
  .calendar-year h3 {{ font-size: 1em; color: #374151; margin-bottom: 8px; }}
  .calendar-year .months {{ display: flex; flex-wrap: wrap; gap: 6px; }}
  .calendar-year .month-link {{ display: inline-block; padding: 4px 8px;
      background: #dbeafe; color: #2563eb; border-radius: 4px;
      font-size: 0.85em; text-decoration: none; }}
  .calendar-year .month-link:hover {{ background: #2563eb; color: #fff; }}
  .page-view {{ background: #fff; padding: 24px; border-radius: 8px;
      border: 1px solid #e5e7eb; margin: 16px 0; }}
  .page-view h2 {{ font-size: 1.3em; margin-bottom: 8px; }}
  .page-view .page-meta {{ color: #6b7280; font-size: 0.85em; margin-bottom: 16px;
      padding-bottom: 12px; border-bottom: 1px solid #e5e7eb; }}
  .page-view .page-meta div {{ margin: 4px 0; }}
  .page-view .page-body {{ line-height: 1.8; white-space: pre-wrap;
      word-break: break-word; }}
  .notice {{ background: #fef3c7; border: 1px solid #f59e0b; padding: 12px 16px;
      border-radius: 8px; margin: 12px 0; color: #92400e; font-size: 0.9em; }}
  .notice a {{ color: #92400e; }}
  .stats {{ display: flex; gap: 16px; flex-wrap: wrap; margin: 16px 0; }}
  .stat-card {{ background: #fff; padding: 16px 20px; border-radius: 8px;
      border: 1px solid #e5e7eb; text-align: center; min-width: 120px; }}
  .stat-card .number {{ font-size: 1.5em; font-weight: bold; color: #2563eb; }}
  .stat-card .label {{ font-size: 0.8em; color: #6b7280; }}
  .nav-links {{ margin: 12px 0; }}
  .nav-links a {{ color: #2563eb; text-decoration: none; font-size: 0.9em; }}
  .nav-links a:hover {{ text-decoration: underline; }}
  .version-list {{ margin-top: 12px; }}
  .version-row {{ padding: 10px 16px; border-bottom: 1px solid #f3f4f6; }}
  .version-row:hover {{ background: #f9fafb; }}
  .version-row a {{ color: #2563eb; text-decoration: none; }}
  footer {{ text-align: center; padding: 32px; color: #9ca3af; font-size: 0.8em; }}
  .badge {{ display: inline-block; padding: 2px 8px; background: #dbeafe;
      color: #1e40af; border-radius: 10px; font-size: 0.8em; margin-left: 4px; }}
</style>
</head>
<body>
<header>
  <h1><a href="/">🌐 Web InfoMall — 中国网页信息博物馆</a></h1>
  <p>历史网页回放系统 · Wayback Machine</p>
</header>
<div class="container">
"""

BASE_FOOTER = r"""</div>
<footer>
  Web InfoMall Archive Replay System · Phase 1
</footer>
</body>
</html>"""


def html_page(title: str, body: str) -> str:
    return BASE_HEADER.format(title=html_escape(title)) + body + BASE_FOOTER


def html_escape(text: str) -> str:
    if text is None:
        return ''
    return (text.replace('&', '&amp;')
                .replace('<', '&lt;')
                .replace('>', '&gt;')
                .replace('"', '&quot;'))


def url_encode(s: str) -> str:
    return urllib.parse.quote(s, safe='')


def format_date(d: str) -> str:
    """Format YYYYMMDD to YYYY-MM-DD."""
    if len(d) >= 8:
        return f"{d[:4]}-{d[4:6]}-{d[6:8]}"
    return d


# ── Page Builders ──────────────────────────────────────────────

def build_home(store: ArchiveStore) -> str:
    stats = store.get_stats()
    random_urls = store.get_random_urls(8)

    parts = []
    # Search bar
    parts.append(r'''<div class="search-bar">
  <form action="/search" method="get" style="display:flex;gap:8px;width:100%;">
    <input type="text" name="q" placeholder="输入 URL 搜索历史网页..." autofocus>
    <input type="date" name="date" title="限定日期 (可选)">
    <button type="submit">🔍 搜索</button>
  </form>
</div>''')

    # Stats
    if stats and stats['total_pages'] > 0:
        parts.append(f'''<div class="stats">
  <div class="stat-card"><div class="number">{stats['total_pages']:,}</div><div class="label">已存档网页</div></div>
  <div class="stat-card"><div class="number">{stats['total_unique_urls']:,}</div><div class="label">不重复URL</div></div>
  <div class="stat-card"><div class="number">{stats['total_hosts']:,}</div><div class="label">网站域名</div></div>
  <div class="stat-card"><div class="number">{stats['first_date']} — {stats['last_date']}</div><div class="label">时间范围</div></div>
</div>''')

    # Random URLs
    if random_urls:
        parts.append(r'<div style="margin-top:24px;"><h3>📌 随机页面</h3>')
        for url in random_urls:
            parts.append(f'<div class="result-item"><a href="/replay?url={url_encode(url)}">{html_escape(url)}</a></div>')
        parts.append('</div>')

    # Help
    parts.append(r'''<div style="margin-top:24px;">
  <h3>ℹ️ 使用说明</h3>
  <div class="result-item">
    <p>输入一个历史网页的 URL 地址，查看其在不同时间点的存档版本。</p>
    <p style="margin-top:8px;color:#6b7280;">支持按日期筛选：在日期框中指定日期后，将显示最接近该日期的网页版本。</p>
  </div>
</div>''')

    return html_page('Web InfoMall — 首页', '\n'.join(parts))


def build_search(query: str, date_filter: str, results: list) -> str:
    parts = []
    parts.append(r'<div class="nav-links"><a href="/">← 返回首页</a></div>')

    # Search bar
    q_esc = html_escape(query)
    df_esc = html_escape(date_filter or '')
    parts.append(f'''<div class="search-bar">
  <form action="/search" method="get" style="display:flex;gap:8px;width:100%;">
    <input type="text" name="q" value="{q_esc}" placeholder="输入 URL 搜索...">
    <input type="date" name="date" value="{df_esc}">
    <button type="submit">🔍 搜索</button>
  </form>
</div>''')

    if query:
        if results:
            parts.append(f'<h3 style="margin-top:16px;">找到 {len(results)} 个匹配的 URL</h3>')
            for r in results:
                parts.append(f'''<div class="result-item">
    <a href="/replay?url={url_encode(r['url'])}">{html_escape(r['url'])}</a>
    <div class="meta">
      <span class="badge">{r['version_count']} 个版本</span>
      首次: {r['first_seen']} · 最后: {r['last_seen']}
    </div>
</div>''')
        else:
            parts.append(f'<div class="notice">未找到匹配 <strong>{q_esc}</strong> 的 URL。请尝试更短的URL前缀或不同的搜索词。</div>')

    return html_page(f'搜索结果: {query}', '\n'.join(parts))


def build_replay(page: dict, version_count: int) -> str:
    parts = []
    parts.append(r'<div class="nav-links">')
    parts.append(f'<a href="/">← 返回首页</a> | ')
    parts.append(f'<a href="/calendar?url={url_encode(page["url"])}">📅 查看所有版本</a>')
    parts.append('</div>')

    title = html_escape(page.get('title') or '(无标题)')
    url = page['url']
    crawl_time = page.get('crawl_time', '')
    host = html_escape(page.get('host', ''))
    content_type = html_escape(page.get('content_type', ''))
    body = page.get('body', '') or '(无正文内容)'

    parts.append(f'''<div class="page-view">
  <h2>{title}</h2>
  <div class="page-meta">
    <div>📍 <strong>URL:</strong> <a href="{html_escape(url)}">{html_escape(url)}</a></div>
    <div>🕐 <strong>存档时间:</strong> {format_date(crawl_time)}</div>
    <div>🏠 <strong>站点:</strong> {host}</div>''')
    if content_type:
        parts.append(f'    <div>📄 <strong>类型:</strong> {content_type}</div>')
    parts.append('  </div>')

    if version_count > 1:
        parts.append(f'''  <div class="notice">
    此 URL 共有 <strong>{version_count}</strong> 个历史版本。
    <a href="/calendar?url={url_encode(url)}">查看所有版本 →</a>
  </div>''')

    parts.append(f'  <div class="page-body">{html_escape(body)}</div>')
    parts.append('</div>')

    return html_page(page.get('title') or page['url'], '\n'.join(parts))


def build_calendar(url: str, host: str, versions: list, calendar: dict) -> str:
    parts = []
    parts.append(r'<div class="nav-links">')
    parts.append(f'<a href="/">← 返回首页</a> | ')
    parts.append(f'<a href="/replay?url={url_encode(url)}">📄 查看最新版本</a>')
    parts.append('</div>')

    parts.append(f'''<h2 style="margin:16px 0;">📅 URL 存档版本历史</h2>
<div class="result-item" style="margin-bottom:20px;">
  <strong>URL:</strong> <a href="{html_escape(url)}">{html_escape(url)}</a><br>
  <strong>站点:</strong> {html_escape(host)}<br>
  <strong>版本数:</strong> {len(versions) if versions else 0}
</div>''')

    # Calendar by year
    if calendar:
        parts.append('<h3>按年份浏览</h3><div class="calendar">')
        for year in sorted(calendar.keys(), reverse=True):
            parts.append(f'<div class="calendar-year"><h3>{year} 年</h3><div class="months">')
            for month in sorted(calendar[year].keys()):
                m = int(month)
                parts.append(
                    f'<a class="month-link" href="/replay?url={url_encode(url)}&date={year}{m:02d}01">'
                    f'{m}月 ({len(calendar[year][month])})</a>'
                )
            parts.append('</div></div>')
        parts.append('</div>')

    # Version list
    if versions:
        parts.append(f'<h3 style="margin-top:20px;">所有版本 ({len(versions)})</h3><div class="version-list">')
        for v in versions:
            d = v['crawl_date']
            parts.append(
                f'<div class="version-row">'
                f'<a href="/replay?url={url_encode(url)}&date={d}">{format_date(d)}</a>'
                f'<span class="meta">({v["page_count"]} 条记录)</span>'
                f'</div>'
            )
        parts.append('</div>')

    return html_page(f'版本历史: {url}', '\n'.join(parts))


def build_notfound(url: str) -> str:
    body = f'''<div class="nav-links"><a href="/">← 返回首页</a></div>
<div class="notice">⚠️ 未找到 URL: <strong>{html_escape(url)}</strong> 的存档。</div>
<div class="search-bar">
  <form action="/search" method="get" style="display:flex;gap:8px;width:100%;">
    <input type="text" name="q" value="{html_escape(url)}" placeholder="输入 URL 搜索...">
    <button type="submit">🔍 搜索</button>
  </form>
</div>'''
    return html_page(f'未找到: {url}', body)


# ── Request Handler ────────────────────────────────────────────

class ReplayHandler(BaseHTTPRequestHandler):
    """HTTP request handler for the Web InfoMall replay server."""

    # Will be set by main()
    store: ArchiveStore = None

    def log_message(self, format, *args):
        # Quieter logging
        sys.stderr.write("[%s] %s\n" % (self.log_date_time_string(), args[0]))

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        params = urllib.parse.parse_qs(parsed.query)

        def get_param(name, default=''):
            vals = params.get(name, [default])
            return vals[0].strip() if vals else default

        try:
            if path == '/' or path == '/index.html':
                body = build_home(self.store)
                self._respond(200, body)

            elif path == '/search':
                query = get_param('q')
                date_filter = get_param('date')
                if not query:
                    self._redirect('/')
                    return

                # Full URL → redirect to replay
                if query.startswith('http://') or query.startswith('https://'):
                    rd = f'/replay?url={url_encode(query)}'
                    if date_filter:
                        rd += f'&date={url_encode(date_filter)}'
                    self._redirect(rd)
                    return

                results = self.store.search_by_url_prefix(query, limit=100)
                body = build_search(query, date_filter, results)
                self._respond(200, body)

            elif path == '/replay':
                url = get_param('url')
                date_str = get_param('date')
                if not url:
                    self._redirect('/')
                    return

                page = self.store.get_page_by_url_date(url, date_str if date_str else None)
                if not page:
                    # Try fuzzy search
                    results = self.store.search_by_url_prefix(url, limit=1)
                    if results:
                        self._redirect(f'/replay?url={url_encode(results[0]["url"])}')
                        return
                    body = build_notfound(url)
                    self._respond(404, body)
                    return

                versions = self.store.get_url_versions(url)
                version_count = len(versions)
                body = build_replay(page, version_count)
                self._respond(200, body)

            elif path == '/calendar':
                url = get_param('url')
                if not url:
                    self._redirect('/')
                    return

                versions = self.store.get_url_versions(url)
                cal = self.store.get_url_calendar(url)
                host = extract_url_host(url)
                body = build_calendar(url, host, versions, cal)
                self._respond(200, body)

            elif path == '/stats':
                stats = self.store.get_stats()
                self._respond(200, json.dumps(stats, ensure_ascii=False, indent=2),
                              content_type='application/json')

            else:
                self._respond(404, html_page('404 Not Found',
                    '<div class="notice"><h2>404</h2><p>页面不存在</p></div>'
                    '<div class="nav-links"><a href="/">← 返回首页</a></div>'))

        except Exception as e:
            import traceback
            traceback.print_exc()
            self._respond(500, html_page('Error',
                f'<div class="notice"><h2>500 Internal Error</h2><pre>{html_escape(str(e))}</pre></div>'))

    def _respond(self, code: int, body: str, content_type: str = 'text/html; charset=utf-8'):
        data = body.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _redirect(self, location: str, code: int = 302):
        self.send_response(code)
        self.send_header('Location', location)
        self.end_headers()


# ── Main ───────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Web InfoMall Replay Server")
    ap.add_argument('--port', type=int, default=5000, help='Server port')
    ap.add_argument('--db', type=str, default='archive.db', help='SQLite database path')
    args = ap.parse_args()

    db_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), args.db)
    store = ArchiveStore(db_path)

    stats = store.get_stats()
    print(f"Archive Store: {db_path}")
    print(f"  Pages: {stats['total_pages']:,}")
    print(f"  Unique URLs: {stats['total_unique_urls']:,}")
    print(f"  Hosts: {stats['total_hosts']:,}")
    print(f"  Date range: {stats['first_date']} — {stats['last_date']}")
    print()
    print(f"Server: http://localhost:{args.port}")
    print()
    print("  *** DEPRECATED: This is Phase 1 (Python prototype). ***")
    print("  For production use the C++ Phase 2 server in src/:")
    print("    cd src && make && ./src/serve <data_dir> <index_dir> <port>")
    print()
    print("Press Ctrl+C to stop.")
    print()

    # Inject store into handler class
    ReplayHandler.store = store

    server = HTTPServer(('0.0.0.0', args.port), ReplayHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.shutdown()
        store.close()


if __name__ == '__main__':
    main()
