# Phase 1 Python 原型 —— 已归档，不再维护

**这个目录不参与交接闸门，也没有人为它的正确性负责。**
生产用途一律使用 C++ Phase 2 系统（`src/`）。

人拍板（2026-08-07，见 `collab/PLAN.md` Decision Log）：保留但显式归档。
在此之前它处于三种可能里最差的一种——**在仓库里、没人跑、README 又说它已弃用**：
读到代码的人以为它是活的，读到 README 的人以为它是死的，而闸门两边都不管。
归档不是为了让它变好，是为了让「它是死的」这件事在目录层面成立，
而不是靠一句藏在 README 第 193 行的话。

## 它是什么

Phase 1 验证原型：Python + SQLite，用来在写 C++ 之前确认数据格式与回放思路可行。

| 文件 | 作用 |
|------|------|
| `parser.py` | `ArticleParser` —— 流式 `.dat` 解析，GB18030→UTF-8（用 Python codecs） |
| `store.py` | `ArchiveStore` —— SQLite 存储，WAL 模式、URL 哈希索引、版本追踪 |
| `server.py` | `ReplayHandler` —— 标准库 `HTTPServer`，路由 `/`、`/search`、`/replay`、`/calendar`、`/stats` |
| `load_data.py` | 加载流水线，组合 `ArticleParser` + `ArchiveStore` |
| `test_parser.py` | 解析与多文件加载回归测试 |
| `test_server.py` | 服务端冒烟测试 |

## 还想跑它

从**本目录内**运行（模块之间是同级导入，换目录会 `ImportError`）：

```bash
cd prototype
python3 load_data.py                 # 只加载 dat0（约 118K 篇）
python3 server.py                    # http://localhost:5000
python3 -m unittest test_parser
python3 test_server.py
```

跑通过一次（2026-08-07 归档时）：`test_parser` 4 项全过。
**但这不构成任何承诺**——闸门不跑它，所以它随时可能因为别处的改动而失效，
而且失效时没有任何机制会告诉你。

## 想改它

先去 `collab/PLAN.md` 提一条任务并由人拍板。
如果结论是「重新维护」，那第一步是把它加回 `tools/handoff.py` 的 `run_verify()`，
否则「维护」只是口头的。
