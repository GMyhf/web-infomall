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
- **HostBlock 嵌入式索引**：每个 shard 头部包含排序的域名块，二分查找 O(log H)
- **排序方式**：`(host, url_hash, crawl_date DESC)` — 同域名条目聚集，同URL多版本连续
- **全部查找 O(log N)**：mmap 文件 + 二分查找，1400万文章亚毫秒级响应

## 快速开始

### 环境要求

- macOS / Linux
- C++17 编译器 (clang++ 或 g++)
- zlib, iconv

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

| 操作 | 说明 |
|------|------|
| 搜索域名 | 输入 `sina` 匹配所有含 "sina" 的域名 |
| 浏览域名 | 输入完整域名如 `dailynews.sina.com.cn` 查看该域名下所有页面 |
| 查看页面 | 点击 URL 链接回放历史网页内容 |
| 版本历史 | 点击 "查看所有版本" 浏览同一URL的不同时间快照 |

## 文件说明

```
src/
  common.h      数据结构定义 (ArticleRecord, UrlIndexEntry, HostBlock)
  parser.cpp    TenMillionArticles 解析器, GBK→UTF-8
  store.cpp     附加式二进制存储 (zlib压缩, YYYYMM目录)
  indexer.cpp   37-shard 排序索引构建器
  query.cpp     查询引擎 (mmap, 二分查找)
  server.cpp    纯 POSIX HTTP 回放服务器
  load.cpp      数据加载流水线
  Makefile

Python原型/ (Phase 1):
  parser.py     备用 Python 解析器
  store.py      SQLite 存储 (小规模验证用)
  server.py     Python HTTP 服务器
```

## 参考

- `TWFormat.pdf` — 天网格式规范
- `Depot/` — 原始 C++ Depot 实现（黄连恩，2001-2007）
- `task_infomall.md` — 项目目标

## License

数据使用需遵循 [CWIRF 使用许可](http://www.cwirf.org/licence.pdf)。
