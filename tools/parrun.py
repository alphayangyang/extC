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
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXTC = ROOT / "build" / "extc"

# 每个我们起的子进程都是一个**新会话**（`start_new_session=True`），会话号就是它的 pid。
# 起的时候往这个目录里记一笔 `<sid>` → `<owner pid> <owner 启动时刻>`；正常收尾时删掉。
# 收尾的 `reap_orphans()` 只回收"**登记过、且所有者进程已不存在**"的会话 ——
# 于是它只杀自己的后代，邻居的进程一个都不碰。按进程名扫杀做不到这一点：进程名认不出
# 谁是自己的后代，会把邻居正在跑的 `build/<名字>` 一起杀掉，所以那条路不能用 ✗。
# 目录而不是单文件：每条记录一个文件，多进程并发登记/删除不需要加锁。
REGISTRY = Path(tempfile.gettempdir()) / "extc-case-sessions"


def _proc_starttime(pid):
    """`/proc/<pid>/stat` 里的启动时刻（第 22 个字段）—— 判 pid 有没有被复用。"""
    try:
        with open(f"/proc/{pid}/stat", "rb") as f:
            body = f.read().rsplit(b")", 1)[1].split()
        return int(body[19])
    except (OSError, IndexError, ValueError):
        return None


def _owner_alive(owner, start):
    """所有者还活着吗？连启动时刻一起比 —— pid 被复用时不会把别人的进程当成自己人。"""
    cur = _proc_starttime(owner)
    return cur is not None and (start < 0 or cur == start)


def _session_alive(sid):
    try:
        os.killpg(sid, 0)
        return True
    except OSError:
        return False


def register_session(sid, owner=None):
    """记下"会话 `sid` 属于运行 `owner`"。owner 默认本进程。"""
    owner = os.getpid() if owner is None else owner
    start = _proc_starttime(owner)
    try:
        REGISTRY.mkdir(parents=True, exist_ok=True)
        (REGISTRY / str(sid)).write_text(f"{owner} {start if start is not None else -1}\n")
    except OSError:
        pass


def unregister_session(sid):
    try:
        os.unlink(REGISTRY / str(sid))
    except OSError:
        pass


def sh(cmd, timeout=120, env=None):
    """Run one command, merge stderr into stdout, and never raise.

    The command runs in its **own session**, and a timeout kills that whole session.
    `extc --run` forks the compiled program, so killing only `extc` leaves that
    program behind as one orphan per timed-out case, spinning at 100% CPU. A hanging case
    must not outlive the test run.

    The session is also **registered** for the duration: if this run is killed outright
    (SIGKILL, so no cleanup here can run), a later `reap_orphans()` can still tell the
    leftover session from a live neighbour's and collect it.
    """
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, env=env, start_new_session=True)
    except OSError as e:
        return 127, f"(cannot run: {e})"
    register_session(p.pid)
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
    finally:
        unregister_session(p.pid)


def reap_orphans():
    """Recover only **our own** leftovers, and name them.

    A case that hangs must not outlive the run: `extc --run` forks the compiled program, so a
    timed-out case leaves that program spinning forever when only the compiler is killed, one
    orphan per case. A name-pattern sweep (`pgrep -f '^build/[A-Za-z0-9_]+$'`) cannot tell our
    leftovers from a neighbour's live program -- it kills whatever matches, so a suite sharing
    the machine with other items also kills *their* running compiled programs.

    Ownership instead of a name pattern: every child we start is its own session and is
    recorded with the run that started it (`register_session`). Here we kill exactly the
    recorded sessions whose owner is gone -- a live owner's session, or any process we never
    started, is left alone. A record whose owner is gone but whose session is empty is just
    dropped.
    """
    killed = []
    try:
        names = os.listdir(REGISTRY)
    except OSError:
        return 0
    for name in names:
        rec = REGISTRY / name
        try:
            sid = int(name)
            owner, start = (int(x) for x in rec.read_text().split()[:2])
        except (OSError, ValueError, IndexError):
            unregister_session(name)          # 坏记录：只清理，不杀任何东西
            continue
        if _owner_alive(owner, start):
            continue                          # 别人的（或自己还在跑的）run：一根手指都不许碰
        if _session_alive(sid):
            try:
                os.killpg(sid, signal.SIGKILL)
                killed.append(sid)
            except OSError:
                pass
        unregister_session(name)
    if killed:
        print(f"  [leak] 清理了 {len(killed)} 个**本次/上次自己运行**遗留的进程组: {killed}")
    return len(killed)


def _selftest_reap():
    """`--reap-selftest`：三条语义判据，**对旧实现（按进程名扫杀）为红**。

    ① 没登记过的进程一律不许动（哪怕它长得像编译产物、哪怕它的父进程还活着）；
    ② 登记过、而所有者进程已不存在的会话，必须被收掉；
    ③ 登记过、而所有者进程仍存在的会话，不许动。

    判据用的替身程序叫 `build/reap_selftest_a` 且**不带参数** —— 名字落在
    `^build/[A-Za-z0-9_]+$` 里，任何"按进程名扫杀"的实现都会在①③上变红，判据因此有牙。
    """
    import shutil
    prog = ROOT / "build" / "reap_selftest_a"
    tmp = Path(tempfile.mkdtemp(prefix="reap-selftest-"))
    (tmp / "s.c").write_text("#include <unistd.h>\nint main(void){sleep(300);return 0;}\n")
    cc = os.environ.get("CC", "cc")
    built = subprocess.run([cc, "-O0", "-o", str(prog), str(tmp / "s.c")],
                           capture_output=True, text=True)
    bad = 0

    def report(ok, what, detail=""):
        nonlocal bad
        print(("  ok   " if ok else "  \033[31mFAIL\033[0m ") + what + (f" —— {detail}" if detail else ""))
        if not ok:
            bad += 1

    def spawn():
        """起一个长命替身：cmdline 正好是 `build/reap_selftest_a`（不带参数），独立会话。"""
        return subprocess.Popen(["build/reap_selftest_a"], cwd=str(ROOT),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                start_new_session=True)

    def dead(p, secs):
        """等它真死。SIGKILL 是异步的，紧接着 `poll()` 会读到 None（判据假绿）。"""
        try:
            p.wait(timeout=secs)
            return True
        except subprocess.TimeoutExpired:
            return False

    def session_gone(sid, secs=2.0):
        """等会话真清空。僵尸进程还挂在组里，`killpg(sid, 0)` 会短暂成功。"""
        deadline = time.monotonic() + secs
        while time.monotonic() < deadline:
            if not _session_alive(sid):
                return True
            time.sleep(0.05)
        return not _session_alive(sid)

    if built.returncode != 0:
        print(f"  FAIL 编译替身程序失败：{built.stderr.strip()[:200]}")
        shutil.rmtree(tmp, ignore_errors=True)
        return 1

    # ① 别人的活进程：**不登记**（模拟另一个工具/另一棵树起的进程）⇒ 不许碰
    other = spawn()
    killed = reap_orphans()
    alive = not dead(other, 0.5)
    report(alive, "① 无关的、父进程还活着的进程没有被杀",
           f"reap_orphans() 返回 {killed}；它{'仍活着' if alive else '被杀了'}")

    # ② 所有者会消失的登记会话：fork 一个"运行"出来，它登记会话、起子进程、然后立刻退出
    rfd, wfd = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(rfd)
        try:
            p = spawn()
            register_session(p.pid, os.getpid())
            os.write(wfd, str(p.pid).encode())
            os.close(wfd)
        finally:
            os._exit(0)
    os.close(wfd)
    sid = int(os.read(rfd, 64).decode())
    os.close(rfd)
    os.waitpid(pid, 0)
    killed = reap_orphans()
    gone = session_gone(sid)
    report(gone, "② 所有者已结束的登记会话被收掉",
           f"sid={sid}，reap_orphans() 返回 {killed}；会话{'已清空' if gone else '还在'}")

    # ③ 自己正在跑的 run：登记了但 owner 还活着 ⇒ 不许碰
    mine = spawn()
    register_session(mine.pid, os.getpid())
    killed = reap_orphans()
    alive = not dead(mine, 0.5)
    report(alive, "③ 自己正在跑的 run 的进程组没有被误收",
           f"reap_orphans() 返回 {killed}；它{'仍活着' if alive else '被杀了'}")

    # 收尾：把自检自己起的东西清干净（三条判据都不依赖它）
    for p in (other, mine):
        try:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            pass
    unregister_session(mine.pid)
    if _session_alive(sid):
        try:
            os.killpg(sid, signal.SIGKILL)
        except OSError:
            pass
    for f in (prog,):
        try:
            os.unlink(f)
        except OSError:
            pass
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"reap 语义自检：{'通过' if not bad else f'失败 {bad}'}")
    return 1 if bad else 0


def kill_group(p):
    """Kill the process group `p` leads, then reap it."""
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
    # 走 `sh()`：会话 + 登记（同 cases 档），超时按进程组杀，跑飞的编译不许赖着。
    rc, out = sh([str(EXTC), str(f), "-o", os.devnull], env=e)
    if rc == 124 and out.endswith("(timed out)"):
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
    ap.add_argument("--reap-selftest", action="store_true",
                    help="只跑孤儿回收的三条语义判据（别人的活进程不许动 / 自己的遗留要收 / "
                         "自己正在跑的不许动），不跑用例")
    args = ap.parse_args()

    if args.reap_selftest:
        return _selftest_reap()

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
        tail = f"（另有 {nDep} 处 `print`/`println` 弃置提醒）" if nDep else ""
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
