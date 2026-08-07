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
| T-001 | **闸门进 CI**：仓库没有 `.github/`，`make -C src test` 只在本地跑，绿不绿全靠自觉。加一个 GitHub Actions（Ubuntu + g++ + zlib）跑 `python3 tools/handoff.py --verify`，并确认它在**全新 clone** 上成立（当前依赖已入库的 `sample_data/dat0`，应当成立，但没人验证过） | Done | Claude | `.github/workflows/gate.yml`：push/PR/手动触发跑同一条 `--verify`，CI 与本地不会分叉。**全新 clone 已实测**：`git clone` 到空目录跑闸门退出码 0（89 core checks + checkpoint + HTTP + 4 原型测试）。顺带堵掉一个静默漏洞——`sample_data/dat0` 缺失时原型回归会**无声跳过**，现在会打印 ⚠️ 并留在输出里。**CI 已实跑并绿**（run `31161679268` @ `447715f`，g++ 13.3.0 / Python 3.12.3 / zlib 1.3）。**Codex 复核通过**：workflow 从干净 checkout 在 push/PR/手动触发中调用唯一的闸门定义；依赖安装和并发取消配置合理。CI 故意红一次仍未演示，因它是需人批准的对外仓库动作，而非实现缺陷 |
| T-002 | **给 `99d97d3` 放宽的日期校验补回归测试**：该提交删掉了 `MappedShard::open` 里对 `valid_crawl_date` 的拒绝（`src/query.cpp:93` 附近），理由是历史快照里存在 `20030229` 这类历法非法值。改动有理有据，但**没有任何测试钉住新行为**：现在既没有「含 20030229 的 shard 能被服务」的正向用例，也没有「其它结构性损坏仍被拒绝」的反向用例。红线 3 被放宽了一格，而这一格是无人看守的 | Done | Codex | `src/test_core.cpp` 现有按真实 v2 布局写 shard 的夹具；`20030229` 可 `open()`、`QueryEngine::init()`、取页和列版本，且日期原样返回。覆盖 URL pool 越界、host/entry 排序、host 范围和 v2 精确布局拒绝。日期拒绝与 URL-pool 检查均做变异自检并变红；`verify` 对该告警的端到端测试留给 T-003（它负责将 verify 纳入闸门） — **Claude 复核通过**：独立复跑 4 条变异（恢复日期拒绝 / `entry_in_pool` 恒真 / 去掉 v2 精确 EOF / 去掉 host 有序性）全部变红，Codex 记的「恢复日期拒绝 → 5 条断言红」逐字对上。**补了一条弱断言**：`CHECK(legacy_page.valid)` 近乎恒真（`Article::valid` 默认 true、语义是「完整性 OK」而非「找到了」，`get_page` 每条早退都返回 `{}` 即 valid==true），它是变异下唯一没红的一条；已加 `legacy_page.url == legacy_url` 与生产调用方的守卫方式对齐。112 → 113 项 |
| T-003 | **`verify` 与 `bench` 进闸门**：`make test` 的依赖是 `test_parse test_core load serve`，`verify` 和 `bench` 只被 `all` 构建、从不被执行。给样例归档跑一次 `./src/verify` 作为端到端一致性检查（索引↔数据交叉核对是它唯一的自动化出口） | Done | Codex | `test_archive_tools.py` 用临时样例 archive 验证 `verify` 成功、`bench` 输出 QPS；并篡改一条 index 日期为 `20030229`，断言 `verify` 非零退出且打印无效日期告警。`make test` 现构建并运行两工具 — **Claude 复核通过**：独立复跑两条变异（索引侧日期告警恒真 / 改掉成功文案）均变红；Python 里手写的二进制偏移逐项对过 `common.h`（头部四个 uint32、HostBlock 40 字节、`url_hash` uint64 故 `crawl_date` 在 offset 8）全部正确。`bench` 只断言输出 QPS、不把百分位当闸门条件——**这个判断是对的**，把噪声性能数据设成正确性门槛会造出一个偶尔发红的闸门，而偶尔发红正是让真红被忽略的机制。**补了记录侧的另一半**：`verify` 有两处日期检查，本轮只钉住索引侧（`verify.cpp:423`），记录侧（`:320`）实测可达且先于 CRC 触发，已加测试并变异自检通过 |
| T-004 | **Python 原型（Phase 1）的去留** —— 需人拍板。现状：README 明写「已弃用，仅保留作参考」，但 `parser.py`/`store.py`/`server.py`/`load_data.py` 连同 `test_parser.py`/`test_server.py` 都还在仓库里，且**不在 `make test` 闸门内**。三选一：①删除；②保留并正式纳入闸门（当前 `--verify` 已跑 `test_parser`，`test_server.py` 未纳入）；③保留并显式标注不维护、从闸门排除 | Done | Claude | **人已拍板：选 ③ 显式归档**（2026-08-07，见 Decision Log）。落地为 `prototype/` 目录 + 该目录自带 README 说明不维护，并从闸门排除 — **Codex 复核通过**（排除留痕与决策一致） |
| T-005 | **反向代理后的限流口径** —— 需人拍板。限流键取自 `getpeername` 的 `sin_addr`（`src/server.cpp:1923–1926`），代码中**没有任何 `X-Forwarded-For` / `X-Real-IP` 处理**。直接暴露时这是正确的（不可伪造）；但 `docker-compose.yml` 与任何反代部署下，所有客户端塌缩成一个源 IP，「每 IP 30 次 / 5 秒」实际变成**全站 30 次 / 5 秒**。要么明确「本服务只应直接暴露」，要么引入可配置的可信代理跳数 | Done | Claude | **人已拍板：可配置可信代理跳数，默认 1**（2026-08-07，见 Decision Log，含代价）。落地为 `--trusted-proxy-hops N`，从链条**右端**按跳数取客户端 IP（不是取最左，最左由客户端完全控制） — **Codex 复核通过**，并**修正了 Claude 的一处自相矛盾**：README 部署表把 `docker compose` 直映射列为「一层可信反代 → 1」，而同一轮 Dockerfile 又显式传 0；已改为 nginx/CDN 等真实反代示例（`c0fedf9`） |
| T-006 | **红队一轮：损坏输入对抗**。本项目最该被对抗的不是逻辑分支，是**坏字节**：截断的 `.dat`、位翻转的 `.idx`、`host_count` 与文件大小对不上的头部、越界的 `url_offset`、畸形 HTTP 请求。由未实现的一方专门写「应当干净拒绝、实际越界读或崩溃」的用例 | Done | Codex | `test_core.cpp` 现对一个合法 v2 shard 的每个真前缀都断言 `open()` 拒绝且内部状态复位；另覆盖非法 magic、零 URL 长度和 ArticleReader 的坏 magic、声明长度截断、段长度不一致。v2 magic 最低位翻转恰为受支持 v1 magic，属兼容等价例外，改以另一单比特翻转验证非法 magic — **Claude 复核通过**：`test_core` 只要 0.9 秒，476 项不贵。独立变异确认（去掉 shard magic 校验 → 红；允许 `url_len == 0` → 红）。「翻转 v2 magic 最低位会变成受支持的 v1 magic」是真发现。**补强了状态复位断言**：原断言只查 `fd`/`data`/`header` 三个字段，而 `fail()` 复位八个——漏掉的 `hosts`/`entries`/`url_pool` 恰恰是指向已 munmap 映射的指针。更关键的是**截断循环钉不住它们**：绝大多数前缀在这些字段被赋值前就失败了，所以复位断言必须加在**后期失败**用例上（排序/越界/尾随字节）。已加，四个字段逐一变异全红。476 → 481 项 |
| T-007 | **历法非法日期的「半可见」状态：没人写下来，也没有测试钉住任何一边**。T-002 证明了 `20030229` 可 `open()`、可 `get_page()`/`get_versions()`；但同一条目被 `get_host_summary`（`query.cpp:581`）、`get_stats`（`:678`）、`get_year_distribution_slow`（`:772`）三处**静默跳过**——它在单页回放里存在，在域名统计、全局日期范围和年份柱状图里不存在。跳过历法非法日期在直方图里是合理的，问题是**这个分叉没有出处、没有测试**：将来任一侧被改动都不会有人发现 | Done | Claude | Claude 复核 T-002 时顺着放宽这条线查出来的。先决定要哪种语义（统一计入 / 统一排除 / 维持现状但写下来），再补测试。**不要**在没拍板前改行为 — **人拍板 2026-08-07：维持现状，写下来并两侧各补测试。** 依据见 Decision Log：32,288 条真实日期里历法非法 **0 条**；且复核时发现**原描述夸张了**——`record_count`/`unique_url_count`/`total_articles` 其实**包含**这类条目，只有 `date_min`/`date_max`/`year_counts`/年份柱状图排除。现有语义已经是有原则的那种：**计入「它存在」，只在需要真实日历日的地方排除**。已加 `test_invalid_date_aggregate_visibility`（`test_core.cpp`）两半都钉住，四条变异（三处守卫各去掉一次 + 把 `record_count` 改成排除）全部变红。481 → 495 项 |
| T-008 | **CI 的红色路径从没在 runner 上演示过**。已验证的只有「闸门失败时 `--verify` 退出码非零」（本地变异自检）+ workflow 最后一步就是那条命令；剩下半步靠 GitHub「步骤非零退出即失败」这条标准行为。按本仓库自己的标准（从没红过的闸门不能证明自己会红），该补：开一个故意弄红的临时 PR 看它失败，再关掉 | Done | Claude | 从 T-001 的 Done 备注里提出来单列。**理由**：Done 行是任务停止被阅读的地方，一条挂在 Done 行里的欠账等于没记。Codex 复核 T-001 时已正确判定它属于需人批准的对外动作、而非实现缺陷——这里只是让它保持可见 — **人已点头，已演示，2026-08-07。** 做法**不开 PR**：建一次性分支 `ci-redpath-check`、故意把 `is_leap_year(2000)` 断言反过来、`gh workflow run --ref` 触发，run `31184266349` **失败**。runner 上的真实输出：`FAIL line 170: !is_leap_year(2000)` ／ `FAIL: 1 of 481 core checks failed` ／ `=== 闸门 ❌ 失败 ===` ／ `##[error]Process completed with exit code 1`。分支已删除（本地 + 远端），仓库里只留下那条失败运行记录——**那条记录就是证据本身**。至此「闸门会红」不再是从退出码推断的，是看着它红过一次 |
| T-009 | **`verify` 把三条不同的不变量塞进同一条错误信息**。`verify.cpp:320` 的复合条件同时管 payload 大小不符、日期非法、offset 溢出，三者共用 `ERROR: Invalid record fields at offset N`。操作员在 1400 万篇的归档上看到这条，无法判断是哪一类。**这一条在 T-002/T-003 之后变得更要紧**：人已拍板「历法非法日期属于可接受的历史数据」，而现在它和真正的结构性损坏共用同一条告警——把良性数据和真损坏混成同一个信号 | Done | Codex | 已拆为独立 offset、payload、date 诊断；date 是 `WARNING` 但仍计作验证失败。`test_archive_tools.py` 分别钉住日期和 payload 文本，变异自检均变红；offset 分支保留独立路径，不在 CI 构造多 GiB 稀疏输入 — **Claude 复核通过**：三条诊断已拆开，`offset` 检查前移到 payload 之前（它是前置条件，位置对）。独立变异三条（记录侧级别漂移 / 索引侧级别漂移 / 去掉 payload 检查）全部变红。**修了一处措辞与行为矛盾**：新加的记录侧日期分支写 `WARNING:`，却 `structural_errors++` + `break` + 退出码 1，而 `scan_data_file` 里它的每一个兄弟分支都是 `ERROR:`——本文件的两级体系是 `ERROR ↔ structural_errors ↔ break`（数据侧）/ `WARNING ↔ bad_entry_refs ↔ continue`（索引侧），它是唯一的例外。已按 Codex 自己注释里的判断改为 `ERROR:`（行为未动），并在测试里**钉住两侧的级别前缀**，否则级别可以静默漂移——这正是它这次跑偏的方式。`offset > UINT32_MAX` 不测是对的且不是覆盖债：`MAX_DAT_FILE` 为 2GB、`file_offset` 是 uint32，`load` 产出的归档到不了该分支，要触发须手工造突破写入侧上限的文件 |
| T-010 | **加载器的 fsync 次数由「月份切换」驱动，而不是由数据量驱动**。`DataStore::append`（`store.cpp:94-101`）只保持一个打开的数据文件：文章月份一变就 `close_current()` —— fflush + fsync(文件) + fclose + **fsync(目录)**。而 `.dat` 里文章日期是乱序的，所以几乎每条记录都换月。实测：样例 dat0（1000 篇、2.9MB、跨 111 个月份目录）一次 `load` 耗 **45 秒**，`strace -c` 显示 **2178 次 fsync、占系统调用时间 99.4%**（≈ 2 × 1089 次月份切换），而 user CPU 只有个位数秒。`verify`/`bench` 各 0.1 秒作对照 | Done | Claude | Claude 复核 T-006 时量出来的。**两个后果**：①闸门 `load` 跑 5 次 ≈ 5 分钟里的绝大部分（已把 `test_archive_tools.py` 的 3 次压成 1 次，省约 100 秒，见 `503ab20` 之后）；②**全量 14M 篇如果同样乱序，这个代价不是常数而是按记录数增长**——没人跑过全量加载，所以从没暴露。**不要用「少 fsync 几次」来修**：红线 1 与 CP3 语义要求的是「数据与全部索引落盘 fsync 后才把源从 tainted 提升为 completed」，那是**一次加载结束时**的契约，不是每次文件切换的契约。可行方向是按月保持多个打开句柄（带上限）、在 `finish()` 时统一 fsync，契约不变而 fsync 从 2178 降到约 222。**这是耐久性/崩溃窗口的取舍，必须人拍板** — **人拍板 2026-08-07：关闭，不动 `store.cpp`。原判断有误，实测推翻。** `dat0` 是**摘选样本**，相邻换月率 **99.0%**（989/999，111 个唯一月份）；`dat111`（真实 `datN`）是 **0.2%**（54 次切换、55 个唯一月份——切换数恰好等于唯一月份数减一，说明真实数据按日期排好序）。**真实全量加载的 fsync 数正比于月份数，不是记录数**，耐久性设计没有问题。残留成本纯属夹具形状：`test_archive_tools.py` 3 次 load 压成 1 次（166s→67s）、`test_http_server.py` 改用 `--max 100`（60s→17s，该套测的是 HTTP 协议、与文章数无关）。`test_load_checkpoint.sh` **不动**——它的多次 load 正是它测试的对象。闸门 367s → 约 100s。教训已写进 `collab/README.md` 硬约束 |
| T-011 | **所有加载器/查询测试只见过一种数据形状**。实测：`dat0` 有 **442 个 host、最大 host 仅 47 条**（宽而浅）；`dat111`（真实 `datN`）有 **2 个 host、最大 25,920 条**（窄而深）。而本系统的核心是**在 host block 内部二分查找**——至今被测过的最大 block 只有 47 条（二分深度 ~5.6 层）。把真实形状的夹具接进闸门 | Done（结论：不实施） | Claude | 承接 T-010 写进硬约束的那条教训。**`dat111` 不能直接当干净夹具**：它是「dat110 的前半段」，按构造从记录中间截断（Python 数到 31,288 条 `time=`，加载器报 31,287 篇 + 一条截断尾），整文件加载**退出码 2、1 tainted**——那是**正确行为**，不是缺陷。做法：测试期截到记录边界取前 6000 条（2.6s、单 host 6000 条、退出码 0、`1 completed`），不新增入库数据 — **实测推翻了这条任务自己的前提，故不实施。** 五条二分查找变异，逐条比较浅/深两种形状：block 内 `hi=mid-1` → 浅 126 / 深 172；block 内少一轮 → 浅 136 / 深 176；block 内 `lo=1` → 浅 245 / **深 0（深夹具是瞎的）**；host 搜索 `hi=mid-1` → 两者都崩；host 搜索 `lo=1` → 浅 63 / 深 500。**没有任何一条是深夹具抓到而浅夹具漏掉的**，C 那条还反过来。给闸门加 2.6 秒却不增加覆盖，不做。**这次调查真正的产物是 T-012** —— 找不到可断言的东西时才发现 `bench` 根本不判命中。`dat111` 按构造从记录中间截断（整文件加载退出码 2、1 tainted，是正确行为），这一点也记下来，免得下次有人当缺陷报 |
| T-012 | **`bench` 分不清命中和未命中**。`bench.cpp:105` 是 `(void)result;`——它从索引里生成 N 个 URL 再查回去，却从不检查是否查到。**二分查找若坏掉，每次全 miss，它会报出更高的 QPS 而毫无提示**：一个正确性 bug 会被报成提速。而 `bench` 已经在闸门里（T-003），一直只对着 `dat0` 那个浅归档跑 | Done | Claude | Claude 做 T-011 时发现——本想用 `bench` 当深 block 的正确性断言，才发现它没有可断言的东西。修法：统计命中（`result.url == 请求 URL`）、进统计表、**有任何 miss 即非零退出**。这样 bench 的数字才以正确性为前提 — **已实施，且证明它抓到了现有 495 项完全看不见的缺陷。** `bench.cpp` 现在统计`result.url != 请求 URL` 的次数、进统计表、有 miss 即非零退出。**证据**：block 内二分查找的两条变异（`hi=mid-1`、循环少一轮）下 `test_core` **495 项全绿**，而 bench 报出 126/500 与 136/500 misses。闸门里 bench 查询数 32 → 300。**顺带修掉我自己写的一条空断言**：只断言「Lookup misses = 0」是无效的——摘掉计数器后 misses 恒为 0、那行照打、断言照过。已加**阳性对照**`check_bench_counts_misses`：删掉数据文件后索引仍在，每次查找都必然 miss，bench 必须计数并非零退出。四条变异全红（摘计数器 / 去掉非零退出 / 二分 A / 二分 B）|
| T-013 | **block 内二分查找目前只有概率性覆盖**。`find_first`（`common.h:288`）是整条查询路径的核心，而 `test_core.cpp` 里手搭的 host block **最大只有 2 条 entry**（见 `make_host_block` 全部 6 处调用）。实测两条变异——`hi = mid - 1`、`while (lo + 1 < hi)`——在 **495 项全绿**下通过，只有 `bench` 的随机抽样能抓到（126/500、136/500）。**随机抽样是概率兜底，不是覆盖**：换个种子、换个夹具规模，它就可能漏。用 `write_v2_shard` 造一个几百条 entry 的 block，对**每一条**做确定性查找 | Review | Codex | 新增可复用的 `write_deep_v2_shard`，造单 host 的 88-entry v2 shard（64 个不同 URL + 同 URL 24 个版本）。对所有 64 个 URL 做真实 `get_page()`，另钉住未命中与 `get_versions()` 的完整、降序版本段。64 条提供 6 层二分路径且直接逐项查询；24 个版本使返回等 hash 段中间的缺陷可见。两条指定变异均先以 `diff -q` 确认真的改到 `find_first`，随后 `test_core` 分别红 22 / 23 条。495 → 676 项。未扩到 host 层：本任务有 block 内逃逸的实测证据，host 层没有；避免把不同搜索层的覆盖问题混进一个夹具 |


## Decision Log

- 2026-08-07 · **人拍板（T-010）：关闭，不修改 `store.cpp`。Claude 的原判断有误，实测推翻。**
  原判断：`DataStore` 每次换月 fsync 文件 + 目录，样例 `dat0` 一次 `load` 触发 **2178 次 fsync**、
  耗时 45 秒、占系统调用时间 99.4%（`strace -c` 实测），据此推断全量 1400 万篇的代价按记录数增长。
  **推翻依据**：`dat0` 是**摘选样本**，相邻记录换月率 **99.0%**（989/999，跨 111 个唯一月份）；
  而 `dat111`（真实 `datN` 前半段，31,288 条）换月率 **0.2%**——54 次切换、55 个唯一月份，
  切换数恰好等于唯一月份数减一，**说明真实数据按日期排好序**。
  所以真实加载的 fsync 数正比于**月份数**（1991–2017 约 320 个），不是记录数。
  **耐久性设计（红线 1、CP3 语义）保持原样，一个字不改。**
  残留成本纯属夹具形状，只在测试侧处理（见 T-010 备注）。
  **这次误判的教训已写成 `collab/README.md` 的硬约束**：拿夹具下的测量去下生产结论前，
  先证明夹具在那一维上有代表性。

- 2026-08-07 · **人拍板（T-007）：历法非法日期维持现有语义，写下来并两侧各补测试。**
  出处：`query.cpp` 的 `get_host_summary`（:581 附近）、`get_stats`（:678）、
  `get_year_distribution_slow`（:772）。**语义（现在正式记下来）**：
  这类条目**计入「它存在」**——`record_count`、`unique_url_count`、`total_articles` 都包含它；
  **只在需要真实日历日的地方排除**——`date_min`/`date_max`、`year_counts`、年份柱状图。
  这条规则本来就是实现在做的事，只是从没写下来，也没有任何一侧有测试。
  **依据**：32,288 条真实日期（`dat0` + `dat111`）里历法非法 **0 条**——
  该条件真实存在（`99d97d3` 修的是一个真的加载不了的归档，不是假想），但罕见到样本里一次未见；
  为一个 <1/32000 的条件改动聚合计数语义，风险大于收益。
  **复核时同时更正了 Claude 自己的夸张描述**：此前 NOTES/HANDOFF 说这类页面「在域名统计里不存在」，
  实际只有日期派生字段排除，计数一直是包含的。
  已由 `test_invalid_date_aggregate_visibility` 钉住两半，四条变异全部变红。

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
  已核实）。~~**当前没有测试钉住这条**，见 T-002。~~
  **销账（2026-08-07，T-002 落地后）**：已由 `src/test_core.cpp` 的
  `test_v2_shard_validation_and_legacy_dates` 钉住——真实 v2 夹具证明 `20030229`
  可 `open()` / `init()` / `get_page()` / `get_versions()` 且日期原样返回，
  同时 URL 池越界、host 与 entry 排序、host 范围、v2 精确 EOF 五项仍被拒绝。
  变异自检：恢复 `open()` 里的日期拒绝 → 6 条断言变红。
  **仍未钉住的那半边**：该条目在 `get_host_summary` / `get_stats` /
  `get_year_distribution_slow` 三处被静默跳过——见 T-007。
  `verify` 对无效日期的端到端告警也仍无测试，随 T-003 补。

- 长期 · **零第三方依赖**（出处：`README.md` 工具链一节、`CLAUDE.md` 环境要求）。
  C++ 侧只用 C++17 标准库 + zlib + iconv + pthread；Python 侧只用标准库。
  引入任何库或框架属于架构决策，必须先在本文件里由人拍板。
