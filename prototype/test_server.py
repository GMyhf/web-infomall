"""End-to-end checks for the retained Python prototype server."""

import json
import os
import sys
import tempfile
import threading
import urllib.error
import urllib.parse
import urllib.request
from http.server import HTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import server


def fetch(base_url, path, expected_status):
    try:
        response = urllib.request.urlopen(base_url + path, timeout=5)
    except urllib.error.HTTPError as exc:
        response = exc
    body = response.read()
    assert response.status == expected_status, (path, response.status, body[:200])
    return body


def main():
    url = "http://example.com/news/item.html"
    encoded_url = urllib.parse.quote(url, safe="")

    with tempfile.TemporaryDirectory(prefix="web-infomall-python-test-") as tmp:
        store = server.ArchiveStore(os.path.join(tmp, "archive.db"))
        store.insert_page(url, "20030101", "Old title", "Old body")
        store.insert_page(url, "20040102", "Latest title", "Latest body")
        server.ReplayHandler.store = store

        httpd = HTTPServer(("127.0.0.1", 0), server.ReplayHandler)
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        base_url = f"http://127.0.0.1:{httpd.server_port}"

        try:
            assert b"Web InfoMall" in fetch(base_url, "/", 200)
            assert b"example.com" in fetch(base_url, "/search?q=http", 200)
            assert b"Latest title" in fetch(base_url, f"/replay?url={encoded_url}", 200)
            assert b"Old title" in fetch(
                base_url, f"/replay?url={encoded_url}&date=20030101", 200
            )
            assert b"Latest title" in fetch(
                base_url, f"/replay?url={encoded_url}&date=2004-01-02", 200
            )
            assert b"2003-01-01" in fetch(base_url, f"/calendar?url={encoded_url}", 200)
            fetch(base_url, "/replay?url=http%3A%2F%2Fmissing.example%2F", 404)
            fetch(base_url, "/does-not-exist", 404)
            stats = json.loads(fetch(base_url, "/stats", 200))
            assert stats["total_pages"] == 2
            assert stats["total_unique_urls"] == 1
        finally:
            httpd.shutdown()
            httpd.server_close()
            thread.join(timeout=5)
            assert not thread.is_alive()
            store.close()

    print("PASS: Python prototype server checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
