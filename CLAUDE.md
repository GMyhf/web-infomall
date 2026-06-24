# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Web InfoMall is a Wayback Machine-like historical web page replay system for Chinese news articles (1991–2017), based on Peking University Network Lab's Depot architecture. The data source is the [CWIRF 1000万新闻网页数据集](https://www.cwirf.org/).

## Build & Run

### C++ system (Phase 2 v2 — production)

```bash
cd src && make            # Build load + serve binaries (v2: threaded, gzip)
cd src && make test       # Build and run parser smoke test (10 articles)
```

Requires: C++17 (clang++ or g++), zlib, iconv, pthread.

**macOS note**: If `clang++` can't find C++ stdlib headers (`cstdint` not found), the Makefile auto-detects the SDK path. If that fails, set it manually:
```bash
make CXX_STDLIB=/path/to/sdk/usr/include/c++/v1
```

### Data loading pipeline (C++)

```bash
./src/load <dat_dir> <archive_dir> [--max N] [--files 0,1,2] [--all]
./src/load /path/to/TenMillionArticles/dat ./archive --all   # Full 14M articles (~40 min)
```

### Start C++ replay server

```bash
./src/serve <data_dir> <index_dir> [port]
./src/serve archive/data archive/index 8088
```
The server now uses a **4-worker thread pool** and supports **gzip compression** and **HTTP cache headers** (ETag, Last-Modified, Cache-Control, 304 Not Modified).

### Python prototype (Phase 1 — validation, no deps)

```bash
python3 load_data.py                          # Load dat0 only (~118K articles)
python3 server.py                             # http://localhost:5000
python3 test_server.py
```

## Architecture

### Two-phase design

| Phase | Language | Storage | Server | Purpose |
|-------|----------|---------|--------|---------|
| Phase 1 | Python | SQLite | stdlib HTTP | Prototype, validation, small-scale (~100K articles) |
| Phase 2 | C++17 | Binary + zlib, mmap'd shard index | POSIX sockets | Production, full 14M+ articles |

Both phases share the same data flow: **Parse → Store → Index → Query → Serve**.

### Data pipeline (C++)

```
TenMillionArticles (.dat) → parser.cpp (GBK→UTF-8) → store.cpp (zlib compress, YYYYMM dirs)
                                                              │
                          indexer.cpp (37-shard index with HostBlocks)
                                                              │
                          query.cpp (mmap, binary search on HostBlocks and UrlIndexEntry)
                                                              │
                          server.cpp (pure POSIX HTTP, serves replay pages)
```

### Core data structures (`src/common.h`)

- **`ArticleRecord`** — fixed 40-byte header + variable URL/title/body; body optionally zlib-compressed.
- **`UrlIndexEntry`** — 28-byte index entry: `(url_hash, crawl_date, file_offset, record_size, url_len, url_offset, reserved)`. In v2 shards, the URL is at `url_pool + url_offset` — no data-file IO needed for search or host listing.
- **`HostBlock`** — 40-byte embedded index block per hostname (32-byte padded name + first_entry + entry_count). O(log H) binary search.
- **`ShardFileHeader`** — v2 header with magic `0x49445821` (`"IDX!"`), entry_count, host_count, url_pool_size. Followed by HostBlock array, UrlIndexEntry array, and the URL string pool (concatenated null-terminated URLs).

v1 shards (magic `0x49445820`, `"IDX "`) are still readable by the query engine via a fallback path that reads URLs from data files.

### Shard file layout (v2)

```
ShardFileHeader (16 bytes)
HostBlock[host_count]          (40 bytes each, sorted by hostname)
UrlIndexEntry[entry_count]     (28 bytes each, sorted by host+url_hash+date DESC)
char url_pool[url_pool_size]   (concatenated URLs, each entry points via url_offset+url_len)
```

### Server (v2)

- **4-worker thread pool** (`std::thread` + condition variable), replacing the old single-threaded accept loop.
- **gzip compression**: Responses > 1KB are compressed if `Accept-Encoding: gzip` is present and savings ≥ 5%.
- **Cache headers**: `ETag` (url_hash + date), `Last-Modified` (crawl date), `Cache-Control: public, max-age=86400`. Supports `If-None-Match` → `304 Not Modified`.

### HTTP routes

| Route | Description |
|-------|-------------|
| `/` | Home page with stats and search |
| `/search?q=<term>` | Search by URL prefix or host substring |
| `/replay?url=<url>[&date=YYYYMMDD]` | Replay a specific archived page |
| `/calendar?url=<url>` | Version history with CSS timeline |
| `/host?h=<domain>` | Domain overview: stats, year chart, URL listing |
| `/stats` | JSON API: total articles, hosts, date range |
| `/ping` | Health check (plain text "pong") |

### Sharding strategy

- **37 shards** (prime number, following Depot's pattern).
- **Shard assignment**: XOR hostname characters → rotate → mod 37 (`shard_for_host`).
- Each shard file is independent and self-contained — all entries for a given host live in exactly one shard.
- Shard file name: `url_XX.idx` (XX = 00–36).

### Query engine (`src/query.cpp`)

- **mmap'd shard files** — zero-copy, OS-page-cached.
- **Lookup flow**: URL → extract_host → `shard_for_host` → open shard → binary search HostBlock by hostname → binary search UrlIndexEntry by url_hash within host range.
- **All lookups are O(log N)** with sub-millisecond latency for 14M articles.
- Metadata cached in `meta.dat` for fast stats; falls back to scanning shard headers if missing.

### Storage layout (`src/store.cpp`)

- Data files organized by `YYYYMM/` directories like Depot's DptGroupUp.
- Files split at 2GB boundaries, indexed by `data_NNNN.dat` sequence.
- Body compression uses zlib and only applies if savings > 5%.

### Python modules

| File | Role |
|------|------|
| `parser.py` | `ArticleParser` class — streaming `.dat` parser, GBK→UTF-8 using Python codecs |
| `store.py` | `ArchiveStore` class — SQLite storage with WAL mode, URL hash indexing, version tracking |
| `server.py` | `ReplayHandler` — stdlib `HTTPServer`, routes: `/`, `/search`, `/replay`, `/calendar`, `/stats` |
| `load_data.py` | Data loading pipeline using `ArticleParser` + `ArchiveStore` |

### Templates directory

Jinja2-style templates (`templates/*.html`) for a potential Flask/Jinja2-based server variant. The Python `server.py` embeds HTML inline (not using these templates); the C++ `server.cpp` embeds HTML as C string literals. Templates serve as reference layout/design.

## RFC / encoding handling

- Source data uses **GB2312/GBK** encoding. C++ parser uses `iconv` with fallback chain: GBK → GB18030 → GB2312. Python parser uses `'gb2312'` codec.
- Internally all text is **UTF-8**.
- Record separators: `\x1e` (line), `\x1f` (record/article).

## Key constraints

- Default port: Python 5000, C++ 8088.
- C++ server uses 4-worker thread pool; QueryEngine mmap is thread-safe (read-only).
- SQLite uses WAL mode with 64MB cache for Phase 1 performance.
- Shards target ~500K entries each for 14M total (fits in memory during index build).
- v2 index embeds URLs inline (~80 bytes/entry avg); total index size ~400 MB for 14M articles (vs ~400 MB data files + index).
