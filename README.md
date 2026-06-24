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

## 使用方式

| 操作 | 路由 | 说明 |
|------|------|------|
| 首页 | `/` | 统计卡片、年份柱状图、域名排行榜、历史上的今天 |
| 搜索 | `/search?q=` | 输入 URL 前缀或域名片段搜索 |
| 回放 | `/replay?url=` | 查看历史网页内容，支持按日期选择版本 |
| 版本历史 | `/calendar?url=` | CSS 时间线 + 列表视图，含统计信息 |
| 版本对比 | `/diff?url=&a=日期&b=日期` | 段落级 diff，新增/删除高亮 |
| 域名概览 | `/host?h=域名` | 统计卡片、年份分布图、页面列表 |
| 站点地图 | `/sitemap?h=域名` | URL 路径目录树 |
| 按日期浏览 | `/browse?d=YYYYMMDD` | 查看某天所有存档页面 |
| 随机浏览 | `/random` | 跳转到随机存档页面 |
| 统计 | `/stats-page` | HTML 统计页：年份表、域名排行、服务器信息 |
| 统计 API | `/stats` | JSON 格式统计数据 |

## 文件说明

```
src/
  common.h         数据结构定义 (ArticleRecord, UrlIndexEntry, HostBlock)
  parser.cpp       TenMillionArticles 解析器, GBK→UTF-8
  store.cpp        附加式二进制存储 (zlib压缩, YYYYMM目录)
  indexer.cpp      37-shard 排序索引构建器 (v2: URL池)
  query.cpp        查询引擎 (mmap, 二分查找, 零IO搜索)
  server.cpp       多线程 HTTP 回放服务器 (线程池, gzip, 缓存)
  load.cpp         数据加载流水线
  test_parser.cpp  解析器独立测试
  test_store.cpp   存储写入独立测试
  Makefile

templates/         Jinja2 参考模板（实际 HTML 内嵌在 C++ 源码中）

Python原型/ (Phase 1):
  parser.py        备用 Python 解析器
  store.py         SQLite 存储 (小规模验证用)
  server.py        Python HTTP 服务器
  load_data.py     数据加载脚本
  test_server.py   服务端冒烟测试
```

## Shard 文件布局 (v2)

```
ShardFileHeader (16 bytes)              — magic=0x49445821, counts
HostBlock[host_count] (40 bytes each)   — 排序的域名块
UrlIndexEntry[entry_count] (28 bytes)   — 排序的 (host, url_hash, date DESC)
char url_pool[url_pool_size]            — 拼接的 URL 字符串，entry 通过 offset 指向
```

URL 直接嵌入索引文件后，搜索前缀和列出域名页面完全不需要访问数据文件。

## 参考

- `TWFormat.pdf` — 天网格式规范
- `Depot/` — 原始 C++ Depot 实现（黄连恩，2001-2007）
- `task_infomall.md` — 项目目标

## License

数据使用需遵循 [CWIRF 使用许可](http://www.cwirf.org/licence.pdf)。
