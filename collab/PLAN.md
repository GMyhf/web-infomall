# PLAN · 唯一任务清单与决策记录

> 这是 Claude 与 Codex 共享的**唯一任务事实源**。人拍板任务与优先级；
> 两个 agent 认领任务、更新状态、署名。状态流转：`Backlog → In progress → Review → Done`。
> 每条任务用一个 `T-<编号>` 标识，交接与提交信息里引用它。
>
> **格式硬约束**：每行恰好 5 列；追加复核结论写进备注列、用 ` — ` 分隔，**不要**用 `| `
> 另起一格——超出表头的单元格在 GitHub 渲染时被直接丢弃，人看到的会是另一份 PLAN。
> 描述里含 `|` 的代码片段要转义（`\|`），否则会把行切碎。

## 状态看板

| ID | 任务 | 状态 | 负责 | 关联提交 / 备注 |
| --- | --- | --- | --- | --- |
| T-000 | 搭建 Claude⇄Codex 协作脚手架（本目录 + `tools/handoff.py`） | Done | Claude | 移植自 `cs101.openjudge.cn/collab`，红线清单与闸门按本项目改写。闸门实测通过：core 89 项、加载器 checkpoint、C++ HTTP 回归三套全绿 |
| T-001 | **闸门进 CI**：仓库没有 `.github/`，`make -C src test` 只在本地跑，绿不绿全靠自觉。加一个 GitHub Actions（Ubuntu + g++ + zlib）跑 `python3 tools/handoff.py --verify`，并确认它在**全新 clone** 上成立（当前依赖已入库的 `sample_data/dat0`，应当成立，但没人验证过） | Review | Claude | `.github/workflows/gate.yml`：push/PR/手动触发跑同一条 `--verify`，CI 与本地不会分叉。**全新 clone 已实测**：`git clone` 到空目录跑闸门退出码 0（89 core checks + checkpoint + HTTP + 4 原型测试）。顺带堵掉一个静默漏洞——`sample_data/dat0` 缺失时原型回归会**无声跳过**，现在会打印 ⚠️ 并留在输出里。**CI 已实跑并绿**（run `31161679268` @ `447715f`，g++ 13.3.0 / Python 3.12.3 / zlib 1.3）。**未验到**：CI 的红色路径没在 runner 上演示过，见 HANDOFF。待 Codex 复核 |
| T-002 | **给 `99d97d3` 放宽的日期校验补回归测试**：该提交删掉了 `MappedShard::open` 里对 `valid_crawl_date` 的拒绝（`src/query.cpp:93` 附近），理由是历史快照里存在 `20030229` 这类历法非法值。改动有理有据，但**没有任何测试钉住新行为**：现在既没有「含 20030229 的 shard 能被服务」的正向用例，也没有「其它结构性损坏仍被拒绝」的反向用例。红线 3 被放宽了一格，而这一格是无人看守的 | Backlog | **Codex（人指派，待认领）** | 出处：`99d97d3`、`src/query.cpp` v1 fallback 三处（387/615/750 行附近）。注释声称「integrity verification reports these values separately」——已核实属实，见 `src/verify.cpp:423`。**任务书见 `NOTES-claude.md` 顶部**：含三条已侦察事实（现有 9 条日期断言完全感知不到这次放宽；仓库无「构造合法 v2 shard」的夹具，这是工作量大头；`verify` 兜底自身无测试）+ 建议用例 + 变异自检硬要求 |
| T-003 | **`verify` 与 `bench` 进闸门**：`make test` 的依赖是 `test_parse test_core load serve`，`verify` 和 `bench` 只被 `all` 构建、从不被执行。给样例归档跑一次 `./src/verify` 作为端到端一致性检查（索引↔数据交叉核对是它唯一的自动化出口） | Backlog | 待认领 | 顺带确认 `verify` 的「数据文件中存在但索引未引用的记录」检测在样例归档上不误报 |
| T-004 | **Python 原型（Phase 1）的去留** —— 需人拍板。现状：README 明写「已弃用，仅保留作参考」，但 `parser.py`/`store.py`/`server.py`/`load_data.py` 连同 `test_parser.py`/`test_server.py` 都还在仓库里，且**不在 `make test` 闸门内**。三选一：①删除；②保留并正式纳入闸门（当前 `--verify` 已跑 `test_parser`，`test_server.py` 未纳入）；③保留并显式标注不维护、从闸门排除 | Review | Claude | **人已拍板：选 ③ 显式归档**（2026-08-07，见 Decision Log）。落地为 `prototype/` 目录 + 该目录自带 README 说明不维护，并从闸门排除 |
| T-005 | **反向代理后的限流口径** —— 需人拍板。限流键取自 `getpeername` 的 `sin_addr`（`src/server.cpp:1923–1926`），代码中**没有任何 `X-Forwarded-For` / `X-Real-IP` 处理**。直接暴露时这是正确的（不可伪造）；但 `docker-compose.yml` 与任何反代部署下，所有客户端塌缩成一个源 IP，「每 IP 30 次 / 5 秒」实际变成**全站 30 次 / 5 秒**。要么明确「本服务只应直接暴露」，要么引入可配置的可信代理跳数 | Review | Claude | **人已拍板：可配置可信代理跳数，默认 1**（2026-08-07，见 Decision Log，含代价）。落地为 `--trusted-proxy-hops N`，从链条**右端**按跳数取客户端 IP（不是取最左，最左由客户端完全控制） |
| T-006 | **红队一轮：损坏输入对抗**。本项目最该被对抗的不是逻辑分支，是**坏字节**：截断的 `.dat`、位翻转的 `.idx`、`host_count` 与文件大小对不上的头部、越界的 `url_offset`、畸形 HTTP 请求。由未实现的一方专门写「应当干净拒绝、实际越界读或崩溃」的用例 | Backlog | 待认领 | 现有 `test_core.cpp` 已覆盖部分损坏索引场景，从它没覆盖的地方切入 |

## Decision Log

- 2026-08-07 · **人拍板（T-005）：引入可配置的可信代理跳数 `--trusted-proxy-hops N`，
  默认 1。** 出处：`src/server.cpp`（`resolve_client_key` / `g_trusted_proxy_hops`）。
  取值 0–8，超范围或非整数**拒绝启动**（不静默截断——这个参数配错的表现是限流悄悄失效）。
  限流键从 `[XFF 各条目（左→右）, TCP peer]` 链的**右端**往左数 N 位取；
  **从右端数是这条设计的全部意义**：最左那条由客户端书写，取最左等于把钥匙交给攻击者，
  取右端意味着伪造的前缀只会被跳过。XFF 缺失、链短于 N、条目非合法 IPv4 时回落 peer。

  **代价明写（Claude 提出，人已确认仍取默认 1）**：默认值 1 意味着**开箱即信任一跳 XFF**。
  README 的默认启动方式 `./src/serve …` 是直接暴露的——在那种部署下，任何人发一个伪造的
  `X-Forwarded-For` 就能每次请求换一个限流桶，**限流等于关闭**。
  三条对冲，都已落地：①`N>0` 时启动打一条 `[WRN]` 说明该风险；②README 用一张表写明
  「直接暴露必须 0」；③`Dockerfile` 的 `CMD` 显式传 `0`（该镜像自己发布 8088、
  compose 直接映射到宿主，前面没有任何代理，默认 1 在那里只剩「允许伪造」这一个效果）。
  **仍然挂着的口子**：裸跑 `./src/serve` 而不加 `--trusted-proxy-hops 0` 的人，
  只会收到一条 WRN 日志，不会被拦。要彻底关掉这个口子只能改默认值为 0，那是人的选择。

- 2026-08-07 · **人拍板（T-004）：Phase 1 Python 原型选 ③ —— 保留、显式归档、不维护、
  从闸门排除。** 落地：`prototype/` 目录 + `prototype/README.md` 写明不维护，
  `tools/handoff.py` 的 `python_sources()` 不含该目录，闸门既不跑它的回归也不做它的语法检查。
  **排除本身在闸门输出里留一行 `ℹ️`**——一个「不跑」的决定和一个「忘了跑」的事故在输出里
  长得一模一样，前者是决策、后者是缺陷，不写出来半年后没人分得清。
  归档前的状态是三种可能里最差的一种：在仓库里、没人跑、README 又说它已弃用——
  读代码的人以为它活着，读 README 的人以为它死了，闸门两边都不管。
  **代价**：`prototype/` 从此会静默腐坏，且腐坏时没有任何机制报告。这是归档的定义，不是缺陷。
  归档时它是绿的（`test_parser` 4 项全过）。想重新维护，先在本文件提任务由人拍板，
  第一步必须是把它加回 `run_verify()`，否则「维护」只是口头的。

- 2026-08-07 · **Claude 建档：引入 `collab/` 协作脚手架与 `tools/handoff.py` 闸门。**
  移植自 `cs101.openjudge.cn/collab` 的既成实践，不是新的架构决策，因此没有走人拍板。
  相对源实现做了一处改动并说明理由：源版本的 `run_verify` 在步骤成功时只打印 `✅ ok`，
  而它自己的 README 要求「交回时必须附一次真正跑完的验证结果，输出必须包含全套测试尾部计数」——
  **一个 `✅` 证明不了跑过多少**。本版成功时保留每步尾部 12 行，让 `make test` 的
  `PASS: 89 core checks` 这类真实计数进入交接记录。
  ~~闸门 = `make -C src test` → `py_compile` 全部 `.py` → Python 原型 `test_parser`（样例数据在位时）。~~
  **销账（2026-08-07，同日 T-004 落地后）**：原型步骤已随 `prototype/` 一起移出闸门。
  当前闸门 = `make -C src test` → `py_compile`（`prototype/` 除外），排除在输出里留一行 `ℹ️`。

- 2026-07-13 · **回填的既有决策（非当场拍板）：`MappedShard::open` 对历法非法的
  `crawl_date` 放行。** 出处 `99d97d3` / `src/query.cpp`。原因：历史快照里存在 `20030229`
  这类值，在这里拒绝会让「结构上完全安全」的历史索引无法服务。
  **代价明写**：红线 3「索引是不可信输入」在日期这一维上被放宽了一格——该字段仍受数值边界
  和排序约束，但不再受历法约束。对冲是 `verify` 工具单独报告这些值（`src/verify.cpp:423`，
  已核实）。**当前没有测试钉住这条**，见 T-002。

- 长期 · **零第三方依赖**（出处：`README.md` 工具链一节、`CLAUDE.md` 环境要求）。
  C++ 侧只用 C++17 标准库 + zlib + iconv + pthread；Python 侧只用标准库。
  引入任何库或框架属于架构决策，必须先在本文件里由人拍板。
