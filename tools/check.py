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
    ./check.sh --only <名或组>  可多次；名字或组都行
    ./check.sh --list          名字 + 组 + 最近一次耗时 + 一行理由
    ./check.sh --changed       看 git diff --name-only HEAD + 未跟踪文件 ⇒ 说该跑哪档
    ./check.sh --changed --run 直接跑它建议的那档

选择不到任何项 = **响亮报错**（列名字/组），不静默跳过。
"""

from __future__ import annotations

import argparse
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
             echo: bool = True, quiet: bool = False) -> Proc:
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
    def __init__(self, quiet: bool = False):
        self.quiet = quiet

    def run(self, argv, timeout=None, env=None, merge=True, echo=True) -> Proc:
        return run_proc(argv, timeout=timeout, env=env, merge=merge,
                        echo=echo, quiet=self.quiet)


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
            extra = extra_ok(p.out) if extra_ok else []
            return Outcome(True, _resolve_msg(ok_msg, p.out) if ok_msg is not None else tail1(p.out), extra)
        msg = bad_msg or name
        dump = bad_dump(p.out) if bad_dump else []
        if p.timed_out:
            msg = f"{msg}（超时 {timeout:g}s 被杀 ⇒ FAIL）"
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
        bad_dump=dump_tail(12)))

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
        bad_dump=dump_tail(12)))

    # ---- 闸门·实例接线 ----
    items.append(cmd(
        "gate-callsite", "gate",
        "闸门·实例接线：每个被发出的实例必须调对它该调的那个（生成物结构判据）；基线 = 施工单，只许减",
        sh('python3 tools/gate_callsite.py'), timeout=600,
        ok_msg=ok_first(r"^\[callsite\]"),
        bad_msg="tools/gate_callsite.py（实例接线变红）",
        bad_dump=dump_tail(12)))

    # ---- 闸门·差分 ----
    items.append(cmd(
        "gate-difffuzz", "gate",
        "闸门·差分：extC 与等价 C 必须给出同一结论（挡静默算错）；基线 tools/gate-diff-known-bad.txt 只许缩小",
        sh('python3 tools/gate_difffuzz.py'), timeout=600,
        ok_msg=ok_first(r"^\[diff\]"),
        bad_msg="tools/gate_difffuzz.py（差分用例变红）",
        bad_dump=dump_tail(12)))

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
        bad_msg="tests/qname/run.sh", bad_dump=dump_tail(8)))

    # ---- std::fs 命名规范 ----
    items.append(cmd(
        "fs-shape", "tests", "std::fs 命名规范：读型/写型分开 ⇒ 误用编不过",
        ["./tests/fs-shape/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（1 正例 + 2 反例 + 常量不外露 + 规范入档）"),
        bad_msg="tests/fs-shape/run.sh", bad_dump=dump_tail(8)))

    # ---- 文件归属 ----
    items.append(cmd(
        "fs", "tests", "文件归属：程序拥有 ⇒ 忘关报警告 · 双关安全 · ASan",
        ["./tests/fs/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（fd 恒定 + canary + 泄漏警告 + readAll + 双关 + closed + ASan）"),
        bad_msg="tests/fs/run.sh", bad_dump=dump_tail(8)))

    # ---- 命令行 ----
    items.append(cmd(
        "argv", "tests", "命令行（IO.md §7）：`main(args)` ⇒ args.len 含程序名 · 形状写错编译期挡住",
        ["./tests/argv/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（正例 + 边界 + 无参哨兵 + 3 反例）"),
        bad_msg="tests/argv/run.sh", bad_dump=dump_tail(8)))

    # ---- 池 ----
    items.append(cmd(
        "pool", "tests",
        "池（期 0 库级 slot map + 期 1 运行期池注册表：handle · dense · churn 不涨 · 块退出带走子树 —— 见 POOLS.md）",
        ["./tests/pool/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（handle/dense/世代 · churn 平 · canary 会响 · ASan）"),
        bad_msg="tests/pool/run.sh", bad_dump=dump_tail(8)))

    # ---- 有序 map ----
    items.append(cmd(
        "map", "tests", "有序 map<K, V>（POOLS.md §11：B 树；第一步是单节点有序表）",
        ["./tests/map/run.sh"], timeout=600,
        ok_msg=tail1, bad_msg="tests/map/run.sh", bad_dump=dump_tail(8)))

    # ---- 实例化类型上的 impl 与 dyn（4 条各自计数）----
    for nm, argv, to in (("instimpl", ["./tests/instimpl/run.sh"], 600),
                         ("lambda", ["./tests/lambda/run.sh"], 600),
                         ("ext", ["./tests/ext/run.sh"], 600),
                         ("time", ["./tests/time/run.sh"], 900)):
        items.append(cmd(
            nm, "tests",
            "实例化类型上的 impl 与 dyn（slice<u8> 挂方法 · 按实例隔离 · dyn 派发 · L 债金丝雀）",
            argv, timeout=to, ok_msg=tail1,
            bad_msg=f"{argv[0].lstrip('./')}", bad_dump=dump_tail(8)))

    # ---- 期 0 · 哈希表 ----
    items.append(cmd(
        "hashmap", "tests",
        "期 0 · 哈希表（key→value 随机的正解：开放寻址 + 墓碑 · 见 POOLS.md §10.2）",
        ["./tests/hashmap/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（新键/覆盖/碰撞/墓碑 churn 平/canary/ASan）"),
        bad_msg="tests/hashmap/run.sh", bad_dump=dump_tail(8)))

    # ---- 展示代码 ----
    items.append(cmd(
        "showcase", "tests",
        "展示代码（examples/showcase-*.extc：文档里的六段，每次编译并运行）",
        ["bash", "tests/showcase/run.sh"], timeout=300,
        ok_msg=ok_count_ok(" 项（Arena 返回值 · home 机制 · 池身份 · 视图 · impl · extern!）"),
        bad_msg="tests/showcase/run.sh", bad_dump=dump_tail(8)))

    # ---- STL ----
    items.append(cmd(
        "stl", "tests",
        "STL 库（一个库装所有动态容器；容器建在池上 —— 见 POOLS.md 期 3）",
        ["./tests/stl/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（vector：翻倍/dense 连续/shrink/clear · ASan）"),
        bad_msg="tests/stl/run.sh", bad_dump=dump_tail(8)))

    # ---- 线性关联容器 ----
    items.append(cmd(
        "linmap", "tests",
        "线性关联容器（std::linmap / std::linset：只用 ==，不依赖 #57 的 hash）",
        ["bash", "tests/linmap/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（覆盖/缺失/删除/扩容 · 结构体键 · 集合）"),
        bad_msg="tests/linmap/run.sh", bad_dump=dump_tail(6)))

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
        bad_msg="tests/ops/run.sh", bad_dump=dump_tail(12)))

    # ---- @noCopy ----
    items.append(cmd(
        "nocopy", "tests",
        "@noCopy（状态有身份的类型不许按值拷：正例 4 件事 · 四个复制点全挡）",
        ["bash", "tests/nocopy/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（新值/ref 传参/读字段/调方法照旧 · 绑定·实参·字段·赋值四个复制点全挡 ✓）"),
        bad_msg="tests/nocopy/run.sh", bad_dump=dump_tail(8)))

    # ---- 构造函数 ----
    items.append(cmd(
        "ctor", "tests",
        "构造函数（`T(args)` 就是 `T::new(args)`：糖=显式 · 可失败 · 泛型 · 3 反例）",
        ["bash", "tests/ctor/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（正例 3 · 反例 3 —— 无 new 教你怎么写 · 返回类型必须对 · 实参个数）"),
        bad_msg="tests/ctor/run.sh", bad_dump=dump_tail(8)))

    # ---- 泛型组合矩阵 ----
    items.append(cmd(
        "genmatrix", "tests",
        "泛型组合矩阵（T 的位置 × 特性 · 13 格 · 缺口清单空 —— 见 GENERICS.md）",
        ["bash", "tests/genmatrix/run.sh"], timeout=600,
        ok_msg=ok_count_ok(" 项（13 格全过 ✓ · #60/#61/#62/#63/#64/#65 六条当天撞到当天修完、当天搬进矩阵或正例 ⇒ 缺口清单空，机制留着）"),
        bad_msg="tests/genmatrix/run.sh", bad_dump=dump_tail(8)))

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
        bad_msg="tools/gate_checkc.py（全语料 --check-c 变红）", bad_dump=dump_tail(14)))
    items.append(cmd(
        "gate-asan-corpus", "bench",
        "闸门②：正例生成物 ASan+UBSan（-O2 会把 UB 优化掉，这里必须真跑）",
        sh('python3 tools/gate_asan_corpus.py --scope full'), timeout=1800, bench=True,
        ok_msg=ok_first(r"^\[asan\]"),
        bad_msg="tools/gate_asan_corpus.py（正例生成物出现 sanitizer 报告）", bad_dump=dump_tail(14)))
    items.append(cmd(
        "golden", "bench",
        "生成物快照（观察项：与冻结哈希的差异不作为判据；判据只有「必须是合法 C」）",
        ["./tools/golden.sh"], timeout=900, bench=True,
        ok_msg=lambda out: first_matching(out, r"^  ok").strip(),
        bad_msg="tools/golden.sh（生成物里有非法 C）", bad_dump=dump_tail(8),
        extra_ok=lambda out: [ln for ln in out.split("\n") if ln.startswith("  · ")]))

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
        bad_msg="bench/dyn/stress", bad_dump=dump_tail(8)))
    items.append(cmd(
        "bench-dyn-alloc", "bench",
        "基准·dyn 构造成本（拷贝 vs 分配，N=50000）",
        ["./bench/dyn/alloc/run.sh"], timeout=None, bench=True, env={"N": "50000"},
        ok_msg=ok_const("dyn 构造成本（拷贝 vs 分配）"),
        bad_msg="bench/dyn/alloc", bad_dump=dump_tail(6)))
    items.append(cmd(
        "bench-dyn-real", "bench",
        "基准·dyn 现实负载（P=512 MIX=8 RUNS=1）",
        ["./bench/dyn/real/run.sh"], timeout=None, bench=True,
        env={"P": "512", "MIX": "8", "RUNS": "1"},
        ok_msg=lambda out: f"dyn 现实负载（{first_matching(out, r'^通过')}）",
        bad_msg="bench/dyn/real", bad_dump=dump_tail(8)))

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
        bad_msg="综合场景横评（构建失败 / 语言之间对拍不一致 ✗）", bad_dump=dump_tail(20)))

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
MODE_TOKENS = {"all", "full", "quick", "docs"}


def select(items: list, mode: Optional[str], only: list) -> list:
    if mode in (None, "all", "full"):
        sel = list(items)
    elif mode == "quick":
        sel = [it for it in items if not it.bench]
    elif mode == "docs":
        sel = [it for it in items if it.group == "docs"]
    else:
        raise SystemExit(f"未知模式：{mode}（可用：all / full / quick / docs）")

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
def run_items(items: list, ctx: Runner, args, all_items: list) -> int:
    tpath = times_path(args)
    n_pass = n_fail = 0
    results = []          # (name, group, secs, status)
    times_out = {}
    for it in items:
        print(f"\n== {it.name}（{it.group}）==", flush=True)
        t0 = time.monotonic()
        try:
            oc = it.fn(ctx, it)
        except Exception as exc:  # 回调自身炸了 = 这一项 FAIL，别把整轮吞掉
            oc = Outcome(False, f"{it.name}：编排回调异常 {type(exc).__name__}: {exc}")
        secs = time.monotonic() - t0
        if oc.ok is None:
            status = "skip" if oc.msg.startswith("跳过") else "ran"
            print(f"  --   {oc.msg}", flush=True)
        elif oc.ok:
            n_pass += 1
            status = "ok"
            print(f"  \033[32mok\033[0m   {oc.msg}", flush=True)
        else:
            n_fail += 1
            status = "FAIL"
            print(f"  \033[31mFAIL\033[0m {oc.msg}", flush=True)
        for ln in oc.extra:
            print(ln, flush=True)
        if oc.ok is not None:
            print(f"       ⏱ {secs:.3f}s", flush=True)
        results.append((it.name, it.group, secs, status))
        times_out[it.name] = (secs, status, time.strftime("%Y-%m-%dT%H:%M:%S"), it.group)
        if oc.ok is False and it.abort_on_fail:
            print(f"\n构建失败 ⇒ 中止（其余项都假设 build/extc 在）。", flush=True)
            write_times(tpath, times_out)
            print(f"\n通过 {n_pass}，失败 {n_fail}", flush=True)
            return 1

    write_times(tpath, times_out)

    top = getattr(args, "top", 10)
    slow = sorted([r for r in results if r[3] in ("ok", "FAIL")],
                  key=lambda r: r[2], reverse=True)[:top]
    if slow:
        print(f"\n== 最慢的 {len(slow)} 项 ==", flush=True)
        for name, group, secs, status in slow:
            print(f"  {secs:8.3f}s  {status:<4}  {name}（{group}）", flush=True)
    print(f"\n耗时表：{tpath}", flush=True)
    print(f"\n通过 {n_pass}，失败 {n_fail}", flush=True)
    return 0 if n_fail == 0 else 1


# --------------------------------------------------------------------------
# --list / --changed
# --------------------------------------------------------------------------
def do_list(items: list, tpath: str) -> int:
    tf = read_times_full(tpath)
    print(f"{'名称':<26}{'组':<9}{'最近耗时':<10}说明")
    for it in items:
        last = f"{tf[it.name][0]:.2f}s" if it.name in tf else "-"
        print(f"{it.name:<26}{it.group:<9}{last:<10}{it.desc}")
    n_bench = sum(1 for it in items if it.bench)
    print(f"\n共 {len(items)} 项：非基准 {len(items) - n_bench} · 基准 {n_bench}"
          f"（quick = 非基准；docs = {sum(1 for it in items if it.group == 'docs')} 项文档/工具）")
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
        return "quick", "有源码/测试/工具改动 ⇒ quick（跳过基准）", docs, bench, code, self_
    if docs:
        return "docs", "只有文档改动 ⇒ docs（秒级）", docs, bench, code, self_
    if self_:
        return "quick", "只有编排层自身改动 ⇒ quick（被改的就是 runner，值得完整验一遍）", docs, bench, code, self_
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
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="./check.sh", add_help=True,
        description="extC 自检编排层：检查项是数据，runner 负责选/跑/计时/统计。")
    ap.add_argument("mode", nargs="?", default=None,
                    help="all（默认，含基准）/ quick（跳过基准）/ docs（文档·工具，秒级）")
    ap.add_argument("--only", action="append", default=[], metavar="名或组",
                    help="只跑这些项（名字或组；可多次）")
    ap.add_argument("--list", action="store_true", help="列出项：名字 + 组 + 最近耗时 + 一行说明")
    ap.add_argument("--changed", action="store_true",
                    help="看 git diff --name-only HEAD + 未跟踪文件，说该跑哪档")
    ap.add_argument("--run", action="store_true", help="配合 --changed：直接跑建议的那档")
    ap.add_argument("--quiet", "-q", action="store_true",
                    help="不流式打印子进程输出（失败时仍打印该看的行）")
    ap.add_argument("--top", type=int, default=10, help="末尾打印最慢 N 项（默认 10）")
    ap.add_argument("--times", default=None, help=f"耗时表路径（默认 $SEC_TIMES 或 {SEC_TIMES_DEFAULT}）")
    args = ap.parse_args(argv)

    all_items = build_items()
    tpath = times_path(args)

    if args.list:
        return do_list(all_items, tpath)
    if args.changed:
        return do_changed(all_items, args, all_items)

    mode = args.mode
    only = list(args.only)
    if mode is not None and mode not in MODE_TOKENS:
        # 位置参数也可能是名字/组（方便 ./check.sh tests）
        if mode in {it.name for it in all_items} or mode in {it.group for it in all_items}:
            only.append(mode)
            mode = None
        else:
            near = difflib.get_close_matches(mode, sorted({it.name for it in all_items}
                                                          | {it.group for it in all_items}), n=5)
            hint = ("；相近的有：" + " ".join(near)) if near else ""
            raise SystemExit(f"选不出来：位置参数 {mode!r} 既不是模式(all/quick/docs)也不是名字/组{hint}")

    sel = select(all_items, mode, only)
    if not sel:
        raise SystemExit("选出来 0 项 —— 不静默跳过。用 --list 看名字/组。")
    sel = with_build(sel, all_items)
    print(f"extC 自检：{len(sel)} 项（模式 {mode or 'all'}）", flush=True)
    return run_items(sel, Runner(quiet=args.quiet), args, all_items)


if __name__ == "__main__":
    sys.exit(main())
