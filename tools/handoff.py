#!/usr/bin/env python3
"""handoff.py — 把一方的 git 改动整理成给另一方 AI 的 review 输入包。

用法:
  python3 tools/handoff.py --from claude --to codex
  python3 tools/handoff.py --from claude --to codex --base main
  python3 tools/handoff.py --from codex --to claude --range HEAD~3..HEAD --verify
  python3 tools/handoff.py --from claude --stdout        # 打印而不写文件
  python3 tools/handoff.py --verify                      # 只跑闸门（回归测试 + 语法检查）

参数:
  --from <name>   交接方（claude|codex），默认 claude
  --to <name>     接收方，默认取另一方
  --base <ref>    审查 <ref>..HEAD 的全部改动
  --range <a..b>  显式 git range，优先级高于 --base
  --out <path>    输出路径，默认 collab/review-input.md
  --verify        附带运行闸门并把结果写进包里
  --stdout        打印到 stdout，不写文件
  --no-proto      跳过已弃用的 Python 原型回归（只跑 C++ 闸门 + 语法检查）

无 --base/--range 时自动推断：工作区有未提交改动 → 对比 HEAD；否则 → HEAD~1..HEAD。
只用 Python 标准库 + git + make，无第三方依赖。移植自 cs101.openjudge.cn/tools/handoff.py。
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COLLAB = ROOT / "collab"
OTHER = {"claude": "codex", "codex": "claude"}
MAX_DIFF_BYTES = 200_000  # 超过则截断，避免生成一个没法读的巨文件
MAX_UNTRACKED_BYTES = 100_000  # 单个未跟踪文件超过这个大小就不进包（本仓库随手会有 GB 级归档产物）
TAIL_LINES = 12  # 成功的步骤也保留尾部输出——交接记录里要有真实计数，不能只有一个 ✅

CHECKLIST = """## Review 检查清单（本项目红线）

- [ ] **归档写入的原子性**：新写的输出文件是否仍走 `.tmp` → `fflush` → `fsync` → `rename` → fsync 父目录？
      `fwrite`/`fclose` 的返回值是否都检查了？半截文件在崩溃后会被当成有效归档吗？
- [ ] **checkpoint 语义（CP3）**：数据源是否仍在首次写入前置 `tainted`、在数据与全部索引落盘 fsync 后才升 `completed`？
      `--incremental` 是否仍要求有效 checkpoint、非增量是否仍拒绝写入非空归档？`flock` 互斥是否还在？
      被中断的源是否仍**不会**被自动重放（重放 = 重复追加 = 归档静默损坏）？
- [ ] **索引是不可信输入**：`MappedShard::open` 的头部/布局/URL 池边界/HostBlock 有序性/排序校验有没有被放宽或跳过？
      `entry_in_pool` 是否仍对每条 entry 生效？损坏或被截断的 `.idx` 会不会变成越界读而不是干净拒绝？
- [ ] **回放沙箱**：归档 HTML 是真实抓取的历史网页，带脚本和外链。`/proxy` 是否仍是 `Content-Security-Policy: sandbox`？
      页面的 CSP、`X-Frame-Options: DENY`、`X-Content-Type-Options: nosniff` 是否原样？有没有新路径把归档正文按本服务的源直出？
- [ ] **HTTP 解析与 DoS 面**：仅 GET、请求行/头部严格校验、408/413/503、30s 响应超时、线程池队列上限 256、
      每 IP 30 次/5 秒限流——改动有没有放宽其中任何一条？重定向 `Location` 的 CR/LF 注入检查还在吗？
- [ ] **输入解析**：URL / 域名 / 日期（`YYYYMMDD`）是否仍走严格解析函数，而不是新引入的 `atoi`/`sscanf`/裸 `stoi`？
      新增的用户输入有没有长度与字符集上限？
- [ ] **线程安全**：4 worker 共享的新状态有没有加锁？`ArticleReader` 的 fd 缓存、日志输出的锁是否还在？
      mmap 是否仍是只读、无写入路径？
- [ ] **编码**：GB18030 优先（GBK/GB2312 回退）是否保持？畸形序列是否仍输出 U+FFFD 且**不截断后续文本**？内部是否一律 UTF-8？
- [ ] **数据不入库**：`data/`、`index/`、`archive/`、`*.db` 是否仍在 `.gitignore`？diff 里有没有混进归档产物或二进制？
- [ ] **零第三方依赖**：有没有引入 C++ 库或 Python 三方包？若有，PLAN 里有人拍板吗？
- [ ] **可回归**：`python3 tools/handoff.py --verify` 是否通过？交接记录里有没有贴出真实的尾部计数，而不是一句「我觉得没问题」？"""


def git(args, soft=False):
    try:
        return subprocess.run(
            ["git", *args], cwd=ROOT, capture_output=True, text=True, check=True
        ).stdout.rstrip("\n")
    except subprocess.CalledProcessError:
        if soft:
            return ""
        raise


def git_out(args):
    """取 stdout，忽略退出码。

    `git diff --no-index` 在**有差异时**退出码就是 1——而有差异正是我们要的那一步。
    走 soft=True 会连同 stdout 一起丢掉，包里就只剩文件名、没有内容。
    """
    return subprocess.run(
        ["git", *args], cwd=ROOT, capture_output=True, text=True, errors="replace"
    ).stdout.rstrip("\n")


def resolve_range(opts):
    if opts.range:
        return opts.range, "range"
    if opts.base:
        return f"{opts.base}..HEAD", "range"
    dirty = git(["status", "--porcelain"], soft=True)
    if dirty:
        return "HEAD", "worktree"  # git diff HEAD == 未提交(已跟踪)改动
    has_parent = git(["rev-parse", "--verify", "--quiet", "HEAD~1"], soft=True)
    if not has_parent:
        return "HEAD", "worktree"
    return "HEAD~1..HEAD", "range"


def collect(opts):
    rng, mode = resolve_range(opts)
    diff_args = ["diff", "HEAD"] if mode == "worktree" else ["diff", rng]
    data = {
        "range": rng,
        "mode": mode,
        "diff_args": diff_args,
        "branch": git(["rev-parse", "--abbrev-ref", "HEAD"], soft=True),
        "head_sha": git(["rev-parse", "--short", "HEAD"], soft=True),
        "stat": git([*diff_args, "--stat"], soft=True),
        "name_status": git([*diff_args, "--name-status"], soft=True),
        "untracked": git(["ls-files", "--others", "--exclude-standard"], soft=True),
        "log": git(["log", "--oneline", "--no-decorate", rng], soft=True) if mode == "range" else "",
    }
    diff = git(diff_args, soft=True)
    # 未跟踪的新文件不在 `git diff` 里。只列文件名等于让审查方审一份看不见内容的改动——
    # 而「新加的文件」恰恰是最需要被读的那部分。用 --no-index 对 /dev/null 生成正规
    # diff hunk，不动 index（不用 `git add -N`，那会改变工作区状态）。
    for rel in [f for f in data["untracked"].splitlines() if f.strip()]:
        path = ROOT / rel
        try:
            if not path.is_file() or path.stat().st_size > MAX_UNTRACKED_BYTES:
                continue
            path.read_text(encoding="utf-8")  # 二进制/非 UTF-8 直接跳过，别把归档数据灌进包里
        except (OSError, UnicodeDecodeError):
            continue
        hunk = git_out(["diff", "--no-index", "--", "/dev/null", rel])
        if hunk:
            diff += ("\n" if diff else "") + hunk
    data["truncated"] = len(diff.encode()) > MAX_DIFF_BYTES
    if data["truncated"]:
        diff = diff.encode()[:MAX_DIFF_BYTES].decode(errors="replace")
    data["diff"] = diff
    return data


def read_notes(who):
    path = COLLAB / f"NOTES-{who}.md"
    return path.read_text(encoding="utf-8").strip() if path.is_file() else ""


def read_open_items():
    path = COLLAB / "PLAN.md"
    if not path.is_file():
        return ""
    # 抽出状态看板里非 Done 的任务行，给审查方一眼看到还在飞的任务
    rows = [
        line
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.lstrip().startswith("| T-") and "Done" not in line
    ]
    return "\n".join(rows)


def python_sources():
    dirs = [ROOT, ROOT / "src", ROOT / "tools"]
    files = sorted(
        {str(p.relative_to(ROOT)) for d in dirs if d.is_dir() for p in d.glob("*.py")}
    )
    return files


def run_verify(skip_proto=False):
    """跑闸门：C++ 全套回归 → Python 语法检查 → （可选）Python 原型回归。

    `make -C src test` 已经包含 test_parse / test_core / 加载器 checkpoint /
    C++ HTTP 端到端四套。它是本项目最硬的仲裁，放在第一步：后面的检查再绿，
    这一步红了交接就是红的。
    """
    py_files = python_sources()
    steps = [["make", "-C", "src", "test"], ["python3", "-m", "py_compile", *py_files]]
    # Python 原型（Phase 1）已弃用，但测试还在仓库里且能跑；只在样例数据在位时跑。
    # 它依赖 sample_data/dat0 —— 该文件已入库，所以全新 clone 上闸门同样成立。
    if not skip_proto and (ROOT / "sample_data" / "dat0").is_file():
        steps.append(["python3", "-m", "unittest", "-v", "test_parser"])

    outputs, ok = [], True
    for step in steps:
        # stderr 并进 stdout：分开抓再拼接会把两路日志按流而不是按时间排，尾部就不再是
        # 「最后发生的事」——而尾部正是交接记录里唯一被引用的部分。
        # errors="replace": test_parse 打印的正文预览按字节截断，收尾常是半个 UTF-8 字符。
        # 严格解码会让闸门自己崩在一条 UnicodeDecodeError 上——被测代码没问题，闸门有问题。
        proc = subprocess.run(
            step,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        label = " ".join(step if len(step) < 6 else [*step[:3], f"<{len(py_files)} files>"])
        body = proc.stdout.rstrip()
        if proc.returncode == 0:
            # 成功也保留尾部：交接时要能看到「跑过多少」，一个 ✅ 证明不了任何事。
            tail = "\n".join(body.splitlines()[-TAIL_LINES:]) if body else ""
            outputs.append(f"$ {label}\n✅ exit 0\n{tail}".rstrip())
        else:
            outputs.append(f"$ {label}\n❌ exit {proc.returncode}\n{body}")
            ok = False
    return ok, "\n\n".join(outputs)


def build(opts, data, verify_result):
    to = opts.to or OTHER[opts.sender]
    lines = [f"# Review 输入包 · {opts.sender} → {to}", ""]
    lines += [
        "> 由 `tools/handoff.py` 自动生成，不入库。审查方读完请把意见写进 "
        f"`collab/NOTES-{to}.md`，并在 `collab/HANDOFF.md` 追加一条交接记录。",
        "",
        "## 概况",
        "",
        f"- 分支: `{data['branch']}` @ `{data['head_sha']}`",
        f"- 对比范围: `{data['range']}`（{'未提交改动 vs HEAD' if data['mode'] == 'worktree' else '提交区间'}）",
    ]
    if data["truncated"]:
        lines.append(
            f"- ⚠️ diff 超过 {MAX_DIFF_BYTES} 字节已截断，完整改动请用 `git {' '.join(data['diff_args'])}` 查看"
        )
    lines.append("")

    open_items = read_open_items()
    if open_items:
        lines += ["## PLAN 中未完成的任务", "", "```", open_items, "```", ""]
    if data["log"]:
        lines += ["## 本区间提交", "", "```", data["log"], "```", ""]

    lines += ["## 改动文件", "", "```", data["name_status"] or "(无跟踪改动)", "```"]
    if data["untracked"]:
        lines += ["", "未跟踪(新增未 add)文件：", "```", data["untracked"], "```"]
    lines.append("")

    if data["stat"]:
        lines += ["<details><summary>diffstat</summary>", "", "```", data["stat"], "```", "", "</details>", ""]

    notes = read_notes(opts.sender)
    if notes:
        lines += [f"## 交接方留言（NOTES-{opts.sender}.md）", "", notes, ""]

    if verify_result:
        ok, out = verify_result
        lines += [f"## 闸门结果：{'✅ 通过' if ok else '❌ 失败'}", "", "```", out, "```", ""]

    lines += ["## 完整 Diff", "", "```diff", data["diff"] or "(空)", "```", "", CHECKLIST, ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(add_help=True, description="生成给另一方 AI 的 review 输入包")
    parser.add_argument("--from", dest="sender", default="claude", choices=["claude", "codex"])
    parser.add_argument("--to", choices=["claude", "codex"])
    parser.add_argument("--base")
    parser.add_argument("--range")
    parser.add_argument("--out")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--stdout", action="store_true")
    parser.add_argument("--no-proto", dest="no_proto", action="store_true")
    opts = parser.parse_args()

    # 只想跑闸门时（没给任何生成相关参数）快速返回
    only_verify = (
        opts.verify
        and not any([opts.to, opts.base, opts.range, opts.out, opts.stdout])
        and "--from" not in sys.argv
    )
    verify_result = run_verify(skip_proto=opts.no_proto) if opts.verify else None
    if only_verify:
        ok, out = verify_result
        print(out)
        print(f"\n=== 闸门 {'✅ 通过' if ok else '❌ 失败'} ===")
        sys.exit(0 if ok else 1)

    data = collect(opts)
    markdown = build(opts, data, verify_result)
    if opts.stdout:
        print(markdown)
        return
    out_path = (ROOT / opts.out) if opts.out else (COLLAB / "review-input.md")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(markdown, encoding="utf-8")
    rel = out_path.relative_to(ROOT)
    print(f"✅ 已生成 review 输入包: {rel}")
    print(f"   把它交给 {opts.to or OTHER[opts.sender]}，或让对方直接读这个文件。")
    if verify_result and not verify_result[0]:
        print("⚠️ 闸门失败——交接前请先修红，或在 HANDOFF 里写明为什么带红交接。")
        sys.exit(1)


if __name__ == "__main__":
    main()
