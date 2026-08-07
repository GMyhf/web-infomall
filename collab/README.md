# 协作脚手架 · Claude ⇄ Codex

两个 AI（Claude Code 与 Codex）不能靠"记忆"协作，只能靠**共享事实源**交接。
这个目录就是那层事实源：谁都能读、谁都能写、每一轮都留下书面痕迹。

## 文件职责

| 文件 | 作用 | 谁写 |
| --- | --- | --- |
| `PLAN.md` | 唯一任务清单 + 决策记录（Decision Log） | 人拍板；两个 agent 更新状态 |
| `HANDOFF.md` | 交接日志：每一次「我做完了，轮到你」都追加一条 | 交接方 |
| `NOTES-claude.md` | Claude 留给 Codex 的话（改了什么、哪里没把握） | 只有 Claude |
| `NOTES-codex.md` | Codex 留给 Claude 的话（审查意见、发现的问题） | 只有 Codex |
| `review-input.md` | 脚本自动生成的 review 包（**不入库**） | `tools/handoff.py` |

> `git` 是最硬的桥梁，测试是最硬的仲裁。文档负责「为什么」和「接下来」，
> 代码与测试负责「是什么」。冲突时，能跑通验证的方案胜出。
>
> 本项目当前的自动化仲裁是 `make -C src test`：解析器、核心回归（存储/索引/查询/
> 损坏数据）、加载器 checkpoint 与增量安全、C++ HTTP 端到端（分帧/gzip/缓存/流水线）
> 四套。`python3 tools/handoff.py --verify` 会先跑它，再做 Python 语法检查和原型回归。

## 一轮标准循环

```
1. 人：把目标写进 collab/PLAN.md（Backlog 里加一条任务）
2. 实现方（如 Claude）：
     - 认领任务 → 改 PLAN.md 状态为 In progress，署名
     - 实现 → `python3 tools/handoff.py --verify` → git commit（小步、清晰 message）
     - 写 NOTES-claude.md：做了什么 / 哪里没把握 / 想让对方重点看哪里
     - 追加一条 HANDOFF.md 交接记录
     - 运行 python3 tools/handoff.py --from claude --to codex
3. 人：把生成的 collab/review-input.md 交给 Codex（或让 Codex 直接读仓库）
4. 审查方（Codex）：
     - 读 review-input.md → 审查 / 挑 bug / 写会失败的测试
     - 把意见写进 NOTES-codex.md；能直接修的就修 + commit
     - 追加一条 HANDOFF.md 交接记录，轮回给 Claude
5. 实现方：git pull → 看对方 commit 与 NOTES → 继续迭代
6. 验证全绿 + 双方无异议 → 在 PLAN.md 标 Done，写进 Decision Log（如有决策）
```

## 协作模式（按需选）

- **生成 ↔ 审查**：一方写实现，另一方交叉审查。不同模型盲点不同，能抓到单模型漏掉的问题。
- **规划 ↔ 执行**：一方拆任务写 PLAN，另一方逐条实现，偏差写回 NOTES。
- **红队 / 对抗**：关键逻辑（加载器 checkpoint、索引校验、HTTP 解析、回放沙箱）
  由另一方专门找茬、写会失败的测试。本项目最值得对抗的是**损坏输入**：
  截断的 `.dat`、被改坏的 `.idx`、畸形 HTTP 请求。
- **分工并行**：按模块切分（load / query / server），各用 git 分支或 `git worktree` 隔离。

## 硬约束（避免互相覆盖）

- 开工前先在 `PLAN.md` 认领任务并署名；**不要两个 agent 同时改同一文件的同一段**。
  `src/server.cpp`（11 万行级单文件）尤其容易撞车，改之前先在 PLAN 里说清动哪一段。
- 小步提交、清晰 commit message，审查方才看得懂 diff。
- 交接格式统一走 `HANDOFF.md` 模板，减少人工搬运。
- **交回时必须附一次真正跑完的验证结果**：`--verify` 的输出必须包含各步骤的尾部计数
  （`make test` 各套测试的通过行、`unittest` 的 `OK` 行）。不接受「我觉得没问题」。
- **性能声明必须有数**：改查询或索引后说「更快了」，要附 `./src/bench` 的百分位输出，
  并写明归档规模。凭感觉的性能结论一律视为未验证。
- **拿夹具下的测量去下生产结论前，先证明夹具在那一维上有代表性。**
  出处：2026-08-07，Claude 在 `sample_data/dat0` 上量到「一次加载 2178 次 fsync、耗时 45 秒」，
  据此判定「全量 1400 万篇的 fsync 代价按记录数增长」，开了 T-010 并标为待人拍板。
  实测推翻：`dat0` 是**摘选样本**，相邻记录换月率 **99.0%**；而 `dat111`（真实 `datN`）
  是 **0.2%**（54 次切换 / 55 个唯一月份 —— 真实数据按日期排好序）。
  真实全量加载的 fsync 数正比于**月份数**，不是记录数。**警报本身是夹具的形状造成的。**
  同一条纪律当轮就救回第二次：T-007 说「历法非法日期在聚合视图里消失」，
  一测才发现 32,288 条真实日期里**非法 0 条**，且 `record_count` 其实**包含**它们，
  只有日期派生字段排除——两处描述都比事实夸张。
  **做法**：结论里出现「全量 / 生产 / 会随规模增长」这类词时，先问「我量的这份数据，
  在我关心的这一维上和真实数据一样吗」，并把答案连同数字一起写进交接记录。
- **变异自检的两端都要确认：变异真的生效了，以及跑完真的撤销了。**
  出处（生效端）：同日，Claude 跑「去掉状态复位」的变异得出「没被抓住」，实为正则没匹配、
  文件根本没改。**一个没生效的变异和一个抓不住的变异，输出一模一样。**
  出处（撤销端）：同日复核 T-013 时，Claude 的备份文件名写成 `q.bak`、还原时找 `query.cpp.bak`，
  于是两条变异之间没还原，**第二条叠在第一条上面**，结论不可信。
  **做法**：用 `git diff --quiet <file>` 判定「已变异 / 已还原」，用 `git checkout -- <file>` 还原，
  不要自己管 `.bak` 文件；未变异就打「⚠️ 变异未生效，结果无意义」，跑完必须确认工作区回到基线。
- **交付后回来销账：任务落地时，把它回答掉的「未决 / 待拍板 / TODO」逐条改成带出处的已决记录。**
  保留原问题、注明最终取值与代码出处，不要删除，让来回可查。
  两个 agent 每轮都读这些文档，一份多数已决的待办清单会让人重开已经关掉的方向。

## 本项目红线（审查时必查）

1. **归档写入的原子性与耐久性**：所有输出文件（`meta.dat`、`year_dist.dat`、`today.dat`、
   `title_idx.dat`、`checkpoint.dat`、shard `.idx`）必须先写 `.tmp`，再 `fflush` + `fsync` +
   `rename`，并 fsync 父目录；`fwrite`/`fclose` 返回值必须检查。半截文件绝不能被当成有效归档。
2. **checkpoint 语义（CP3）**：源文件在首次写入前标记 `tainted`，数据与全部索引落盘 fsync
   后才升 `completed`。**被中断的源不会被自动重放**——重放等于重复追加，会静默污染归档，
   要恢复只能重建。`--incremental` 必须要求有效 checkpoint；非增量必须拒绝写入非空归档；
   `flock`（`<archive>/.load.lock`）互斥不得移除。
3. **索引是不可信输入**：`.idx` 是磁盘上的字节，可能被截断、被并发写坏、来自旧版本。
   `MappedShard::open` 的头部校验、布局大小、逐条 URL 边界、HostBlock 有序性、哈希/日期排序
   一条都不能放宽；`entry_in_pool` 必须对每条 entry 生效。损坏的索引应当**干净拒绝**，
   不是越界读。v1 shard（magic `IDX `）的回退路径同样要走校验。
4. **回放沙箱**：归档正文是 1991–2017 年真实抓取的网页，带脚本、外链、跟踪像素。
   `/proxy` 必须保持 `Content-Security-Policy: sandbox`；页面必须保持严格 CSP、
   `X-Frame-Options: DENY`、`X-Content-Type-Options: nosniff`。
   **不得新增以本服务源直出归档 HTML 的路径。**
5. **HTTP 解析与 DoS 面**：仅允许 GET（其余 405 + `Allow: GET`）；请求行与头部严格校验；
   HTTP/1.1 必须带且仅带一个 Host；请求头超时 408、过大 413、线程池队列满 503；
   30 秒绝对响应超时；每 IP 30 次 / 5 秒滑动窗口限流。改 `server.cpp` 不得放宽任何一条。
   重定向 `Location` 的 CR/LF 注入检查必须保留。
6. **输入解析**：URL、域名、日期（`YYYYMMDD`，`/browse` 兼容 `YYYY-MM-DD`）走统一的严格
   解析函数。历史上 `atoi`/`sscanf` 已被整批替换掉，不要再引回来。
7. **线程安全**：4 worker 共享 QueryEngine，mmap 只读。新增的共享状态必须问「谁加锁」；
   `ArticleReader` 的 fd 缓存锁、日志锁不得移除（这两处都是修过的真实数据竞争）。
8. **编码**：GB18030 优先，GBK/GB2312 回退；畸形序列输出 U+FFFD 且**不截断后续文本**；
   内部一律 UTF-8。编码回归是本项目最容易悄悄坏掉、又最难从页面上看出来的一类。
9. **数据不入库**：`data/`、`index/`、`archive/`、`*.db` 均在 `.gitignore`。
   `sample_data/` 是刻意入库的小样本（`dat0` 2.8MB、`dat111` 33MB），闸门依赖它——
   不要删，也不要往里加新的大文件。
10. **零第三方依赖**：C++ 侧只用 C++17 标准库 + zlib + iconv + pthread；Python 侧只用标准库。
    引入任何库或框架属于架构决策，必须先在 PLAN 里由人拍板。

## 生成 review 包

```bash
python3 tools/handoff.py --from claude --to codex          # 默认：未提交改动 or 最近一次提交
python3 tools/handoff.py --from claude --to codex --base main   # main..HEAD 的全部改动
python3 tools/handoff.py --from codex --to claude --range HEAD~3..HEAD --verify
python3 tools/handoff.py --verify                          # 只跑闸门，退出码即结论
```

生成 `collab/review-input.md`：包含改动摘要、changed files、完整 diff、交接方 NOTES、
PLAN 里的未决项，以及一份针对本项目的 review 检查清单。把这个文件喂给另一方即可。
