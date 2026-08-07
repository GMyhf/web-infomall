# NOTES · Codex → Claude

> 只有 Codex 写这个文件。倒序追加：审查意见、发现的问题、直接修掉的东西。
> 审查结论请给到「会失败的测试」这一层——「这里看起来不太对」对下一轮没有约束力。
>
> 写一条的建议结构：
>
> ```
> ## <日期 YYYY-MM-DD> · <一句话结论>
>
> - **审的是什么**：<commit / range / review-input.md>
> - **认可的**：<明确说认可，避免下一轮重复审>
> - **要返工的**：<每条给出：现象 → 触发条件 → 我写的失败用例 / 建议判据>
> - **我直接修了的**：<commit + 为什么可以直接修而不是退回>
> - **闸门**：<`python3 tools/handoff.py --verify` 退出码与尾部计数>
> ```

## 2026-08-07 · T-013：深块逐项查询取代随机兜底，待 Claude 复核

- **改了什么**：新增 `write_deep_v2_shard`，由 fixture URL 按 `url_hash`、日期降序排序后
  回填 URL pool 偏移。`test_deep_block_binary_search` 用一个单 host、88-entry 的 v2 shard
  走真实 `QueryEngine`：64 个不同 URL 全量 `get_page()`，一个不存在 URL 必须 miss；同 URL 的
  24 个版本必须由 `get_versions()` 全数、按日期降序返回。
- **取值与范围**：64 已有六层二分决策，逐项查找使每个 hash 位置确定执行，不需要把夹具扩到
  几百条。24 版本足以让 `find_first` 返回同 hash 段中间时漏掉旧版本成为可见失败。未把
  `find_host_range` 混入本轮：`find_first` 有两条已实测逃过 495 项的变异，host 层没有同等
  证据，且两层需要不同的夹具形状与判据。
- **变异自检**：每次均先保存 `common.h`，再用 `diff -q` 输出 `Files ... differ`，确认变异
  已生效后才运行测试。`else hi = mid - 1` 令 22 条新增逐项查找断言失败；
  `while (lo + 1 < hi)` 令 23 条失败。两次都已恢复为 `find_first` 原实现。
- **闸门前冒烟**：`make -C src test_core && ./src/test_core` 通过，`PASS: 676 core checks`；
  `git diff --check` 通过。完整 `handoff.py --verify` 交接时复跑。
- **请重点看**：helper 将 `record_size` 保留为 `uint32_t`，写 v2 前显式拒绝不能编码为
  `uint16_t` 的 fixture，避免测试自身通过窄化掩盖错误。生产查询和二分实现没有改动。

## 2026-08-07 · T-006 红队：把每一个部分 shard 都当成中断写入

- **改了什么**：有效 v2 shard 的每个真前缀都送入 `MappedShard::open()`；每一个必须返回 false，
  同时 fd、mmap 指针和 header 指针均恢复为空。另针对损坏 magic、零 URL 长度与 ArticleReader 的
  坏 magic、声明记录截断、分段长度不一致验证干净拒绝。
- **兼容例外**：v2 magic 的 bit 0 从 `IDX!` 翻转为 `IDX `，恰好是支持的 v1 magic，按 v1 接受。
  它不能区分为损坏或旧格式，故不把它记作漏检；测试改翻另一位，验证真正非法 magic。
- **变异自检**：去掉 shard magic 检查时非法 magic 被接受，断言变红；去掉 `ArticleReader` 的
  payload length 一致性检查时坏记录被标为有效，断言变红。均已恢复。

## 2026-08-07 · T-009 拆开 record-header 的三种失败原因

- **改了什么**：`verify.cpp` 不再把 offset 超过索引范围、payload 长度不符、记录日期非法合并为
  `Invalid record fields`。前两者为 `ERROR`，日期为独立 `WARNING`；三者都保留原有的
  `structural_errors` 计数与非零退出，只有操作员读到的原因变得明确。
- **认可的边界**：日期不标成结构大小错误，但也不被默许。历史 index 日期可为非法日历值，
  record header 日期仍是不完整 archive 的信号；用 `WARNING` + 非零退出表达这一区别。
- **测试与变异**：临时 archive 分别只改 record date 和 `record_size`，各自要求专属文本。
  移除日期检查后只出现 CRC/identity mismatch，日期文本断言变红；移除 payload 检查后只出现
  file-bounds 错误，payload 文本断言变红。offset 超范围不在 CI 制造多 GiB 稀疏文件，但代码路径
  已与前两者分离。

## 2026-08-07 · T-001 复核通过，T-003 将 archive 工具纳入闸门

- **T-001 审的是什么**：`447715f` 的 `.github/workflows/gate.yml` 和对 `handoff.py` 的闸门改动。
  workflow 在干净 checkout 的 push、PR、手动触发中执行唯一的 `python3 tools/handoff.py --verify`；
  显式 zlib 安装、20 分钟超时与同 ref 并发取消都合理。没有实现缺陷。CI 故意红一次仍未演示，
  因它需要对外创建临时失败提交/PR，保留为人批准的操作。
- **T-003**：`make test` 现在构建 `verify`、`bench` 并运行 `test_archive_tools.py`。测试从
  `sample_data/dat0` 生成临时 archive，要求 `verify` 报 `ALL CHECKS PASSED`，要求 32-query
  `bench` 输出 QPS；再将一个 index entry 日期改为 `20030229`，要求 `verify` 非零退出并明确告警。
- **变异自检**：将 `verify.cpp` 的 invalid-date 条件变为恒假后，工具仍因 identity mismatch
  非零退出，但缺少要求的 `invalid crawl date 20030229` 文本，新增测试变红。已恢复。

## 2026-08-07 · T-004/T-005 审查通过，T-002 已落地待审

- **审的是什么**：`d9f2d6e` 的 T-004/T-005，以及 T-002 任务书。
- **认可的**：`resolve_client_key` 从 XFF 链右端数，且 31 个不同伪造前缀共享右端地址的
  429 回归能区分左端误取；`hops=0` 的显式双 guard 不是过度防御，消除了无符号下溢的偶然正确。
  非法配置拒绝启动、原型从闸门排除并留痕的行为也与任务决定一致。
- **我直接修了的**：README 曾把直映射端口的 `docker compose` 误列为一层可信反代、建议 hops=1；
  这与 Dockerfile 显式传 0 相矛盾，并会让直接访问者可伪造 XFF 绕过限流。已改为 nginx/CDN 等
  实际反代示例，不改变二进制默认值或 Docker 行为。
- **T-002**：新增真实 v2 shard fixture。`20030229` fixture 通过 `MappedShard::open()`，随后
  `QueryEngine::init()`、`get_page()`、`get_versions()` 均成功且日期原样返回。fixture 还逐项拒绝
  URL pool 越界、host 无序、entry hash 无序、host entry 范围越界和 v2 尾随字节。
- **变异自检**：在 `MappedShard::open()` 恢复 `valid_crawl_date` 拒绝，核心测试 5 条变红；
  将 `entry_in_pool` 检查改为恒假后，URL offset 越界断言变红。两次均已恢复生产代码。
- **闸门**：`make -C src test` 通过：`PASS: 112 core checks`、checkpoint、HTTP proxy hops。
  `git diff --check` 通过。`verify` 对无效日期的端到端告警仍无测试；这是 T-003 将 verify 纳入闸门时应补的范围，避免把它伪装成已覆盖。
