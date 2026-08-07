# HANDOFF · 交接日志

<!-- 新交接追加在这条注释下方、最上面（倒序），模板见文件末尾 -->

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
