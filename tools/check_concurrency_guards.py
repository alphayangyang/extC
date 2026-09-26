#!/usr/bin/env python3
"""并发前身判据：在特性落地**之前**，把四条 soundness 规则里今天就能检查的那一半钉住。

规则见 `docs/topics/CONCURRENCY.md` §12。这个脚本检查的是**前身不变式** ——
特性（`spawn` / `yield` / 跨任务传值）还没写，但它要依赖的东西今天已经存在，
一旦被改坏，特性落地时就会以"不可复现的交错"形式暴露 ✗ 所以先在这里钉死。

**J1 · 规则 1 的前身（生成物）**：`place` 在创建时钉死，恢复点禁止重新推导。
  今天对应的不变式：**传给"带 zone 参数的函数"的那个实参，只能是**
  `__extc_home_zone`（把调用者选的地方原样往下传）、`__extc_zm<k>`（本帧在第 k 层记下的记号）、
  或整数字面量（钉死在某一层）。**不允许**任何"重新算出来"的表达式。
  ⇒ 将来 `spawn`/`resume` 只要照抄这个约定就不会重新推导；一旦有人从当前帧状态现算，这里立刻红 ✓

**J2 · 规则 3 的前身（生成物）**：`yield` 不许出现在地方边界内。
  今天对应的不变式：**地方边界自洽** ——
  (a) `extc_arena_release(&__extc_a[k])` 里的 `k` 必须小于该函数声明的 `__extc_a[N]`；
  (b) `__extc_zm<k>` 必须先由 `= extc_pool_zoneEnter()` 定义、后使用（`zoneLeaveTo` 的实参）。
  这两条把"边界在哪里、编号几层"变成可核对的事实；`yield` 落地后，规则 3 就是
  "挂起点不得落在这些配对之间" ✓

**J3 · 规则 4/7 的前身（源码）**：池的元数据（`pid`/`gen`/`pgen`/槽表）只允许在 `src/pools.c`
  里读写 —— 将来要把"取槽 + 校验世代"做成原子，只能有一个地方可改 ✓
  （唯一允许的例外：`codegen.c` 里那句 `ExtcDynHandleS` 的类型串，它是接口不是访问 ✓）

用法：
  tools/check_concurrency_guards.py            # 报告 + 违规即退出 1
  tools/check_concurrency_guards.py --list     # 顺带打印语料里找到的边界数量
"""
import re
import subprocess
import sys
import pathlib
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXTC = ROOT / "build" / "extc"

# 语料：真正走到 zone 机制的那些 fixture（小而全 —— 判据要能进 check.sh 的快速模式）
CORPUS = [
    "tests/arena-promoted/*.extc",
    "tests/coro/*.extc",
    "tests/dyn/dyn_stored_call.extc",
    "tests/pool/churn.extc",
    "examples/out-param.extc",
    "examples/tour-plots.extc",
]

ZONE_PARAM = "int64_t __extc_home_zone"
# J1 允许的实参形状：转发 / 本帧记号 / 整数字面量 / **帧字段**（`f->zone`）。
# 最后一种正是规则 1 在任务体内要求的形状：恢复点**只读字段**，不重新推导 ——
# 就是上面那条兜底注释里写的"spawn 落地时收紧成必须来自帧字段"。
ALLOWED_ZONE_ARG = re.compile(r"^(__extc_home_zone|__extc_zm\d+|-?\d+|[A-Za-z_]\w*->zone)$")
# 帧字段只许出现在**协程单元**里（文件里有 `$step`）：别处的 zone 实参该是转发或本帧记号。
FRAME_ZONE_ARG = re.compile(r"^[A-Za-z_]\w*->zone$")

# **有理由的兜底**：`codegen.c:2663-2690` 里那条"当前 zone 顶减一"，
# 只对没有 zone 上下文的函数（`noArena`）发射。单栈下它是良定义的（动态链上的当前顶），
# 但它正是**规则 1 禁止的形状**：从环境状态现算，而不是转发调用者钉死的地方。
# 协程一旦落地，同一个帧在恢复时的 zone 深度可能不同 ⇒ 这个值会变 ⇒ 必须改成帧字段。
# 在那之前：**计数棘轮** —— 只许少、不许增，且每次都要重新看一眼这条豁免还成不成立 ✓
# "我此刻所在的那个地方"：运行期自己的名字。它**不是**派生算术（`extc_zoneTop` 的含义是直接的），
# 但仍然是**环境**而不是"调用者钉死的地方" ⇒ 单栈下正确，**任务体内不许用**（规则 1）：
# `spawn` 落地时，这条要么收紧成"必须来自帧字段"，要么由规则 1 的判据覆盖 ✓ 计数棘轮先钉住它。
FALLBACK = "extc_zoneTop"
FALLBACK_BUDGET = 2        # 2026-09-26 在下面这份语料上量到的个数
# arena 那一个隐藏实参同样只许两种形状：转发 `__extc_home`，或钉死在某一层 `&__extc_a[k]`。
ALLOWED_ARENA_ARG = re.compile(r"^(__extc_home|&__extc_a\[\d+\]|-?\d+)$")


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments and literals, keeping every offset intact."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def match_paren(text: str, open_at: int) -> int:
    """Index just past the `)` matching the `(` at `open_at`, or -1."""
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
    return -1


def split_top_level(args: str):
    """Split an argument list on commas that are not inside (), [] or {}."""
    out, depth, cur = [], 0, []
    for ch in args:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


ARENA_PARAM = "extc_arena *__extc_home"


def zone_taking_functions(code: str):
    """Names of functions taking the hidden zone parameter, and whether they take the arena too.

    Both hidden arguments come as a pair only when the function needs both: `string$string_new`'s
    signature is `(int64_t c, int64_t __extc_home_zone)` -- checking its second-to-last argument as
    an arena was 12 false positives."""
    names = {}
    for m in re.finditer(r"([A-Za-z_$][A-Za-z0-9_$]*)\s*\(", code):
        end = match_paren(code, m.end() - 1)
        if end < 0:
            continue
        params = code[m.end():end - 1]
        if ZONE_PARAM in params:
            names[m.group(1)] = ARENA_PARAM in params
    return names


def check_generated(path: pathlib.Path, stats_fallback):
    """J1 + J2 on one generated translation unit. Returns (violations, stats)."""
    code = strip_comments_and_strings(path.read_text(encoding="utf-8"))
    bad = []
    zone_fns = zone_taking_functions(code)
    calls = 0

    # J1: every call to a zone-taking function must forward or pin the zone.
    for m in re.finditer(r"([A-Za-z_$][A-Za-z0-9_$]*)\s*\(", code):
        name = m.group(1)
        if name not in zone_fns:
            continue
        takes_arena = zone_fns[name]
        before = code[max(0, m.start() - 40):m.start()]
        # A declaration/definition is preceded by its return type; extC names may contain `$`
        # (`pool$pool_Shape_withCap`), so the character class has to allow it.
        if re.search(r"\b(static|extern|void|int|bool|char|struct|const)\b[^;{()]*$", before):
            continue                      # a declaration or definition, not a call site
        end = match_paren(code, m.end() - 1)
        if end < 0:
            continue
        args = split_top_level(code[m.end():end - 1])
        if not args:
            continue
        calls += 1
        last = args[-1]
        if takes_arena and len(args) >= 2 and not ALLOWED_ARENA_ARG.match(args[-2]):
            line = code[:m.start()].count("\n") + 1
            bad.append(f"J1 {path.name}:{line} {name}(...) 的 arena 实参是 `{args[-2][:50]}`"
                       f" —— 只允许转发 `__extc_home` 或钉死 `&__extc_a[k]`")
        if last == FALLBACK:
            stats_fallback[0] += 1
            continue
        if FRAME_ZONE_ARG.match(last):
            # The task's own place, read from the frame: legal inside a coroutine's step, and only
            # there -- a unit without a `$step` has no frame to read it from.
            if "$step" not in code:
                line = code[:m.start()].count("\n") + 1
                bad.append(f"J1 {path.name}:{line} {name}(...) 的 zone 实参是帧字段 `{last[:50]}`"
                           f" —— 帧字段只许出现在协程单元里（那里才有 $step）")
            continue
        if not ALLOWED_ZONE_ARG.match(last):
            line = code[:m.start()].count("\n") + 1
            bad.append(f"J1 {path.name}:{line} {name}(...) 的 zone 实参是 `{last[:50]}`"
                       f" —— 只允许转发 `__extc_home_zone` / 本帧记号 `__extc_zm<k>` / 整数字面量"
                       f" / 任务体内的帧字段 `<f>->zone`")

    # J2a: `extc_arena_release(&__extc_a[k])` must stay inside the declared array.
    released = set()
    arena_n = None
    for m in re.finditer(r"extc_arena\s+__extc_a\[(\d+)\]", code):
        arena_n = max(arena_n or 0, int(m.group(1)))
    for m in re.finditer(r"extc_arena_release\(&__extc_a\[(\d+)\]\)", code):
        k = int(m.group(1))
        released.add(k)
        if arena_n is not None and k >= arena_n:
            line = code[:m.start()].count("\n") + 1
            bad.append(f"J2 {path.name}:{line} 释放 __extc_a[{k}]，但该函数只声明了 "
                       f"__extc_a[{arena_n}]")
    # J2b: a `__extc_zm<k>` must be defined (by zoneEnter) before it is used.
    defined = set()
    for m in re.finditer(r"__extc_zm(\d+)\s*=\s*extc_pool_zoneEnter\(\)", code):
        defined.add(m.group(1))
    for m in re.finditer(r"__extc_zm(\d+)", code):
        if m.group(1) not in defined:
            line = code[:m.start()].count("\n") + 1
            bad.append(f"J2 {path.name}:{line} 用了 __extc_zm{m.group(1)}，但这段代码里没有它的"
                       f" `= extc_pool_zoneEnter()` 定义")
            break
    # J1 收紧（切片 C 落地后才有意义）：**任务体**里不许出现环境 zone。
    # `$step` 是任务体，恢复点的地方只能从帧字段读；`$next`（驱动器）不在这个范围内 ——
    # 它本来就该在这里读写环境并把帧里的地方设为当前。
    for m in re.finditer(r"\nEXTC_UNUSED static bool \w+\$step\([^)]*\)\s*\{", code):
        end = code.find("\n}\n", m.end())
        body = code[m.end():end if end != -1 else len(code)]
        for env in ("extc_zoneTop", "__extc_home_zone"):
            if env in body:
                line = code[:m.start()].count("\n") + 1
                bad.append(f"J1 {path.name}:{line} `$step` 里出现了 `{env}` —— "
                           f"任务体的地方必须从帧字段读（规则 1）")
    return bad, {"zone_fns": len(zone_fns), "calls": calls, "arena_levels": sorted(released)}



def check_pool_funnel():
    """J3: pool metadata is touched only in src/pools.c."""
    bad = []
    allowed = {
        # The interface, not an access: the emitted handle type says which fields exist.
        ("codegen.c", "struct ExtcDynHandleS { int64_t pid, slot, gen, pgen; }"),
    }
    pat = re.compile(r"(pgen|->gen\b|->pid\b|->slots\b|\.slots\b)")
    for path in sorted((ROOT / "src").glob("*.c")):
        if path.name == "pools.c":
            continue
        for n, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
            if not pat.search(line):
                continue
            stripped = line.strip()
            if any(path.name == f and marker in stripped for f, marker in allowed):
                continue
            bad.append(f"J3 src/{path.name}:{n} 碰到池的元数据：{line.strip()[:70]}")
    return bad


def main() -> int:
    listing = "--list" in sys.argv
    bad = []
    stats = {"programs": 0, "calls": 0, "levels": set()}
    stats_fallback = [0]
    with tempfile.TemporaryDirectory() as td:
        for pattern in CORPUS:
            for src in sorted(ROOT.glob(pattern)):
                out = pathlib.Path(td) / (src.stem + ".c")
                r = subprocess.run([str(EXTC), "-w", "--no-line-map", "-o", str(out), str(src)],
                                   capture_output=True, text=True)
                if r.returncode != 0:
                    continue                    # 反例语料不生成 C，跳过（它们由别的套件管）
                stats["programs"] += 1
                v, s = check_generated(out, stats_fallback)
                bad += v
                stats["calls"] += s["calls"]
                stats["levels"] |= set(s["arena_levels"])
    bad += check_pool_funnel()
    if stats_fallback[0] > FALLBACK_BUDGET:
        bad.append(f"J1 兜底 `{FALLBACK}` 出现 {stats_fallback[0]} 次，预算 {FALLBACK_BUDGET} —— "
                   f"它正是规则 1 禁止的'从环境现算'，只许少不许增（协程落地前必须改成帧字段）")

    for b in bad:
        print("  " + b)
    print()
    if listing:
        print(f"  语料：{stats['programs']} 个程序 · J1 检查了 {stats['calls']} 个 zone 调用点 · "
              f"J2 见到的 arena 层级 {sorted(stats['levels'])}")
        print(f"  J1 有理由的兜底 `{FALLBACK}`：{stats_fallback[0]} 次（预算 {FALLBACK_BUDGET}）")
    if bad:
        print(f"  {len(bad)} 条违规 —— 见上（规则见 docs/topics/CONCURRENCY.md §12）")
        return 1
    print("  J1 zone 实参只转发或钉死 ✓ · J2 地方边界自洽 ✓ · J3 池元数据只经 pools.c ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
