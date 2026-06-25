# Web InfoMall — 中国网页信息博物馆 回放系统

基于北京大学网络实验室 [Web InfoMall](http://www.infomall.cn/) 技术架构的历史网页回放系统，类似 archive.org / Wayback Machine。

## 数据来源

[1000万新闻网页数据集](https://www.cwirf.org/) — 从中国Web信息博物馆保存的历史网页中摘选，涵盖1991-2017年间超过1400万篇新闻。

## 系统架构

学习 Pekin University Network Lab 黄连恩的 Depot 系统设计：

```
TenMillionArticles (.dat) ──► Parser (GBK→UTF-8) ──► Store (zlib压缩) ──► Indexer (37-shard索引)
                                                                              │
                                                                              ▼
                                                              QueryEngine (mmap+二分查找)
                                                                              │
                                                                              ▼
                                                              HTTP Server ──► 浏览器回放
```

### 索引设计

- **37 个 shard**，按域名 hash 分配（XOR hostname chars mod 37）
- **v2 格式**：URL 字符串池嵌入 shard 文件末尾，搜索和浏览无需读取数据文件
- **HostBlock 嵌入式索引**：每个 shard 头部包含排序的域名块，二分查找 O(log H)
- **排序方式**：`(host, url_hash, crawl_date DESC)` — 同域名条目聚集，同URL多版本连续
- **全部查找 O(log N)**：mmap 文件 + 二分查找，1400万文章亚毫秒级响应

### 服务器特性 (v2)

- **4 线程池**：std::thread + 条件变量，支持并发访问
- **gzip 压缩**：响应 >1KB 自动压缩（节省 60-76% 带宽）
- **HTTP 缓存**：ETag、Last-Modified、Cache-Control、304 Not Modified
- **Keep-Alive 支持**：持久连接，减少连接建立开销
- **速率限制**：每 IP 30 次请求 / 5 秒滑动窗口，超出返回 429
- **结构化日志**：`[timestamp] [LEVEL] message` 格式，支持 INFO/WARN/ERROR
- **CRC-32 数据完整性**：数据文件和索引文件均带 CRC-32 校验

### 路由

| 路由 | 说明 |
|------|------|
| `/` | 首页，统计卡片、年份柱状图、域名排行榜、历史上的今天 |
| `/search?q=` | 输入 URL 前缀或域名片段搜索 |
| `/replay?url=` | 查看历史网页内容，支持按日期选择版本 |
| `/proxy?url=` | 代理模式：返回原始存档内容，自动设置对应 Content-Type（根据 URL 扩展名） |
| `/calendar?url=` | CSS 时间线 + 列表视图，含统计信息 |
| `/diff?url=&a=日期&b=日期` | 段落级 diff，新增/删除高亮 |
| `/host?h=域名` | 统计卡片、年份分布图、页面列表 |
| `/sitemap?h=域名` | URL 路径目录树 |
| `/browse?d=YYYYMMDD` | 查看某天所有存档页面 |
| `/random` | 跳转到随机存档页面 |
| `/stats-page` | HTML 统计页：年份表、域名排行、服务器信息 |
| `/stats` | JSON 格式统计数据 API |

## 快速开始

### 环境要求

- macOS / Linux
- C++17 编译器 (clang++ 或 g++)
- zlib, iconv, pthread

### 编译

```bash
cd src && make
```

### 加载样例数据（~1000 篇文章，2.8MB）

```bash
./src/load ./sample_data . --files 0    # 加载 sample_data/dat0
```

### 增量加载

```bash
./src/load ./data_source . --incremental   # 仅加载新文件，跳过已处理的
```

`--incremental` 标记会检查 archive 目录中的进度标记，只处理尚未加载的数据文件。

### 启动回放服务器

```bash
./src/serve data index 8088
# 浏览器打开 http://localhost:8088
```

### 加载全量数据（1400万文章）

```bash
# 将 TenMillionArticles/dat/ 放到 data_source/ 目录下
./src/load ./data_source . --all   # 约 40 分钟
```

### 数据完整性校验

```bash
./verify <archive_dir>     # 扫描 archive 目录，校验所有数据文件和索引文件的 CRC-32
```

### 性能基准测试

```bash
./bench <data_dir> <index_dir> [N]   # N 为随机查询次数，默认 10000
```

bench 工具从索引中随机抽取 URL 进行查询，测试端到端响应延迟并输出百分位统计。

### Docker 部署

```bash
docker-compose up --build
```

这将构建 C++ Phase 2 系统镜像并启动服务，映射端口 8088，数据目录挂载为 `./data`。

也可直接构建运行：

```bash
docker build -t web-infomall .
docker run -p 8088:8088 -v /path/to/data:/data web-infomall
```

## 使用方式

### /proxy 代理模式

`/proxy?url=<原始URL>` 路由返回存档页面的原始内容（标题 + 正文），并自动根据 URL 扩展名设置 Content-Type（如 `.html` → `text/html`，`.jpg` → `image/jpeg`）。适用于嵌入 iframe 或 API 调用。

### 速率限制

服务器对每个客户端 IP 实施速率限制：5 秒滑动窗口内最多 30 次请求。超出限制返回 HTTP 429（Too Many Requests）以及 `Retry-After` 头部。速率限制状态基于内存计数器，服务重启后重置。

### 结构化日志

服务器所有日志输出采用统一格式：

```
[2026-06-25 10:30:15] [INFO] Listening on port 8088
[2026-06-25 10:30:16] [INFO] 192.168.1.100 "GET /search?q=example.com" 200 12453 23ms
[2026-06-25 10:30:17] [WARN] 192.168.1.101 rate limited (32/30)
[2026-06-25 10:30:20] [ERROR] shard 17: crc mismatch, expected 0xA3B2C1D0 got 0xE5F6A7B8
```

## 工具链

| 命令 | 说明 |
|------|------|
| `./src/load` | 数据加载流水线，支持 `--all`、`--files`、`--max`、`--incremental` |
| `./src/serve` | 多线程 HTTP 回放服务器（线程池、gzip、缓存、限流） |
| `./verify` | 数据完整性校验工具，扫描所有文件的 CRC-32 |
| `./bench` | 性能基准测试，随机查询延迟百分位统计 |
| `make test` | 解析器冒烟测试（10 篇文章） |

## 文件说明

```
src/
  common.h         数据结构定义 (ArticleRecord, UrlIndexEntry, HostBlock)
  parser.cpp       TenMillionArticles 解析器, GBK→UTF-8
  store.cpp        附加式二进制存储 (zlib压缩, YYYYMM目录, CRC-32校验)
  indexer.cpp      37-shard 排序索引构建器 (v2: URL池)
  query.cpp        查询引擎 (mmap, 二分查找, 零IO搜索)
  server.cpp       多线程 HTTP 回放服务器 (线程池, gzip, 缓存, 限流, 日志)
  load.cpp         数据加载流水线 (支持增量加载)
  test_parser.cpp  解析器独立测试
  test_store.cpp   存储写入独立测试
  Makefile

templates/         Jinja2 参考模板（实际 HTML 内嵌在 C++ 源码中）

Python原型/ (Phase 1 — 已弃用):
  parser.py        备用 Python 解析器
  store.py         SQLite 存储 (小规模验证用)
  server.py        Python HTTP 服务器
  load_data.py     数据加载脚本
  test_server.py   服务端冒烟测试

  *** Python 原型已弃用，仅保留作参考。所有生产用途请使用 C++ Phase 2 系统。 ***
```

## Shard 文件布局 (v2)

```
ShardFileHeader (16 bytes)              — magic=0x49445821, counts
HostBlock[host_count] (40 bytes each)   — 排序的域名块
UrlIndexEntry[entry_count] (28 bytes)   — 排序的 (host, url_hash, date DESC)
char url_pool[url_pool_size]            — 拼接的 URL 字符串，entry 通过 offset 指向
```

URL 直接嵌入索引文件后，搜索前缀和列出域名页面完全不需要访问数据文件。

## 存储格式

数据文件按 `YYYYMM/` 目录组织，每个文件包含完整的 CRC-32 校验和以检测数据损坏。文件在 2GB 边界分割，编号为 `data_NNNN.dat`。正文使用 zlib 压缩（压缩节省 >5% 时才启用）。

## 参考

- `TWFormat.pdf` — 天网格式规范
- `Depot/` — 原始 C++ Depot 实现（黄连恩，2001-2007）
- `task_infomall.md` — 项目目标

## License

数据使用需遵循 [CWIRF 使用许可](http://www.cwirf.org/licence.pdf)。
