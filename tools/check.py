#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""extC 自检编排层（唯一入口）。

架构：**检查项是数据** —— 每项 = 名字 + 组(tag) + 要跑的命令（或一个小的 Python 回调）
+ 超时 + 一行理由；runner 负责：按名字/组选择、逐个跑子进程、逐项计时、流式打印、
统计 ok/FAIL、末尾打印最慢的 N 项、把耗时表写进 $SEC_TIMES（默认
/tmp/extc-check-times.tsv）。

用法（`./check.sh` 现在只是 exec 到本文件的 shim，README / CI 里的用法不变）：
    ./check.sh                 全部（含基准；几十分钟）
    ./check.sh quick           测试 + 攻击库，跳过基准
    ./check.sh docs            只跑文档/工具项（秒级）
    ./check.sh -j 8 nobench    并行跑（默认 min(4, nproc)；`-j1` = 逐项串行）
    ./check.sh --only <名或组>  可多次；名字或组都行
    ./check.sh --list          名字 + 组 + 基准标记 + 最近一次耗时 + 一行理由
    ./check.sh --list nobench  按档位过滤（基准项默认隐藏；`--all` 看全部）
    ./check.sh --changed       看 git diff --name-only HEAD + 未跟踪文件 ⇒ 说该跑哪档
    ./check.sh --changed --run 直接跑它建议的那档

并行只换**墙钟**、不换**判据**：自己开线程池/进程池的项（`INNER`）走**单槽车道**（彼此串行，
可与池并发）；墙钟敏感项（`WALL_CLOCK`）进池但优先取 worker（j ≤ nproc ⇒ 各自独占一个核）；
写固定 `build/<名字>` 的项（`COLLIDE`）**彼此之间**串行（一把锁）⇒ `extc --run` 不跨项撞车。
每项输出攒完整块打印（不交错）。`-j1` 精确回到逐项串行的旧行为。

选择不到任何项 = **响亮报错**（列名字/组），不静默跳过。

跳过协议（`skip` 与 `ok` 是两回事）：某项的子脚本**自己**发现环境缺件（如本机没有 valgrind）时，
打印一行 `  --   跳过：<原因>` 并退出 0 ⇒ 该项记 `skip`、**不计数**，但汇总里单列「跳过 N」——
绝不让"没验"混进"通过"。CI 另有一条相反方向的判据：装了 valgrind 却仍跳过 ⇒ CI 失败。
"""

from __future__ import annotations

import argparse
import concurrent.futures
import difflib
import os
import queue
import re
import shlex
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Iterable, Optional

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEC_TIMES_DEFAULT = "/tmp/extc-check-times.tsv"
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def sh(cmd: str) -> list:
    """把一条命令写成原来 check.sh 里的样子（如 `python3 tools/<工具>.py --gate`）再切成 argv。"""
    return shlex.split(cmd)


# 编排层自身：--changed 不拿它们判断"该跑哪档"（改 runner 不是改了被检查对象；
# 若只有它们变了，建议仍是 quick —— runner 自己值得跑一遍）。
SELF_FILES = {"check.sh", "tools/check.py", "tools/check-coverage.tsv"}


# --------------------------------------------------------------------------
# 小工具：复刻 check.sh 里那几种 shell 取数方式（tail -1 / head -1 / grep -c ...）
# --------------------------------------------------------------------------
def tail1(s: str) -> str:
    """`echo "$out" | tail -1`"""
    return s.rstrip("\n").split("\n")[-1]


def head1(s: str) -> str:
    """`printf '%s' "$out" | head -1`"""
    return s.split("\n")[0]


def lines(s: str) -> list[str]:
    return s.rstrip("\n").split("\n") if s else []


def head(s: str, n: int) -> list[str]:
    return lines(s)[:n]


def tail(s: str, n: int) -> list[str]:
    return lines(s)[-n:]


def count_matching(s: str, pat: str) -> int:
    """`grep -c PAT`（按行计；PAT 同 ripgrep/GNU grep 正则）"""
    rx = re.compile(pat)
    return sum(1 for ln in s.split("\n") if rx.search(ln))


def first_substring(s: str, pat: str) -> str:
    """`grep -o PAT | head -1`（只要匹配到的子串，不是整行）"""
    m = re.search(pat, s)
    return m.group(0) if m else ""


def first_matching(s: str, pat: str) -> str:
    """`grep -m1 PAT`（返回第一行，命中不到返回空串）"""
    rx = re.compile(pat)
    for ln in s.split("\n"):
        if rx.search(ln):
            return ln
    return ""


def skip_line(out: str) -> str:
    """子脚本的跳过协议：命中 `  --   跳过：<原因>` ⇒ 返回 `跳过：<原因>`（否则空串）。

    `skip` 不是 `ok`：脚本缺件时**明说没验**，runner 记 skip、不计数（见文件头）。
    """
    ln = first_matching(out, r"^  --   跳过：")
    if not ln:
        return ""
    return "跳过：" + ln.split("跳过：", 1)[1].strip()


def line_count(s: str) -> int:
    """`echo "$out" | wc -l`（注意 shell 那个 echo 会补一个换行）"""
    return len(s.rstrip("\n").split("\n"))


def ind(ls: Iterable[str], prefix: str = "  ") -> list[str]:
    return [prefix + ln for ln in ls]


def sed_range(ls: list[str], start_pat: str, end_pat: Optional[str]) -> list[str]:
    """复刻 `sed -n '/START/,/END/p'`（END 为空 = 到文件尾）"""
    out: list[str] = []
    on = False
    sx = re.compile(start_pat)
    ex = re.compile(end_pat) if end_pat else None
    for ln in ls:
        if not on and sx.search(ln):
            on = True
        if on:
            out.append(ln)
            if ex is not None and len(out) > 1 and ex.search(ln):
                break
    return out


# --------------------------------------------------------------------------
# 子进程执行（流式）
# --------------------------------------------------------------------------
@dataclass
class Proc:
    rc: int
    out: str          # merge=True 时 = stdout+stderr
    err: str          # merge=False 时的 stderr，否则空
    secs: float
    timed_out: bool


def _pump(tag, stream, q: "queue.Queue", bufs: dict) -> None:
    try:
        for ln in stream:
            bufs[tag].append(ln)
            q.put((tag, ln))
    finally:
        q.put((tag, None))


def run_proc(argv: list, timeout: Optional[float] = None,
             env: Optional[dict] = None, merge: bool = True,
             echo: bool = True, quiet: bool = False,
             capture: Optional[list] = None) -> Proc:
    """跑一个子进程。

    `capture`：不空时，子进程的输出**逐行追加到它**（而不是写给 stdout）—— 并行模式下每项
    自己攒一份，跑完整体打印，于是**各项输出不会交错**。`echo=False`（调用方主动噤声）时
    两个去处都不写。
    """
    full_env = os.environ.copy()
    if env:
        full_env.update(env)
    t0 = time.monotonic()
    proc = subprocess.Popen(
        argv, cwd=ROOT, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT if merge else subprocess.PIPE,
        env=full_env, text=True, encoding="utf-8", errors="replace", bufsize=1,
    )
    q: "queue.Queue" = queue.Queue()
    bufs: dict = {}
    streams = [("out", proc.stdout)]
    if not merge:
        streams.append(("err", proc.stderr))
    for tag, st in streams:
        bufs[tag] = []
        threading.Thread(target=_pump, args=(tag, st, q, bufs), daemon=True).start()

    deadline = None if timeout is None else t0 + timeout
    hard_deadline: Optional[float] = None
    timed_out = False
    pending = len(streams)
    while pending > 0:
        wait: Optional[float] = None
        if hard_deadline is not None:
            wait = max(0.0, hard_deadline - time.monotonic())
            if wait <= 0:
                break
        elif deadline is not None:
            wait = deadline - time.monotonic()
            if wait <= 0:
                timed_out = True
                proc.kill()
                hard_deadline = time.monotonic() + 5.0
                continue
        try:
            _tag, ln = q.get(timeout=wait)
        except queue.Empty:
            if hard_deadline is not None:
                break
            timed_out = True
            proc.kill()
            hard_deadline = time.monotonic() + 5.0
            continue
        if ln is None:
            pending -= 1
            continue
        if echo and not quiet:
            if capture is not None:
                capture.append(ln)
            else:
                sys.stdout.write(ln)
                sys.stdout.flush()

    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    rc = 124 if timed_out else (proc.returncode if proc.returncode is not None else 1)
    return Proc(rc, "".join(bufs["out"]), "".join(bufs.get("err", [])),
                time.monotonic() - t0, timed_out)


# --------------------------------------------------------------------------
# 项与结果
# --------------------------------------------------------------------------
@dataclass
class Outcome:
    ok: Optional[bool]          # None = 不计数（display / skip）
    msg: str = ""
    extra: list = field(default_factory=list)


@dataclass
class Item:
    name: str
    group: str
    desc: str
    fn: Callable[["Runner", Item], Outcome]
    needs_build: bool = True
    bench: bool = False
    abort_on_fail: bool = False
    counts: bool = True


class Runner:
    def __init__(self, quiet: bool = False, capture: Optional[list] = None,
                 env: Optional[dict] = None):
        self.quiet = quiet
        self.capture = capture
        self.env = env or {}          # 逐项固定环境（如私有的 EXTC_RUN_DIR），调用点的 env 覆盖它

    def run(self, argv, timeout=None, env=None, merge=True, echo=True) -> Proc:
        full = dict(self.env)
        if env:
            full.update(env)
        return run_proc(argv, timeout=timeout, env=full or None, merge=merge,
                        echo=echo, quiet=self.quiet, capture=self.capture)


# --------------------------------------------------------------------------
# 通用项构造器
# --------------------------------------------------------------------------
def _resolve_msg(spec, out: str) -> str:
    if callable(spec):
        return spec(out)
    return spec


def cmd(name: str, group: str, desc: str, argv: list, *,
        timeout: Optional[float] = None, env: Optional[dict] = None,
        needs_build: bool = True, bench: bool = False,
        ok_msg=None, bad_msg: Optional[str] = None,
        bad_dump: Optional[Callable[[str], list]] = None,
        extra_ok: Optional[Callable[[str], list]] = None,
        echo: bool = True, abort_on_fail: bool = False) -> Item:
    """一条命令 = 一项：rc==0 且没超时 ⇒ ok；否则 FAIL。"""
    def fn(ctx: Runner, it: Item) -> Outcome:
        p = ctx.run(argv, timeout=timeout, env=env, echo=echo)
        if p.rc == 0:
            sk = skip_line(p.out)      # 子脚本自报"没验"（见文件头）⇒ 记 skip、不计数
            if sk:
                return Outcome(None, sk)
            extra = extra_ok(p.out) if extra_ok else []
            return Outcome(True, _resolve_msg(ok_msg, p.out) if ok_msg is not None else tail1(p.out), extra)
        msg = bad_msg or name
        dump = bad_dump(p.out) if bad_dump else []
        if p.timed_out:
            msg = f"{msg}（超时 {timeout:g}s 被杀 ⇒ FAIL）"
        else:
            # rc 与输出行数挂在判决行上：子脚本"自己的统计说全过、rc 却不是 0"这类现场
            # （尾部只剩统计行）不看 rc 根本判不出来。
            msg = f"{msg}（rc={p.rc}，{len(lines(p.out))} 行输出）"
        return Outcome(False, msg, dump)
    return Item(name, group, desc, fn, needs_build=needs_build, bench=bench,
                abort_on_fail=abort_on_fail)


def ok_const(s: str):
    return lambda _out: s


def ok_out(out: str) -> str:
    """ok "$out" —— 命令替换会剥掉尾部换行"""
    return out.rstrip("\n")


def ok_count_ok(suffix: str = ""):
    return lambda out: f"{count_matching(out, '^  ok')}{suffix}"


def ok_first(pat: str):
    return lambda out: first_matching(out, pat)


def ok_linecount(suffix: str = ""):
    return lambda out: f"{line_count(out)}{suffix}"


def dump_tail(n: int):
    return lambda out: ind(tail(out, n))


def dump_fail(n: int, fails: int = 4):
    """失败现场：判据脚本自己打的 `  FAIL …` 行（**可能不在尾部**）+ 尾部 n 行。

    为什么不能只取尾部：闸门脚本的失败行常出现在输出前段（后面还有逐形状的 ok 与统计），
    只取尾部会把真正的原因切掉 —— CI 上日志留在远端、注解又只有 10 条配额，
    所以这几行必须自己冒出来（见 tools/ci_annotate.py 的文件头）。
    """
    def f(out: str) -> list:
        ls = lines(out)
        badge = [ln for ln in ls if ln.startswith("  FAIL")]
        rest = tail(out, n)
        if not badge:
            return ind(rest)
        # 首尾各取几条：闸门的判决行有时在前面（一开头就生成输入失败），有时在最后
        # （逐形状跑完才汇总），只取一头就会漏。
        picks: list[str] = []
        for ln in badge[:fails // 2 + fails % 2] + badge[-(fails // 2):]:
            if ln not in picks:
                picks.append(ln)
        extra = [] if len(badge) <= len(picks) else [f"…（共 {len(badge)} 条判据 FAIL 行）"]
        return ind(picks + extra) + ind([f"--- 尾部 {len(rest)} 行 ---"]) + ind(rest)
    return f


def dump_head(n: int):
    return lambda out: ind(head(out, n))


def dump_all(out: str) -> list:
    return ind(lines(out))


# --------------------------------------------------------------------------
# 逐条从 check.sh 搬过来的检查项（顺序与 check.sh 一致）
# --------------------------------------------------------------------------
def build_items() -> list:
    items: list = []

    # ---- 构建（失败 = 整体中止，所有其它项都假设 build/extc 在）----
    def fn_build(ctx: Runner, it: Item) -> Outcome:
        log = "/tmp/extc-build.log"
        p = ctx.run(["make", "-s"], echo=False)
        with open(log, "w", encoding="utf-8") as f:
            f.write(p.out)
        if p.rc == 0:
            return Outcome(True, "make")
        return Outcome(False, "make", ind(lines(p.out)))
    items.append(Item(
        "build", "build",
        "构建：`make -s`（写 /tmp/extc-build.log；失败即打印日志并整体中止 —— 其余项都假设 build/extc 在）",
        fn_build, needs_build=True, abort_on_fail=True))

    # ---- 测试（例子 / 反例 / trap）----
    def fn_tests(ctx: Runner, it: Item) -> Outcome:
        p = ctx.run(["./tests/run.sh"], timeout=600)
        if p.rc == 0:
            return Outcome(True, tail1(p.out))
        plain = ANSI_RE.sub("", p.out)
        extra = [ln for ln in plain.split("\n") if ln.startswith("  FAIL")]
        if not extra:
            return Outcome(True, tail1(p.out) + "（只差 field-strong-update —— 见 KNOWN-ISSUES.md ✓）")
        return Outcome(False, "tests/run.sh", ind([ln for ln in p.out.split("\n") if "FAIL" in ln][:5]))
    items.append(Item(
        "tests", "core",
        "例子/反例/trap 全量；已知误拒只许以一条已记档失败出现，白名单现在是空的，多出任何别的失败即红",
        fn_tests))

    # ---- 闸门·规模 ----
    items.append(cmd(
        "gate-scale", "gate",
        "闸门·规模：任何输入都要给结论（不许 SIGSEGV / 不许挂住）；基线 tools/gate-scale-known-bad.txt 只许缩小",
        sh('python3 tools/gate_scale.py'), timeout=600,
        ok_msg=ok_first(r"^\[scale\]"),
        bad_msg="tools/gate_scale.py（规模用例变红）",
        bad_dump=dump_fail(12)))

    # ---- 断言版全量（EXTC_DBG=1）+ 闸门⑤ ----
    def fn_assert(ctx: Runner, it: Item) -> Outcome:
        p = ctx.run(["./tests/run.sh"], timeout=1800, env={"EXTC_DBG": "1"})
        if p.rc != 0:
            return Outcome(False,
                           "断言版 tests/run.sh 变红（断言或兜底被走到 ⇒ 见上面的 [assert]/[fallback]）",
                           ind([ln for ln in p.out.split("\n") if re.search(r"assert\]|fallback\]", ln)][:5]))
        esc = ctx.run(sh('python3 tools/gate_escape.py'), timeout=600,
                      env={"EXTC_DBG": "1"}, echo=False)
        esc_last = tail1(esc.out)
        if re.search(r"^\[escape\] ok", esc_last) and "失败 0" in tail1(p.out):
            notes = count_matching(p.out, r"\[note\]")
            return Outcome(True,
                           f"断言版：{tail1(p.out)}；闸门⑤ ok；断言/兜底 0 命中；已知活路径 note×{notes}")
        return Outcome(False, f"断言版跑出问题：{esc_last}",
                       ind([ln for ln in p.out.split("\n") if re.search(r"assert\]|fallback\]", ln)][:5]))
    items.append(Item(
        "tests-assert", "gate",
        "断言版全量：EXTC_DBG=1 跑 tests/run.sh（断言/兜底被走到即 abort），再验闸门⑤ gate_escape",
        fn_assert))

    # ---- 闸门·逃逸健全性 ----
    items.append(cmd(
        "gate-escape", "gate",
        "闸门·逃逸健全性：记账比真实情况小的语料必须被拒；control_* 孪生对照永远不许进基线",
        sh('python3 tools/gate_escape.py'), timeout=600,
        ok_msg=ok_first(r"^\[escape\]"),
        bad_msg="tools/gate_escape.py（逃逸健全性语料变红）",
        bad_dump=dump_fail(12)))

    # ---- 闸门·实例接线 ----
    items.append(cmd(
        "gate-callsite", "gate",
        "闸门·实例接线：每个被发出的实例必须调对它该调的那个（生成物结构判据）；基线 = 施工单，只许减",
        sh('python3 tools/gate_callsite.py'), timeout=600,
        ok_msg=ok_first(r"^\[callsite\]"),
        bad_msg="tools/gate_callsite.py（实例接线变红）",
        bad_dump=dump_fail(12)))

    # ---- 闸门·差分 ----
    items.append(cmd(
        "gate-difffuzz", "gate",
        "闸门·差分：extC 与等价 C 必须给出同一结论（挡静默算错）；基线 tools/gate-diff-known-bad.txt 只许缩小",
        sh('python3 tools/gate_difffuzz.py'), timeout=600,
        ok_msg=ok_first(r"^\[diff\]"),
        bad_msg="tools/gate_difffuzz.py（差分用例变红）",
        bad_dump=dump_fail(12)))

    # ---- 闸门·回收的因果闭合（清扫真的压住了峰值）----
    # 这条钉的是"界是谁给的"：同一份生成物只改清扫触发阈值 ⇒ 峰值必须重新随轮数涨；
    # 关档也必须涨（否则 run-switch.sh ①b 的对比没牙）。约 5 秒，所以进 quick。
    items.append(cmd(
        "gate-reclaim-evidence", "gate",
        "闸门·回收因果闭合：关档涨 / 开档平 / 清扫永不触发涨（三方对照）+ gdb 数清扫次数",
        ["./tests/reclaim-shapes/run-evidence.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（三种变体语义逐字一致 · 开档 < 1.5× · 关档与永不触发都 > 2×）"),
        bad_msg="tests/reclaim-shapes/run-evidence.sh（清扫的作用不再可证）",
        bad_dump=dump_fail(20)))

    # ---- 闸门·C 洞健全性（先赤后修的那一支；本帧拥有的区域才允许触发清扫）----
    items.append(cmd(
        "gate-hole-c", "gate",
        "闸门·C 洞：内层帧不得扫外层帧的家区域（开/关档输出逐字 + ASan 干净 + 生成物里分配点不发触发）",
        ["bash", "tests/reclaim-shapes/hole_c/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（开档 rc=0 · ASan 无 UAF · 关档对照绿 · 分配点不发触发 · 压力线前提）"),
        bad_msg="tests/reclaim-shapes/hole_c/run.sh（C 洞复发：种子不完整时下手了）",
        bad_dump=dump_fail(14)))

    # ---- 闸门·回收性能（入口守卫的指令数上限）----
    # 判据线 = 改动前实测 × 0.6；`gate.sh` 里写清了为什么是 0.6（根表物化占 56.1%）。
    items.append(cmd(
        "gate-reclaim-perf", "gate",
        "闸门·回收性能：cyclic_pressure N=200000 的 callgrind 总指令数 ≤ 现行线（线在 gate.sh 里随落地收紧）",
        ["./tests/reclaim-perf/gate.sh"], timeout=900,
        ok_msg=ok_first(r"^  ok"),
        bad_msg="tests/reclaim-perf/gate.sh（指令数超上限：回收开销被付回了）",
        bad_dump=dump_fail(10)))

    # ---- 闸门·分配路径的缩放（每分配 O(1)，不随存活 slab 数涨）----
    # 判据：同一探针在 N 与 4N 两档上，callgrind 每分配指令数之比 ≤ 1.15（指令数是确定量，
    # 无墙钟噪声）。探针用 -DEXTC_RC_SLAB_BITS=14 压小 slab，让旧「热缓存失手 ⇒ 线性走链」
    # 的 O(存活 slab) 在小规模下就显露；--selftest 注入旧扫描形状要求判据变红（有牙）。
    # 数字（旧实现 35.7 s / 5.2 s → 亚秒）记在 changelog，不在这里副本。
    items.append(cmd(
        "gate-slab-scale", "gate",
        "闸门·分配缩放：发射运行期上 N vs 4N 的每分配指令数比 ≤ 1.15、绝对值 ≤ 400，且注入旧扫描会变红",
        ["./tests/reclaim-perf/gate-slab-scale.sh", "--selftest"], timeout=900,
        ok_msg=ok_first(r"^  ok   分配路径缩放成立"),
        bad_msg="tests/reclaim-perf/gate-slab-scale.sh（每分配成本随存活 slab 数涨，或判据失去牙）",
        bad_dump=dump_fail(12)))

    # ---- 闸门·两张按结构体规模存活的表的缩放（own 登记表、每 slab 小类型表）----
    # 判据：own 表每次 add+del 在 S=1024 与 4096 两档的 callgrind 指令数比 ≤ 2.0、绝对值
    # ≤ 800；小类型表每次 typeIdx 在 T=8 与 256 两档之比 ≤ 2.0、绝对值 ≤ 200。--selftest
    # 分别注入「整表线性探测」与「类型表哈希分支短路」两种线性形状，要求判据变红（有牙）。
    items.append(cmd(
        "gate-reclaim-tables", "gate",
        "闸门·登记表/类型表缩放：own 每次 add+del 与 typeIdx 每次查找的成本不随表规模涨，且注入线性形状会变红",
        ["./tests/reclaim-perf/gate-reclaim-tables.sh", "--selftest"], timeout=900,
        ok_msg=ok_first(r"^  ok   登记表维护成本与存活 slab 数无关"),
        bad_msg="tests/reclaim-perf/gate-reclaim-tables.sh（表维护/查找退化成线性，或判据失去牙）",
        bad_dump=dump_fail(12)))

    # ---- 闸门·标记遍历的访存局部性（批量弹出是**延迟**机制，只能拿 cache-sim 守）----
    # 判据：P5（N=500000、固定种子）的 cache-sim 末级读失手 DLmr ≤ 190000、一级数据读失手
    # D1mr ≤ 700000。批量弹出拆掉后**指令数反而变少**（markStep 里那一处 ownSlab 被内联），
    # DLmr 却涨 4×、墙钟慢 4–6× ⇒ 指令数棘轮与墙钟各管一件事，这条专门守延迟那一半。
    # --selftest 把批量填充换成短批（_lim = 1）要求判据变红。具体数字记在 gate 脚本与 changelog。
    items.append(cmd(
        "gate-mark-locality", "gate",
        "闸门·标记局部性：P5 的 cache-sim DLmr ≤ 190000、D1mr ≤ 700000（批量弹出不许被拆），且注入短批会变红",
        ["./tests/reclaim-perf/gate-mark-locality.sh", "--selftest"], timeout=900,
        ok_msg=ok_first(r"^  ok   标记遍历的访存局部性成立"),
        bad_msg="tests/reclaim-perf/gate-mark-locality.sh（标记遍历的局部性退化，或判据失去牙）",
        bad_dump=dump_fail(12)))

    # ---- 闸门·回收开关的形状断言（EXTC_RECLAIM=1）----
    # 这是 `tests/reclaim-shapes/` 的**主门禁**：30 条断言把一个旋钮（开关）的效果钉在
    # 形状上 —— 语义逐字一致、以及每个形状的 N vs 4N 峰值/登记量棘轮（<1.5×）。
    # 它长期没进 check.py（只在手工跑），于是一类"回收不生效 ⇒ 峰值随轮数涨"的回归
    # 从 `storeRefOwned` 的宿主兜底里溜了进来（见 CHANGELOG）。门禁不进电池 = 没有门禁。
    items.append(cmd(
        "gate-reclaim-switch", "gate",
        "闸门·开关形状断言（run-switch.sh 30 条）：语义一致 + 每个形状 N vs 4N 峰值/登记量 <1.5×",
        ["./tests/reclaim-shapes/run-switch.sh"], timeout=1800,
        ok_msg=ok_first(r"^  ok"),
        bad_msg="tests/reclaim-shapes/run-switch.sh（回收开关的形状/内存界回归）",
        bad_dump=dump_fail(12)))

    # ---- 事件档（计数开 + 清扫关）：**诊断档，判据不进电池** ----------------------------
    # 实测判定：它在语料里没有一个能赢的形状 —— ① 有环形状上不成立（强制用在 `ring_per_round`
    # 上：N=8000 涨到 8 369 MB）；② 无环形状上计数不完备（局部绑定只加不减）会漏；
    # ③ 就算不漏，计数的代价也是清扫侧的 +100%~+455%。三条都在
    # `tests/reclaim-perf/TAXMAP.md` 第 7 节。机制与语料**保留**（`EXTC_RC_SWEEPFREE` 仍是
    # 默认关的诊断开关，见 main.c 的 --help），门禁摘出电池：
    # `tests/reclaim-acyclic/gate-acyclic.sh` 自报 `# manual:`；原先那条
    # `gate-event-bounded` 已删除（它钉的正是这条已知坏的路径）。

    # ---- 闸门·idle 投影（规则 P）：把"纯税族"判 idle 的**红判据** ---------------------
    # 目标形状（建好—遍历—块退出整片丢）现在付 1.0–2.5× 墙钟却不省内存；要放宽的规则见
    # `tests/reclaim-perf/IDLE-PROJECTION-RECON.md`。判据两侧：
    #   · 红集（9 个形状）必须判 idle（默认档与关档**逐字节相同**）—— 今天全红，入已知欠账；
    #   · 护栏（真的丢引用的 6 个形状）**不许** idle，且默认档 N vs 4N 峰值 <1.5×、语义一致
    #     —— 两侧今天都成立（有牙）；护栏翻红是安全问题，不是欠账。
    items.append(cmd(
        "gate-idle-projection", "gate",
        "闸门·idle 投影：目标形状必须判 idle（逐字节=关档）+ 护栏必须仍付钱/有界/语义一致",
        ["python3", "tests/reclaim-perf/gate-idle-projection.py"], timeout=1200,
        ok_msg=ok_first(r"^通过（已知欠账"),
        bad_msg="tests/reclaim-perf/gate-idle-projection.py（idle 判据：护栏被放行 / 目标未转绿 / 语义或界）",
        bad_dump=dump_fail(14)))

    # ---- 闸门·覆盖点定点释放（环垃圾不等清扫）------------------------------------
    # 形状 `ring_per_round`：每轮建一条 256 结点的闭环、环头存进长命槽 `keep`，下一轮覆盖
    # ⇒ 判据：清扫次数 ≤ 1（第 1 次是第 2 轮循环头的启动空扫）、N vs 4N 峰值 < 1.5×、
    # 语义与关档逐字相同、**释放槽数 > 0**（防空转）。红证明 = 注入 `release-off`。
    items.append(cmd(
        "gate-release", "gate",
        "闸门·定点释放：ring_per_round 清扫 ≤1 + N vs 4N 峰值 <1.5× + 语义一致 + 释放槽数 >0",
        ["bash", "tests/reclaim-gaps/gate-release.sh"], timeout=600,
        ok_msg=ok_first(r"^通过 "),
        bad_msg="tests/reclaim-gaps/gate-release.sh（定点释放失效：清扫回来 / 界变坏 / 空转）",
        bad_dump=dump_fail(12)))

    # ---- 元判据·判据自己：自称门禁的脚本必须在电池可达闭包里 ----
    # 起因：`run-switch.sh` 自称门禁却从没进 check.py，于是"回收不生效 ⇒ 峰值随轮数涨"
    # 的一整类回归在 nobench 全绿的情况下溜了进来。判据不进电池 = 没有判据。
    items.append(cmd(
        "gate-wiring", "gate",
        "元判据·接线：每个 run*/gate* 脚本要么在电池可达闭包里，要么文件开头自报 `# manual: 理由`",
        ["python3", "tools/check_gate_wiring.py"], timeout=120,
        ok_msg=ok_first(r"^\[wiring\] ok"),
        bad_msg="tools/check_gate_wiring.py（有自称门禁的脚本没进电池）",
        bad_dump=dump_fail(8)))

    # ---- 元判据·树卫生：runner 的裸转储不许入库 ----
    # 起因：`tests/reclaim-gaps/out/*.on.c` 这类转储入库 2.5 MB，复跑一次就要 `git checkout --`
    # 还债。光撤文件不够：`.gitignore` 不写，下一次 `run-gaps.sh` 就把它们加回来 —— 所以判据
    # 同时钉住"没入库"与"被忽略"。
    items.append(cmd(
        "check-tree", "gate",
        "元判据·树卫生：跑出来的裸转储不许入库，且 `.gitignore` 必须覆盖它们",
        ["python3", "tools/check_tree.py", "--gate"], timeout=60, needs_build=False,
        ok_msg=ok_first(r"^ok "),
        bad_msg="tools/check_tree.py（有转储入库，或 `.gitignore` 漏了模式）",
        bad_dump=dump_fail(10)))

    # ---- 元判据·验牙注入不许没写进文档 ----
    # 两张表（义务表 / 跳过表）的"判据"列引用的是 `tools/inject.py` 里的注入名。表的价值取决于
    # 引用完整：新写一条注入却不进表，读表的人就会以为那条路径没有牙。名字从工具的 `--list` 读，
    # 不在检查器里另抄一份（避免"清单与真相各有一份"）。
    items.append(cmd(
        "gate-doc-refs", "gate",
        "元判据·文档引用：`tools/inject.py` 里的每条注入都必须出现在 docs/RECLAMATION.md 里",
        ["python3", "tools/check_doc_refs.py"], timeout=120,
        ok_msg=ok_first(r"^ok "),
        bad_msg="tools/check_doc_refs.py（有注入没写进文档）",
        bad_dump=dump_fail(8)))

    # ---- 元判据·验牙注入的锚点不许漂 ----
    # inject.py 的每处原文是**逐字**匹配源码的；发射文本一加注释、一改缩进，锚点就失效。
    # 失效只会在真去跑那条注入时才炸，而 inject.py 不在电池里 ⇒ 曾一次漂了 16 处没人发现。
    # 这一条不重编、毫秒级，只核对每处原文在目标文件恰好出现一次（且 old != new）。
    items.append(cmd(
        "gate-inject-anchors", "gate",
        "元判据·验牙锚点：`tools/inject.py` 每处注入原文在源码里恰好命中 1 次（不重编）",
        ["python3", "tools/inject.py", "--check-anchors"], timeout=60, needs_build=False,
        ok_msg=ok_first(r"注入锚点自检："),
        bad_msg="tools/inject.py --check-anchors（注入锚点陈旧：源码演进后没重锚）",
        bad_dump=dump_fail(8)))

    # ---- 元判据·跳过清单与代码同步 ----
    # 运行期里每处"直接返回"都是一刀；`docs/RECLAMATION.md` 的跳过表写明它们的前提与失效方向。
    # 义务表已有门禁（gate-invariant-table），这条是跳过表的对应物：标记 ↔ 表行双向对齐，
    # 并且"未标记的单行早退"是个只许往下拧的棘轮（当前 0）。
    items.append(cmd(
        "gate-skip-table", "gate",
        "元判据·跳过清单：每处 `if (…) return` 都带 `/* skip:<id> */`，且与 docs 的跳过表双向对齐",
        ["python3", "tools/check_skip_table.py"], timeout=120,
        ok_msg=ok_first(r"^ok "),
        bad_msg="tools/check_skip_table.py（跳过清单与代码不同步）",
        bad_dump=dump_fail(8)))

    # ---- 元判据·不变量清单不许漏项 ----
    # 清单（`docs/RECLAMATION.md` 那张表）的价值全在完整：新写一条义务忘了进表，表就退化成
    # 抽查记录。清单本身没法自动生成（"在哪强制""怎么证明"要人写），但完整性可以机械检查。
    items.append(cmd(
        "gate-invariant-table", "gate",
        "元判据·不变量清单：运行期里出现的每个「义务 N」都必须在 docs/RECLAMATION.md 的表里",
        ["python3", "tools/check_invariant_table.py"], timeout=120,
        ok_msg=ok_first(r"^ok "),
        bad_msg="tools/check_invariant_table.py（不变量清单漏项）",
        bad_dump=dump_fail(8)))

    # ---- 元判据·参与区域数不变量（单区域免登记快路径的健全性前提）----
    # 快路径"只在 `partN == 1` 时跳过跨区登记"，它的前提是"每个会登记/会交出槽的函数都记了
    # `partN`"。漏一处，失效方向是**不安全**的（该登记没登记），而语义/输出类判据看不出来
    # ⇒ 用这条结构门禁钉住（它在修好之前就在真实代码上红过一次，见 CHANGELOG）。
    items.append(cmd(
        "gate-note-part", "gate",
        "元判据·参与区域数：会登记跨区边 / 会交出 rc 槽的函数都必须记 partN（否则单区域快路径不安全）",
        ["python3", "tools/check_note_part.py"], timeout=120,
        ok_msg=ok_first(r"^ok "),
        bad_msg="tools/check_note_part.py（参与区域数不变量被破坏）",
        bad_dump=dump_fail(8)))

    # ---- 闸门·计数模式（EXTC_RC_NOCOUNT=0，非默认；**gate 档**：这一档只在很少的地方跑，
    # 放 bench 会 rot —— 2026-10-13 的教训就是"判据不进常规档 = 判据会僵"）----
    # 默认走**无计数**（见 CHANGELOG 第十一刀，`gate-reclaim-switch` 覆盖它）；
    # 这一项保护**非默认的计数模式**：同 31 条形状棘轮必须一字不差地成立。
    items.append(cmd(
        "gate-reclaim-count", "gate",
        "闸门·计数模式（EXTC_RC_NOCOUNT=0）：31 条形状棘轮仍然全绿（语义 + N vs 4N 峰值）",
        ["./tests/reclaim-shapes/run-switch.sh"], timeout=1800,
        env={"EXTC_RC_NOCOUNT": "0"},
        ok_msg=ok_first(r"^  ok"),
        bad_msg="tests/reclaim-shapes/run-switch.sh（计数模式回归）",
        bad_dump=dump_fail(12)))

    # 计数模式还缺两块：**单测驱动**（阈值缓存/自学习/登记/生命周期 116 条）与**跨语言对拍**。
    # 非默认的计数档若只被形状那一条判据守着，就容易悄悄腐烂 ⇒ 把这两块也补成棘轮。
    items.append(cmd(
        "gate-reclaim-count-unit", "gate",
        "闸门·计数模式：单测驱动 116 条断言（阈值缓存 / 自学习 / 跨区登记 / 区域生命周期 / 着色缓存）",
        ["./tests/reclaim/run.sh"], timeout=1800,
        env={"EXTC_RC_NOCOUNT": "0"},
        ok_msg=ok_first(r"通过 \d+"),
        bad_msg="tests/reclaim/run.sh（计数模式单测回归）",
        bad_dump=dump_fail(12)))
    items.append(cmd(
        "gate-reclaim-count-cross", "gate",
        "闸门·计数模式：跨语言对拍（bench/app 三语言校验和一致；分配密集，走 rc 路径）",
        ["./bench/app/run.sh"], timeout=1800,
        env={"EXTC_RC_NOCOUNT": "0", "SCALE": "1", "RUNS": "1", "WRITE_RESULTS": "0"},
        ok_msg=ok_const("计数模式跨语言对拍：三语言校验和一致 ✓"),
        bad_msg="bench/app（计数模式跨语言对拍不一致）",
        bad_dump=dump_fail(20)))

    # ---- 闸门·分片路径（EXTC_RC_SHARDS）----
    # 分片是 codegen 的第四条发射路径，run-shard.sh 只是证据脚本 ⇒ 这里把三条判据钉住：
    # 旋钮非空转（1 与 8 的生成物必须不同）、语义一致（两个规模、输出逐字相同）、
    # 界与轮数无关（分片=8 下 N vs 4N 峰值比 < 1.5；峰值 RSS 是跨环境读数 ⇒ 红了才复测一对
    # 取较小值，FAIL 行自带原始 KB 与 SHARDS=1 的 4N 参考）。
    items.append(cmd(
        "gate-shard", "gate",
        "闸门·分片路径：EXTC_RC_SHARDS=1/8 生成物必须不同、输出逐字一致、分片=8 下 N vs 4N 峰值比 < 1.5（红了才复测取小）",
        ["./tests/reclaim-shapes/gate-shard.sh"], timeout=900,
        ok_msg=ok_first(r"^  ok"),
        bad_msg="tests/reclaim-shapes/gate-shard.sh（分片路径语义/内存界回归）",
        bad_dump=dump_fail(10)))

    # ---- 闸门·回收性能（真实形状）----
    # 合成形状之外，`bt` / `rebuild` 这两条真实形状的指令数也钉住（线在脚本里随落地收紧）；
    # 同时要求开档/关档输出逐字一致 —— 性能判据不许靠改语义换。小输入缺了就地生成。
    items.append(cmd(
        "gate-reclaim-perf-shapes", "gate",
        "闸门·回收性能（真实形状）：bt / rebuild 小输入的 callgrind 总指令数 ≤ 各自的线，且开/关档输出逐字一致",
        ["./tests/reclaim-perf/gate-shapes.sh"], timeout=900,
        ok_msg=ok_count_ok(" 项（两条形状：指令数在线上 + 开/关档输出一致）"),
        bad_msg="tests/reclaim-perf/gate-shapes.sh（真实形状的回收开销被付回，或开关语义漂了）",
        bad_dump=dump_fail(12)))

    # ---- 闸门·回收税（目标：不需要回收的形状，回收开销趋近 0）----
    # 判据 = bt 的"开档 Ir / 关档 Ir"。bt 不产生环、整棵树的死期就是区域死期 ⇒ 理想值 1.00×。
    # 现行线是棘轮：每收窄一块就往下拧一次。rebuild / cyclic_pressure 只打印（TAX_EXTRA=1）。
    items.append(cmd(
        "gate-reclaim-tax", "gate",
        "闸门·回收税：bt（不需要回收的形状）开档/关档指令数比 ≤ 现行线（棘轮只许往下拧）",
        ["./tests/reclaim-perf/gate-tax.sh"], timeout=600,
        ok_msg=ok_first(r"^  ok"),
        bad_msg="tests/reclaim-perf/gate-tax.sh（不需要回收的形状上，回收税超线）",
        bad_dump=dump_fail(8)))

    # ---- 闸门·「不需要回收 ⇒ 不发射 rc」的内存界 ----
    # 判据 = 被 ②（判据 (a)(b)(c)）判成 idle 的形状，峰值必须 N vs 4N < 1.5× 且有绝对上界；
    # 另带一个**必须仍在回收档**的对照形状（防判据瞎放行）与反空跑（②真的生效：rc 记号 → 0）。
    # 这条是实测抓出来的那个洞（按值返回的带引用聚合只当临时量用 ⇒ 峰值线性涨）的常驻门禁。
    items.append(cmd(
        "gate-reclaim-idle", "gate",
        "闸门·不发射 rc 的内存界：被判 idle 的形状 N vs 4N 峰值 < 1.5×（含对照形状 + 反空跑）",
        ["./tests/reclaim-perf/gate-idle-bounded.sh"], timeout=600,
        ok_msg=ok_first(r"^  ok"),
        bad_msg="tests/reclaim-perf/gate-idle-bounded.sh（被 ② 判成 idle 的形状峰值随规模涨）",
        bad_dump=dump_fail(10)))

    # ---- 闸门·「不需要回收 ⇒ 不发射 rc」的能力棘轮（固定语料清单）----
    # ②的能力必须能被**数出来**：固定清单里"产物不含任何 `extc_rc_`"的形状数只许往上（8 → 14：
    # 细化 (a′)+(b′) 把"循环里建、每轮丢"那一类也判成 idle）。此前这份进度只散在别处的形状
    # 名单里 ⇒ 某个形状悄悄退回回收档不会有人红。另带一个**必须仍在回收档**的对照
    # （`cyclic_pressure`），防这条退化成"一律 idle"。
    items.append(cmd(
        "gate-rc-idle-corpus", "gate",
        "闸门·不发射 rc 的能力棘轮：固定语料清单里产物无 rc 的形状数 ≥ 14（含对照形状）",
        ["./tests/reclaim-perf/gate-rc-idle-corpus.sh"], timeout=600,
        ok_msg=ok_first(r"^  ok"),
        bad_msg="tests/reclaim-perf/gate-rc-idle-corpus.sh（有能力判 idle 的形状退回了回收档）",
        bad_dump=dump_fail(10)))

    # ---- 闸门·孤儿回收的语义（只回收自己的后代）----
    # 三条判据：没登记过的进程一根手指都不许碰 / 登记过而所有者已结束的会话必须回收 /
    # 登记过而所有者还活着的会话不许动。替身程序的名字落在 `^build/[A-Za-z0-9_]+$` 里 ⇒
    # 任何"按进程名扫杀"的实现都会在①③变红（判据有牙）。实现见 tools/parrun.py。
    items.append(cmd(
        "gate-reap-orphans", "gate",
        "闸门·孤儿回收语义：只回收自己（登记过、所有者已结束）的会话，别人的活进程一律不碰",
        ["python3", "tools/parrun.py", "--reap-selftest"], timeout=300, needs_build=False,
        ok_msg=ok_first(r"^reap 语义自检：通过"),
        bad_msg="tools/parrun.py --reap-selftest（孤儿回收越界或漏收）",
        bad_dump=dump_fail(8)))

    # ---- 闸门·编译期崩溃（未初始化堆读）----
    # `MALLOC_PERTURB_=165` 把未初始化堆内存填成确定值 ⇒ 这类崩溃 100% 复现（不扰动堆时要撞运气，
    # 实测 1–4%）。判据：三个源在扰动堆下各跑 3 次必须 rc==0，再在普通堆下跑 10 次也必须 rc==0。
    # 对应 `symFieldsGrow` 少置零那类 bug：字段表的新格 `.src` 没人写，却被 `promoteFieldsAt`
    # 当 `Expr *` 解引用（`src/check/check_escape.c` 的 `promoteInto2`）。
    def fn_perturb_run(ctx: Runner, it: Item) -> Outcome:
        srcs = ["tests/stl/clone.extc", "tests/stl/string.extc", "examples/hello.extc"]
        bad, n = [], 0
        for src in srcs:
            for _ in range(3):
                n += 1
                p = ctx.run(["./build/extc", "--run", src],
                            env={"MALLOC_PERTURB_": "165"}, echo=False)
                if p.rc != 0:
                    bad.append(f"{src}（MALLOC_PERTURB_=165）rc={p.rc}"
                               f"{(p.out + p.err).strip()[:100]}")
        for _ in range(10):
            n += 1
            p = ctx.run(["./build/extc", "--run", "tests/stl/clone.extc"], echo=False)
            if p.rc != 0:
                bad.append(f"tests/stl/clone.extc（普通堆）rc={p.rc}"
                           f"{(p.out + p.err).strip()[:100]}")
        if bad:
            return Outcome(False, f"{len(bad)}/{n} 次 `extc --run` 崩溃（未初始化堆读一类）", bad[:6])
        return Outcome(True, f"{n} 次 `extc --run` 全部 rc=0"
                             f"（扰动堆 {len(srcs) * 3} 次 + 普通堆 10 次）")
    items.append(Item(
        "gate-perturb-run", "gate",
        "闸门·编译期崩溃：`MALLOC_PERTURB_=165` 下 `--run` 三个源必须 rc=0（未初始化堆读）",
        fn_perturb_run))

    # ---- 闸门·扰动普查（bench 档：过全语料）----
    # `MALLOC_PERTURB_=165` 把"未初始化堆读"从偶发变成必现；普查脚本在 `tests/perturb/` 里。
    # 判据 = 崩的源集合与 `tests/perturb/known-bad.txt` **逐条相同**（多=新增坏源，少=清单过期）。
    # 这是 ASan/UBSan 照不到的那一类（MSan 领域），也是 task-10 那条 `symFieldsGrow` 的常驻守卫。
    items.append(cmd(
        "gate-perturb-corpus", "gate",
        "闸门·扰动普查：MALLOC_PERTURB_ 42/165 + EXTC_ARENA_FILL 四档过全语料，崩的源只许在 known-bad.txt 里",
        ["python3", "tests/perturb/census.py", "--check",
         "--arms", "plain,42,165,afill"], timeout=3600, bench=True,
        ok_msg=ok_first(r"^扰动普查："), bad_msg="tests/perturb/census.py --check（扰动下崩的源变了）",
        bad_dump=dump_fail(12)))

    # ---- 闸门·扰动普查的进程卫生（超时"只杀编译器"就留下孤儿）----
    # `extc --run` 会 fork 出被编译的程序（`echo_server_fork` 这类还会再 fork 后代）；超时只杀
    # 编译器，那些程序就被 double-fork 甩给 init 当孤儿，跨多轮普查会累积到上百个。修法是每个
    # 用例跑在自己的会话/进程组 + 登记（复用 `tools/parrun.py` 的原语），超时 `killpg` 整组。
    # 判据 = 一次**有界子集**普查（2 个源 × 1 档，必含超时用例）跑完，`/tmp/extc-perturb-runs/`
    # 下 0 进程；挂 `nobench` 的理由：约 5 秒，且它防的是**跨轮累积**，只有每轮都跑才有意义。
    items.append(cmd(
        "gate-perturb-orphans", "gate",
        "闸门·扰动普查不留孤儿：有界子集（含超时用例）跑完，/tmp/extc-perturb-runs/ 下必须 0 进程",
        ["bash", "tests/perturb/gate-orphans.sh"], timeout=300,
        ok_msg=ok_first(r"^  ok   跑完 0 个残留"),
        bad_msg="tests/perturb/gate-orphans.sh（超时只杀了编译器，留下了孤儿进程）",
        bad_dump=dump_fail(10)))

    # ---- 闸门·运行期注释声称的默认值 vs 产物真实 #define（便宜：只编译一份产物）----
    # 由来：这段注释曾写"默认 0 = 保持计数"，而产物真是 `#define EXTC_RC_NOCOUNT 1` ⇒ 注释与
    # 事实漂移，靠人读才发现。判据见 `tests/reclaim-perf/gate-rt-defaults.sh`。
    items.append(cmd(
        "gate-rt-defaults", "gate",
        "闸门·运行期注释与产物一致：注释里声称的 `EXTC_RC_*` 默认值必须等于产物里的 `#define`",
        ["bash", "tests/reclaim-perf/gate-rt-defaults.sh"], timeout=300,
        ok_msg=ok_first(r"^  ok   运行期注释里声称的默认值与产物一致"),
        bad_msg="tests/reclaim-perf/gate-rt-defaults.sh（运行期注释声称的默认值漂移了）",
        bad_dump=dump_fail(10)))

    # ---- 闸门·工作量界（[work] 确定性 + 蕴含式界）----
    # 挂 nobench 的理由：**约 3 秒**（五形状现场生成+编译+各跑 3 次），而且它是**确定的**
    # 窗口判据 —— `[step]`/墙钟已被证明不可靠（同产物 `step_smax` 差 8×，与 `involuntary csw`
    # 无一致关系 ⇒ 外部干扰），所以它必须每轮都跑，不能只挂 bench。
    # 判据见 `tests/reclaim-perf/gate-work.py`：`[work]` 行存在且 n>0（否则文案是"插桩失败"）、
    # 每形状 3 次逐字相同、`walk_max ≤ EXTC_RC_MARK_BATCH_MAX`、`scan_max ≤ EXTC_RC_SEED_WORDS`、
    # `push_max ≤ EXTC_RC_SEED_PUSHES + (字宽-1)`，三个参数与字宽都从**产物**里 grep（不许硬编码）。
    items.append(cmd(
        "gate-reclaim-work", "gate",
        "闸门·工作量界：[work] 存在/确定/n>0，且五形状 walk/scan/push max 都在本树产物预算内",
        ["python3", "tests/reclaim-perf/gate-work.py"], timeout=600,
        ok_msg=ok_first(r"^  ok   工作量界"),
        bad_msg="tests/reclaim-perf/gate-work.py（工作量越界，或 `[work]` 插桩失败）",
        bad_dump=dump_fail(14)))

    # ---- 闸门·跳过旧值登记的不变量（FS1/FS2/FS3 静态 + G1 运行期守卫）----
    # 挂 nobench 的理由：**约 3 秒**（现场发射运行期文本 + 编 P4 跑一次），但它是**健全性**门禁：
    # "region 没出现过外来指针就省掉旧值登记查表"这条优化一旦被新代码破坏，失效方向是**不安全**
    # （漏撤一条跨区入边 ⇒ 目标区域可能提前放掉还被指着的对象），语义/输出判据看不出来。
    # 判据见 `tools/check_foreign_skip.py`：每个 `rsDec` 被 `foreign` 把门、每个 `rsInc` 同行置
    # `foreign = 1`、字段与跳过判定存在；再编 P4（`-DEXTC_RC_FOREIGN_GUARD=1`）要求
    # `[fg] checks>0 bad==0`（拿不到 `[fg]` 行 ⇒ 文案是"插桩失败"，不许当绿）。
    items.append(cmd(
        "gate-foreign-skip", "gate",
        "闸门·旧值登记跳过：FS1/FS2/FS3 静态前提 + G1 运行期守卫（checks>0、bad==0）",
        ["python3", "tools/check_foreign_skip.py"], timeout=600,
        ok_msg=ok_first(r"^  ok   跳过旧值登记"),
        bad_msg="tools/check_foreign_skip.py（旧值登记跳过的不变量被破坏，或守卫插桩失效）",
        bad_dump=dump_fail(14)))

    # ---- 闸门·区域 slab 分配缓存结构 ----
    # 这是语义不变量的静态门禁：region 缓存必须经过尺寸/容量校验，链表备用路径不能删除，
    # 结构自测会注入每个缺口并确认判据真的变红。
    items.append(cmd(
        "gate-alloc-hot", "gate",
        "闸门·区域 slab 分配缓存：缓存字段/尺寸容量校验/链表备用路径/成功回写，且红自测有效",
        ["python3", "tools/check_alloc_hot.py"], timeout=60,
        ok_msg=ok_first(r"^ok allocation cache structure"),
        bad_msg="tools/check_alloc_hot.py（区域 slab 分配缓存结构或红自测失效）",
        bad_dump=dump_fail(10)))

    # ---- 闸门·槽粒度（8 字节取整）与它的对齐前提 ----
    # 买的是**峰值 RSS**：槽粒度直接决定内部碎片，而 8 是一切类型的自然对齐上界。
    # 谁把粒度改回 16（或改细）、把尺寸类表/类号单位改得与粒度不符、或让穷举判据不再覆盖
    # 真实粒度，这里就红；对齐前提（内建类型没有 >64 位、TypeKind 没有向量种类）也在判据里，
    # 新加宽类型必须让这条红一次再同步。数字记在 CHANGELOG.md，不在这里副本。
    items.append(cmd(
        "gate-slot-unit", "gate",
        "闸门·槽粒度 8 字节：取整/下限/溢出守卫 + 尺寸类表与单位 + 穷举步长一致 + 无宽类型前提",
        ["python3", "tools/check_slot_unit.py"], timeout=60,
        ok_msg=ok_first(r"^ok slot unit structure"),
        bad_msg="tools/check_slot_unit.py（槽粒度、尺寸类单位、穷举步长或对齐前提被破坏）",
        bad_dump=dump_fail(14)))

    # ---- 闸门·标记步专用的 ownSlab 副本 ----
    # 买的是**标记步的常数**：这个调用点占指令两成，强制内联唯一值得；全站点内联会让
    # cross_region 这类形状慢两成。副本的代价是两份体可能漂移，所以判据 = 逐字相同 +
    # 副本带 always_inline、原件不带 + markStep 只调副本，--selftest 每条各注入一次。
    items.append(cmd(
        "gate-ownslab-body", "gate",
        "闸门·标记步 ownSlab 副本：两份体逐字相同/副本 always_inline/原件不内联/markStep 只调副本",
        ["python3", "tools/check_ownslab_body.py"], timeout=60,
        ok_msg=ok_first(r"^ok ownSlab mark-step copy structure"),
        bad_msg="tools/check_ownslab_body.py（两份 ownSlab 漂移，或标记步没走内联副本）",
        bad_dump=dump_fail(10)))

    # ---- 闸门·着色缓存的旋转散列结构 ----
    # 买的是**插入屏障的常数**（P5 上 shadeSlow 仍是仅次于 markStep 的项）：缓存必须精确
    # （假阳性 = 白对象当黑），所以格号把代际号**旋进**格号、只存地址单字，等号即精确；
    # 每窗口回绕前整表清零，旧记录不可能复活。失效契约=四处清 mark 的路径都调
    # extc_rc_shadeNextGen()（不许出现裸 extc_rc_shadeGen++）。--selftest 六条注入全红。
    items.append(cmd(
        "gate-shade-cache", "gate",
        "闸门·着色缓存旋转散列：单字格/格号旋入代际号/回绕清表/四站点只调 NextGen，且 6 条注入全红",
        ["python3", "tools/check_shade_cache.py", "--selftest"], timeout=60,
        ok_msg=ok_first(r"^ok shade cache structure"),
        bad_msg="tools/check_shade_cache.py（着色缓存退回两字结构/绕不过回绕清零，失效契约有洞）",
        bad_dump=dump_fail(12)))

    # ---- 闸门·空清扫之后的退场倍数 ----
    # 买的是**触发策略**：活集大、垃圾少的负载（chase）只做一轮便宜的整堆标记，不再付
    # 「标记 32 MiB 活集只换回几 MB」的第二轮。判据=默认 4 / 退场走宏（不许字面量 2）/
    # 无旧 *2 残留 / 退场后有 SCALE_MAX 夹紧；--selftest 四条注入全红。
    items.append(cmd(
        "gate-retreat", "gate",
        "闸门·空清扫退场倍数：默认 4 / 退场走宏 / 无旧 *2 / 溢出夹紧，且 4 条注入全红",
        ["python3", "tools/check_retreat.py", "--selftest"], timeout=60,
        ok_msg=ok_first(r"^ok retreat structure"),
        bad_msg="tools/check_retreat.py（退场倍数退回 2 或绕开宏，活集大垃圾少的负载要多付整堆标记）",
        bad_dump=dump_fail(12)))
    # ---- 闸门·rc 类型描述符按布局去重 ----
    # 买的是**分配路径的常数**：描述符若每个分配点各发一份，同一类型的 8 个站点会让每块
    # slab 的小类型表堆 8 个同构表项、lastTd 缓存永远失手（shard_bands 分配路径吃掉该形状
    # 三成以上指令数）。判据跑真实发射：合成程序 3 种布局 / 5 个分配点，必须只发 3 份
    # **文件作用域**描述符、同布局站点引用同一符号；--selftest 三条注入全红。
    items.append(cmd(
        "gate-rc-desc-dedup", "gate",
        "闸门·rc 描述符按布局去重：3 种布局/5 个分配点共用 3 份文件作用域描述符，且 3 条注入全红",
        ["python3", "tools/check_rc_desc_dedup.py", "--selftest"], timeout=120,
        ok_msg=ok_first(r"^ok rc 描述符去重"),
        bad_msg="tools/check_rc_desc_dedup.py（描述符退回每个分配点一份/块作用域，分配路径常数会退化）",
        bad_dump=dump_fail(12)))

    # ---- 闸门·BLAS 扩展库的绑定 ----
    # 买的是**扩展库这条路本身**：std 之外的东西能不能只靠 -I + std::dl 接进来，且接进来
    # 不丢性能。判据自带 LP64 shim（本机常常只有 numpy 的 ILP64，系统没有 libcblas），
    # 两条 ABI 分支都要逐元素对上裸循环；机器上有真 CBLAS 时再加一条「512³ 至少快
    # MIN_RATIO 倍」，否则跳过（shim 那半永远跑）。没有 CBLAS 的干净机器上不会红。
    items.append(cmd(
        "gate-blas", "gate",
        "闸门·extlib/blas：LP64 与 ILP64 两条 ABI 分支都逐元素对上裸循环（有真库时还要快 2× 以上）",
        ["./extlib/blas/tests/run.sh"], timeout=600,
        ok_msg=ok_first(r"^  ok   blas"),
        bad_msg="extlib/blas/tests/run.sh（BLAS 绑定认错 ABI/算错数/封装把性能吃掉了）",
        bad_dump=dump_fail(12)))

    # ---- 闸门·ggml 扩展库的量化内核 ----
    # 买的是**量化格式的位级正确性**：q4_0（18 字节块、低 nibble 先、f16 尺度）与 q8_0
    # （34 字节块）的手写解码必须与 ggml 的 dequantize_row_* 逐位一致，vec_dot 必须等于
    # 「反量化后 f32 点积」——按 4 字节宽或 nibble 序反了，这两条立刻红，而不是悄悄算错数。
    # 再加一条收益判据：q4_0 GEMV 至少比 f32 GEMV 快 MIN_RATIO 倍，否则绑定白绑。
    # ggml 是可选第三方库（要自己 cmake 构建）：找不到时脚本先编过模块与判据程序再跳过。
    items.append(cmd(
        "gate-ggml", "gate",
        "闸门·extlib/ggml：q4_0/q8_0 块布局与 dequantize 逐位一致、q4_0 GEMV 比 f32 快 2× 以上（无 ggml 时跳过）",
        ["./extlib/ggml/tests/run.sh"], timeout=600,
        ok_msg=ok_first(r"^  ok   ggml"),
        bad_msg="extlib/ggml/tests/run.sh（量化块布局读错/vec_dot 算错/q4 的带宽优势没兑现）",
        bad_dump=dump_fail(12)))

    # ---- 闸门·标记 walk 的批弹出结构 ----
    # 这条买的是**延迟**（LIFO 逐个弹是串行指针链），不是指令数，所以判据是结构 + 红自测：
    # 谁把"先弹一批→整批预取→逐个扫"改回逐对象，门禁就红。
    items.append(cmd(
        "gate-mark-batch", "gate",
        "闸门·标记 walk 批弹出：批常量/批数组/弹出循环/预算记账/整批预取/逐个扫，且红自测有效",
        ["python3", "tools/check_mark_batch.py"], timeout=60,
        ok_msg=ok_first(r"^ok batched mark walk structure"),
        bad_msg="tools/check_mark_batch.py（标记 walk 的批弹出结构或红自测失效）",
        bad_dump=dump_fail(10)))

    # ---- 闸门·复用链存在 tid[] 里（不是槽载荷）----
    # 这条买的是 **P5 的墙钟**（0.910×）：被释放的槽是冷的，把链的 8 字节头插指针写进载荷
    # 等于清扫每放一槽多一次末级写失手；两种编码**语义等价**，所以行为判据抓不到它回潮，
    # 只判结构 + 红自测。数字（墙钟/RSS/cachegrind 失手）记在 CHANGELOG.md，不在这里副本。
    items.append(cmd(
        "gate-reuse-chain", "gate",
        "闸门·复用链存在 tid[] 里：freeSlot 只写 tid[]、不碰载荷、不清零，分配端解 +1 编码，槽数上限有守卫",
        ["python3", "tools/check_reuse_chain.py"], timeout=60,
        ok_msg=ok_first(r"^ok reuse chain lives in tid"),
        bad_msg="tools/check_reuse_chain.py（复用链的存放位置回潮或红自测失效）",
        bad_dump=dump_fail(10)))

    # ---- 闸门·mark-both 屏障（删除屏障 + 本地槽省屏障）----
    # 这条买的是 **P6 的墙钟**：无计数档把"每次本地槽写都着色"整段省掉，保护改由"被覆盖的旧边"
    # 承担。判据是结构 + 红自测：谁把删除屏障删了、把本地槽的门撤了、或让 storeRefLocal 又
    # 着色，门禁就红。数字（六档墙钟/RSS/停顿）记在 CHANGELOG.md，不在这里副本。
    items.append(cmd(
        "gate-mark-both", "gate",
        "闸门·mark-both 屏障：删除屏障定义/只对无计数档/四个堆写入口 + 本地槽三处把门，且红自测有效",
        ["python3", "tools/check_mark_both.py"], timeout=60,
        ok_msg=ok_first(r"^ok mark-both barrier structure"),
        bad_msg="tools/check_mark_both.py（mark-both 屏障结构或红自测失效）",
        bad_dump=dump_fail(10)))

    # ---- 闸门·slab 发放容量按尺寸类定 ----
    # 这条买的是**峰值 RSS**：一块 slab 发出去的槽数按槽大小定（至少 1024 个槽、块大小下限
    # 64 KiB 并按 2 的幂上取到 1 MiB），不再一律发满 1 MiB。判据是结构 + 红自测：谁把固定
    # 1 MiB 的旧公式放回来、把"至少 1024 个槽"这一条撤了、或动了 1 MiB 对齐这个前提，
    # 门禁就红。数字（六档墙钟/RSS）记在 CHANGELOG.md，不在这里副本。
    items.append(cmd(
        "gate-slab-cap", "gate",
        "闸门·slab 容量按槽大小定：常量/上取循环/cap 由 chunk 推/超大块判定/1 MiB 对齐前提/按槽记账，且红自测有效",
        ["python3", "tools/check_slab_cap.py"], timeout=60,
        ok_msg=ok_first(r"^ok slab cap structure"),
        bad_msg="tools/check_slab_cap.py（slab 发放容量的结构或红自测失效）",
        bad_dump=dump_fail(10)))

    # 挂 bench 档的理由：要跑 1M 规模的形状 + 停顿探针（单次约 5s，但**不是**秒级项，且
    # 它测的是"回收片"这个旋钮本身，属于性能线一族 ⇒ 与 `gate-reclaim-tax`/`gate-shapes` 同类）。
    # 判据见 `tests/reclaim-perf/gate-shard-cycles.sh`：digraph(`SHARDS=4`) 4N/N < 1.5× 且
    # 4N ≤ 2× 默认档；`shard_bands` 上分片必须真的把工作切开 —— 判据是运行期 `[work]` 的
    # `walk_max ≤ 0.5×`、工作项数 `n ≥ 2×`（确定性计数，与计时无关；平均切片只作参考值，
    # 它会被步粒度稀释），另加计时侧两条绝对上界 `over1ms == 0`、`over64 ≤ 5`（旋钮没被钉死）。
    items.append(cmd(
        "gate-shard-cycles", "gate",
        "闸门·分片有界：环负载 digraph 上 SHARDS=4 的 4N/N < 1.5×（且 4N ≤ 2× 默认档），"
        "对齐形状 shard_bands 的单步遍历量被切成 ≤ 0.5×（工作项数 ≥ 2×，窗口上界仍在）",
        ["bash", "tests/reclaim-perf/gate-shard-cycles.sh"], timeout=900, bench=True,
        ok_msg=ok_first(r"^  ok   分片在高环负载形状上有界"),
        bad_msg="tests/reclaim-perf/gate-shard-cycles.sh（分片把环切开 ⇒ 界随轮数涨，或旋钮被钉死）",
        bad_dump=dump_fail(14)))

    # ---- 闸门·回收能力账本（全语料，棘轮）----
    # 每个能编译的程序按生成物分类：ok-trigger / owe-settled / silent-both / …
    # 红计数①（silent-both + owe-no-settle）基线 13 条，逐条相同才绿：
    # 多一条 = 回收能力退化；少一条 = 优化拿回了能力，基线（在 tests/ 下）要一起缩。
    # 入口在 tests/ 下（那里没有 tools 的历史词棘轮），`--out` 落 build/，不冲入库的 out/*.tsv。
    items.append(cmd(
        "gate-reclaim-ledger", "gate",
        "闸门·回收能力账本：全语料红计数①（silent-both + owe-no-settle）与基线逐条相同（只许缩）",
        ["python3", "tests/reclaim-gaps/ledger-gate.py"], timeout=1800,
        ok_msg=ok_first(r"^\[ledger\]"),
        bad_msg="tests/reclaim-gaps/ledger-gate.py（有程序丢了回收者，或基线过期）",
        bad_dump=dump_fail(16)))

    # ---- 闸门·计数按编译期收窄的语料（棘轮 + 反空跑）----
    # 「该省」4 行的红 = 收窄还没实现（在基线里，实现一条删一条）；「不该省」4 行必须一直有计数
    # （少了就是误省，健全性方向）。接线未生效（0 处调用点）时 rc=2 —— 那时"该省"的红是假红。
    items.append(cmd(
        "gate-reclaim-elide", "gate",
        "闸门·计数收窄语料：4 个「该省」形状 = 基线，4 个「不该省」形状必须仍有计数（棘轮 + 反空跑）",
        sh('python3 tests/reclaim-elide/gate.py'), timeout=600,
        ok_msg=ok_first(r"^\[elide\]"),
        bad_msg="tests/reclaim-elide/gate.py（计数被误省 / 新红 / 基线过期 / 接线失效）",
        bad_dump=dump_fail(12)))

    # ---- arena ----
    items.append(cmd(
        "arena", "tests", "arena：按块细化，150MB 上限下不许涨",
        ["./tests/arena/run.sh"], timeout=600,
        ok_msg=ok_linecount(" 个用例"),
        bad_msg="tests/arena/run.sh", bad_dump=dump_all))

    # ---- 警告 ----
    items.append(cmd(
        "warnings", "tests", "警告：该响的响 · 正例语料零误报 · `-w` 能关",
        ["./tests/warnings/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（含正例语料零误报 ✓）"),
        bad_msg="tests/warnings/run.sh", bad_dump=dump_all))

    # ---- 泛型自由函数 ----
    items.append(cmd(
        "generics", "tests", "泛型自由函数：推导 / 显式实参 / 推迟的 T:（1 正例 + 3 反例）",
        ["./tests/generics/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（1 正例 + 3 反例）"),
        bad_msg="tests/generics/run.sh", bad_dump=dump_all))

    # ---- 驱动开关页 ----
    items.append(cmd(
        "gen-flags", "docs",
        "驱动开关页与 `extc --help` 一致（单一真源 docs/usage/flags.md）；不一致 ⇒ 跑 tools/gen_flags.py",
        sh('python3 tools/gen_flags.py --check'),
        ok_msg=ok_out, bad_msg="docs/usage/flags.md 已过期",
        bad_dump=lambda _out: ["  跑 python3 tools/gen_flags.py"]))

    # ---- 文档覆盖 ----
    items.append(cmd(
        "check-manual", "docs",
        "文档覆盖：驱动开关逐项 + 公开面棘轮（欠账只许不增长，新增欠账即红）",
        sh('python3 tools/check_manual.py --gate'),
        ok_msg=ok_out, bad_msg="文档覆盖出现新增欠账", bad_dump=dump_head(6)))

    # ---- 公开面清单 ----
    items.append(cmd(
        "manual-surface", "docs",
        "公开面清单与源码一致（tools/manual-surface.txt + docs/usage/surface.md）",
        sh('python3 tools/manual_surface.py --check'),
        ok_msg=ok_out, bad_msg="公开面清单已过期",
        bad_dump=lambda _out: ["  跑 python3 tools/manual_surface.py"]))

    # ---- 活文档不写历史 ----
    items.append(cmd(
        "check-no-history", "docs",
        "活文档不写历史（棘轮：决策编号与时代用词只许减；历史只进 CHANGELOG）",
        sh('python3 tools/check_no_history.py --gate'),
        ok_msg=ok_out, bad_msg="活文档出现新的历史词", bad_dump=dump_head(8)))

    # ---- 开发历史按月分页 ----
    items.append(cmd(
        "check-changelog", "docs",
        "开发历史按月分页：条目落在当月页，索引里的月份表与月份页一致",
        sh('python3 tools/check_changelog.py --gate'),
        ok_msg=ok_first(r"^ok "), bad_msg="历史分页或索引不一致", bad_dump=dump_head(8)))

    # ---- 头文件 Doxygen 覆盖 ----
    items.append(cmd(
        "check-doxygen", "docs",
        "头文件 Doxygen 覆盖（棘轮：只许变好）",
        sh('python3 tools/check_doxygen.py --gate'),
        ok_msg=ok_out, bad_msg="头文件出现没写 Doxygen 的新声明", bad_dump=dump_head(8)))

    # ---- 注释里的 @code 使用例 ----
    items.append(cmd(
        "check-code-examples", "docs",
        "注释里的 @code 使用例：extC 要能编译并跑过；C 至少 -fsyntax-only",
        sh('python3 tools/check_code_examples.py --gate'), timeout=600,
        ok_msg=head1, bad_msg="注释里有 @code 例子烂掉", bad_dump=dump_head(8)))

    # ---- 文档里写的 API 名 ----
    items.append(cmd(
        "check-doc-facts", "docs",
        "文档里写的 API 名真的存在（免得文档先烂）",
        sh('python3 tools/check_doc_facts.py --gate'),
        ok_msg=ok_out, bad_msg="文档里有源码里找不到的 API 名", bad_dump=dump_head(8)))

    # ---- 活文档里的示例 ----
    items.append(cmd(
        "check-doc-examples", "docs",
        "活文档里的示例是判据（完整程序必须编过；片段与负例要标注）",
        sh('python3 tools/check_doc_examples.py --gate'),
        ok_msg=ok_out, bad_msg="文档示例编不过或没标注", bad_dump=dump_head(8)))

    # ---- stdlib/INDEX ----
    items.append(cmd(
        "gen-index", "docs",
        "stdlib/INDEX 与源码一致（impl 方法表不许漂）；过期 ⇒ 跑 tools/gen_index.py",
        sh('python3 tools/gen_index.py --check'),
        ok_msg=ok_out, bad_msg="stdlib/INDEX 已过期",
        bad_dump=lambda _out: ["  跑 tools/gen_index.py 重新生成"]))

    # ---- 类型分支穷尽性 ----
    items.append(cmd(
        "check-tykind", "docs",
        "类型分支的穷尽性（新构造器不许静默走 default）",
        sh('python3 tools/check_tykind.py'),
        ok_msg=ok_linecount(" 行报告（5 处 kind-switch：1 处枚举齐全 + 4 处 default 带理由）"),
        bad_msg="tools/check_tykind.py", bad_dump=dump_all))

    # ---- impl 块 ----
    items.append(cmd(
        "impl", "tests",
        "impl 块（方法挂载点：内建标量也能挂 ⇒ hashMap<i64, V> 直接可用；1 正例 + 8 反例项）",
        ["./tests/impl/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（1 正例 + 8 反例项：含 coherence 重名与内建标量）"),
        bad_msg="tests/impl/run.sh", bad_dump=dump_all))

    # ---- dyn 阶段 1：一组接缝检查 ----
    items.append(cmd(
        "check-walkers", "seam",
        "dyn 阶段 1：构造即调用（派发经表、拒绝存储、生成物合同）；手写 walker 数棘轮",
        sh('python3 tools/check_walkers.py'),
        ok_msg=tail1, bad_msg="tools/check_walkers.py", bad_dump=dump_all))
    items.append(cmd(
        "check-plan-seam", "seam",
        "计划接缝 X1：codegen 只能经 src/plan.h 读分析产物/编译计划，直读即红",
        sh('python3 tools/check_plan_seam.py'),
        ok_msg=tail1, bad_msg="tools/check_plan_seam.py", bad_dump=dump_all))
    items.append(cmd(
        "check-ast-freeze", "seam",
        "AST 冻结 R1：parser 之外不许写 AST 节点成员；基线 = 施工单，只许减",
        sh('python3 tools/check_ast_freeze.py'),
        ok_msg=tail1, bad_msg="tools/check_ast_freeze.py", bad_dump=dump_all))
    items.append(cmd(
        "check-layering", "seam",
        "只读边界 R3：不许 include 别的阶段的私有头（codegen→check_internal 是红的起点）",
        sh('python3 tools/check_layering.py'),
        ok_msg=tail1, bad_msg="tools/check_layering.py", bad_dump=dump_all))
    items.append(cmd(
        "check-tmpl-owners", "seam",
        "字段归属 X3：同名 `tmpl` 按声明所在结构体认属主，未定即红；钉住 codegen 只经 planTemplate 读它",
        sh('python3 tools/check_tmpl_owners.py --verify'),
        ok_msg=tail1, bad_msg="tools/check_tmpl_owners.py --verify", bad_dump=dump_all))
    items.append(cmd(
        "check-switches", "seam",
        "EXTC_ 开关表：写进 --help 的与挂 UNDOCUMENTED 的棘轮（只能降）",
        sh('python3 tools/check_switches.py'), timeout=120,
        ok_msg=tail1, bad_msg="tools/check_switches.py", bad_dump=dump_head(8)))
    items.append(cmd(
        "check-concurrency-guards", "seam",
        "并发护栏 J1/J2/J3：zone 实参只转发或钉死 · 地方边界自洽 · 池元数据只经 pools.c",
        sh('python3 tools/check_concurrency_guards.py'),
        ok_msg=tail1, bad_msg="tools/check_concurrency_guards.py", bad_dump=dump_all))
    items.append(cmd(
        "tests-coro", "tests", "协程用例（tests/coro）",
        ["./tests/coro/run.sh"], timeout=600,
        ok_msg=ok_first(r"^通过"), bad_msg="tests/coro/run.sh", bad_dump=dump_all))
    items.append(cmd(
        "tests-dyn", "tests", "dyn 用例：正例 + 拒绝存储/字段 + 派发经表 + 合同编译",
        ["./tests/dyn/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（正例 + 拒绝存储/字段 + 派发经表 + 合同编译）"),
        bad_msg="tests/dyn/run.sh", bad_dump=dump_all))

    # ---- IO ----
    items.append(cmd(
        "io", "tests",
        "IO 第一块：std::sys 原语 + std::io 库 —— 能从 stdin 读了",
        ["./tests/io/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（stdin 读取 + 分层 + 流式：控制台 cin >> x · 文件 fin >> 整数 >> 一行 >> 一个字节 · cerr 与 stdout 分开）"),
        bad_msg="tests/io/run.sh", bad_dump=dump_all))

    # ---- extern! ----
    items.append(cmd(
        "extern", "tests",
        "extern! + 信任声明：签字才放行 · 默认最保守（1 正例 + 6 反例）",
        ["./tests/extern/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（1 正例 + 6 反例 · 指针过界：ref T / ref void / ?ref void）"),
        bad_msg="tests/extern/run.sh", bad_dump=dump_all))

    # ---- 链接通道 ----
    items.append(cmd(
        "linkflags", "tests",
        "链接通道（-l / -L / --ccflag / --pkg-config：extern! 的库名真的参与链接）",
        ["./tests/linkflags/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（三条路真跑 · flag 承重 · 模块自带 .link 自动链 · 链接需求写进生成物 · 不带 flag 产物一字不变；缺库显式跳过）"),
        bad_msg="tests/linkflags/run.sh", bad_dump=dump_all))

    # ---- @frozen ----
    items.append(cmd(
        "frozen", "tests",
        "@frozen（C-ABI.md §9.8）：作者签字「布局就是 C 的布局」⇒ 按值过界",
        ["./tests/frozen/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（真调 libc 按值返回 · C 侧镜像比 sizeof/offsetof · 产物断言与镜像各有牙 · 5 反例）"),
        bad_msg="tests/frozen/run.sh", bad_dump=dump_all))

    # ---- C-ABI ----
    items.append(cmd(
        "cabi", "tests",
        "C-ABI 线（C-ABI.md §9）：跟 C 打交道，进与出两个方向",
        ["./tests/cabi/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（@export：C 宿主真的链接并调用 · .so 里 nm -D 看得见符号 · 10 种「没有那一个 C 符号」的反例）"),
        bad_msg="tests/cabi/run.sh", bad_dump=dump_all))

    # ---- 攻击测试 ----
    items.append(cmd(
        "hostile", "tests",
        "攻击测试（tests/hostile）：不规范的 .so 打主程序 —— 判据是「怎么爆」",
        ["./tests/hostile/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（设计赢：伪造指针被拒 · 板内 containment · 关板后硬缺页；攻击赢：食言/越界写栈/munmap 重映射/改表 —— 边界写在明处）"),
        bad_msg="tests/hostile/run.sh", bad_dump=dump_all))

    # ---- 契约验证 ----
    items.append(cmd(
        "contract", "tests",
        "契约验证（tests/contract）：签字说错 ⇒ 当场可证伪；与 hostile 是一对孪生",
        ["./tests/contract/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（诚实库 contract=holds · 食言库 + 隔离 rc=139 · 毒化路径 falsified · 对照组 silent=stale）"),
        bad_msg="tests/contract/run.sh", bad_dump=dump_all))

    # ---- 包验证工具：诚实包 ----
    def fn_extpkg_honest(ctx: Runner, it: Item) -> Outcome:
        p = ctx.run(sh('python3 tools/extpkg.py verify tests/pkg/honest'), timeout=600)
        if (p.rc == 0 and re.search(r"通过 .*no-retain", p.out)
                and "不可验证  handle-ownership" in p.out):
            return Outcome(True, "诚实包：1 通过 + 1 不可验证（三态同现 · 台账不把「没验」说成「验过」）")
        return Outcome(False, "extpkg verify tests/pkg/honest", ind(tail(p.out, 8)))
    items.append(Item(
        "extpkg-honest", "tools",
        "包验证工具：诚实包必须 1 通过 + 1 不可验证（三态同现，不把「没验」说成「验过」）",
        fn_extpkg_honest))

    # ---- 包验证工具：食言包 ----
    def fn_extpkg_keeper(ctx: Runner, it: Item) -> Outcome:
        p = ctx.run(sh('python3 tools/extpkg.py verify tests/pkg/keeper'), timeout=600)
        if p.rc == 0:
            return Outcome(False, "食言包：应当被证伪（rc=1），实得 rc=0", ind(tail(p.out, 8)))
        if "被证伪" in p.out:
            return Outcome(True, "食言包：当场被证伪（rc=1 · 隔离页上的硬缺页）")
        return Outcome(False, "食言包：输出里没有被证伪", ind(tail(p.out, 8)))
    items.append(Item(
        "extpkg-keeper", "tools",
        "包验证工具：食言包必须被证伪（rc=1 且输出含「被证伪」）—— 工具自己有牙",
        fn_extpkg_keeper))

    # ---- 哈希与 UUID ----
    items.append(cmd(
        "hash", "tests",
        "哈希与 UUID：RFC 向量 · 与 Python hashlib 对拍 · 吞吐与内存",
        ["./tests/hash/run.sh"], timeout=900,
        ok_msg=lambda out: (f"{count_matching(out, '^  ok')} 项（{first_substring(out, r'sha256 [0-9]* MB/s')}"
                            f" · {first_substring(out, r'峰值 RSS [0-9]* KB')} · uuid5 与 Python 逐字节一致）"),
        bad_msg="tests/hash/run.sh", bad_dump=dump_all))

    # ---- JSON ----
    items.append(cmd(
        "json", "tests",
        "JSON：与 Python 规范形对拍 · 坏输入必拒 · 攻击自己 fuzz · 零拷贝证据 · 吞吐与内存",
        ["./tests/json/run.sh"], timeout=900,
        ok_msg=lambda out: (f"{count_matching(out, '^  ok')} 项（{first_substring(out, r'parse [0-9]* MB/s')}"
                            f" · {first_substring(out, r'峰值 RSS [0-9]* KB')} · 17 条坏输入全拒 · 2 万轮变异零崩溃）"),
        bad_msg="tests/json/run.sh", bad_dump=dump_all))

    # ---- f-string ----
    items.append(cmd(
        "fstring", "tests",
        'f"…" 编译期格式拼接：输出 · 与手写 << 链逐字节相同 · 六条反例',
        ["./tests/fstring/run.sh"], timeout=300,
        ok_msg=ok_count_ok(" 项（多槽/转义/每槽求值一次 · 生成物与手写链逐字节相同 · 六条反例）"),
        bad_msg="tests/fstring/run.sh", bad_dump=dump_all))

    # ---- WebSocket ----
    items.append(cmd(
        "ws", "tests",
        "WebSocket（RFC 6455）：握手向量 · 拒绝面 · 变异 fuzz · 帧 vs Python",
        ["./tests/ws/run.sh"], timeout=300,
        ok_msg=ok_count_ok(" 项（握手向量 · 拒绝面 · 往返 · 变异 2 万次 · 24 帧与 Python 逐字节一致）"),
        bad_msg="tests/ws/run.sh", bad_dump=dump_all))

    # ---- Base64 ----
    items.append(cmd(
        "b64", "tests",
        "Base64（RFC 4648）：向量 · 拒绝面 · 与 Python 逐字节对拍",
        ["./tests/b64/run.sh"], timeout=300,
        ok_msg=ok_count_ok(" 项（RFC 向量 · URL 表 · 拒绝面 · 往返 · 25 组 vs Python）"),
        bad_msg="tests/b64/run.sh", bad_dump=dump_all))

    # ---- 事件循环 ----
    items.append(cmd(
        "loop", "tests",
        "事件循环：定时策略离线判据 + 真事件源（批量/不丢唤醒/多实例）",
        ["./tests/loop/run.sh"], timeout=300,
        ok_msg=ok_count_ok(" 项（超时计算五边界 · 到期恰好一次 · 等值稳定 · 表满响亮 · 500 个乱序 deadline；真事件源：一轮拿回 3 个 · 先写后注册不丢 · 两个实例并行 · runOnce 等 25ms）"),
        bad_msg="tests/loop/run.sh", bad_dump=dump_all))

    # ---- 裸 return ----
    items.append(cmd(
        "voidret", "tests",
        "裸 return 在 void 函数里（方法那一格报过假错）",
        ["./tests/voidret/run.sh"], timeout=300,
        ok_msg=ok_count_ok(" 项（方法/自由函数的裸 return 合法 · 非 void 的裸 return 仍然报错）"),
        bad_msg="tests/voidret/run.sh", bad_dump=dump_all))

    # ---- 包工具全链路 ----
    items.append(cmd(
        "extpkg-chain", "tests",
        "包工具全链路（extpkg fetch/vendor/build：假 registry 走 file://，不联网）",
        ["./tests/extpkg/run.sh"], timeout=900,
        ok_msg=ok_count_ok(" 项（首次 fetch 写锁 · --offline 命中缓存 · 改锁一个字节必红 · 空缓存必红且消息可执行 · vendor 后清缓存仍能离线 build · 锁逐字节决定论 · build 自动补 vendor）"),
        bad_msg="tests/extpkg/run.sh", bad_dump=dump_all))

    # ---- 绑定生成器 ----
    items.append(cmd(
        "cbindgen", "tests",
        "绑定生成器（tools/cbindgen.py）：类型映射 · 零 effects · 生成物能编译并真调用",
        ["./tests/cbindgen/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（8 条声明逐条对拍 · 零 effects · --check · 生成的绑定真 dlopen libc 调 getpid）"),
        bad_msg="tests/cbindgen/run.sh", bad_dump=dump_all))

    # ---- 真库 ----
    items.append(cmd(
        "real-lib", "tests",
        "真库（C-ABI.md §9.11）：用 cairo 画一张画 —— dlopen 真第三方 .so，不链接它",
        ["./tests/real-lib/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（dlopen libcairo + 20 个符号 + 签名的表槽 + 板里的画布 + PNG；「画对了」由像素数与四边判据钉住；无 cairo 的机器显式跳过）"),
        bad_msg="tests/real-lib/run.sh", bad_dump=dump_all))

    # ---- 板 / Heap ----
    items.append(cmd(
        "heap", "tests",
        "板 / Heap（HEAP.md · C-ABI.md §9.10）：保留便宜 · 按需 commit · 门有牙 · close 即 munmap",
        ["./tests/heap/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（4 GiB 保留只涨 4 KB RSS · commit 1 MiB 粒度 · 板外指针被拒 · close 之后再用 = SIGSEGV）"),
        bad_msg="tests/heap/run.sh", bad_dump=dump_all))

    # ---- 模块 ----
    items.append(cmd(
        "modules", "tests",
        "模块：语义导入 · 一个文件一个模块 · @private · 禁环",
        ["./tests/modules/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（8 正例 + 14 反例 + mangle 判据 · 含 use mod::* 与 use mod::{a,b} 各 1 正例 + 6 条边界）"),
        bad_msg="tests/modules/run.sh", bad_dump=dump_all))

    # ---- ASan ----
    items.append(cmd(
        "asan", "tests", "ASan：内存安全的形状必须真的跑得干净",
        ["./tests/asan/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 个形状 ASan 干净"),
        bad_msg="tests/asan/run.sh", bad_dump=dump_all))

    # ---- 回收运行期的语义网（74 条断言 + ASan/UBSan；0.9s）----
    # 这一套一直在跑却**没接进来**（`grep tests/reclaim tools/check.py` 只有 shapes/perf/gaps）。
    # 它覆盖的正是"计数/清扫/归还"的语义面，是安全那一半的主网。
    items.append(cmd(
        "reclaim-unit", "tests",
        "回收运行期语义网：74 条断言 + ASan/UBSan（tests/reclaim/run.sh）",
        ["./tests/reclaim/run.sh"], timeout=600,
        ok_msg=ok_first(r"ASan/UBSan 运行干净"),
        bad_msg="tests/reclaim/run.sh（回收语义/内存安全回归）", bad_dump=dump_fail(12)))

    # ---- arena 层号 ----
    items.append(cmd(
        "arena-levels", "tools",
        "arena 层号：检查器是唯一权威、codegen 只翻译（哨兵 EXTC_DBG_ARENA=1 在生成时比对层号，全量并行编译）",
        sh('python3 tools/parrun.py --mode arena-scan'),
        ok_msg=ok_out, bad_msg="arena 层号漂移", bad_dump=dump_all))

    # ---- 全限定名 ----
    items.append(cmd(
        "qname", "tests", "全限定名：全名是权利 · `as` 别名是方便",
        ["./tests/qname/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（6 正例 + 1 反例 + 1 结构判据）"),
        bad_msg="tests/qname/run.sh", bad_dump=dump_fail(8)))

    # ---- std::fs 命名规范 ----
    items.append(cmd(
        "fs-shape", "tests", "std::fs 命名规范：读型/写型分开 ⇒ 误用编不过",
        ["./tests/fs-shape/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（1 正例 + 2 反例 + 常量不外露 + 规范入档）"),
        bad_msg="tests/fs-shape/run.sh", bad_dump=dump_fail(8)))

    # ---- 文件归属 ----
    items.append(cmd(
        "fs", "tests", "文件归属：程序拥有 ⇒ 忘关报警告 · 双关安全 · ASan",
        ["./tests/fs/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（fd 恒定 + canary + 泄漏警告 + readAll + 双关 + closed + ASan）"),
        bad_msg="tests/fs/run.sh", bad_dump=dump_fail(8)))

    # ---- 命令行 ----
    items.append(cmd(
        "argv", "tests", "命令行（IO.md §7）：`main(args)` ⇒ args.len 含程序名 · 形状写错编译期挡住",
        ["./tests/argv/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（正例 + 边界 + 无参哨兵 + 3 反例）"),
        bad_msg="tests/argv/run.sh", bad_dump=dump_fail(8)))

    # ---- 池 ----
    items.append(cmd(
        "pool", "tests",
        "池（期 0 库级 slot map + 期 1 运行期池注册表：handle · dense · churn 不涨 · 块退出带走子树 —— 见 POOLS.md）",
        ["./tests/pool/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（handle/dense/世代 · churn 平 · canary 会响 · ASan）"),
        bad_msg="tests/pool/run.sh", bad_dump=dump_fail(8)))

    # ---- 有序 map ----
    items.append(cmd(
        "map", "tests", "有序 map<K, V>（POOLS.md §11：B 树；第一步是单节点有序表）",
        ["./tests/map/run.sh"], timeout=600,
        ok_msg=tail1, bad_msg="tests/map/run.sh", bad_dump=dump_fail(8)))

    # ---- 实例化类型上的 impl 与 dyn（4 条各自计数）----
    for nm, argv, to in (("instimpl", ["./tests/instimpl/run.sh"], 600),
                         ("lambda", ["./tests/lambda/run.sh"], 600),
                         ("ext", ["./tests/ext/run.sh"], 600),
                         ("time", ["./tests/time/run.sh"], 900)):
        items.append(cmd(
            nm, "tests",
            "实例化类型上的 impl 与 dyn（slice<u8> 挂方法 · 按实例隔离 · dyn 派发 · L 债金丝雀）",
            argv, timeout=to, ok_msg=tail1,
            bad_msg=f"{argv[0].lstrip('./')}", bad_dump=dump_fail(8)))

    # ---- 期 0 · 哈希表 ----
    items.append(cmd(
        "hashmap", "tests",
        "期 0 · 哈希表（key→value 随机的正解：开放寻址 + 墓碑 · 见 POOLS.md §10.2）",
        ["./tests/hashmap/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（新键/覆盖/碰撞/墓碑 churn 平/canary/ASan）"),
        bad_msg="tests/hashmap/run.sh", bad_dump=dump_fail(8)))

    # ---- 展示代码 ----
    items.append(cmd(
        "showcase", "tests",
        "展示代码（examples/showcase-*.extc：文档里的六段，每次编译并运行）",
        ["bash", "tests/showcase/run.sh"], timeout=300,
        ok_msg=ok_count_ok(" 项（Arena 返回值 · home 机制 · 池身份 · 视图 · impl · extern!）"),
        bad_msg="tests/showcase/run.sh", bad_dump=dump_fail(8)))

    # ---- STL ----
    items.append(cmd(
        "stl", "tests",
        "STL 库（一个库装所有动态容器；容器建在池上 —— 见 POOLS.md 期 3）",
        ["./tests/stl/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（vector：翻倍/dense 连续/shrink/clear · ASan）"),
        bad_msg="tests/stl/run.sh", bad_dump=dump_fail(8)))

    # ---- 线性关联容器 ----
    items.append(cmd(
        "linmap", "tests",
        "线性关联容器（std::linmap / std::linset：只用 ==，不依赖 #57 的 hash）",
        ["bash", "tests/linmap/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（覆盖/缺失/删除/扩容 · 结构体键 · 集合）"),
        bad_msg="tests/linmap/run.sh", bad_dump=dump_fail(6)))

    # ---- 注解（可执行才跑；不可执行时原脚本静默跳过 —— 这里显式报「跳过」且不计数）----
    def fn_annot(ctx: Runner, it: Item) -> Outcome:
        script = os.path.join(ROOT, "tests/annot/run.sh")
        if not os.access(script, os.X_OK):
            return Outcome(None, "跳过：tests/annot/run.sh 不可执行（原 check.sh 同样跳过且不计数）")
        p = ctx.run(["tests/annot/run.sh"], echo=True)
        if p.rc == 0:
            return Outcome(True, tail1(p.out))
        return Outcome(False, "tests/annot/run.sh", ind(lines(p.out)))
    items.append(Item(
        "annot", "tests",
        "注解（@inline 要真的生效 · 写错的注解必须编译期报错）；脚本不可执行则跳过",
        fn_annot))

    # ---- 运算符重载 ----
    items.append(cmd(
        "ops", "tests",
        "运算符重载（方法名就是运算符：比较六个 + 算术五个 + 流 << >> · 按右操作数类型重名 · 泛型体里实例化时检查）",
        ["bash", "tests/ops/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（具体类型 · 有牙 · 泛型体 · 流运算符 · 重名分派 · mangle · 9 反例 · 两条不许漏到 gcc）"),
        bad_msg="tests/ops/run.sh", bad_dump=dump_fail(12)))

    # ---- @noCopy ----
    items.append(cmd(
        "nocopy", "tests",
        "@noCopy（状态有身份的类型不许按值拷：正例 4 件事 · 四个复制点全挡）",
        ["bash", "tests/nocopy/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（新值/ref 传参/读字段/调方法照旧 · 绑定·实参·字段·赋值四个复制点全挡 ✓）"),
        bad_msg="tests/nocopy/run.sh", bad_dump=dump_fail(8)))

    # ---- 构造函数 ----
    items.append(cmd(
        "ctor", "tests",
        "构造函数（`T(args)` 就是 `T::new(args)`：糖=显式 · 可失败 · 泛型 · 3 反例）",
        ["bash", "tests/ctor/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（正例 3 · 反例 3 —— 无 new 教你怎么写 · 返回类型必须对 · 实参个数）"),
        bad_msg="tests/ctor/run.sh", bad_dump=dump_fail(8)))

    # ---- 泛型组合矩阵 ----
    items.append(cmd(
        "genmatrix", "tests",
        "泛型组合矩阵（T 的位置 × 特性 · 13 格 · 缺口清单空 —— 见 GENERICS.md）",
        ["bash", "tests/genmatrix/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（13 格全过 ✓ · #60/#61/#62/#63/#64/#65 六条当天撞到当天修完、当天搬进矩阵或正例 ⇒ 缺口清单空，机制留着）"),
        bad_msg="tests/genmatrix/run.sh", bad_dump=dump_fail(8)))

    # ---- 头文件 include guard ----
    items.append(cmd(
        "guards", "docs",
        "头文件 include guard：每个 src/*.h 的最后一个非空行必须是 #endif（内容不许落在 #endif 之后）",
        sh('python3 tools/check_guards.py'),
        ok_msg=tail1, bad_msg="tools/check_guards.py", bad_dump=dump_head(8)))

    # ---- 攻击库 ----
    def fn_attacks(ctx: Runner, it: Item) -> Outcome:
        p = ctx.run(sh('python3 tools/parrun.py --mode compiles'),
                    merge=False, echo=False)
        if p.err and not ctx.quiet:
            sys.stdout.write(p.err)
            sys.stdout.flush()
        baseline_path = os.path.join(ROOT, "tests/attacks/BASELINE")
        with open(baseline_path, encoding="utf-8") as f:
            baseline = f.read()
        if p.out == baseline:
            return Outcome(True,
                           f"通过集合与基线一致（{baseline.count(chr(10))} 条已知安全 + 其余全部被挡）")
        diff = list(difflib.unified_diff(baseline.splitlines(), p.out.splitlines(),
                                         "BASELINE", "now", lineterm=""))
        return Outcome(False, "攻击库的通过集合变了！", ind(diff))
    items.append(Item(
        "attacks", "tools",
        "攻击库：通过的必须是 BASELINE 里那几条（并行编译，结果集与串行逐字节一致 ⇒ 没放松）",
        fn_attacks))

    # ================= 基准（quick 跳过；显示项不计数）=================
    items.append(cmd(
        "gate-checkc", "bench",
        "闸门①：全语料 --check-c（挡「extc 报成功、生成物编不过」；gcc + clang -c -O2）",
        sh('python3 tools/gate_checkc.py --scope full'), timeout=1800, bench=True,
        ok_msg=ok_first(r"^\[checkc\]"),
        bad_msg="tools/gate_checkc.py（全语料 --check-c 变红）", bad_dump=dump_fail(14)))
    items.append(cmd(
        "gate-asan-corpus", "bench",
        "闸门②：正例生成物 ASan+UBSan（-O2 会把 UB 优化掉，这里必须真跑）",
        sh('python3 tools/gate_asan_corpus.py --scope full'), timeout=1800, bench=True,
        ok_msg=ok_first(r"^\[asan\]"),
        bad_msg="tools/gate_asan_corpus.py（正例生成物出现 sanitizer 报告）", bad_dump=dump_fail(14)))
    items.append(cmd(
        "golden", "bench",
        "生成物快照（观察项：与冻结哈希的差异不作为判据；判据只有「必须是合法 C」）",
        ["./tools/golden.sh"], timeout=900, bench=True,
        ok_msg=lambda out: first_matching(out, r"^  ok").strip(),
        bad_msg="tools/golden.sh（生成物里有非法 C）", bad_dump=dump_fail(8),
        extra_ok=lambda out: [ln for ln in out.split("\n") if ln.startswith("  · ")]))

    # ---- 闸门·回收缺口矩阵的棘轮（bench 档：跑一遍矩阵约 1 分钟）----
    # `run-gaps.sh` 自己恒 exit 0，这里把它的结果变成有牙的棘轮：① 任何形状的 `asan=<非 0>`；
    # ② 19 行规模判据里"收不掉"的集合。两者都是"多一条红、少一条也红"（只许缩）。
    items.append(cmd(
        "gate-reclaim-gaps", "bench",
        "闸门·回收缺口矩阵棘轮：19 个形状的收不掉集合 + 每个形状的 sanitizer 结论都只许缩（两份基线）",
        sh('python3 tests/reclaim-gaps/gate.py'), timeout=1800, bench=True,
        ok_msg=ok_first(r"^\[reclaim-gaps\] 收不掉"),
        bad_msg="tests/reclaim-gaps/gate.py（新增收不掉的形状 / 新增 sanitizer 失败，或基线过期）",
        bad_dump=dump_fail(12)))

    def display(name: str, desc: str, argv: list, post) -> Item:
        def fn(ctx: Runner, it: Item) -> Outcome:
            p = ctx.run(argv, echo=False)
            for ln in post(p.out):
                print(ln, flush=True)
            return Outcome(None, f"{name}（观察项，不计 ok/FAIL）")
        return Item(name, "bench", desc, fn, bench=True)
    items.append(display(
        "bench-extc-vs-c", "基准：extC vs C（观察项，输出非判据）",
        ["./bench/run.sh"], lambda out: ind(tail(out, 12))))
    items.append(display(
        "bench-heavy", "基准：重负载 bt / radix / mandelbrot（观察项）",
        ["./bench/heavy/run.sh"], lambda out: ind(lines(out))))

    # ---- STL 容器横评：dyn 三档 + stl ----
    items.append(cmd(
        "bench-dyn-stress", "bench",
        "基准·dyn 极端压测（测量不是闸门，但校验和不符必须红 —— 见 bench/dyn/REPORT.md）",
        ["./bench/dyn/stress/run.sh"], timeout=None, bench=True,
        env={"N": "5000000", "NIMM": "200", "RUNS": "1"},
        ok_msg=lambda out: f"dyn 极端压测（{first_matching(out, r'^通过')}）",
        bad_msg="bench/dyn/stress", bad_dump=dump_fail(8)))
    items.append(cmd(
        "bench-dyn-alloc", "bench",
        "基准·dyn 构造成本（拷贝 vs 分配，N=50000）",
        ["./bench/dyn/alloc/run.sh"], timeout=None, bench=True, env={"N": "50000"},
        ok_msg=ok_const("dyn 构造成本（拷贝 vs 分配）"),
        bad_msg="bench/dyn/alloc", bad_dump=dump_fail(6)))
    items.append(cmd(
        "bench-dyn-real", "bench",
        "基准·dyn 现实负载（P=512 MIX=8 RUNS=1）",
        ["./bench/dyn/real/run.sh"], timeout=None, bench=True,
        env={"P": "512", "MIX": "8", "RUNS": "1"},
        ok_msg=lambda out: f"dyn 现实负载（{first_matching(out, r'^通过')}）",
        bad_msg="bench/dyn/real", bad_dump=dump_fail(8)))

    def fn_bench_stl(ctx: Runner, it: Item) -> Outcome:
        log = "/tmp/extc-bench-stl.log"
        p = ctx.run(["./bench/stl/run.sh"], echo=False)
        with open(log, "w", encoding="utf-8") as f:
            f.write(p.out)
        if p.rc == 0:
            return Outcome(True, first_matching(p.out, r"^通过"))
        return Outcome(False, "bench/stl", ind(tail(p.out, 12)))
    items.append(Item(
        "bench-stl", "bench",
        "基准·STL 容器横评（extC 的容器 vs C++ STL；同算法同参数，校验和必须一致）",
        fn_bench_stl, bench=True))

    items.append(cmd(
        "bench-app", "bench",
        "基准·综合应用场景横评（会话表 / 路由表 / 日志缓冲；extC STL vs C++ STL vs Go；SCALE=1 当回归、WRITE_RESULTS=0 不冲表）",
        ["./bench/app/run.sh"], timeout=None, bench=True,
        env={"SCALE": "1", "RUNS": "1", "WRITE_RESULTS": "0"},
        ok_msg=ok_const("综合场景横评（三语言校验和一致 ✓ · 完整表见 bench/app/RESULTS.md）"),
        bad_msg="综合场景横评（构建失败 / 语言之间对拍不一致 ✗）", bad_dump=dump_fail(20)))

    items.append(display(
        "bench-stress", "基准：随机负载压力（同种子对拍 C；观察项）",
        ["./bench/stress/run.sh"], lambda out: ind(lines(out))))

    def fn_bench_compile(ctx: Runner, it: Item) -> Outcome:
        p = ctx.run(["./bench/compile/run.sh"], env={"NS": "500", "RUNS": "1"}, echo=False)
        if p.rc != 0:
            return Outcome(False, "编译时长压测")
        for ln in ind(sed_range(lines(p.out), r"^N ", None)):
            print(ln, flush=True)
        if "ERR" in p.out:
            return Outcome(False, "编译时长压测（有 ERR）")
        return Outcome(True, "编译时长压测（N=500 全通）")
    items.append(Item(
        "bench-compile", "bench",
        "基准·编译时长（合成大程序 NS=500；顺带抓「生成的 C 编不过」那类问题）",
        fn_bench_compile, bench=True))

    def fn_bench_oi(ctx: Runner, it: Item) -> Outcome:
        p = ctx.run(["./bench/oi/run.sh"], env={"N": "500000", "RUNS": "1"}, echo=False)
        if p.rc == 0:
            for ln in ind(sed_range(lines(p.out), r"^语言", r"^$")):
                print(ln, flush=True)
            return Outcome(True, "OI 横评（四语言输出一致 ✓）")
        for ln in ind(tail(p.out, 20)):
            print(ln, flush=True)
        return Outcome(False, "OI 横评（有构建失败 / 语言之间对拍不一致 ✗）")
    items.append(Item(
        "bench-oi", "bench",
        "基准·OI 数量级四语言横评（三维偏序 CDQ+BIT；N=500000 缩小规模当回归）",
        fn_bench_oi, bench=True))

    def fn_bench_oi_persist(ctx: Runner, it: Item) -> Outcome:
        p = ctx.run(["./bench/oi/persist/run.sh"], echo=False,
                    env={"N": "200000", "Q": "200000", "V": "200000", "RUNS": "1"})
        if p.rc == 0:
            ls = lines(p.out)
            for ln in ind([l for l in ls if l.startswith("  小规模对拍")]
                          + sed_range(ls, r"^语言", r"^$")):
                print(ln, flush=True)
            return Outcome(True, "主席树横评（四语言一致 + 暴力对拍 ✓）")
        for ln in ind(tail(p.out, 20)):
            print(ln, flush=True)
        return Outcome(False, "主席树横评（构建失败 / 对拍不一致 ✗）")
    items.append(Item(
        "bench-oi-persist", "bench",
        "基准·主席树四语言横评（P3834；N=Q=V=200000 小规模 + 暴力对拍）",
        fn_bench_oi_persist, bench=True))

    return items


# --------------------------------------------------------------------------
# 耗时表
# --------------------------------------------------------------------------
def times_path(args) -> str:
    return args.times or os.environ.get("SEC_TIMES") or SEC_TIMES_DEFAULT


def read_times_full(path: str) -> dict:
    """name -> (seconds, status, time, group)，缺失/坏行忽略。"""
    out: dict = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for ln in f:
            if ln.startswith("#") or not ln.strip():
                continue
            parts = ln.rstrip("\n").split("\t")
            if len(parts) >= 4:
                try:
                    out[parts[0]] = (float(parts[2]), parts[3],
                                     parts[4] if len(parts) > 4 else "",
                                     parts[1])
                except ValueError:
                    pass
    return out


def write_times(path: str, results: dict) -> None:
    """合并写回：results = name -> (seconds, status, time, group)。"""
    prev = read_times_full(path)
    prev.update(results)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("# name\tgroup\tseconds\tstatus\ttime\n")
        for name, (secs, status, ts, group) in sorted(prev.items()):
            f.write(f"{name}\t{group}\t{secs:.3f}\t{status}\t{ts}\n")
    os.replace(tmp, path)


# --------------------------------------------------------------------------
# 选择
# --------------------------------------------------------------------------
# 分档（名字要与实际相符）：
#   smoke   —— 快速高信号集，改完代码第一件事跑这个（见 SMOKE，≈10s）
#   nobench —— 非基准全量（CI 跑这档）；`quick` 是它的历史别名（会自己说明一句）
#   all     —— 含基准（`full` 同义）
#   docs    —— 只要文档/工具项（秒级）
MODE_TOKENS = {"all", "full", "smoke", "nobench", "quick", "docs"}

# smoke 集：**编译 + 改动面最相关的高信号判据**，按"改名要一起改这里"维护。
# 选的标准是"能在 10 秒内给出'这次改动有没有把地基弄坏'的信号"，
# 不是"跑得快的项都塞进来"（≤4s 的项有 70 个、合计 75s，那还是慢）。
SMOKE = (
    "build",                   # 编译器自己得先编出来
    "check-plan-seam",         # codegen 只许经 plan.h 读分析（接缝）
    "check-layering",          # 分层不许倒挂
    "check-no-history",        # 活文档/注释不许出现历史词（词表棘轮）
    "gate-callsite",           # 运行期调用点的发射形状
    "gate-hole-c",             # C 洞最小复现（种子完整性）
    "gate-reclaim-perf",       # 合成形状的指令数（回收开销不许被付回）
    "gate-reclaim-perf-shapes",# 真实形状 bt/rebuild 的指令数 + 开关语义
    "gate-reclaim-elide",      # 计数收窄语料（含两条健全性判据 + 口径自检）
    "gate-reclaim-tax",        # 目标度量：不需要回收的形状上，回收税趋近 1.00×
    "gate-wiring",             # 判据自己：自称门禁的脚本必须进电池（<0.1s）
    "gate-note-part",           # 参与区域数不变量（结构门禁，~1s）
    "gate-invariant-table",     # 不变量清单完整性（结构门禁，~1s）
    "gate-skip-table",           # 跳过清单同步（结构门禁，~1s）
    "gate-doc-refs",             # 注入 ↔ 文档引用完整（结构门禁，~1s）
    "reclaim-unit",            # 回收运行期语义网（70 断言 + ASan，0.9s）
)


def select(items: list, mode: Optional[str], only: list) -> list:
    if mode in (None, "all", "full"):
        sel = list(items)
    elif mode in ("nobench", "quick"):
        if mode == "quick":
            print("注：`quick` 就是 `nobench`（非基准**全量**）—— 名字不实，"
                  "要快请用 `./check.sh smoke`（≈10s）。", flush=True)
        sel = [it for it in items if not it.bench]
    elif mode == "smoke":
        known = {it.name: it for it in items}
        missing = [n for n in SMOKE if n not in known]
        if missing:
            raise SystemExit("smoke 集里有不存在的项（名字改了要同步 SMOKE）："
                             + " ".join(missing))
        sel = [known[n] for n in SMOKE]
    elif mode == "docs":
        sel = [it for it in items if it.group == "docs"]
    else:
        raise SystemExit(f"未知模式：{mode}（可用：all / nobench / smoke / docs）")

    if only:
        known_names = {it.name for it in items}
        known_groups = {it.group for it in items}
        matched = set()
        for tok in only:
            if tok in known_names or tok in known_groups:
                matched.add(tok)
            else:
                near = difflib.get_close_matches(tok, sorted(known_names | known_groups), n=5)
                hint = ("；相近的有：" + " ".join(near)) if near else ""
                raise SystemExit(
                    f"--only {tok}：没有这个名字/组{hint}\n"
                    f"名字：{' '.join(sorted(known_names))}\n"
                    f"组：{' '.join(sorted(known_groups))}")
        sel = [it for it in sel if it.name in matched or it.group in matched]
    return sel


def with_build(items: list, all_items: list) -> list:
    """选了不含 build 的组合时自动先跑构建（只要选中项里有需要 build/extc 的）。"""
    if any(it.name == "build" for it in items):
        build = [it for it in items if it.name == "build"]
        rest = [it for it in items if it.name != "build"]
        return build + rest
    if any(it.needs_build for it in items):
        build = [it for it in all_items if it.name == "build"]
        print("（选中项依赖 build/extc ⇒ 自动先跑构建）", flush=True)
        return build + items
    return items


# --------------------------------------------------------------------------
# 跑
# --------------------------------------------------------------------------
# `-j N` 的三条**敏感清单**：并行只许换**墙钟**，不许换**判据** ✗。
#
#   · WALL_CLOCK —— 结论会随负载漂的（吞吐 MB/s、`/usr/bin/time -f %e`、`date +%s`、
#     sleep/tick 的界）。它们进池，但**优先取 worker**：`j <= nproc` ⇒ 各自独占一个核。
#   · INNER —— 自己开线程池/进程池的项（parrun 与三个 gate 工具，`min(nproc,16)` 个 worker）。
#     它们彼此之间**单槽**（同一时刻只跑一个），但可以和别的项并发：两个撞在一起就是 32 个
#     worker 抢 24 个核，既慢又可能把超时判据推红；和"单进程"项并发则没有正确性问题。
#   · COLLIDE —— 用**显式 `-o build/<名字>`** 写同一路径的项。`extc --run` 的固定路径已经由
#     逐项 `EXTC_RUN_DIR` 隔离（见 `_exec_item`），但 `-o` 是脚本自己写的字面路径，隔离不了：
#     `map` 与 `hashmap` 都建 `build/map-churn`、`build/map-leak`。它们彼此之间串行（一把锁），
#     但可以和不相干的项并发。
# 孤儿回收不再需要独占：`reap_orphans()` 按**会话所有权**回收（只收登记过、且所有者已结束的
# 会话），不认识进程名，所以邻居正在跑的 `build/<名字>` 一个都不碰。它自己有常驻判据
# `gate-reap-orphans`（`tools/parrun.py --reap-selftest`）。
WALL_CLOCK = {
    "tests-coro": "tests/coro/run.sh 量墙钟与 RSS（`/usr/bin/time -q -f %M`）；协程调度时序",
    "hash": "吞吐绝对阈值（实测 sha1 287 MB/s vs 线 200 MB/s，余量只有 1.4×）",
    "hashmap": "同 hash/pool 一族：分配吞吐 + 峰值 RSS",
    "io": "tests/io/run.sh 用 `date +%s%N` 量「分块读」那一段（10 万行只量跑、不量编；线 50ms）",
    "json": "吞吐绝对阈值（`parse ≥ 200 MB/s`）",
    "map": "同 pool/hashmap 一族：分配/查找吞吐 + 峰值 RSS",
    "pool": "`/usr/bin/time -f %e` 量 pool-perf 的墙钟（小池 vs 大池）",
    "time": "tests/time/run.sh：sleep_ms ∈ [45,5000] 且 min_tick_ns < 1e6（墙钟界）",
    "gate-reclaim-evidence": "run-evidence.sh 量三条实现的 N/4N **耗时**（三方对照）",
    "gate-reclaim-switch": "run-switch.sh 量 N vs 4N 峰值/登记量 + 30 条形状语义",
    "gate-reclaim-count": "同一个 run-switch.sh（EXTC_RC_NOCOUNT=0 档）",
}
# **自己开池、彼此单槽**的项：脚本内部按 `min(nproc,16)` 起 worker，两个同时跑就超订。
INNER = {
    "tests": "tests/run.sh → `tools/parrun.py`（cases 模式，16 worker）",
    "tests-assert": "同一个 tests/run.sh（EXTC_DBG=1 断言版）",
    "warnings": "tests/warnings/run.sh → `parrun.py --mode warn-scan`",
    "arena-levels": "`parrun.py --mode arena-scan`",
    "attacks": "`parrun.py --mode compiles`",
    "gate-difffuzz": "`tools/gate_difffuzz.py` → ProcessPoolExecutor(min(8, jobs))",
    "gate-checkc": "`tools/gate_checkc.py` → ProcessPoolExecutor(jobs)（基准档）",
    "gate-asan-corpus": "`tools/gate_asan_corpus.py` → ProcessPoolExecutor(jobs)（基准档）",
    "gate-perturb-corpus": "`tests/perturb/census.py` 自己开 ThreadPool（基准档：过全语料）",
}
# parrun.py 的默认 worker 上界（`min(cpu_count, 16)`）—— 估核数用，见 `_lane_room`。
INNER_WORKERS = 16
# **互相撞 `build/` 的项**：这些脚本用 `extc --run`/`--build` 往**固定的** `build/<名字>` 写，
# 两个脚本同名字就会互相截断/覆盖（静态可查的两组：7 个脚本都写 `build/main`；`map` 与
# `hashmap` 都写 `build/map-churn`、`build/map-leak`）。它们**彼此之间**串行（一把锁），
# 但仍可以和别的项并发 —— 不碰 CWD（沙箱会把 `-I $PWD/stdlib` 的路径变掉，触发编译器的
# "特权层" 判定，实测 `modules` 因此报错）。
COLLIDE = {
    "map": "`-o build/map-churn`、`-o build/map-leak`（与 hashmap 同名）",
    "hashmap": "`-o build/map-churn`、`-o build/map-leak`（与 map 同名）",
}


def resolve_jobs(args) -> int:
    """生效的并行度：`-j N`；默认 `min(4, nproc)`；`-j1` = 逐项串行（今天的行为）。

    显式给得比核数大时按核数收（并发不是越多越快，超订只会互相拖），并把收窄打印出来。
    """
    nproc = os.cpu_count() or 1
    want = getattr(args, "jobs", None)
    if want is None:
        return min(4, nproc)
    if want < 1:
        raise SystemExit(f"-j 要 ≥1（给的是 {want}）；-j1 = 逐项串行")
    if want > nproc:
        print(f"注：-j {want} 超过核数 {nproc} ⇒ 收到 -j {nproc}（超订只会互相拖）", flush=True)
        return nproc
    return want


def print_run_config(items: list, j: int) -> None:
    """打印生效的并行度与三批敏感项 —— 结论要可复现，配置就必须看得见。"""
    wall = [it for it in items if it.name in WALL_CLOCK]
    inner = [it for it in items if it.name in INNER]
    coll = [it for it in items if it.name in COLLIDE]
    if j <= 1:
        print("并行：关（-j1 = 逐项串行，今天的行为）", flush=True)
        return
    nproc = os.cpu_count() or 1
    print(f"并行：-j {j}（默认 min(4, nproc)={min(4, nproc)}；-j1 = 旧行为）· nproc={nproc}", flush=True)
    if inner:
        room = _lane_room(j)
        print(f"  **单槽** {len(inner)} 项（自己开 min(nproc,{INNER_WORKERS}) 个 worker ⇒ 彼此串行，"
              f"不与同类嵌套；与池并发：{'是' if room else '否（核数放不下 ⇒ 先单槽、再池）'}）", flush=True)
        for it in inner:
            print(f"    {it.name} —— {INNER[it.name]}", flush=True)
    if coll:
        print(f"  **互斥** {len(coll)} 项（彼此之间串行，可与别的项并发；都往固定的 `build/<名字>` 写）", flush=True)
        for it in coll:
            print(f"    {it.name} —— {COLLIDE[it.name]}", flush=True)
    if wall:
        print(f"  墙钟敏感 {len(wall)} 项：进池、**优先**取 worker（j ≤ nproc ⇒ 各自独占一个核）", flush=True)
        for it in wall:
            print(f"    {it.name} —— {WALL_CLOCK[it.name]}", flush=True)




def _exec_item(it: Item, args, capture: Optional[list]):
    """跑一项：返回 (Outcome, 秒数)。回调炸了算这一项 FAIL，不吞掉整轮。

    并行时给每项一个**私有的 `--run` 输出目录**（`EXTC_RUN_DIR`，绝对路径 —— 脚本会 `cd`）：
    `extc --run` 默认写 `build/<源文件基名>`，两个 item 撞上同名源文件（`tests/ops/generic.extc`
    与 `tests/ctor/generic.extc`）就会互相截断/覆盖（实测两个并发 `--run` 同一基名，50 轮里
    21 轮有一个失败）。默认（`-j1`、或环境里没有这个变量）路径仍是 `build/<基名>`，逐字节不变。
    """
    t0 = time.monotonic()
    env = None
    if (getattr(args, "jobs_eff", 1) or 1) > 1:
        d = os.path.join(ROOT, "build", "run", it.name)
        try:
            os.makedirs(d, exist_ok=True)
            env = {"EXTC_RUN_DIR": d}
        except OSError:
            env = None               # 建不出来就退回默认路径（宁可慢/偶发，也不许跑不起来）
    try:
        oc = it.fn(Runner(quiet=args.quiet, capture=capture, env=env), it)
    except Exception as exc:
        oc = Outcome(False, f"{it.name}：编排回调异常 {type(exc).__name__}: {exc}")
    return oc, time.monotonic() - t0


def _print_outcome(it: Item, oc: Outcome, secs: float, counters: list) -> str:
    """打印一项的结果行（ok/FAIL/--、extra、⏱）并计数。调用方保证独占 stdout。"""
    if oc.ok is None:
        status = "skip" if oc.msg.startswith("跳过") else "ran"
        if status == "skip":
            counters[2] += 1        # 跳过单列：汇总里说得出来"有几项没验"
        print(f"  --   {oc.msg}", flush=True)
    elif oc.ok:
        counters[0] += 1
        status = "ok"
        print(f"  \033[32mok\033[0m   {oc.msg}", flush=True)
    else:
        counters[1] += 1
        status = "FAIL"
        print(f"  \033[31mFAIL\033[0m {oc.msg}", flush=True)
    for ln in oc.extra:
        print(ln, flush=True)
    if oc.ok is not None:
        print(f"       ⏱ {secs:.3f}s", flush=True)
    return status


def _run_live(it: Item, args, counters: list) -> tuple:
    """独占跑一项：表头先打、子进程输出现场流式打印（`-j1` 的跑法）。"""
    print(f"\n== {it.name}（{it.group}）==", flush=True)
    oc, secs = _exec_item(it, args, capture=None)
    return oc, secs, _print_outcome(it, oc, secs, counters)



def _lane_room(j: int) -> bool:
    """单槽车道（内部开 `min(nproc,INNER_WORKERS)` 个 worker）能不能和池（j 个 worker）并发。

    判据是"两边都拿得到自己那份核"：`j + min(nproc,INNER_WORKERS) <= nproc`。核数不够就退化成
    "先单槽跑完、再跑池" —— 多花的时间换的是**不把机器压满**（超订既慢又可能把超时判据推到红）。
    """
    nproc = os.cpu_count() or 1
    return j + min(nproc, INNER_WORKERS) <= nproc


def _run_captured(it: Item, args, counters: list, lock: threading.Lock) -> tuple:
    """并行跑一项：子进程输出攒进 cap，跑完**整块**打出来（各项不许交错）。"""
    cap: list = []
    oc, secs = _exec_item(it, args, capture=cap)
    with lock:
        print(f"\n== {it.name}（{it.group}）==", flush=True)
        if cap:
            sys.stdout.write("".join(cap))
            sys.stdout.flush()
        status = _print_outcome(it, oc, secs, counters)
    return oc, secs, status


def run_items(items: list, ctx: Runner, args, all_items: list) -> int:
    j = getattr(args, "jobs_eff", None) or resolve_jobs(args)
    print_run_config(items, j)
    if j <= 1:
        return _run_items_serial(items, args)
    return _run_items_parallel(items, args, j)


def _times_of(runs: list) -> dict:
    out = {}
    for it, secs, status in runs:
        out[it.name] = (secs, status, time.strftime("%Y-%m-%dT%H:%M:%S"), it.group)
    return out


def _summary(counters: list) -> str:
    """汇总行：有跳过就单列出来 —— 跳过 = 没验，不许混进「通过」。"""
    if len(counters) > 2 and counters[2]:
        return f"通过 {counters[0]}，跳过 {counters[2]}，失败 {counters[1]}"
    return f"通过 {counters[0]}，失败 {counters[1]}"


def _finish(items_runs: list, tpath: str, args, counters: list) -> int:
    """写耗时表 + 打印最慢 N 项 + 总计。`items_runs` = [(Item, secs, status)]。"""
    times_out = _times_of(items_runs)
    results = [(it.name, it.group, secs, status) for it, secs, status in items_runs]
    write_times(tpath, times_out)
    top = getattr(args, "top", 10)
    slow = sorted([r for r in results if r[3] in ("ok", "FAIL")],
                  key=lambda r: r[2], reverse=True)[:top]
    if slow:
        print(f"\n== 最慢的 {len(slow)} 项 ==", flush=True)
        for name, group, secs, status in slow:
            print(f"  {secs:8.3f}s  {status:<4}  {name}（{group}）", flush=True)
    print(f"\n耗时表：{tpath}", flush=True)
    print(f"\n{_summary(counters)}", flush=True)
    return 0 if counters[1] == 0 else 1


def _run_items_serial(items: list, args) -> int:
    """`-j1`：逐项串行，表头先打、输出现场流式 —— 与加并行之前逐字节一致的行为。"""
    tpath = times_path(args)
    counters = [0, 0, 0]
    runs = []
    for it in items:
        oc, secs, status = _run_live(it, args, counters)
        runs.append((it, secs, status))
        if oc.ok is False and it.abort_on_fail:
            print(f"\n构建失败 ⇒ 中止（其余项都假设 build/extc 在）。", flush=True)
            write_times(tpath, _times_of(runs))
            print(f"\n{_summary(counters)}", flush=True)
            return 1
    return _finish(runs, tpath, args, counters)


def _run_items_parallel(items: list, args, j: int) -> int:
    """并行两段：build 独占 → 单槽车道（`INNER`）与 j-worker 池并发。

    三条纪律（都是被测出来的）：
      · `build` 必须独占最先（其余项都假设 build/extc 在）；
      · `INNER`（自己开 min(nproc,16) 个 worker 的项）彼此**单槽**：两个同时跑就是 32 个 worker
        抢 24 个核，既慢又可能把超时判据推红。和"单进程"项并发没有正确性问题，所以只限同类。
      · 池里写**固定** `build/<名字>` 的项（`COLLIDE`）彼此之间串行 —— 两个脚本同名就会互相
        截断/覆盖（静态可查：`build/main` 7 个脚本、`build/map-churn`/`map-leak` map 与 hashmap）。
        不碰 CWD：沙箱会把 `-I $PWD/stdlib` 的路径变掉，触发编译器的"特权层"判定（实测 `modules` 红）。

    `INNER` 车道先起（它是现在的最长边），池的墙钟项先提交；`j <= nproc` 保证每个 worker 有
    自己的核。每项输出攒完整块打印 ⇒ 不交错。
    """
    tpath = times_path(args)
    counters = [0, 0, 0]
    runs: list = []
    build = [it for it in items if it.name == "build"]
    inner = [it for it in items if it.name in INNER]
    rest = [it for it in items if it.name not in INNER and it.name != "build"]
    # 墙钟项**先提交**：池的前几个 worker 先服务它们（判据最脆的一批），其余项随后。
    pooled = [it for it in rest if it.name in WALL_CLOCK] + \
             [it for it in rest if it.name not in WALL_CLOCK]

    # ① build（独占、在仓库根：它就是 `make`；失败即中止）
    for it in build:
        oc, secs, status = _run_live(it, args, counters)
        runs.append((it, secs, status))
        if oc.ok is False and it.abort_on_fail:
            print(f"\n构建失败 ⇒ 中止（其余项都假设 build/extc 在）。", flush=True)
            write_times(tpath, _times_of(runs))
            print(f"\n{_summary(counters)}", flush=True)
            return 1

    # ② 单槽车道（INNER：自己开 min(nproc,16) 个 worker）+ ③ 池（j 个 worker）。
    #    顺序上**先起单槽车道**：那 5 项合计 ~76s，是现在的最长边；它和池并发 ⇒ 总时长 ≈ 76s。
    #    核数放不下（`_lane_room` 假）就退化成"先单槽、再池"，只慢不错。
    lock = threading.Lock()
    collide_lock = threading.Lock()

    def run_pooled(it: Item) -> tuple:
        """池里的项：INNER 不会走到这里（它们走单槽车道）；COLLIDE 组内串行。"""
        if it.name in COLLIDE:
            with collide_lock:
                return _run_captured(it, args, counters, lock)
        return _run_captured(it, args, counters, lock)

    def run_lane() -> None:
        for it in inner:
            oc, secs, status = _run_captured(it, args, counters, lock)
            runs.append((it, secs, status))

    if _lane_room(j):
        with concurrent.futures.ThreadPoolExecutor(max_workers=j) as ex:
            futs = [ex.submit(run_pooled, it) for it in pooled]
            run_lane()
            for it, fut in zip(pooled, futs):
                oc, secs, status = fut.result()
                runs.append((it, secs, status))
    else:
        run_lane()
        if pooled:
            with concurrent.futures.ThreadPoolExecutor(max_workers=j) as ex:
                futs = [ex.submit(run_pooled, it) for it in pooled]
                for it, fut in zip(pooled, futs):
                    oc, secs, status = fut.result()
                    runs.append((it, secs, status))

    return _finish(runs, tpath, args, counters)


# --------------------------------------------------------------------------
# --list / --changed
# --------------------------------------------------------------------------
def do_list(items: list, tpath: str, mode: Optional[str] = None,
            total: Optional[int] = None) -> int:
    """列项。带 `基准` 列 —— 于是"`nobench` 里到底有没有 bench"一眼可查。

    给了档位（`./check.sh --list nobench`）就**按档位过滤**（bench 项默认不出现，这正是用户
    被老 `--list` 骗到的地方），并用 `--all` 看全部；不给档位时列全部、但标出基准列。
    """
    tf = read_times_full(tpath)
    print(f"{'名称':<26}{'组':<9}{'基准':<7}{'最近耗时':<10}说明")
    for it in items:
        last = f"{tf[it.name][0]:.2f}s" if it.name in tf else "-"
        print(f"{it.name:<26}{it.group:<9}{'bench' if it.bench else '-':<7}{last:<10}{it.desc}")
    n_bench = sum(1 for it in items if it.bench)
    shown = f"共 {len(items)} 项：非基准 {len(items) - n_bench} · 基准 {n_bench}"
    if mode is not None:
        shown += f"（档位 {mode} ⇒ 基准项已隐藏；--all 或去掉档位看全部）"
    elif total is not None and total != len(items):
        shown += f"（全部 {total} 项）"
    else:
        shown += "（nobench/quick 跳过带 bench 标记的项；`--list nobench` 只看非基准）"
    print(f"\n{shown}")
    print(f"（smoke = 快速高信号集 {len(SMOKE)} 项；docs = {sum(1 for it in items if it.group == 'docs')} 项文档/工具）")
    groups: dict = {}
    for it in items:
        groups.setdefault(it.group, []).append(it.name)
    print("组：" + " · ".join(f"{g}({len(v)})" for g, v in sorted(groups.items())))
    return 0


def git_changed() -> list:
    def g(*a):
        p = subprocess.run(["git", "-C", ROOT] + list(a), text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if p.returncode != 0:
            raise SystemExit(f"git {' '.join(a)} 失败：{p.stderr.strip()}")
        return p.stdout
    tracked = [ln for ln in g("diff", "--name-only", "HEAD").splitlines() if ln]
    untracked = [ln for ln in g("ls-files", "--others", "--exclude-standard").splitlines() if ln]
    return sorted(set(tracked) | set(untracked))


def classify(paths: list):
    docs, bench, code, self_ = [], [], [], []
    for p in paths:
        if p in SELF_FILES:
            self_.append(p)
        elif p.startswith("bench/"):
            bench.append(p)
        elif p.startswith("docs/") or p.endswith(".md"):
            docs.append(p)
        else:
            code.append(p)
    return docs, bench, code, self_


def recommend(paths: list):
    docs, bench, code, self_ = classify(paths)
    if bench:
        return "all", "有 bench/ 改动 ⇒ 该跑含基准的完整档", docs, bench, code, self_
    if code:
        return "nobench", "有源码/测试/工具改动 ⇒ nobench（非基准全量）", docs, bench, code, self_
    if docs:
        return "docs", "只有文档改动 ⇒ docs（秒级）", docs, bench, code, self_
    if self_:
        return "nobench", "只有编排层自身改动 ⇒ nobench（被改的就是 runner，值得完整验一遍）", docs, bench, code, self_
    return "docs", "工作区相对 HEAD 没有改动 ⇒ docs（秒级自检）", docs, bench, code, self_


def do_changed(items: list, args, all_items: list) -> int:
    paths = git_changed()
    mode, why, docs, bench, code, self_ = recommend(paths)
    print(f"git diff --name-only HEAD + 未跟踪：{len(paths)} 个文件")
    for group, label in ((code, "代码/测试/工具"), (bench, "bench"), (docs, "文档"), (self_, "编排层自身")):
        if group:
            print(f"  [{label}] " + " ".join(group))
    print(f"\n建议跑：{mode} —— {why}")
    if not args.run:
        print(f"（加 --run 直接跑；或 ./check.sh {mode}）")
        return 0
    print(f"\n⇒ 实跑模式 {mode}\n", flush=True)
    sel = select(items, mode, [])
    sel = with_build(sel, all_items)
    return run_items(sel, Runner(quiet=args.quiet), args, all_items)


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def resolve_mode(all_items: list, mode: Optional[str], only_in: list) -> tuple:
    """位置参数解析：模式（all/nobench/smoke/docs）或名字/组；返回 (mode, only)。

    `main` 与 `--list` 共用这一份，于是两处的口径不会漂。
    """
    only = list(only_in)
    if mode is not None and mode not in MODE_TOKENS:
        # 位置参数也可能是名字/组（方便 ./check.sh tests）
        if mode in {it.name for it in all_items} or mode in {it.group for it in all_items}:
            only.append(mode)
            mode = None
        else:
            near = difflib.get_close_matches(mode, sorted({it.name for it in all_items}
                                                          | {it.group for it in all_items}), n=5)
            hint = ("；相近的有：" + " ".join(near)) if near else ""
            raise SystemExit(f"选不出来：位置参数 {mode!r} 既不是模式(all/nobench/smoke/docs)也不是名字/组{hint}")
    return mode, only


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="./check.sh", add_help=True,
        description="extC 自检编排层：检查项是数据，runner 负责选/跑/计时/统计。")
    ap.add_argument("mode", nargs="?", default=None,
                    help="all（默认，含基准）/ nobench（非基准全量，别名 quick）/ smoke（快速高信号集，≈10s）/ docs（文档·工具，秒级）")
    ap.add_argument("--only", action="append", default=[], metavar="名或组",
                    help="只跑这些项（名字或组；可多次）")
    ap.add_argument("-j", "--jobs", type=int, default=None, metavar="N",
                    help=f"并行度：默认 min(4, nproc)={min(4, os.cpu_count() or 1)}；"
                         "`-j1` = 逐项串行（旧行为）。自己开池的项（`INNER`）走单槽车道，"
                         "墙钟敏感项（`WALL_CLOCK`）进池但优先占核；写同一 `build/<名字>` 的项（`COLLIDE`）彼此串行")
    ap.add_argument("--all", action="store_true",
                    help="配合 --list：连基准（bench）项一起列；带档位时默认隐藏基准项")
    ap.add_argument("--list", action="store_true",
                    help="列出项：名字 + 组 + 基准标记 + 最近耗时 + 一行说明（带档位则按档位过滤）")
    ap.add_argument("--changed", action="store_true",
                    help="看 git diff --name-only HEAD + 未跟踪文件，说该跑哪档")
    ap.add_argument("--run", action="store_true", help="配合 --changed：直接跑建议的那档")
    ap.add_argument("--quiet", "-q", action="store_true",
                    help="不流式打印子进程输出（失败时仍打印该看的行）")
    ap.add_argument("--top", type=int, default=10, help="末尾打印最慢 N 项（默认 10）")
    ap.add_argument("--times", default=None, help=f"耗时表路径（默认 $SEC_TIMES 或 {SEC_TIMES_DEFAULT}）")
    args = ap.parse_args(argv)
    # 并行度先校验/定格（`-j0` 当场响亮报错；超订在这里收窄一次并说明）—— `--list` 也照解析。
    args.jobs_eff = resolve_jobs(args)

    all_items = build_items()
    tpath = times_path(args)

    if args.list:
        # 档位口径：不给档位/--only 就列全部（带 `基准` 列）；给了就按档位过滤（bench 默认隐藏）。
        if args.all or (args.mode is None and not args.only):
            return do_list(all_items, tpath, mode=None, total=len(all_items))
        mode_l, only_l = resolve_mode(all_items, args.mode, args.only)
        return do_list(select(all_items, mode_l, only_l), tpath,
                       mode=(args.mode or "only"), total=len(all_items))
    if args.changed:
        return do_changed(all_items, args, all_items)

    mode, only = resolve_mode(all_items, args.mode, args.only)

    sel = select(all_items, mode, only)
    if not sel:
        raise SystemExit("选出来 0 项 —— 不静默跳过。用 --list 看名字/组。")
    sel = with_build(sel, all_items)
    print(f"extC 自检：{len(sel)} 项（模式 {mode or 'all'}）", flush=True)
    return run_items(sel, Runner(quiet=args.quiet), args, all_items)


if __name__ == "__main__":
    sys.exit(main())
