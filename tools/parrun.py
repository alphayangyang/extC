#!/usr/bin/env python3
"""Run the case-based test suites in parallel, with deterministic output.

Why this exists
    Every case starts the compiler and then gcc, so the suite is dominated by process
    startup rather than by arithmetic. Run one case at a time and a machine with two
    dozen cores uses one of them; run them all at once and nothing finishes.

Why Python and not job control in bash
    The first attempt used a shell semaphore built on `jobs -rp`. Inside a pipeline
    `jobs` runs in a subshell and reports the subshell's jobs, which are none, so the
    limit never engaged and every case was started at once -- observed from `top` as
    a machine that was busy doing nothing. `concurrent.futures` bounds the pool
    directly, and the pool size is the one number that has to be right.

Determinism
    Results are collected into a list and printed in submission order, not in
    completion order, so two runs of the suite produce byte-identical output and a
    failing run can be compared against the previous one.

Usage
    ./build/extc must exist.
    python3 tools/parrun.py [--jobs N] [--filter SUBSTR] [--verbose]

    Default jobs: min(cpu_count, 16). Not cpu_count itself: the compiler and gcc
    each spawn their own children, and oversubscribing slows the whole suite down.
"""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXTC = ROOT / "build" / "extc"


def sh(cmd, timeout=120):
    """Run one command, merge stderr into stdout, and never raise.

    The command runs in its **own process group**, and a timeout kills the whole group.
    `extc --run` forks the compiled program, so killing only `extc` used to leave that
    program behind: two of them spun at 100% CPU for over half an hour after a `--run`
    case timed out (measured 2026-10-02), one orphan per timed-out case. A hanging case
    must not outlive the test run.
    """
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, start_new_session=True)
    except OSError as e:
        return 127, f"(cannot run: {e})"
    try:
        out, _ = p.communicate(timeout=timeout)
        return p.returncode, out or ""
    except subprocess.TimeoutExpired:
        kill_group(p)
        try:
            out, _ = p.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            out = ""
        return 124, ((out or "") + "(timed out)")


def reap_orphans():
    """Report and kill compiled test programs still running from this directory.

    A case that hangs must not outlive the run: `extc --run` forks the compiled program,
    and before the process-group kill was added, a timed-out case left that program
    spinning forever (measured 2026-10-02: two orphans, 31 and 34 minutes of 100% CPU).
    Anything matched here is by definition a leak, so it is killed and named.
    """
    import signal
    try:
        out = subprocess.run(["pgrep", "-f", r"^build/[A-Za-z0-9_]+$"],
                             capture_output=True, text=True, cwd=str(ROOT)).stdout
    except OSError:
        return 0
    pids = [int(x) for x in out.split()]
    for pid in pids:
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
    if pids:
        print(f"  [leak] 清理了 {len(pids)} 个跑飞的编译产物进程: {pids}")
    return len(pids)


def kill_group(p):
    """Kill the process group `p` leads, then reap it."""
    import signal
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except (ProcessLookupError, PermissionError, OSError):
        try:
            p.kill()
        except OSError:
            pass


def case_positive(f):
    """An example: it must compile, run, and print what its `// expect:` line says."""
    want = ""
    m = re.search(r"// expect:(.*)", f.read_text(encoding="utf-8", errors="replace"))
    if m:
        want = m.group(1).strip()
    rc, out = sh([str(EXTC), "--run", str(f)])
    if rc != 0:
        return False, "编译/运行失败", out
    if not want:
        # 全输出压成一行。**不截断**：截断会让"上一轮与这一轮"的 diff 变成假差异，
        # 而这份输出的用处之一正是"跑两遍对照" ✓
        return True, out.replace("\n", "|").rstrip("|"), ""
    if want in out:
        return True, f"含「{want}」（{len(out.splitlines())} 行输出）", ""
    return False, f"输出里没有「{want}」", out


def case_must_reject(f):
    """A counterexample: the compiler must refuse it, with a source position."""
    rc, out = sh([str(EXTC), str(f)])
    if rc == 0:
        return False, "应该报错但通过了", out
    want_match = re.search(r"(?m)^[ \t]*// expect:[ \t]*(.*?)[ \t]*$",
                           f.read_text(encoding="utf-8", errors="replace"))
    want = want_match.group(1).strip() if want_match else ""
    # 取**第一条 error**，不是第一行：警告（比如 `println` 弃置提醒）也写 stderr，而且
    # 先于错误渲染 ⇒ 拿第一行会把"这条反例到底在说什么"记成一条无关的提醒 ✗
    line = next((l for l in out.splitlines() if ": error:" in l), "")
    if not line:
        return False, "编译器退出但没有带源位置的 error 诊断", out
    if want and want not in out:
        return False, f"诊断未包含「{want}」", out
    msg = line.split(": ", 1)[-1]
    return True, msg, ""


def case_must_trap(f):
    """A runtime trap: it must compile, then die with a located `trap:` message."""
    rc, out = sh([str(EXTC), "--run", str(f)])
    if rc == 124:
        return False, "(超时：既没 trap 也没结束)", out
    m = re.search(r"trap:.*", out)
    if m:
        return True, m.group(0)[:100], ""
    if rc == 0:
        return False, "跑完了但没有 trap", out
    return False, "退出了但没有 trap 消息", out


# 「编译整个语料，看谁吐了某个标记」这一类检查。它有两个实例：
#   - `warning:`  正例语料不许有误报（tests/warnings 的 ②）
#   - `arena!`    检查器算的层号与 codegen 的当前块不许漂（check.sh 的哨兵）
# 两者都是"全量编译一遍"，都是整个套件里最贵的单步（各 ~30s），而且**彼此无依赖、
# 没有断言** ⇒ 交给同一个并行实现 ✓ """
SCAN_MODES = {
    "warn-scan":  ("warning:", "正例语料", "**零警告** ⇒ 不是噪音",
                   ["examples/*.extc", "bench/*/*.extc"], None),
    "arena-scan": ("arena!", "arena 层号哨兵语料", "层号零漂移",
                   ["examples/*.extc", "bench/*/*.extc", "bench/oi/*.extc",
                    "bench/oi/persist/*.extc"], {"EXTC_DBG_ARENA": "1"}),
}


def strip_deprecated(out):
    """拿走**弃置提醒**的整块（头一行 + 它带缩进的源码行/插入符/note），返回 (剩余, 条数)。

    为什么要有这个：`print` / `println` 2026-09-26 起弃置（`io::cout` 是唯一的控制台出口），
    语料里还有几百处要搬。它们**必须看得见**（所以编译器照吐、这里照数），但**不算误报** ——
    这条扫描的判据是"警告通道不是噪音"，而弃置提醒是**故意的**、每处都指向同一句迁移说明 ✓
    搬完之后这个函数和它的调用点一起删掉，判据就回到"零豁免" ✓ """
    kept, n, skipping = [], 0, False
    for line in out.splitlines():
        if "is deprecated" in line:
            n += 1
            skipping = True
            continue
        if skipping and line.startswith("  "):   # 同一块的续行：源码、插入符、note
            continue
        skipping = False
        kept.append(line)
    return "\n".join(kept), n


def case_scan(f, token, env):
    """编译一个文件，只问"输出里有没有这个标记"。

    只看输出、不看退出码：这两个扫描关心的是"编译器说了什么"，不是"编译成没成"
    （语料本身另有套件管成败）✓ """
    e = dict(os.environ)
    if env:
        e.update(env)
    try:
        p = subprocess.run([str(EXTC), str(f), "-o", os.devnull],
                           capture_output=True, text=True, timeout=120, env=e)
        out = (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return False, 0, "(timed out)"
    out, nDep = strip_deprecated(out)
    return (token not in out), nDep, out


def scan_files(globs):
    files = []
    for g in globs:
        files += list(ROOT.glob(g))
    return sorted(set(files))


def case_compiles(f):
    """攻击库：只问"这个文件编译通过了吗"。

    通过与否本身就是判据 —— attack 语料是"已知安全的那几条必须仍然通过，其余必须被挡"，
    所以这里把结果**原样报回去**，由调用方与基线比 ✓ """
    rc, _out = sh([str(EXTC), str(f), "-o", os.devnull])
    return f.stem, ("PASS" if rc == 0 else "REJECT")


def collect(filter_sub):
    """(section title, [(name, callable)]) in the order the suite prints them."""
    ex = sorted((ROOT / "examples").glob("*.extc"))
    er = sorted((ROOT / "tests" / "errors").glob("*.extc"))
    tp = sorted((ROOT / "tests" / "traps").glob("*.extc"))
    groups = [
        ("正例：extC -> C -> gcc -> 运行", ex, case_positive),
        ("反例：必须被编译期挡掉", er, case_must_reject),
        ("反例·运行时：必须 trap（带源码位置）", tp, case_must_trap),
    ]
    out = []
    for title, files, fn in groups:
        cases = []
        for f in files:
            if filter_sub and filter_sub not in f.stem:
                continue
            cases.append((f.stem, fn, f))
        out.append((title, cases))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--filter", default="")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--mode", default="cases",
                    choices=["cases", "compiles"] + sorted(SCAN_MODES),
                    help="cases=三个用例套件；其余=并行编译整个语料查某个标记")
    args = ap.parse_args()

    if not EXTC.exists():
        print(f"parrun: {EXTC} 不存在 —— 先 make", file=sys.stderr)
        return 2

    jobs = args.jobs or min(os.cpu_count() or 4, 16)

    if args.mode == "compiles":
        # 攻击库：打印**通过集合**（每行一个名字，已排序），调用方拿去与 BASELINE 比 ✓
        files = sorted((ROOT / "tests" / "attacks").glob("*.extc"))
        files = [f for f in files if not args.filter or args.filter in f.stem]
        passed = []
        with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
            for name, payload in pool.map(case_compiles, files):
                if payload == "PASS":
                    passed.append(name)
        print("\n".join(sorted(passed)))
        return 0

    if args.mode in SCAN_MODES:
        token, label, verdict, globs, env = SCAN_MODES[args.mode]
        files = [f for f in scan_files(globs)
                 if not args.filter or args.filter in str(f)]
        hits = []
        nDep = 0
        with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
            futs = {pool.submit(case_scan, f, token, env): f for f in files}
            for fut in cf.as_completed(futs):
                good, dep, _extra = fut.result()
                nDep += dep
                if not good:
                    hits.append(futs[fut])
        if hits:
            rel = [str(f.relative_to(ROOT)) for f in sorted(hits)]
            print(f"{label}里有 {len(rel)} 处吐了 `{token}`：" + " ".join(rel))
            return 1
        # 弃置提醒单列：它不是误报，但**要看得见**（每搬一处少一条，搬完这条附注就该消失）✓
        tail = f"（另有 {nDep} 处 `print`/`println` 弃置提醒，迁移中 ⇒ PLAN #67）" if nDep else ""
        print(f"{len(files)} 个{label}：{verdict} ✓{tail}")

    groups = collect(args.filter)

    # 一次提交**全部**用例，让线程池自己调度：用例之间没有依赖，也没有共享可写状态 ✓
    flat = [(title, name, fn, f) for title, cases in groups for name, fn, f in cases]
    results = [None] * len(flat)
    done = 0
    with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
        futs = {pool.submit(fn, f): i for i, (_, _, fn, f) in enumerate(flat)}
        for fut in cf.as_completed(futs):
            i = futs[fut]
            results[i] = fut.result()
            done += 1
            if args.verbose:
                print(f"  …  [{done}/{len(flat)}] {flat[i][1]}", file=sys.stderr)

    ok = bad = 0
    idx = 0
    for title, cases in groups:
        if not cases:
            continue
        print(f"== {title} ==")
        for name, _fn, _f in cases:
            good, detail, extra = results[idx]
            idx += 1
            if good:
                print(f"  \033[32mok\033[0m   {name}  ->  {detail}")
                ok += 1
            else:
                print(f"  \033[31mFAIL\033[0m {name} （{detail}）")
                for line in (extra or "").splitlines()[:8]:
                    print(f"        {line}")
                bad += 1
    reap_orphans()
    print(f"通过 {ok}，失败 {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
