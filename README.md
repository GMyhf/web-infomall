# Web InfoMall — 中国网页信息博物馆 回放系统

基于北京大学网络实验室 [Web InfoMall](http://www.infomall.cn/) 技术架构的历史网页回放系统，类似 archive.org / Wayback Machine。

## 数据来源

[1000万新闻网页数据集](https://www.cwirf.org/) — 从中国Web信息博物馆保存的历史网页中摘选，涵盖1991-2017年间超过1400万篇新闻。

## 系统架构

参考 Peking University Network Lab 黄连恩的 Depot 系统设计：

```
TenMillionArticles (.dat) ──► Parser (GB18030→UTF-8) ──► Store (zlib压缩) ──► Indexer (37-shard索引)
                                                                              │
                                                                              ▼
                                                              QueryEngine (mmap+二分查找)
                                                                              │
                                                                              ▼
                                                              HTTP Server ──► 浏览器回放
```

### 索引设计

- **37 个 shard**，按域名字节逐次 XOR、循环移位后 mod 37 分配
- **v2 格式**：URL 字符串池嵌入 shard 文件末尾，搜索和浏览无需读取数据文件
- **HostBlock 嵌入式索引**：每个 shard 头部包含排序的域名块，二分查找 O(log H)
- **排序方式**：`(host, url_hash, crawl_date DESC)` — 同域名条目聚集，同URL多版本连续
- **精确查找 O(log N)**：mmap 文件 + 二分查找；URL 前缀/域名片段搜索为受限线性扫描

### 服务器特性 (v2)

- **4 线程池**：std::thread + 条件变量，支持并发访问
- **gzip 压缩**：客户端接受 gzip 时，对 >1KB 且压缩后至少节省 5% 的响应启用
- **HTTP 缓存**：ETag、Last-Modified、Cache-Control、304 Not Modified
- **Keep-Alive 支持**：持久连接，减少连接建立开销
- **速率限制**：每 IP 30 次请求 / 5 秒滑动窗口，超出返回 429
- **结构化日志**：`[timestamp] [LEVEL] message` 格式，级别为 DBG/INF/WRN/ERR
- **完整性校验**：文章记录使用 CRC-32；索引启动时校验布局、边界、排序和 URL 池切片

### 路由

| 路由 | 说明 |
|------|------|
| `/` | 首页，统计卡片、年份柱状图、域名排行榜、历史上的今天 |
| `/search?q=` | 输入 URL 前缀或域名片段搜索 |
| `/replay?url=<URL>[&date=YYYYMMDD]` | 查看历史网页内容，支持按日期选择版本 |
| `/topic?q=` | 按标题关键词回溯热点事件 |
| `/proxy?url=<URL>[&date=YYYYMMDD]` | 返回指定版本的存档正文，按 URL 扩展名设置 Content-Type，并用 CSP sandbox 隔离 |
| `/calendar?url=` | CSS 时间线 + 列表视图，含统计信息 |
| `/diff?url=&a=日期&b=日期` | 段落级 diff，新增/删除高亮 |
| `/host?h=域名` | 统计卡片、年份分布图、页面列表 |
| `/sitemap?h=域名` | URL 路径目录树 |
| `/browse?d=YYYYMMDD` | 查看某天所有存档页面 |
| `/random` | 跳转到随机存档页面 |
| `/stats-page` | HTML 统计页：年份表、域名排行、服务器信息 |
| `/stats` | JSON 格式统计数据 API |
| `/ping` | 纯文本健康检查 |

## 快速开始

### 环境要求

- macOS / Linux
- C++17 编译器 (clang++ 或 g++)
- zlib, iconv, pthread
- Python 3（测试套件及保留的原型需要）
- curl（仅 `update.sh` 重启后的健康检查需要）

### 编译

```bash
make -C src
```

### 加载样例数据（~1000 篇文章，2.8MB）

```bash
./src/load ./sample_data ./archive --files 0
```

### 增量加载

```bash
./src/load ./data_source ./archive --incremental
```

`--incremental` 会发现 `datN` 文件并读取 CP3 checkpoint，只处理尚未完成的文件。每个源文件会在追加前标记为 `tainted`，数据与全部索引发布并同步成功后才转为 `completed`；中断或截断的源不会被自动重放，完整加载它们需要重建归档。没有有效 checkpoint 的旧归档无法确定已加载源文件，必须重建到新的空归档。非增量模式会拒绝写入非空归档。

`--max N` 用于在新归档上做有限条数验证。它会留下可查询但不完整的 `tainted` 源文件状态，不能从文件中间继续增量加载；完整导入该源应使用新的空归档。

### 启动回放服务器

```bash
./src/serve archive/data archive/index 8088
# 浏览器打开 http://localhost:8088

# 直接对外暴露（没有反向代理）时，必须关掉对 X-Forwarded-For 的信任：
./src/serve archive/data archive/index 8088 --trusted-proxy-hops 0
```

### 加载全量数据（1400万文章）

```bash
# 将 TenMillionArticles/dat/ 放到 data_source/ 目录下
./src/load ./data_source ./archive --all   # 自动发现全部 datN
```

### 数据完整性校验

```bash
./src/verify <archive_dir>
```

### 性能基准测试

```bash
./src/bench <data_dir> <index_dir> [N]   # N 为随机查询次数，默认 10000
```

bench 工具从索引中随机抽取 URL 进行查询，测试端到端响应延迟并输出百分位统计。

### Docker 部署

```bash
docker compose up --build
```

这会构建 C++ Phase 2 镜像并启动服务，映射端口 8088，并将包含 `data/` 和 `index/` 的 `./archive` 以只读方式挂载到容器的 `/archive`。

也可直接构建运行：

```bash
docker build -t web-infomall .
docker run -p 8088:8088 -v /path/to/archive:/archive:ro web-infomall
```

## 使用方式

### /proxy 代理模式

`/proxy?url=<原始URL>[&date=YYYYMMDD]` 返回指定版本（未给日期时为最新版本）的存档正文，并根据 URL 扩展名设置 Content-Type。HTML 响应带严格 CSP sandbox，不会以本服务的权限执行归档脚本或向外部站点加载资源。

### 速率限制

服务器对每个客户端 IP 实施速率限制：5 秒滑动窗口内最多 30 次请求。超出限制返回 HTTP 429（Too Many Requests）以及 `Retry-After` 头部。速率限制状态基于内存计数器，服务重启后重置。

#### 客户端 IP 从哪里来：`--trusted-proxy-hops N`

`N` 是本服务前面**你自己运维的**反向代理数量，取值 0–8，**默认 1**。

限流键从 `[X-Forwarded-For 各条目（左→右）, TCP peer]` 这条链的**右端**往左数 `N` 位取得。
从右端数是关键：最左那条由客户端书写，谁都能伪造；从右端数意味着伪造的前缀只会被跳过。
XFF 缺失、条目数少于 `N`、或该条目不是合法 IPv4（IPv6 字面量、`unknown`、乱码）时，
一律回落到内核观测到的 TCP peer 地址。

| 部署形态 | 取值 | 后果 |
|---|---|---|
| 直接暴露（`./src/serve` 对公网） | **必须 0** | 用默认值 1 时，任何人发一个伪造的 `X-Forwarded-For` 就能每次请求换一个限流桶——**限流等于关闭** |
| 恰好一层自有反向代理（如 nginx / CDN） | 1（默认） | 正确：所有客户端不再塌缩成代理那一个 IP |
| 两层自有代理（如 CDN → nginx） | 2 | — |

`N > 0` 时服务器启动会打印一条 `[WRN]` 提醒这个风险；`N = 0` 时打印键为 peer 地址。
取值非法（负数、超过 8、非整数）会**拒绝启动**，不会静默截断——
这个参数配错的表现是限流悄悄失效，所以它不该被容忍。

### 结构化日志

服务器所有日志输出采用统一格式：

```
[2026-06-25 10:30:16] [INF] GET /search?q=example.com -> 200 (23.0ms)
[2026-06-25 10:30:17] [WRN] Rate limit exceeded for 192.168.1.101
[2026-06-25 10:30:20] [ERR] Unhandled request exception: allocation failed
```

## 工具链

| 命令 | 说明 |
|------|------|
| `./src/load` | 数据加载流水线，支持 `--all`、`--files`、`--max`、`--incremental` |
| `./src/serve` | 多线程 HTTP 回放服务器（线程池、gzip、缓存、限流、`--trusted-proxy-hops`） |
| `./src/verify` | 校验 shard 布局、记录 CRC-32 和精确 data 文件交叉引用 |
| `./src/bench` | 性能基准测试，随机查询延迟百分位统计 |
| `cd src && make test` | 解析器、核心逻辑、增量 checkpoint、C++ HTTP、`verify` 与小样本 `bench` 回归套件 |
| `python3 tools/handoff.py --verify` | 交接闸门：跑上面全套回归 + Python 语法检查（`prototype/` 不在闸门内） |
| `python3 tools/handoff.py --from claude --to codex` | 生成 `collab/review-input.md` 交给另一方 AI 审查 |

## 协作方式（Claude ⇄ Codex）

本仓库用 `collab/` 目录作为两个 AI agent 的共享事实源：`PLAN.md`（唯一任务清单与决策记录）、
`HANDOFF.md`（交接日志）、`NOTES-claude.md` / `NOTES-codex.md`（双方留言）。
一轮标准循环、协作模式与**本项目红线清单**见 [`collab/README.md`](collab/README.md)。

交回代码时必须附一次真正跑完的 `python3 tools/handoff.py --verify` 结果（含各步尾部计数）；
性能声明必须附 `./src/bench` 的百分位输出与归档规模。

## 文件说明

```
src/
  common.h         数据结构定义 (ArticleRecord, UrlIndexEntry, HostBlock)
  parser.cpp       TenMillionArticles 解析器, GB18030→UTF-8
  store.cpp        附加式二进制存储 (zlib压缩, YYYYMM目录, CRC-32校验)
  indexer.cpp      37-shard 排序索引构建器 (v2: URL池)
  query.cpp        查询引擎 (mmap, 二分查找, v2 索引内搜索无需读取正文文件)
  server.cpp       多线程 HTTP 回放服务器 (线程池, gzip, 缓存, 限流, 日志)
  load.cpp         数据加载流水线 (支持增量加载)
  test_parser.cpp  解析器独立测试
  test_store.cpp   存储写入独立测试
  test_core.cpp    存储、索引、查询、损坏数据与长域名回归测试
  test_http_server.py  C++ HTTP framing、gzip、缓存与流水线端到端测试
  test_load_checkpoint.sh  加载器 checkpoint 与增量安全回归测试
  Makefile

templates/         Jinja2 参考模板（实际 HTML 内嵌在 C++ 源码中）

prototype/         Phase 1 Python 原型，**已归档、不维护、不参与闸门**
  README.md        归档说明与运行方式（人拍板 2026-08-07）
  parser.py        备用 Python 解析器
  store.py         SQLite 存储 (小规模验证用)
  server.py        Python HTTP 服务器
  load_data.py     数据加载脚本
  test_parser.py   Python 解析与加载回归测试
  test_server.py   服务端冒烟测试

  *** 所有生产用途请使用 C++ Phase 2 系统。prototype/ 的失效不会被任何机制发现。 ***
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

数据文件按 `YYYYMM/` 目录组织，每条文章记录包含 CRC-32。文件在 2GB 边界分割，编号为 `data_NNNN.dat`；索引会保存准确的文件序号。正文使用 zlib 压缩（压缩节省 >5% 时才启用），单条存储记录上限为 64 MiB，解压正文上限为 256 MiB。

## License

数据使用需遵循 [CWIRF 使用许可](http://www.cwirf.org/licence.pdf)。
