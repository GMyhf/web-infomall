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
