# 更新日志 (Changelog)

本文件记录 Web InfoMall 回放系统的所有重要变更，按时间倒序排列。

## 2026-07-10 — 加载与 HTTP 服务加固 (`29d7469`)

一次以可靠性为主题的大规模加固（约 3700 行新增），覆盖加载器、HTTP 服务器、解析器、查询引擎，并新增多套回归测试。

### 加载器加固与检查点
- 新检查点格式 **CP3**：按数据源记录 tainted（写入中）/ completed（已发布）状态。数据源在首次写入前标记 tainted，数据、主索引、辅助索引全部落盘并 fsync 后才提升为 completed；中断的数据源不会被自动重放（避免重复追加），需全量重建。兼容读取旧格式检查点。
- 增量加载（`--incremental`）要求存在有效检查点；非增量加载拒绝写入非空归档目录。
- 通过 `flock`（`<archive>/.load.lock`）实现加载器互斥锁，拒绝并发加载。
- `--all` 改为自动扫描 dat 目录发现真实存在的 `datN` 文件（不再硬编码文件列表）；增量模式未指定文件时自动发现待加载文件。
- 严格参数校验：`--max`/`--files` 必须为非负整数，`--all` 与 `--files` 互斥，未知选项报错，指定的源文件缺失为硬错误。
- **原子且持久的写入**：所有输出文件（meta.dat、year_dist.dat、today.dat、title_idx.dat、checkpoint.dat、shard `.idx`）先写 `.tmp` 再 fflush + fsync + rename，并 fsync 父目录；所有 fwrite 返回值均被检查。
- 增量合并回读时校验辅助索引：边界检查、日期/URL 校验、要求精确 EOF，并与 meta.dat、主索引交叉核对，任何不一致在写入前中止。
- DataStore：新增 `finish()`（发布索引前 flush/close/fsync）；写入失败置粘性 `failed_` 标志；记录大小上限（单条 64 MiB、正文 256 MiB）；追加前校验磁盘上已有记录（magic/大小/日期/连续性）；`latest_file_sequence()` 正确续接月内 `data_NNNN.dat` 序号。
- IndexBuilderV2：拒绝合并畸形或非 v2 shard；条目级 URL/日期/大小/序号校验；超长 URL 报错而非静默截断；去重键扩展。
- 加载期逐篇文章校验（8 位日期、URL 合法性）；命中 `--max`、解析错误、字段非法或零有效文章的数据源标记 tainted；整轮无有效文章或有 tainted 源时以退出码 2 失败。
- `UrlIndexEntry.reserved` 字段正式用于存储 `data_NNNN.dat` 文件序号（0 = 旧索引，等同序号 1）。

### HTTP 服务器加固
- 重写请求解析：严格校验请求行（协议版本、路径、控制字符）、头部名/值；HTTP/1.1 必须带 Host 且拒绝重复 Host；拒绝带消息体的请求和冲突的 Content-Length/Transfer-Encoding；仅允许 GET（其余返回 405 + `Allow: GET`）。
- 规范的头部语义：按 token 解析 `Connection`；`Accept-Encoding` 支持 q 值和 `*` 通配；`If-None-Match` 按 RFC 弱标签比较。
- 慢客户端 / DoS 缓解：非阻塞发送 + 30s 绝对响应超时（poll）；读请求头超时返回 408；请求过大返回 413；线程池队列上限 256，队满返回 503；worker 循环 try/catch 防止异常杀死线程。
- 修复 keep-alive 流水线：连接级缓冲保留超出当前请求的字节，跨请求不再丢弃。
- 安全响应头：`X-Content-Type-Options: nosniff`、`Referrer-Policy: no-referrer`、`Vary: Accept-Encoding`；HTML 页面附带严格 CSP 和 `X-Frame-Options: DENY`；`/proxy` 使用 `Content-Security-Policy: sandbox`；重定向 Location 做 CR/LF 注入检查。
- 全路由输入校验：URL/域名/日期（`YYYYMMDD`，`/browse` 兼容 `YYYY-MM-DD`）严格解析，替换所有 `atoi`/`sscanf`；端口号校验 1–65535。
- 缓存正确性：ETag 基于内容哈希并区分 gzip 变体，统一 304 处理路径。
- 页面构建修复：`page_header()` 消除定长栈缓冲区截断并转义标题；sitemap 路径树与 scheme 无关（修复 http/https 混合域名可能的越界）；修复 `url_decode`/`url_encode`；日志加锁线程安全；`/topic` 空查询显示搜索表单；`inet_ntop` 替代 `inet_ntoa`。

### 解析器与公共库
- iconv 转换器改用 **GB18030**（GBK/GB2312 回退），E2BIG 时扩展输出缓冲、畸形序列输出 U+FFFD 且保留后续文本。
- 解析器分块消费改为非二次方复杂度；记录打开/读取失败、畸形记录、末尾截断记录的错误信息。
- `common.h` 新增校验工具：`valid_archive_url`、`valid_crawl_date`（真实历法/闰年校验）、大小写不敏感的 scheme 检查等；`compute_record_crc32` 改为增量计算（不再整条拷贝）；`entry_in_pool` 去掉 `pool_size==0` 跳过检查的漏洞。

### 查询引擎与校验工具
- `MappedShard::open` 打开时完整校验 mmap 的 shard：头部、布局大小、逐条 URL 边界、HostBlock 有序性、哈希/日期排序，拒绝服务不一致的索引。
- `read_article` 加固：大小上限、以磁盘上的 record_size 为准重读、载荷精确匹配；CRC/解压失败通过 `integrity_ok` 标记但保留数据用于诊断。
- `QueryEngine::init`：区分 ENOENT 与真实错误、仅统计非空 shard、零 shard 时失败、worker 启动前预计算缓存统计。
- 新增 `get_host_summary()`（记录数、唯一 URL 数、日期范围、逐年统计）供 host 页面使用。
- `verify` 工具：按文件序号交叉核对索引与数据记录；新增"数据文件中存在但索引未引用的记录"检测。

### 测试与工具链
- 新增 `src/test_core.cpp`（C++ 核心回归：校验、GB18030 转换、存储、索引、损坏索引处理）。
- 新增 `src/test_http_server.py`（真实服务器端到端协议测试：HTTP 分帧、gzip、缓存/ETag、流水线）。
- 新增 `src/test_load_checkpoint.sh`（加载器检查点/增量安全回归）。
- 新增 `test_parser.py`、重写 `test_server.py`（Python 原型回归）。
- Makefile：自动依赖生成（`-MMD -MP`）；`make test` 运行全部四套测试。
- Docker：改用 g++/libstdc++，卷改为只读 `/archive`；新增 `.dockerignore`。
- `update.sh` 重构：路径解析、参数校验、健康检查、按 PID+命令行匹配的重启逻辑。
- Python 原型：`--max` 改为跨文件全局上限；parser 默认编码 gb18030；store 兼容 `YYYY-MM-DD`。

## 2026-07-04 — 代码审计缺陷修复 (`634e8ba`)

修复代码审计发现的崩溃、并发和正确性缺陷。

### 崩溃 / 并发
- ArticleReader 改用 fd + `pread` 并对 fd 缓存加锁 — 修复 4 个 worker 线程竞争共享文件位置和无锁 map 的数据竞争。
- 忽略 SIGPIPE，客户端中途断开不再杀死整个服务进程。
- 修复 socket 双重 close 竞争（可能误关复用同一 fd 的无关连接）。
- `build_replay` 防护 `url.find(host)==npos`（大写域名的归档 URL 曾抛出 `std::out_of_range` 终止 worker）。

### 功能
- `/replay` 正确响应 `date` 参数 — 日历/时间线的版本链接真正回放所选历史版本。
- `/browse` 接受 `<input type=date>` 的 `YYYY-MM-DD` 格式（此前 atoi 只取年份，永不匹配）。
- `UrlIndexEntry.reserved` 存储 `data_NNNN.dat` 序号，单月超 2GB 拆分后的文件可读；旧索引兼容。

### 内存安全
- `read_article`：真实大小超过索引中 uint16 `record_size` 的记录（>64KB）重新读取，修复堆越界读；变长段边界检查；解压上限 256MB。

### HTTP 正确性 / 安全
- `write_all()` 循环替代裸 `write()`，大响应不再因部分写而截断。
- 修正状态短语（404 曾发送 "404 OK"）；302 补 `Content-Length: 0` 保证 keep-alive 正确。
- `/proxy` 原始归档 HTML 加 `Content-Security-Policy: sandbox` 和 `X-Content-Type-Options: nosniff`；`.ico` 返回 `image/x-icon`。
- RateLimiter 改为带周期性过期清扫的 unordered_map（此前超过 1024 个不同 IP 后静默失效）。
- `send_response` 去掉每响应的正文拷贝；gzip 级别 9 → 6。

## 2026-06-25 — 优化与增量索引合并 (`82dc029`, `5023cff`, `f9870c0`)

### 健壮性与并发
- 年份分布缓存的懒加载加锁，消除 4 worker 线程间的数据竞争。
- 限流器滑动窗口环形缓冲区扩容至 32 槽，计数不再被覆写破坏。
- CRC/解压失败通过 `Article.valid` 暴露：损坏记录返回 404 而非渲染乱码，且不进缓存。
- URL 池访问边界检查（`entry_in_pool`），防御损坏的索引。

### 性能
- 多词标题 AND 搜索改用哈希集合（O(n·m) 代替 O(n·m·k)）。
- `get_top_hosts` 用 `partial_sort` 取 top-k 代替全排序。
- replay 正文按 const 引用绑定，消除数 MB 拷贝。

### 增量索引合并
- `IndexBuilderV2::load_existing()` 读回已有 v2 shard；`build_shard()` 精确去重。
- `--incremental` 同时合并 year_dist/today/title_idx/meta — 增量加载扩展归档而非覆盖旧索引。

### 修复与工具
- **话题搜索改为 AND 逻辑**（`5023cff`）：多字 CJK 查询要求所有字出现在同一标题（"非典"从 14 条噪声结果降至 1 条精确结果）。
- indexer 防护 `file_offset`（uint32）和 `url_len`（uint16）截断；修正误导性 "MD5" 注释（`mini_md5` → `mini_hash`，实为 FNV-1a）。
- 跨平台 Makefile（Linux g++ / macOS clang++，自动探测 `~/.local` 下免 root 的 zlib）。
- 新增 `update.sh`：构建 → 加载（自动全量/增量）→ 重启服务，增量改写前备份索引。
- 新增样例数据 `sample_data/dat111`（`f9870c0`）：dat110 前半部分 31,287 页，用于演练增量加载路径。

## 2026-06-25 — Phase 2 v3：话题时间线、代理回放与工程化改造 (`e41b130`)

### 新功能
- **热点事件时间线** `/topic?q=关键词`：CJK 标题倒排索引、按时间排序的时间线视图、来源分布、报道热度图。
- **资源代理** `/proxy?url=...`：按 URL 扩展名推断 Content-Type 返回归档内容。
- HTML 链接改写：回放正文自动链接裸 URL；href/src/CSS `url()` 改写为经归档代理。
- 回放页横幅：日期、版本数、永久链接、原始内容链接。

### 性能与协议
- HTTP/1.1 keep-alive（单连接最多 10 请求，5s 超时）。
- 按 IP 限流（5 秒窗口 30 请求，超限返回 429）。
- 结构化日志 `[时间戳] [级别] 消息`；请求缓冲读循环（修复 8191 字节截断）。
- 每条记录 CRC-32 完整性校验（写时计算、读时验证）。
- 优雅停机（SIGINT/SIGTERM + accept 超时轮询）。

### 工程化
- 头文件拆分（parser.h/store.h/indexer.h/query.h），分离编译与增量构建。
- 增量加载（`--incremental` + checkpoint.dat）。
- 归档校验工具 `src/verify.cpp`（shard 结构、CRC32、索引↔数据交叉核对）。
- 查询基准工具 `src/bench.cpp`（P50/P95/P99 延迟）。
- Docker 部署（Dockerfile + docker-compose.yml）。

### 缺陷修复
- 修复 diff 页静态变量数据竞争；`clock()` 改为墙钟时间（原报告的是 CPU 时间）。
- 修复 `tolower` 有符号 char 未定义行为；新增写入失败检测（`StoredRecord.ok`）。
- Python Phase 1 模块标记为已弃用。

## 2026-06-24 — Phase 2 v2：线程池、gzip、缓存头与内嵌 URL 索引 (`0e48564`–`fc26b11`)

### 索引 v2 — URL 内嵌于 shard 文件
- `UrlIndexEntry` 增加 url_offset/url_len，指向 shard 文件末尾的 URL 字符串池（结构体保持 28 字节）。
- 前缀搜索和域名浏览直接从 mmap 读 URL，不再访问数据文件；保留 v1 shard 回退路径。
- SHARD_MAGIC 升至 `0x49445821`（v2）。

### 服务器
- 4-worker 线程池（`std::thread` + 条件变量）。
- gzip 响应压缩；ETag / Last-Modified / Cache-Control；`If-None-Match` 304 支持；美化 404 页。

### 新页面与路由（`25c38d7`, `875e8ed`, `3ebdb02`）
- `/host?h=域名` — 域名总览：统计卡片、年份分布图、去重 URL 列表。
- `/calendar` — CSS 时间线可视化 + 紧凑列表、统计行。
- `/replay` — 可折叠元数据面板、同域名页面推荐、前后版本对比链接。
- `/random`、`/sitemap?h=域名`、`/browse?d=日期`、`/diff?url=...&a=&b=`（段落级版本对比）。
- 首页：年份分布柱状图、Top 12 域名榜、历史上的今天、随机浏览入口。
- `/stats-page` — HTML 统计页（首页"查看完整统计"改指此页；`/stats` JSON API 不变）。
- QueryEngine 新增：`get_top_hosts`、`get_random_url`、`get_year_distribution`、`get_today_in_history`、`get_by_date`、`get_page_by_date`。

### 构建清理与文档
- 移除废弃的 IndexBuilder v1；测试拆分为独立文件；Makefile 加 `-lpthread` 与 macOS SDK 处理。
- 新增 CLAUDE.md（`0ce00d3`）；README 更新 v2 特性与全部路由（`fc26b11`）。

## 2026-06-16 — 初始版本 (`534ecd3`–`5e73745`)

- C++ 存储引擎：37-shard 排序索引（mmap + 二分查找）。
- GBK→UTF-8 解析器，支持 TenMillionArticles 数据集。
- 追加式二进制存储 + zlib 压缩。
- 纯 POSIX HTTP 回放服务器（零外部依赖）。
- Python 原型（Phase 1）；样例数据约 1000 篇文章；1400 万文章全量加载管线。
- 回放 UI 打磨与 UX 优化（`cf2095c`, `5e73745`）。
