"""
Parser for the TenMillionArticles dataset format.

Each article uses \\x1e (ASCII 30) as line separator and \\x1f (ASCII 31)
as article separator. Fields: id, time, url, title, body.
Encoding: GB2312/GBK for Chinese text fields.
"""

import re
from dataclasses import dataclass, field
from typing import Iterator, Optional


@dataclass
class Article:
    """A parsed news article from the dataset."""
    article_id: int
    time: str          # YYYYMMDD format
    url: str
    title: str
    body: str
    raw_size: int = 0  # original bytes size in the data file


class ArticleParser:
    """
    Streaming parser for TenMillionArticles data files.

    Usage:
        parser = ArticleParser()
        for article in parser.parse_file("dat/dat0"):
            process(article)
    """

    # Record separators
    LINE_SEP = b'\x1e'
    RECORD_SEP = b'\x1f'

    # Maximum article size to guard against malformed data (10MB)
    MAX_ARTICLE_BYTES = 10 * 1024 * 1024

    def __init__(self, encoding: str = 'gb2312'):
        self.encoding = encoding
        self.errors = []  # collect parse errors for diagnostics

    def parse_file(self, filepath: str, max_articles: Optional[int] = None) -> Iterator[Article]:
        """
        Parse a single .dat file, yielding Article objects.

        Args:
            filepath: Path to a .dat file
            max_articles: If set, stop after yielding this many articles
        """
        with open(filepath, 'rb') as f:
            buffer = b''
            article_count = 0

            while True:
                if max_articles and article_count >= max_articles:
                    break

                chunk = f.read(16 * 1024 * 1024)  # 16MB chunks
                if not chunk:
                    break

                buffer += chunk

                # Split on record separator
                while self.RECORD_SEP in buffer:
                    idx = buffer.index(self.RECORD_SEP)
                    raw_record = buffer[:idx]
                    buffer = buffer[idx + 1:]

                    if not raw_record.strip():
                        continue

                    if len(raw_record) > self.MAX_ARTICLE_BYTES:
                        self.errors.append(f"Skipping oversized record ({len(raw_record)} bytes)")
                        continue

                    article = self._parse_record(raw_record)
                    if article:
                        article_count += 1
                        yield article
                        if max_articles and article_count >= max_articles:
                            return

                # Prevent unbounded buffer growth
                if len(buffer) > self.MAX_ARTICLE_BYTES * 2:
                    self.errors.append("Buffer overflow, discarding partial data")
                    buffer = b''

    def _parse_record(self, raw: bytes) -> Optional[Article]:
        """Parse a single article record from raw bytes."""
        lines = raw.split(self.LINE_SEP)
        fields = {}

        for line in lines:
            line = line.lstrip(b'\n\r')
            if line.startswith(b'id='):
                try:
                    fields['id'] = int(line[3:].strip())
                except ValueError:
                    return None
            elif line.startswith(b'time='):
                fields['time'] = line[5:].strip().decode('ascii', errors='replace')
            elif line.startswith(b'url='):
                fields['url'] = line[4:].strip().decode('ascii', errors='replace')
            elif line.startswith(b'title='):
                fields['title'] = line[6:].strip().decode(self.encoding, errors='replace')
            elif line.startswith(b'body='):
                fields['body'] = line[5:].strip().decode(self.encoding, errors='replace')

        if 'id' not in fields or 'url' not in fields:
            return None

        return Article(
            article_id=fields.get('id', 0),
            time=fields.get('time', ''),
            url=fields.get('url', ''),
            title=fields.get('title', ''),
            body=fields.get('body', ''),
            raw_size=len(raw),
        )


def extract_url_host(url: str) -> str:
    """Extract host from a URL for host-based grouping."""
    m = re.match(r'https?://([^/:]+)', url)
    return m.group(1).lower() if m else url.lower()


def normalize_url(url: str) -> str:
    """Normalize URL for consistent lookup (lowercase host, preserve path case)."""
    url = url.strip()
    m = re.match(r'(https?://)([^/]+)(.*)', url, re.IGNORECASE)
    if m:
        return m.group(1).lower() + m.group(2).lower() + m.group(3)
    return url
