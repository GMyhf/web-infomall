"""
Storage layer for web page archive.

Phase 1: SQLite-based storage with URL+timestamp indexing.
Designed to be compatible with Tianwang Format concepts:
- Each record has URL, crawl time, content (title+body), and metadata.
- URL index supports multiple versions (same URL, different timestamps).
- Host index for host-based browsing.
"""

import sqlite3
import time as _time
from datetime import datetime, timedelta
from typing import Optional, List, Tuple
from contextlib import contextmanager


CREATE_TABLES_SQL = """
CREATE TABLE IF NOT EXISTS pages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    url TEXT NOT NULL,
    url_hash TEXT NOT NULL,        -- MD5-like hash for fast lookup
    host TEXT NOT NULL,             -- extracted host for grouping
    crawl_time TEXT NOT NULL,       -- YYYYMMDD or ISO date
    crawl_timestamp INTEGER NOT NULL, -- unix timestamp for range queries
    title TEXT,
    body TEXT,
    content_type TEXT DEFAULT 'text/html',
    raw_size INTEGER DEFAULT 0,
    source_file TEXT,               -- which .dat file this came from
    created_at TEXT DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_url_hash ON pages(url_hash, crawl_timestamp);
CREATE INDEX IF NOT EXISTS idx_host ON pages(host, crawl_timestamp);
CREATE INDEX IF NOT EXISTS idx_crawl_time ON pages(crawl_timestamp);
CREATE INDEX IF NOT EXISTS idx_url_timestamp ON pages(url_hash, crawl_timestamp DESC);

-- For the "calendar view" of available snapshots per URL
CREATE TABLE IF NOT EXISTS url_versions (
    url_hash TEXT NOT NULL,
    url TEXT NOT NULL,
    crawl_date TEXT NOT NULL,       -- YYYYMMDD
    page_count INTEGER DEFAULT 1,
    PRIMARY KEY (url_hash, crawl_date)
);

CREATE INDEX IF NOT EXISTS idx_url_versions_date ON url_versions(url_hash, crawl_date DESC);
"""


def _url_hash(url: str) -> str:
    """Simple fast hash for URL indexing."""
    import hashlib
    return hashlib.md5(url.encode('utf-8', errors='replace')).hexdigest()


def _date_to_timestamp(date_str: str) -> int:
    """Convert YYYYMMDD to unix timestamp (noon UTC)."""
    try:
        if len(date_str) == 8:
            dt = datetime(int(date_str[:4]), int(date_str[4:6]), int(date_str[6:8]), 12, 0, 0)
            return int(dt.timestamp())
    except (ValueError, IndexError):
        pass
    return 0


class ArchiveStore:
    """Manages storage and retrieval of archived web pages."""

    def __init__(self, db_path: str = "archive.db"):
        self.db_path = db_path
        self._init_db()

    @contextmanager
    def _get_conn(self):
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA synchronous=NORMAL")
        conn.execute("PRAGMA cache_size=-64000")  # 64MB cache
        try:
            yield conn
            conn.commit()
        except Exception:
            conn.rollback()
            raise
        finally:
            conn.close()

    def _init_db(self):
        with self._get_conn() as conn:
            conn.executescript(CREATE_TABLES_SQL)

    def insert_page(self, url: str, crawl_time: str, title: str, body: str,
                    host: str = "", source_file: str = "", raw_size: int = 0) -> int:
        """
        Insert a single page record. Returns the row ID.
        """
        uh = _url_hash(url)
        if not host:
            from parser import extract_url_host
            host = extract_url_host(url)
        ts = _date_to_timestamp(crawl_time)

        with self._get_conn() as conn:
            cur = conn.execute(
                """INSERT INTO pages (url, url_hash, host, crawl_time, crawl_timestamp,
                   title, body, raw_size, source_file)
                   VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                (url, uh, host, crawl_time, ts, title, body, raw_size, source_file)
            )
            page_id = cur.lastrowid

            # Update url_versions summary
            conn.execute(
                """INSERT OR REPLACE INTO url_versions (url_hash, url, crawl_date, page_count)
                   VALUES (?, ?, ?,
                   COALESCE((SELECT page_count FROM url_versions WHERE url_hash=? AND crawl_date=?), 0) + 1)""",
                (uh, url, crawl_time, uh, crawl_time)
            )
            return page_id

    def insert_batch(self, articles, source_file: str = "") -> int:
        """
        Bulk insert articles. Returns count of inserted pages.

        Args:
            articles: iterable of (url, crawl_time, title, body) tuples
        """
        from parser import extract_url_host

        count = 0
        with self._get_conn() as conn:
            for art in articles:
                uh = _url_hash(art[0])
                host = extract_url_host(art[0])
                ts = _date_to_timestamp(art[1])

                conn.execute(
                    """INSERT INTO pages (url, url_hash, host, crawl_time, crawl_timestamp,
                       title, body, raw_size, source_file)
                       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                    (art[0], uh, host, art[1], ts, art[2], art[3],
                     getattr(art, 'raw_size', 0) if hasattr(art, 'raw_size') else 0,
                     source_file)
                )
                conn.execute(
                    """INSERT OR REPLACE INTO url_versions (url_hash, url, crawl_date, page_count)
                       VALUES (?, ?, ?,
                       COALESCE((SELECT page_count FROM url_versions WHERE url_hash=? AND crawl_date=?), 0) + 1)""",
                    (uh, art[0], art[1], uh, art[1])
                )
                count += 1
        return count

    def get_page_by_url_date(self, url: str, date_str: str = None) -> Optional[dict]:
        """
        Get a page by URL and optional date. Returns the closest match.

        If date_str is provided, returns the page closest to that date.
        If None, returns the latest version.
        """
        uh = _url_hash(url)

        with self._get_conn() as conn:
            if date_str:
                cur = conn.execute(
                    """SELECT * FROM pages
                       WHERE url_hash = ?
                       ORDER BY ABS(crawl_timestamp - ?) ASC
                       LIMIT 1""",
                    (uh, _date_to_timestamp(date_str))
                )
            else:
                cur = conn.execute(
                    """SELECT * FROM pages
                       WHERE url_hash = ?
                       ORDER BY crawl_timestamp DESC
                       LIMIT 1""",
                    (uh,)
                )
            row = cur.fetchone()
            return dict(row) if row else None

    def get_page_by_id(self, page_id: int) -> Optional[dict]:
        """Get a specific page by its internal ID."""
        with self._get_conn() as conn:
            row = conn.execute("SELECT * FROM pages WHERE id = ?", (page_id,)).fetchone()
            return dict(row) if row else None

    def get_url_versions(self, url: str) -> List[dict]:
        """Get all available versions (dates) for a URL, newest first."""
        uh = _url_hash(url)
        with self._get_conn() as conn:
            rows = conn.execute(
                """SELECT crawl_date, page_count
                   FROM url_versions
                   WHERE url_hash = ?
                   ORDER BY crawl_date DESC""",
                (uh,)
            ).fetchall()
            return [dict(r) for r in rows]

    def get_url_calendar(self, url: str) -> dict:
        """
        Get a calendar-friendly view of available snapshots:
        Returns {year: {month: [days]}} structure.
        """
        versions = self.get_url_versions(url)
        calendar = {}
        for v in versions:
            date_str = v['crawl_date']
            if len(date_str) >= 8:
                year = date_str[:4]
                month = str(int(date_str[4:6]))  # remove leading zero
                day = str(int(date_str[6:8]))
                calendar.setdefault(year, {}).setdefault(month, []).append(day)
        return calendar

    def search_by_url_prefix(self, prefix: str, limit: int = 100) -> List[dict]:
        """Search URLs by prefix. Returns distinct URLs with their version counts."""
        with self._get_conn() as conn:
            rows = conn.execute(
                """SELECT url, COUNT(*) as version_count,
                   MIN(crawl_time) as first_seen, MAX(crawl_time) as last_seen
                   FROM pages
                   WHERE url LIKE ?
                   GROUP BY url_hash
                   ORDER BY version_count DESC
                   LIMIT ?""",
                (prefix + '%', limit)
            ).fetchall()
            return [dict(r) for r in rows]

    def search_by_host(self, host: str, limit: int = 100) -> List[dict]:
        """Get all URLs for a given host."""
        with self._get_conn() as conn:
            rows = conn.execute(
                """SELECT url, COUNT(*) as version_count,
                   MIN(crawl_time) as first_seen, MAX(crawl_time) as last_seen
                   FROM pages
                   WHERE host = ?
                   GROUP BY url_hash
                   ORDER BY version_count DESC
                   LIMIT ?""",
                (host.lower(), limit)
            ).fetchall()
            return [dict(r) for r in rows]

    def get_stats(self) -> dict:
        """Get overall archive statistics."""
        with self._get_conn() as conn:
            total_pages = conn.execute("SELECT COUNT(*) as n FROM pages").fetchone()['n']
            total_urls = conn.execute("SELECT COUNT(DISTINCT url_hash) as n FROM pages").fetchone()['n']
            total_hosts = conn.execute("SELECT COUNT(DISTINCT host) as n FROM pages").fetchone()['n']
            date_range = conn.execute(
                "SELECT MIN(crawl_time) as first, MAX(crawl_time) as last FROM pages"
            ).fetchone()
            return {
                'total_pages': total_pages,
                'total_unique_urls': total_urls,
                'total_hosts': total_hosts,
                'first_date': date_range['first'],
                'last_date': date_range['last'],
            }

    def get_random_urls(self, limit: int = 10) -> List[str]:
        """Get random URLs from the archive (for browsing/discovery)."""
        with self._get_conn() as conn:
            rows = conn.execute(
                """SELECT DISTINCT url FROM pages
                   ORDER BY RANDOM() LIMIT ?""",
                (limit,)
            ).fetchall()
            return [r['url'] for r in rows]

    def close(self):
        pass  # SQLite connections are managed per-operation
