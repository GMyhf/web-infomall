# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Web InfoMall is a Wayback Machine-like historical web page replay system for Chinese news articles (1991–2017), based on Peking University Network Lab's Depot architecture. The data source is the [CWIRF 1000万新闻网页数据集](https://www.cwirf.org/).

## Collaboration scaffold (Claude ⇄ Codex) — read before starting work

`collab/` is the shared source of truth between the two AI agents. **Read `collab/PLAN.md`
and `collab/README.md` before touching code**; claim a task in `PLAN.md` (sign it, flip it to
`In progress`) before implementing, so the two agents don't edit the same region of the same file.

| File | Role |
|------|------|
| `collab/README.md` | The loop, the collaboration modes, and **this project's red lines** (the review checklist) |
| `collab/PLAN.md` | Single task board (`T-NNN`) + Decision Log |
| `collab/HANDOFF.md` | Handoff log, newest first; template at the bottom of the file |
| `collab/NOTES-claude.md` / `NOTES-codex.md` | Each agent's notes to the other — write what you're *unsure* about |
| `collab/review-input.md` | Generated per handoff by `tools/handoff.py`; **not versioned** |

```bash
python3 tools/handoff.py --verify                        # the gate: make -C src test + py_compile (prototype/ excluded)
python3 tools/handoff.py --from claude --to codex        # generate the review package
```

Handing work back requires a real `--verify` run pasted into `HANDOFF.md`, including each
step's tail counts — not "looks fine to me". Performance claims require `./src/bench`
percentile output plus the archive size they were measured on.

## Build & Run

### C++ system (Phase 2 v2 — production)

```bash
make -C src               # Build load, serve, verify, and bench
make -C src test          # Parser, core, loader checkpoint, and C++ HTTP regressions
```

Requires: C++17 (clang++ or g++), zlib, iconv, pthread, and Python 3 for tests.

On macOS the Makefile automatically adds the active SDK path when `xcrun` is available. Compiler and zlib paths remain overridable through `CXX`, `ZLIB_CFLAGS`, and `ZLIB_LIBS`.

### Data loading pipeline (C++)

```bash
./src/load <dat_dir> <archive_dir> [--max N] [--files 0,1,2 | --all] [--incremental]
./src/load /path/to/TenMillionArticles/dat ./archive --all   # Full dataset
```

### Start C++ replay server

```bash
./src/serve <data_dir> <index_dir> [port] [--trusted-proxy-hops N]
./src/serve archive/data archive/index 8088
./src/serve archive/data archive/index 8088 --trusted-proxy-hops 0   # exposed directly
```

`--trusted-proxy-hops N` (0-8, **default 1**) is how many reverse proxies you operate
in front of this server. The rate-limit key is read N positions left of the right end
of `[X-Forwarded-For entries..., TCP peer]`. Counting from the right is what makes it
safe: the leftmost entry is written by the client. **Pass 0 when exposing the server
directly** — otherwise anyone can forge the header and get a fresh rate-limit bucket
per request, which turns rate limiting off. Unparseable entries and chains shorter
than N fall back to the peer address.

The server now uses a **4-worker thread pool** and supports **gzip compression** and **HTTP cache headers** (ETag, Last-Modified, Cache-Control, 304 Not Modified).

### Python prototype (Phase 1 — validation, no deps)

**Archived and unmaintained** (T-004, decided 2026-08-07). It is excluded from the
gate — nothing verifies it, and nothing will report it when it breaks. Run it from
inside the directory; the modules import each other as siblings.

```bash
cd prototype
python3 load_data.py                          # Load dat0 only (~118K articles)
python3 server.py                             # http://localhost:5000
python3 -m unittest test_parser
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
TenMillionArticles (.dat) → parser.cpp (GB18030→UTF-8) → store.cpp (zlib compress, YYYYMM dirs)
                                                              │
                          indexer.cpp (37-shard index with HostBlocks)
                                                              │
                          query.cpp (mmap, binary search on HostBlocks and UrlIndexEntry)
                                                              │
                          server.cpp (pure POSIX HTTP, serves replay pages)
```

### Core data structures (`src/common.h`)

- **`ArticleRecord`** — fixed 40-byte header + variable URL/title/body; body optionally zlib-compressed.
- **`UrlIndexEntry`** — 28-byte index entry: `(url_hash, crawl_date, file_offset, record_size, url_len, url_offset, file_seq)`. The on-disk `reserved` field carries the `data_NNNN.dat` sequence; in v2 shards, the URL is at `url_pool + url_offset`.
- **`HostBlock`** — 40-byte embedded index block per hostname (32-byte padded name + first_entry + entry_count). O(log H) binary search.
- **`ShardFileHeader`** — v2 header with magic `0x49445821` (`"IDX!"`), entry_count, host_count, url_pool_size. Followed by HostBlock array, UrlIndexEntry array, and a pool of concatenated URL byte slices.

v1 shards (magic `0x49445820`, `"IDX "`) are still readable by the query engine via a fallback path that reads URLs from data files.

### Shard file layout (v2)

```
ShardFileHeader (16 bytes)
HostBlock[host_count]          (40 bytes each, sorted by hostname)
UrlIndexEntry[entry_count]     (28 bytes each, sorted by host+url_hash+date DESC)
char url_pool[url_pool_size]   (concatenated byte slices, each entry points via url_offset+url_len)
```

### Server (v2)

- **4-worker thread pool** (`std::thread` + condition variable), replacing the old single-threaded accept loop.
- **gzip compression**: Responses > 1KB are compressed if `Accept-Encoding: gzip` is present and savings ≥ 5%.
- **Cache headers**: Strong `ETag` values include the rendered-content fingerprint and actual content-encoding variant; replay tags also include URL/date identity. `Last-Modified`, `Cache-Control`, weak/list `If-None-Match`, and `304 Not Modified` are supported.

### HTTP routes

| Route | Description |
|-------|-------------|
| `/` | Home page with stats and search |
| `/search?q=<term>` | Search by URL prefix or host substring |
| `/replay?url=<url>[&date=YYYYMMDD]` | Replay a specific archived page |
| `/topic?q=<term>` | Search the title index for an event or topic |
| `/proxy?url=<url>[&date=YYYYMMDD]` | Return sandboxed archived content with its inferred media type |
| `/calendar?url=<url>` | Version history with CSS timeline |
| `/host?h=<domain>` | Domain overview: stats, year chart, URL listing |
| `/sitemap?h=<domain>` | URL path tree for a host |
| `/browse?d=YYYYMMDD` | Browse pages captured on a date |
| `/diff?url=<url>&a=<date>&b=<date>` | Paragraph-level comparison of two captures |
| `/random` | Redirect to a random archived URL |
| `/stats-page` | HTML archive statistics |
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
- Exact URL/host lookup is O(log N); prefix and host-substring search scan bounded result sets linearly.
- Metadata is cached in `meta.dat`; missing/legacy metadata is recomputed by scanning every validated entry once at startup.

### Storage layout (`src/store.cpp`)

- Data files organized by `YYYYMM/` directories like Depot's DptGroupUp.
- Files split at 2GB boundaries, indexed by `data_NNNN.dat` sequence.
- Body compression uses zlib and only applies if savings > 5%.

### Python modules

| File | Role |
|------|------|
| `prototype/parser.py` | `ArticleParser` class — streaming `.dat` parser, GB18030→UTF-8 using Python codecs |
| `prototype/store.py` | `ArchiveStore` class — SQLite storage with WAL mode, URL hash indexing, version tracking |
| `prototype/server.py` | `ReplayHandler` — stdlib `HTTPServer`, routes: `/`, `/search`, `/replay`, `/calendar`, `/stats` |
| `prototype/load_data.py` | Data loading pipeline using `ArticleParser` + `ArchiveStore` |
| `prototype/test_parser.py` | Parser and multi-file loader regression tests |

### Templates directory

Jinja2-style templates (`templates/*.html`) for a potential Flask/Jinja2-based server variant. The archived Python `prototype/server.py` embeds HTML inline (not using these templates); the C++ `server.cpp` embeds HTML as C string literals. Templates serve as reference layout/design.

## RFC / encoding handling

- Source data uses the GB2312/GBK family. Both parsers prefer **GB18030** (a superset); C++ falls back to GBK and GB2312, replacing malformed sequences without truncating the remaining text.
- Internally all text is **UTF-8**.
- Record separators: `\x1e` (line), `\x1f` (record/article).

## Key constraints

- Default port: Python 5000, C++ 8088.
- C++ server uses 4-worker thread pool; QueryEngine mmap is thread-safe (read-only).
- SQLite uses WAL mode with 64MB cache for Phase 1 performance.
- Shards target ~500K entries each for 14M total (fits in memory during index build).
- v2 index size scales with the 28-byte entry table plus the exact URL byte pool and host blocks.
