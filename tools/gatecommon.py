#!/usr/bin/env python3
"""Shared helpers for the four corpus gates.

Why a module and not four copies: every gate needs the same three things -- the corpus
definition, "generate C for this file", and "compile that C and tell me what went wrong".
Four separate copies drift on the notion of "which warnings count", and a gate that counts
the wrong warnings is worse than no gate.

The allowlists exist for one reason: the gates must be **green by baseline** so that any
*new* failure is red. Each allowlist entry is `key<TAB>reason<TAB>tracking`; an entry whose
failure is fixed is reported as STALE and must be deleted (the ratchet only turns one way).
"""
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor

# 孤儿回收只有一份实现（`tools/parrun.py`，按会话**所有权**回收）。gatecommon 与 parrun 同目录，
# 但调用方可能从别处 import（`python3 -m tools.gate_x`），所以把本目录追加进 sys.path 再 import。
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from parrun import reap_orphans as reap_orphans_scoped          # noqa: E402
from parrun import register_session, unregister_session          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTC = os.environ.get("EXTC_BIN") or os.path.join(ROOT, "build", "extc")
GATEDIR = os.path.join(ROOT, "build", "gate")

# corpora -------------------------------------------------------------------

def corpus(scope):
    """All .extc files, or the quick subset."""
    import glob
    files = []
    for pat in ("tests/**/*.extc", "examples/*.extc", "stdlib/**/*.extc", "bench/**/*.extc"):
        files += glob.glob(os.path.join(ROOT, pat), recursive=True)
    files = sorted(set(files))
    if scope == "quick":
        keep = []
        for f in files:
            rel = os.path.relpath(f, ROOT)
            if rel.startswith("examples/") or rel.startswith("stdlib/"):
                keep.append(f)
        files = keep
    return files


def positive_corpus(scope):
    """Programs that are meant to run: examples plus the lifetime/sanitizer suites.

    `tests/escape-fuzz/cases/` 与 `tests/reclaim-shapes/hole_c/` 是**回收健全性的风险面**：
    前者的夹具全部自足（不用 argv、不读 stdin），后者是 C 洞的最小复现。它们原先只有
    "恒 exit 0 的证据脚本"在看，所以把这个语料纳进来，让 UB/悬垂在 CI 里**必须报**。
    需要 argv 的回收形状（`tests/reclaim-shapes/*.extc`、`tests/reclaim-gaps/shapes/*.extc`）
    还不在这一份里：本闸门不带实参跑程序，塞进来只会得到"用法错误"的噪声。
    """
    import glob
    files = sorted(glob.glob(os.path.join(ROOT, "examples/*.extc")))
    for d in ("tests/asan", "tests/arena", "tests/arena-promoted", "tests/traps",
              "tests/ops", "tests/ctor", "tests/impl", "tests/ext",
              "tests/escape-fuzz/cases", "tests/reclaim-shapes/hole_c"):
        files += sorted(glob.glob(os.path.join(ROOT, d, "*.extc")))
    files = sorted(set(files))
    if scope == "quick":
        files = [f for f in files if os.path.relpath(f, ROOT).startswith("examples/")]
    return files


def escape_corpus():
    """反例语料：闸门⑤（`gate_escape.py`）要求它们**必须被拒绝**。

    ASan 闸门（`gate_asan_corpus.py`）也扫这一份，理由是同一个洞的两面：在编译器还接受它们的
    时候，生成物恰好就是 ASan 该抓到的东西；等编译器改成拒绝，`gate_asan_corpus.check_one` 的
    `gen_c` 会失败并直接跳过（「extC 自己拒绝：不是本闸门的事」），基线里那一条随之变成 STALE，
    照棘轮删掉即可。两个闸门共用一份文件，不复制语料。
    """
    import glob
    return sorted(glob.glob(os.path.join(ROOT, "tools", "escape-corpus", "*.extc")))


def rel(path):
    return os.path.relpath(path, ROOT)


# running -------------------------------------------------------------------

def _limit_mem(mb):
    """preexec_fn: cap the child's address space so a runaway compiler dies cleanly."""
    def f():
        import resource
        lim = mb * 1024 * 1024
        resource.setrlimit(resource.RLIMIT_AS, (lim, lim))
    return f


def asan_env(extra=None):
    """ASan options the corpus needs.

    detect_leaks=0: extC's memory model is "arena, released at process exit" -- bytes still
    owned at exit are by design, not a leak (the compiler's own ASan sweep runs the same way).
    allocator_may_return_null=1: `tests/traps/arena_oom.extc` *intends* to request 1 PB so that
    extC's own "out of arena memory" trap runs; without this ASan aborts first and the trap the
    test is about never executes.
    """
    base = "detect_leaks=0:allocator_may_return_null=1"
    e = dict(os.environ)
    e["ASAN_OPTIONS"] = base + ((":" + extra) if extra else "")
    e["UBSAN_OPTIONS"] = "print_stacktrace=1"
    return e


def run(cmd, timeout=120, cwd=None, env=None, mem_mb=0):
    """Run a command, never raise. Returns (rc, stdout+stderr).

    mem_mb caps the child's address space (0 = no cap). The gates pass a cap because
    "compiler eats all RAM" is exactly the failure this file exists to catch, and an
    OOM that takes the machine down cannot be reported as a red gate.
    """
    # The command runs in its own session, and a timeout kills that whole session. A gate
    # that times out must not leave anything running: `extc --run` forks the compiled
    # program, so killing only the compiler leaves one orphan per timed-out case spinning at
    # 100% CPU.
    #
    # The session is registered for the duration (`tools/parrun.py` owns the registry): a gate
    # killed outright cannot clean up after itself, and a later `reap_orphans()` collects a
    # registered session whose owner is gone while leaving every unregistered process alone.
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, cwd=cwd or ROOT, env=env,
                             start_new_session=True,
                             preexec_fn=_limit_mem(mem_mb) if mem_mb else None)
    except OSError as e:
        return 127, f"(cannot run: {e})"
    register_session(p.pid)
    try:
        out, _ = p.communicate(timeout=timeout)
        return p.returncode, out or ""
    except subprocess.TimeoutExpired:
        _kill_group(p)
        try:
            out, _ = p.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            out = ""
        return 124, ((out or "") + "(timed out)")
    finally:
        unregister_session(p.pid)


def reap_orphans():
    """Collect leftover compiled artifacts belonging to a run that is gone.

    One implementation, in `tools/parrun.py`: it is scoped by **ownership** (registered
    sessions whose owner process is not running), so it never touches a process another
    run started. A name-pattern sweep cannot make that distinction and is not used.
    """
    return reap_orphans_scoped()


def _kill_group(p):
    """Kill the process group `p` leads, then let the caller reap it."""
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except (ProcessLookupError, PermissionError, OSError):
        try:
            p.kill()
        except OSError:
            pass


def gen_c(extc_path, out_c, timeout=120):
    """extC -> C. Returns (True, "") on success, (False, diagnostics) otherwise."""
    rc, out = run([EXTC, "-w", extc_path, "-o", out_c], timeout=timeout)
    if rc != 0 or not os.path.exists(out_c):
        return False, out
    return True, ""


# Warning classes that mean "the generator emitted something it should not have".
# Kept in one place on purpose: `-fsyntax-only` cannot see the middle-end ones
# (`-Warray-bounds`, `-Wmaybe-uninitialized`, `-Wstringop-*`), so every gate that
# compiles generated C must use `-c -O2` and this list.
GEN_WARN = re.compile(
    r"\[-W(uninitialized|maybe-uninitialized|array-bounds|stringop-overflow|"
    r"stringop-overread|stringop-truncation|strict-aliasing|shift-negative-value|"
    r"shift-count-overflow|shift-count-negative|null-dereference|int-conversion|"
    r"incompatible-pointer-types|implicit-function-declaration|div-by-zero|"
    r"tautological-compare|overflow|use-after-free|dangling-pointer|format-overflow)\]")


def compile_c(cfile, cc="gcc", sanitize=False, timeout=180):
    """Compile generated C. Returns (rc, output). `-c -O2` so middle-end warnings fire."""
    if sanitize:
        cmd = [cc, "-std=c11", "-O1", "-g", "-fno-omit-frame-pointer", "-fwrapv",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
               "-pthread", cfile, "-o", cfile[:-2], "-lm"]
    else:
        cmd = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-unused", "-c",
               cfile, "-o", cfile + ".o"]
    return run(cmd, timeout=timeout)


# allowlists ----------------------------------------------------------------

def load_allow(path):
    """{key: (reason, tracking)}; a missing file is an empty allowlist."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            out[parts[0]] = (parts[1] if len(parts) > 1 else "",
                             parts[2] if len(parts) > 2 else "")
    return out


def report(gate, failures, allow, detail_lines=0, eligible=None):
    """Shared verdict logic. `failures` is {key: detail}. Returns exit code.

    Rule: every failure must be in the allowlist, and every allowlist entry **that this
    scope could have produced** must still fail. Anything else is red -- including a fixed
    entry that is still listed, because a baseline that only grows stops being a ratchet.

    `eligible` is the set of keys this run could have reported (the scanned files, or the
    case names). An entry outside it is neither new nor stale: `--scope quick` scans fewer
    files, and without this it would call every full-scope baseline entry "stale".
    """
    new = {k: v for k, v in failures.items() if k not in allow}
    stale = [k for k in allow if k not in failures and (eligible is None or k in eligible)]
    known = len(failures) - len(new)
    print(f"[{gate}] 失败 {len(failures)}（已知 {known} / 新增 {len(new)}）"
          f"，基线 {len(allow)}，过期 {len(stale)}")
    for k in sorted(new):
        print(f"  NEW  {k}")
        if detail_lines:
            for line in str(failures[k]).splitlines()[:detail_lines]:
                print(f"       {line}")
    for k in sorted(stale):
        print(f"  STALE {k}  —— 已不再失败，请从基线删除（棘轮只往一个方向转）")
    if new or stale:
        print(f"[{gate}] FAIL")
        return 1
    print(f"[{gate}] ok（全部失败都在基线里）")
    return 0


def write_allow(path, failures):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("# <key>\t<原因>\t<跟踪>\n")
        for k in sorted(failures):
            f.write(f"{k}\tbaseline 2026-09-30\t未记录\n")
    print(f"wrote {rel(path)} ({len(failures)} entries)")


def jobs():
    n = os.cpu_count() or 4
    return max(2, min(16, n))


def ensure_gatedir():
    os.makedirs(GATEDIR, exist_ok=True)
    return GATEDIR
