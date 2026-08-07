# HANDOFF · 交接日志

<!-- 新交接追加在这条注释下方、最上面（倒序），模板见文件末尾 -->

### 2026-08-07 · Claude → Codex · T-011 否掉（实测推翻），T-012 落地

- **做了什么**：人让我按自己提议的方向做（真实形状 `dat111` 进闸门）。
  **实验否掉了这个提议（T-011 不实施）**，但过程中撞出 `bench` 从不检查命中，
  修掉它（T-012）——**它抓到了现有 495 项完全看不见的缺陷**。
- **改了哪些文件**：`src/bench.cpp`（miss 计数 + 非零退出）、`src/test_archive_tools.py`
  （阳性对照 + 查询数 32→300）、`collab/PLAN.md`、`collab/NOTES-claude.md`、`collab/HANDOFF.md`
- **验证**：闸门退出码 0，五套全绿。四条变异全红：摘掉 miss 计数器 / 去掉非零退出 /
  block 内 `hi=mid-1` / block 内循环少一轮。

- **T-011 否掉，理由是我自己的实验**：原提议是「深 host block（25,920 条）能测出浅夹具
  （最大 47 条）测不出的东西」。五条二分查找变异逐条比较，**没有一条是深夹具抓到、浅夹具漏掉的**；
  `lo=1` 那条还反过来——**深归档 0 misses，浅归档 245**（只跳过每个 block 的第 0 条，
  深归档就 1 个 block、随机抽样几乎抽不中）。**「更深 = 覆盖更多」是错的**，
  深浅只是在不同维度上敏感。加 2.6 秒买不到覆盖，不做。

- **T-012 才是这次的产物**：`bench.cpp:105` 原本是 `(void)result;`——从索引生成 URL 再查回去，
  却不检查有没有查到。**后果是方向相反的**：二分查找坏掉时全 miss，而 miss 比 hit 返回更早，
  于是 bench 报出**更高的 QPS**——**正确性 bug 被报成提速**。
  证据：block 内二分查找的两条变异下 `test_core` **495 项全绿**，bench 报 126/500、136/500。

- **请重点看，也是我这轮最该被记住的一条**：**我第一版的断言是空的。**
  只断言「`Lookup misses` 那行是 0」——摘掉计数器后 misses 恒为 0、那行照打、断言照过。
  我刚在 `collab/README.md` 写完「变异自检必须确认变异生效」，转头就写了一条不会红的断言。
  更糟的是接线也错了：那行被写在 `if ... raise` 块里、多缩进 4 格，
  **健康运行时条件为假、从来没执行过，且在 `raise` 之后本就是死代码**。
  两个错都是变异自检抓出来的——如果不是「摘掉计数器居然还绿」，我不会去看那行跑没跑。
  已加阳性对照 `check_bench_counts_misses`（删数据文件、留索引 → 必然 miss）。
  **一条你没见它红过的断言，你不知道它是不是死的。**

- **顺带确认一件事，免得下次有人当缺陷报**：`sample_data/dat111` 是「dat110 的前半段」，
  **按构造从记录中间截断**。整文件加载 → 退出码 2、`1 tainted`。**那是正确行为**
  （Python 数到 31,288 条 `time=`，加载器报 31,287 篇 + 一条截断尾，差值正好是 1）。

- **红线自检**：`bench` 是工具、非服务端/加载器路径 ✅ ｜ 索引校验未放宽 ✅ ｜
  checkpoint 语义未动 ✅ ｜ DoS 阈值未动 ✅ ｜ 归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅
- **下一步建议**：上表第一、二行值得你接着挖——**现有测试对 block 内二分查找的错误完全免疫**，
  现在靠 bench 的随机抽样兜着，那是概率性的。`test_core` 里用 `write_v2_shard` 造一个
  几百条 entry 的 block、对每一条做确定性查找，是把概率兜底换成确定性覆盖。

### 2026-08-07 · Claude → Codex · 人拍板三条全部收口；看板清空

**看板上已无 Backlog。** T-000 ~ T-010 全部 Done。下一轮要做什么得由人先立任务。

- **做了什么**：按人拍板收口 T-007、T-008、T-010，把两条教训写进 `collab/README.md` 硬约束，
  并把闸门从 6 分 7 秒降到约 100 秒。**未改动任何生产代码。**
- **改了哪些文件**：`src/test_core.cpp`（+T-007 用例）、`src/test_http_server.py`（夹具规模）、
  `collab/README.md`（+2 条硬约束）、`collab/PLAN.md`、`collab/NOTES-claude.md`、`collab/HANDOFF.md`
- **验证**：闸门退出码 0，`PASS: 495 core checks` ／ checkpoint ／ HTTP proxy-hops ／ archive tools。

- **T-010 关闭：我上一轮的判断是错的。** 我说「全量 fsync 代价按记录数增长」——
  实测推翻：`dat0` 相邻换月率 **99.0%**（摘选样本），`dat111`（真实 `datN`）**0.2%**，
  且换月次数恰好等于唯一月份数减一，**真实数据按日期排好序**。fsync 数正比于月份数，
  耐久性设计一字不改。**我的错不是算错数，是没问「我量的这份数据有代表性吗」就外推到生产。**

- **T-007 关闭：拍板前再测一次，又发现我夸张了。** 32,288 条真实日期里历法非法 **0 条**；
  而且我说的「在域名统计里不存在」不对——`record_count`/`unique_url_count`/`total_articles`
  **一直包含**这类条目，只有日期派生字段排除。现有语义本来就是有原则的那种：
  **计入「它存在」，只在需要真实日历日的地方排除**。行为不改，补测试钉住两半，
  四条变异全红（三处守卫各去一次 + `record_count` 改成排除）。

- **T-008 关闭：闸门真的红过一次了。** 不开 PR——一次性分支 + `gh workflow run --ref`，
  run `31184266349` 失败：`FAIL: 1 of 481 core checks failed` ／ `=== 闸门 ❌ 失败 ===` ／
  `##[error]Process completed with exit code 1`。分支已删，只留那条失败运行记录（它就是证据）。

- **`collab/README.md` 新增两条硬约束，请你也照做**：
  1. **拿夹具下的测量去下生产结论前，先证明夹具在那一维上有代表性**；
  2. **变异自检必须先 `diff -q` 确认变异真的生效**——没生效和没抓住，输出一模一样。
  两条都咬过我，不是预防性条款。

- **闸门提速**：`test_archive_tools.py` 三次 load 压成一次（166s→67s）、
  `test_http_server.py` 用 `--max 100`（60s→17s）。
  **`test_load_checkpoint.sh` 特意没动**——它的多次 `load` 正是它测试的对象。

- **红线自检**：未动生产代码 ✅ ｜ 耐久性/checkpoint 语义未动 ✅（T-010 是撤销我提的疑虑，不是改实现）｜
  索引校验未放宽 ✅ ｜ DoS 阈值未动 ✅ ｜ 归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅
- **下一步建议**：没有 Backlog 了。要继续得由人立新任务。
  若要我提议方向：`dat111`（31,287 篇，真实数据形状）至今没进过任何闸门用例，
  而它和 `dat0` 在日期分布上差两个数量级——**我们所有的加载器测试都只见过打乱的那一种**。

### 2026-08-07 · Claude → Codex · 复核 `3c38594`：T-006 认可；顺手量出 T-010

- **做了什么**：复核 T-006。**认可，T-006 → Done。** 补强了状态复位断言（476 → 481），
  把 `test_archive_tools.py` 的 3 次 `load` 压成 1 次（166s → 67s），并开了 T-010。
- **改了哪些文件**：`src/test_core.cpp`、`src/test_archive_tools.py`、`collab/PLAN.md`、
  `collab/NOTES-claude.md`、`collab/HANDOFF.md`。**未动生产代码。**
- **验证**：`test_core` 481 项通过；archive tools 通过。独立变异：去掉 shard magic 校验 → 红、
  允许 `url_len == 0` → 红；重构后把 T-009 的三条变异复跑一遍（记录侧级别漂移、
  去掉 payload 检查、去掉索引侧日期告警）**全部仍变红**。

- **那条 magic 注释是真发现**：「翻转 v2 magic 最低位 → 受支持的 v1 magic」，
  读代码读不出来，只有真动手做变异才会撞见。而且你**发现后换了另一个单比特变异**，
  没有绕开问题去挑个好过的——这一步比发现本身更值得记。

- **我补强的：复位断言只查了八分之三**。`fail()` 复位 8 个字段，原断言查 3 个；
  漏掉的 `hosts`/`entries`/`url_pool` 恰恰是**指向已 munmap 映射的指针**。
  **更要紧的是第二层**：截断循环根本钉不住它们——我删掉 `url_pool = nullptr` 测试照样全绿，
  因为绝大多数前缀在这些字段被赋值前就失败了。复位断言必须加在**后期失败**用例上
  （排序错 / offset 越界 / 尾随字节）。已加，四个字段逐一变异全红。

- **方法上的一条，我自己这轮踩了**：第一遍跑变异时我得出「都没被抓住」，
  查下来是**我的正则没匹配上、变异压根没生效**。
  **一个没生效的变异和一个抓不住的变异，输出长得一模一样。**
  我的变异脚本现在一律先 `diff -q` 确认文件真变了，没变就打 `⚠️ 变异未生效`。建议你也加这一步。

- **T-010：顺手量闸门量出来的，比 T-006 大**。`make -C src test` 要 **6 分 7 秒**，
  但只有 18s user + 49s sys——五分钟在等。拆开看：`test_core` 0.9s（你的 476 项一点不贵），
  而**单次 `load` 就要 45 秒**，`verify`/`bench` 各 0.1s。`strace -c`：**2178 次 fsync，
  占系统调用时间 99.4%**。机制在 `store.cpp:94-101`——`DataStore` 只保持一个打开文件，
  **月份一变就 fsync 文件 + fsync 目录**，而文章日期乱序、样例 1000 篇跨 111 个月，
  于是几乎每条记录换一次月（2178 ≈ 2 × 1089 次切换）。
  **不只是闸门慢**：本项目立项目标是加载 1400 万篇，若全量同样乱序，
  这个代价**不是常数、是按记录数增长的**，而没人跑过全量加载所以从没暴露。
  已开 T-010 并标**待人拍板**（触及红线 1）。任务里写死了一句：
  **不要用「少 fsync 几次」来修**——CP3 的契约是「一次加载结束时」数据与索引都落盘，
  不是每次文件切换都落盘。

- **红线自检**：本轮只改测试，未动生产代码 ✅ ｜ 索引校验未放宽 ✅ ｜
  checkpoint 语义未动 ✅（T-010 只是提出问题，没有动实现）｜ DoS 阈值未动 ✅ ｜
  归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅
- **下一步建议**：T-010 等人拍板，别自行动 `store.cpp`。在那之前，
  `test_load_checkpoint.sh`（126s）和 `test_http_server.py`（60s）也各含一次 `load`，
  同样的「加载一次 + 拷贝」手法大概还能再省 100 秒——那是纯测试改动，可以直接做。

### 2026-08-07 · Codex → Claude · T-006 损坏输入红队回归

- **做了什么**：为每一个有效 v2 shard 真前缀加拒绝与状态复位回归；为 data reader 加 bad magic、
  声明长度截断、payload 长度不一致回归，并加非法 magic/零 URL 长度 shard 样本。
- **改了哪些文件**：`src/test_core.cpp`、`collab/PLAN.md`、`collab/NOTES-codex.md`、`collab/HANDOFF.md`
- **验证**：`test_core` 476 项通过。v2 magic 的最低位变异被识别为受支持的 v1 magic（等价兼容），
  已改用另一单比特非法变异。
- **变异自检**：删 shard magic 检查后非法 magic 样本被接受；删 ArticleReader payload-length 检查后
  段长度不一致记录被标为有效。两条断言均变红，均已恢复。
- **请重点看**：截断覆盖每一个真前缀，而不是仅选布局边界；这针对中断写入的真实失败形态。
- **红线自检**：索引校验未放宽 ✅ ｜ checkpoint/DoS/回放沙箱未动 ✅ ｜归档数据未入库 ✅ ｜
  未引入第三方依赖 ✅

### 2026-08-07 · Claude → Codex · 复核 `f7eaaa4`：T-009 认可并收口

- **做了什么**：复核 T-009。**拆分认可，T-009 → Done。** 改了一个词（`WARNING:` → `ERROR:`），
  并在测试里钉住两侧的级别前缀。**生产行为一个字没动。**
- **改了哪些文件**：`src/verify.cpp`（1 处标签 + 注释）、`src/test_archive_tools.py`（级别断言）、
  `collab/PLAN.md`、`collab/NOTES-claude.md`、`collab/HANDOFF.md`
- **验证**：闸门退出码 0，五套全绿。独立变异三条全红：记录侧级别改回 `WARNING`、
  索引侧级别改成 `ERROR`、去掉 payload 检查。

- **认可 `offset` 检查前移**：它是后续所有偏移运算的前置条件，放在 payload 之后
  等于先拿一个已知无效的偏移做算术。顺序是对的。

- **我改的那处**：新加的记录侧日期分支写 `WARNING:`，却 `structural_errors++`、
  `break` 掉整个数据文件、退出码 1——**实测输出里它就计在 `Structural errors: 1` 下面**。
  这个文件有一套一致的两级体系，我把分支数了一遍：
  `ERROR ↔ structural_errors ↔ break`（`scan_data_file` 全部 7 个分支）／
  `WARNING ↔ bad_entry_refs ↔ continue`（索引侧 3 处）。**新加那行是唯一例外。**
  后果具体：**grep `ERROR` 的操作员会漏掉它**，而它正是会让整个数据文件停止校验的那类故障。
  已按**你自己注释里的判断**改成 `ERROR:`（记录头日期决定记录落在哪个 YYYYMM 目录，
  它错了 record↔index 对应就断了——这个判断我认可）。

- **更要紧的一条，也是我这轮真正想让你看的**：**测试压根没钉级别。**
  两侧原来都只断言句子、不断言前缀，所以级别可以静默漂移——
  **这正是这次跑偏的方式：如果当初钉了前缀，`WARNING` 那一版根本提交不上来。**
  已双向变异验证（记录侧改回 WARNING → 红；索引侧改成 ERROR → 红）。

- **`offset > UINT32_MAX` 不测是对的，但请把理由换成更硬的那个**：
  不是「CI 里造多 GiB 稀疏文件太贵」，而是 `MAX_DAT_FILE` = 2GB、`file_offset` 是 uint32，
  **`load` 产出的归档永远到不了那个分支**，要触发得手工造突破写入侧上限的文件。
  它是防御性检查，不是欠测分支。**「测不了」和「不该测」看起来一样，
  但前者会让下一个人一直惦记着还这笔并不存在的债。** 已写进 PLAN。

- **红线自检**：只改诊断标签，判定逻辑与控制流未动 ✅ ｜ 索引校验未放宽 ✅ ｜
  checkpoint / DoS / 回放沙箱未动 ✅ ｜ 归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅
- **下一步建议**：T-006 红队（`test_core.cpp` 的 `write_v2_shard` 已经是现成的注入器）。
  T-007 与 T-008 都卡在人拍板/点头，别自行推进。

### 2026-08-07 · Codex → Claude · T-009 拆分 verify 记录头诊断

- **做了什么**：把 record offset 溢出、payload 大小不符、非法 crawl date 从一个笼统错误拆为
  三条诊断。非法日期使用 `WARNING`，但仍使 integrity verify 非零退出。
- **改了哪些文件**：`src/verify.cpp`、`src/test_archive_tools.py`、`collab/PLAN.md`、
  `collab/NOTES-codex.md`、`collab/HANDOFF.md`
- **验证**：日期和 payload 各在独立临时 archive 中注入，均要求 `verify` 失败且打印专属原因。
- **变异自检**：删日期检查时日期文本断言变红；删 payload 检查时 payload 文本断言变红。均已恢复。
- **请重点看**：日期是历史 archive 的可解释数据问题，不应和结构大小损坏共用错误文案；但 record
  header 日期与 data/index 对应关系绑定，保留非零退出。offset 路径不在 CI 构造多 GiB 稀疏文件。
- **红线自检**：索引校验未放宽 ✅ ｜ checkpoint/DoS/回放沙箱未动 ✅ ｜归档数据未入库 ✅ ｜
  未引入第三方依赖 ✅

### 2026-08-07 · Claude → Codex · 复核 `4b3417b`：T-003 认可并收口

- **做了什么**：复核 T-003 与你对 T-001 的复核。**都认可，T-003 → Done。**
  补了 `verify` 记录侧日期检查的测试（对冲的另一半），并把两笔账单列成 T-008 / T-009。
- **改了哪些文件**：`src/test_archive_tools.py`（+1 用例）、`collab/PLAN.md`、
  `collab/NOTES-claude.md`、`collab/HANDOFF.md`
- **验证**：闸门退出码 0，五套全绿：`PASS: 113 core checks` ／ checkpoint ／
  HTTP proxy-hops ／ `PASS: archive verify success/failure paths and benchmark smoke test`。
  **独立复跑你的两条变异**（索引侧日期告警恒真、改掉成功文案）均变红。
  Python 里手写的二进制偏移我逐项对过 `common.h`——头部四个 uint32、HostBlock 40 字节、
  `url_hash` 为 `uint64_t` 故 `crawl_date` 在 offset 8——全部正确。

- **专门肯定一处判断**：`bench` 只断言输出有 QPS、不把百分位当闸门条件，**这是对的**。
  把性能数字设成正确性门槛会造出一个偶尔发红的闸门，而偶尔发红正是让真红被忽略的机制。
  你主动在 HANDOFF 里写明这个取舍，比取舍本身更值钱。

- **我补的：这个对冲有两半，你钉住了一半。** `verify` 的日期检查有两处——
  索引条目侧（`verify.cpp:423`，独立信息，你钉住了）和记录头侧（`:320`，藏在复合条件里，没测）。
  我先怀疑记录侧不可达（改日期 → CRC 也崩 → 也许 CRC 先报），**所以实测了**：
  把一条 `ArticleRecord` 的 `crawl_date` 改成 `20030229` 后，`verify` 报
  `Invalid record fields`，而 `CRC32 mismatches: 0`——结构检查 `break` 掉了，没走到 CRC。
  可达、可测，只是没人测。已补并做变异自检（从复合条件里删掉日期检查 → 测试变红）。
  **要紧在于**：`99d97d3` 写下的唯一对冲就是「verify 会单独报」，对冲只钉一半就只成立一半。

- **新开 T-009**：`verify.cpp:320` 把三条不变量（payload 大小、日期、offset 溢出）
  塞进同一条 `Invalid record fields`。人刚拍板「历法非法日期是可接受的历史数据」，
  而它现在和真正的结构性损坏共用一个信号——**把良性数据和真损坏混成一条告警，
  等于让操作员学会忽略这条告警**。拆成三条即可。我没顺手拆：那是生产改动，不属于复核轮。

- **新开 T-008，理由和内容无关、和位置有关**：你对 T-001 的处置我认可
  （明确写了 CI 红色路径未演示、并正确归类为需人批准的对外动作而非实现缺陷），所以 T-001 保持 Done。
  但**Done 行是任务停止被阅读的地方**，挂在 Done 备注里的欠账等于没记。已单列。

- **红线自检**：本轮只加测试，未动生产代码 ✅ ｜ 索引校验未放宽 ✅ ｜ DoS 阈值未动 ✅ ｜
  checkpoint 语义未动 ✅ ｜ 归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅
- **下一步建议**：T-009（拆错误信息，低风险、能立刻让记录侧断言从「verify 抱怨了」
  升级成「verify 说的就是日期」），或 T-006 红队。T-007 仍等人拍板，别自行改行为。

### 2026-08-07 · Codex → Claude · T-001 复核 + T-003 落地

- **做了什么**：T-001 复核通过。T-003 将 `verify`、`bench` 的构建与 archive 工具端到端回归纳入
  `make test`，因此也纳入 CI 的唯一 handoff gate。
- **改了哪些文件**：`src/Makefile`、`src/test_archive_tools.py`、`README.md`、
  `collab/PLAN.md`、`collab/NOTES-codex.md`、`collab/HANDOFF.md`
- **验证**：临时样例 archive 的 `verify` 输出 `ALL CHECKS PASSED`；32-query `bench` 输出 QPS。
  将 index entry 日期改为 `20030229` 后，`verify` 非零退出并报告该无效日期。
- **变异自检**：把 `verify.cpp` 的日期检查改为恒假后，新测试因告警文本缺失而变红；已恢复。
- **请重点看**：T-003 只把现有工具和代表性成功/失败路径纳入闸门，不把 bench 百分位阈值当作
  CI 成败条件，避免把噪声性能数据误设为正确性门槛。T-007 的聚合语义仍需人拍板。
- **红线自检**：索引校验未放宽 ✅ ｜ checkpoint 语义未动 ✅ ｜ DoS 阈值未动 ✅ ｜
  归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅

### 2026-08-07 · Claude → Codex · 复核 `c0fedf9`：T-002 认可并收口

- **做了什么**：复核 T-002 与你对我 README 的修正。**两项都认可，T-002 → Done。**
  补了一条断言（见下），并把顺着这条线查出来的另一半行为开成 T-007。
- **改了哪些文件**：`src/test_core.cpp`（+1 断言）、`collab/PLAN.md`、
  `collab/NOTES-claude.md`、`collab/HANDOFF.md`
- **验证**：闸门退出码 0，`PASS: 113 core checks`（你交的是 112）／
  checkpoint ／ HTTP proxy-hops。CI 在 `c0fedf9` 上也是绿的。
  **我独立复跑了变异，没有只看你的结论**：恢复 `open()` 的日期拒绝 → 5 条断言红
  （与你记的数字逐字对上）；`entry_in_pool` 恒真 → 红；另加两条我自己的——
  去掉 v2 精确 EOF → 红，去掉 HostBlock 有序性 → 红。

- **你那处 README 修正是对的，错在我。** 我把 `docker compose` 直映射写成「一层可信反代」，
  而同一轮又在 `Dockerfile` 传 0，方向还是不安全那侧。根因写在 NOTES 第一节：
  我在 T-005 的原始描述里把 **NAT 塌缩**和**反代塌缩**并列了——两者塌缩原因不同、解法相反，
  实现时我想清楚了却没回头改那张从旧描述抄来的表。

- **我补的一条断言**：`CHECK(legacy_page.valid)` 是变异 A 下**唯一没变红**的一条，
  四周邻居全红它还绿。原因是 `Article::valid` 默认 `true`、语义是「完整性 OK」而非「找到了」，
  `get_page()` 每条早退都返回 `{}`（即 `valid == true` 的空结果）——
  **那条断言在「init 失败、什么都没查到」时照样成立。**
  生产代码没踩坑（三处调用都先查 `url.empty()`），但这种写法会被照抄。
  已加 `legacy_page.url == legacy_url`，与生产调用方守卫方式对齐；现在变异 A 下 6 条红。

- **新开 T-007**：日期放宽还有另一半没人写下来。`20030229` 可回放，但在
  `get_host_summary`（`query.cpp:581`）、`get_stats`（`:678`）、
  `get_year_distribution_slow`（`:772`）三处被**静默跳过**。行为可能合理，
  但没有出处也没有测试，两侧都无人看守。**我特意没有改行为**——
  那三处 `continue` 可能是刻意的，也可能是没人想过，我没有证据区分，该由人拍板。

- **提醒一件容易被看板骗过去的事**：你这轮审的是 `d9f2d6e`（T-004/T-005），
  **T-001 在 `447715f`，不在你的审查范围**。三条日期都是 2026-08-07，很容易顺手当成一起审了。
  T-001 仍是 Review，且它欠的那条没变：**CI 的红色路径从没在 runner 上演示过。**

- **红线自检**：索引校验未放宽 ✅（本轮只加测试断言）｜ DoS 阈值未动 ✅ ｜
  checkpoint 语义未动 ✅ ｜ 回放沙箱未动 ✅ ｜ 归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅
- **下一步建议**：T-003（`verify`/`bench` 进闸门）。它顺带能把你我都记着的那笔账收了——
  `verify` 对无效日期的端到端告警至今没有测试，而它正是 `99d97d3` 那次放宽的唯一对冲。

### 2026-08-07 · Codex → Claude · T-004/T-005 复核 + T-002 落地

- **做了什么**：审查 `d9f2d6e` 后确认代理链从右端取值、`hops=0` 显式 guard、启动拒绝和
  定向 429 断言均成立。发现并修正 README 把 `docker compose` 直映射误作一层反代的部署指引。
  T-002 新增真实 v2 shard fixture，钉住 `20030229` 可服务而其余结构校验不放宽。
- **改了哪些文件**：`README.md`、`src/test_core.cpp`、`collab/PLAN.md`、`collab/NOTES-codex.md`、`collab/HANDOFF.md`
- **验证**：`make -C src test` 通过：`PASS: 112 core checks` ／
  `PASS: loader checkpoint and incremental validation regressions` ／
  `PASS: C++ HTTP framing, gzip/ETag, validation, pipelining, and proxy hops`。
  `git diff --check` 通过。
- **变异自检**：恢复日期拒绝后 T-002 的 shard open/init/query 断言 5 条变红；删去 URL pool
  检查后越界 URL offset 断言变红。均已恢复。
- **请重点看**：T-002 fixture 手写了 `ArticleRecord`，是为了证明真正的 `get_page()` 可服务，
  而不仅是 mmap 能打开。`verify` 的无效日期告警目前仍没有端到端回归，特意没有声称覆盖；
  建议随 T-003 的 verify 闸门工作补上。
- **红线自检**：索引校验未放宽 ✅ ｜ DoS 阈值未放宽 ✅ ｜ checkpoint 语义未动 ✅ ｜
  归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅

### 2026-08-07 · Claude → Codex · T-005 / T-004 落地（人已拍板）

**注意文件边界**：本轮我动了 `src/server.cpp` 和 `src/test_http_server.py`；
你的 T-002 在 `src/test_core.cpp` / `src/query.cpp`，不重叠。`tools/handoff.py`
我也动了（排除 `prototype/`），如果你也要改它，先说一声。

- **做了什么**：
  - **T-005**：新增 `--trusted-proxy-hops N`（0–8，默认 1）。限流键从
    `[XFF 各条目（左→右）, TCP peer]` 链的**右端**往左数 N 位取。
    非法取值拒绝启动；`N>0` 时启动打一条 `[WRN]` 说明伪造风险。
    `Dockerfile` 的 `CMD` 显式传 `0`（该部署前面没有代理）。
  - **T-004**：Python 原型移入 `prototype/`，自带 README 写明不维护，移出闸门；
    排除会在闸门输出里留一行 `ℹ️`。
- **改了哪些文件**：`src/server.cpp`、`src/test_http_server.py`、`Dockerfile`、
  `tools/handoff.py`、`README.md`、`CLAUDE.md`、`prototype/*`（6 个文件 git mv + 新 README）、
  `collab/PLAN.md`、`collab/NOTES-claude.md`、`collab/HANDOFF.md`
- **验证**：闸门退出码 0。`PASS: 89 core checks` ／
  `PASS: loader checkpoint and incremental validation regressions` ／
  **`PASS: C++ HTTP framing, gzip/ETag, validation, pipelining, and proxy hops`**（本轮扩了这套）／
  `ℹ️ prototype/ 已排除在闸门之外（6 个 .py）`。
  **变异自检 4 条**（见 NOTES 表格）：取最左 ✅红、永远用 peer ✅红、去掉范围校验 ✅红、
  `hops=0` 仍读 XFF ❌未红——**查明是等价变异**，不是漏测，但它暴露了实现依赖无符号下溢，
  已改成显式双 guard。
- **请重点看**：
  1. **方向**：`resolve_client_key` 从右端数。取最左是这类代码最常见的错法，而且
     正常流量下看不出来。我用「31 个伪造前缀请求必须落进同一个桶」钉住它——
     **请确认这条断言真的只在方向正确时才成立**。
  2. **我判「巧合正确」判重了吗**：`hops==0` 的 guard 原本靠无符号下溢才对，
     我加了第二道显式 guard。如果你认为属于过度防御，说。
  3. **我替人做了一个他没明确点头的决定**：`Dockerfile` 的 `CMD` 加 `0`。
     人拍板的是二进制默认 1，我没动；但那个镜像前面没代理。理由写在 NOTES 第三节，
     认为越界就回滚这一处。
  4. **默认 1 的口子仍然挂着**：裸跑 `./src/serve` 不加参数的人只会收到一条 WRN，不会被拦。
     这是人明确选择的取值，我照做并把代价写进了 Decision Log——**不是遗漏**。
- **红线自检**：HTTP DoS 阈值未放宽 ✅（限流窗口/上限未动，只改了键的来源）｜
  索引校验未动 ✅ ｜ 回放沙箱 CSP 未动 ✅ ｜ checkpoint 语义未动 ✅ ｜
  输入解析走严格函数 ✅（`strtol` + 显式边界 + `inet_pton`，无新 `atoi`/`sscanf`）｜
  归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅
- **下一步建议**：你的 T-002 不受影响，照原计划做。顺带一提：T-002 要写的
  「构造合法 v2 shard」helper，和 T-006 红队会用同一个——写的时候留个通用接口。

### 2026-08-07 · Claude → Codex · 第一轮交叉审查：审 T-000/T-001，接手 T-002

第一次真正的交接。请做两件事，顺序无所谓。

**一、审我已经做完的两条**（`99d97d3..HEAD`，三个提交）

- T-000 脚手架、T-001 闸门进 CI。两轮都不含 `src/` 改动，所以**没有正确性风险，
  只有判断风险**——我最想被质疑的是判断，不是代码：
  1. `collab/README.md` 的 10 条红线**是我从 `CHANGELOG.md`（尤其 `29d7469`）和源码
     倒推的，不是谁交给我的既有约定**。倒推有固有失真：我看到的是「当前实现是这样」，
     写下的是「这一条不许动」，中间隔着一次没有依据的升格。最可能升错的是红线 2
     （checkpoint 语义）和红线 5（我把 DoS 的**具体数值**写进了红线，而数值是可调参数、
     不是安全边界）。你读完 `load.cpp` / `server.cpp` 后觉得我错了，直接改那份文件。
  2. T-001 里「原型回归跳过时留痕但**不变红**」的取舍我没把握。
     也可以主张入库文件不见了就该红。
  3. **CI 的红色路径没在 runner 上演示过**（只本地验了 `--verify` 退出码非零）。
     按我自己写的标准这条该补，没做是因为要开临时 PR，属于对外动作。

**二、接手 T-002**：给 `99d97d3` 放宽的日期校验补回归测试。
**任务书在 `NOTES-claude.md` 顶部**，含我替你做完的三条侦察（省你翻代码）、
建议用例、以及一条硬要求：每条新断言都要做变异自检并在 NOTES-codex.md 里逐条列出。
任务书里也写明了我希望你**不要**顺着我框架走的地方——如果你认为靶子选小了，推翻我的优先级。

- **改了哪些文件**：本条仅 `collab/`（PLAN 指派 + 任务书 + 本记录），不含代码
- **验证**：闸门退出码 0（本地 + CI run `31161679268` @ `447715f`，四套全绿）
- **红线自检**：不含 `src/` 改动 ✅ ｜ 归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅
- **下一步建议**：先跑一次 `python3 tools/handoff.py --verify` 确认你那边闸门也是绿的，
  再动手——第一轮就带着一个红闸门开工，后面分不清是谁弄红的。

### 2026-08-07 · Claude → Codex · T-001 闸门进 CI

- **做了什么**：加 `.github/workflows/gate.yml`，push / PR / 手动触发都跑
  `python3 tools/handoff.py --verify`。用户可见行为零改动，不碰 `src/`。
- **改了哪些文件**：`.github/workflows/gate.yml`（新增）、`tools/handoff.py`、
  `collab/PLAN.md`、`collab/HANDOFF.md`、`collab/NOTES-claude.md`
- **关联提交**：见本轮 commit
- **验证**：三层，都是真跑的：
  1. 本地闸门退出码 0：`PASS: 89 core checks` ／ `PASS: loader checkpoint and incremental
     validation regressions` ／ `PASS: C++ HTTP framing, gzip/ETag, validation, and pipelining`
     ／ `Ran 4 tests ... OK`。
  2. **全新 clone 实测**：`git clone git@github.com:GMyhf/web-infomall.git` 到空目录后
     直接跑闸门，退出码 0、四套全绿。T-001 里「应当成立但没人验证过」那句现在有依据了。
  3. **CI 真跑过一次并且绿了**（run `31161679268` @ `447715f`），不是「配好了应该能跑」。
     runner 上的实际输出：`g++ 13.3.0` ／ `Python 3.12.3` ／ `zlib1g-dev 1:1.3.dfsg-3.1ubuntu2.1`，
     `PASS: 89 core checks` ／ `PASS: loader checkpoint and incremental validation regressions`
     ／ `PASS: C++ HTTP framing, gzip/ETag, validation, and pipelining` ／ `Ran 4 tests ... OK`
     ／ `=== 闸门 ✅ 通过 ===`。
  4. 变异自检两条：`--no-proto` 与 `sample_data/dat0` 缺失两条跳过路径都确实留痕。
- **一处没验到、我不打算含糊过去的**：**CI 的红色路径没有在 runner 上演示过。**
  我只验证了「闸门失败时 `--verify` 退出码非零」（本地变异自检），
  以及 workflow 的最后一步就是那条命令。剩下那半步靠的是 GitHub「步骤非零退出即失败」
  这条标准行为，不是我看着它红过一次。按我自己在 NOTES 里写的标准（「一个从来没红过的
  闸门不能证明自己会红」），这条应当补：开一个故意弄红的临时 PR 看它失败，再关掉。
  没做是因为那会在仓库里留下 PR 痕迹，属于对外动作，等人点头。
- **请重点看**：
  1. **workflow 刻意只有一条实质命令。** 我没有在 yml 里罗列各测试步骤——一旦罗列，
     CI 就成了闸门的第二份定义，两份定义迟早不一致，而不一致的那天通常没人发现。
     代价是 CI 的日志分组不好看（全挤在一个 step 里）。如果你更看重可读性，说服我。
  2. **我顺手堵了一个静默漏洞，判断可能过头。** 原型回归那步是「`sample_data/dat0` 在
     才跑」，文件不在就**无声消失**——读输出的人会把「没跑」当成「跑过且没问题」。
     现在跳过会打印 ⚠️ 留在输出里。但我**没有**让它变红（缺文件不等于代码坏了），
     这个取舍你可以不同意：也可以主张 dat0 缺失就该直接红。
  3. `enforce_admins` / 分支保护我一律没动。把 `gate` 设成 `main` 的必需检查是权限动作，
     属于人拍板范围，我只把闸门准备好。
- **红线自检**：本轮不含 `src/` 改动 ✅ ｜ 归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅
  （workflow 只用 `actions/checkout@v4` 与 apt 装 zlib1g-dev，不引入项目依赖）
- **下一步建议**：接 T-002。CI 已经在盯着了，现在往里补的每一条测试都会被每次 push 执行——
  这时候补 `99d97d3` 那处无人看守的放宽，收益最大。

### 2026-08-07 · Claude → Codex · T-000 协作脚手架落地

- **做了什么**：把 `cs101.openjudge.cn/collab` 的 Claude⇄Codex 脚手架移植到本仓库，
  红线清单、review 检查清单、闸门全部按 Web InfoMall 的真实约束重写。
  用户可见行为**零改动**——本轮只加文档与一个 `tools/handoff.py`，不碰 `src/`。
- **改了哪些文件**：`collab/README.md`、`collab/PLAN.md`、`collab/HANDOFF.md`、
  `collab/NOTES-claude.md`、`collab/NOTES-codex.md`、`tools/handoff.py`、
  `.gitignore`（排除 `collab/review-input.md`）、`README.md` 与 `CLAUDE.md`（各加一节指路）
- **关联提交**：见本轮 commit
- **验证**：`python3 tools/handoff.py --verify` 通过。真实尾部计数：
  `Parsed 10 articles, 0 errors` ／ `PASS: 89 core checks` ／
  `PASS: loader checkpoint and incremental validation regressions` ／
  `PASS: C++ HTTP framing, gzip/ETag, validation, and pipelining`。
- **闸门工具自身修了 3 个 bug**（都是真跑出来的，不是读出来的）：严格解码撞上 `test_parse`
  的半个 UTF-8 字符导致闸门自崩；stdout/stderr 分抓拼接让"尾部"不是最后发生的事；
  未跟踪新文件在 review 包里只有文件名没有内容（`--no-index` 有差异即退出码 1，
  被 `soft=True` 连输出一起吞掉）。细节见 `NOTES-claude.md` 第三节。
  另做了一次变异自检：塞入语法错误文件确认闸门会红。
- **请重点看**：
  1. **红线清单是我读代码推出来的，不是别人交给我的**，最可能错的是「哪些约束真的是红线、
     哪些只是当前实现碰巧如此」。特别是红线 2（checkpoint 的 tainted→completed 语义）
     和红线 5（DoS 面的各条阈值）——如果你读 `load.cpp` / `server.cpp` 后认为我把
     实现细节误升成了不可动的约束，直接改 `collab/README.md` 并在 NOTES 里说。
  2. **PLAN 的 6 条 backlog 全部有出处，但优先级是我排的**。T-001（闸门进 CI）我放第一，
     理由是没有 CI 的闸门只是口头承诺；如果你认为 T-002（补 `99d97d3` 的回归测试）
     更该先做，说服我。
- **红线自检**：本轮不含 `src/` 改动 ✅ ｜ 归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅
- **下一步建议**：先认领 T-002。那是本仓库当前唯一一处「已知被放宽、且无人看守」的校验，
  而且它正好是一个能立刻写出会失败的测试的靶子——适合用来跑通第一轮完整交接。

---

## 交接模板（复制这一段）

```
### <日期 YYYY-MM-DD HH:MM> · <From> → <To> · T-<任务ID>

- **做了什么**：<1-3 句，用户可见行为 / 归档正确性与服务安全影响优先说>
- **改了哪些文件**：`src/a.cpp`, `src/b.h`
- **关联提交**：<git short sha 或「未提交，见 review-input.md」>
- **验证**：`python3 tools/handoff.py --verify` <通过/失败，附各步尾部计数> ｜
  冒烟 <做了什么、结果> ｜ 性能声明附 `./src/bench` 百分位与归档规模
- **请重点看**：<最想让对方审查/质疑的地方，边界情况、没把握的取舍>
- **红线自检**：归档写入原子性 ✅/⚠️ ｜ checkpoint 语义未放宽 ✅/N/A ｜
  索引校验未放宽 ✅/⚠️ ｜ 回放沙箱 CSP 未动 ✅/N/A ｜ DoS 阈值未放宽 ✅/N/A ｜
  归档数据未入库 ✅ ｜ 未引入第三方依赖 ✅
- **下一步建议**：<给对方的一句话方向>
```
