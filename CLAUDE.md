# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Web InfoMall is a Wayback Machine-like historical web page replay system for Chinese news articles (1991–2017), based on Peking University Network Lab's Depot architecture. The data source is the [CWIRF 1000万新闻网页数据集](https://www.cwirf.org/).

## Build & Run

### C++ system (Phase 2 — production)

```bash
cd src && make            # Build load + serve binaries
cd src && make test       # Build and run parser smoke test (10 articles)
```

Requires: C++17 (clang++ or g++), zlib, iconv.

### Python prototype (Phase 1 — validation, no deps)

```bash
# Load sample data into SQLite
python3 load_data.py                          # Load dat0 only (~118K articles)
python3 load_data.py --files dat0,dat1,dat2   # Load specific files

# Start HTTP replay server
python3 server.py                             # http://localhost:5000
python3 server.py --port 8080 --db my.db

# Smoke test suite
python3 test_server.py
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

- **`ArticleRecord`** — fixed 40-byte header + variable URL/title/body; body optionally zlib-compressed. Sortable by `(url_hash, crawl_date DESC)`.
- **`UrlIndexEntry`** — 28-byte index entry: `(url_hash, crawl_date, file_offset, record_size, shard_id)`. Sorted for O(log N) binary search.
- **`HostBlock`** — 40-byte embedded index block per hostname (32-byte padded name + first_entry + entry_count). Host-level O(log H) binary search drills down to per-URL entries.
- **`ShardFileHeader`** — file header with host_count, entry_count, followed by sorted HostBlock array, then sorted UrlIndexEntry array.

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
- C++ server is single-threaded, blocking — each request handled sequentially.
- SQLite uses WAL mode with 64MB cache for Phase 1 performance.
- Shards target ~500K entries each for 14M total (fits in memory during index build).
