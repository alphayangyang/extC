#!/usr/bin/env python3
"""Shared helpers for the four audit gates (see docs/reviews/COMPILER-AUDIT-2026-09-30.md §7).

Why a module and not four copies: every gate needs the same three things -- the corpus
definition, "generate C for this file", and "compile that C and tell me what went wrong".
When the four gates each carried their own copy, the notion of "which warnings count"
drifted between them, and a gate that counts the wrong warnings is worse than no gate.

The allowlists exist for one reason: the gates must be **green by baseline** so that any
*new* failure is red. Each allowlist entry is `key<TAB>reason<TAB>tracking`; an entry that
no longer fails is reported as STALE and must be deleted (the ratchet only turns one way).
"""
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
from concurrent.futures import ProcessPoolExecutor

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
    """Programs that are meant to run: examples plus the lifetime/sanitizer suites."""
    import glob
    files = sorted(glob.glob(os.path.join(ROOT, "examples/*.extc")))
    for d in ("tests/asan", "tests/arena", "tests/arena-promoted", "tests/traps",
              "tests/ops", "tests/ctor", "tests/impl", "tests/ext"):
        files += sorted(glob.glob(os.path.join(ROOT, d, "*.extc")))
    files = sorted(set(files))
    if scope == "quick":
        files = [f for f in files if os.path.relpath(f, ROOT).startswith("examples/")]
    return files


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
    # The command runs in its own process group, and a timeout kills that group. A gate
    # that times out must not leave anything running: `extc --run` forks the compiled
    # program, so killing only the compiler left one orphan per timed-out case spinning at
    # 100% CPU (measured 2026-10-02: two of them ran for over half an hour).
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, cwd=cwd or ROOT, env=env,
                             start_new_session=True,
                             preexec_fn=_limit_mem(mem_mb) if mem_mb else None)
    except OSError as e:
        return 127, f"(cannot run: {e})"
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


def reap_orphans():
    """Kill and report compiled artifacts still running from the repository.

    Same reason as the one in `tools/parrun.py`: a gate that times out must leave nothing
    behind. Returns the number of processes reaped.
    """
    try:
        out = subprocess.run(["pgrep", "-f", r"^build/[A-Za-z0-9_]+$"],
                             capture_output=True, text=True, cwd=ROOT).stdout
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
            f.write(f"{k}\tbaseline 2026-09-30\t审计 P0 批次\n")
    print(f"wrote {rel(path)} ({len(failures)} entries)")


def jobs():
    n = os.cpu_count() or 4
    return max(2, min(16, n))


def ensure_gatedir():
    os.makedirs(GATEDIR, exist_ok=True)
    return GATEDIR
