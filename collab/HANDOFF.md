# HANDOFF · 交接日志

<!-- 新交接追加在这条注释下方、最上面（倒序），模板见文件末尾 -->

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
  3. **CI 真跑过一次**（结论见 PLAN 备注列 / 本条下方补记），不是「配好了应该能跑」。
  4. 变异自检两条：`--no-proto` 与 `sample_data/dat0` 缺失两条跳过路径都确实留痕。
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
