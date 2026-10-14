#!/usr/bin/env python3
"""**"根表按需化"原型**：把生成物里"每个分配点当场物化全量根表"换成
"每个分配点一份**槽地址描述符**、只建一次、清扫那一刻才解引用"。

这是 `tests/reclaim-perf/README.md` 里那一条建议的**可测量原型**——
它只改 `/tmp` 里的生成物副本，`src/` 一个字节都不动。

变换：
  · 运行时里复制 `extc_rc_mark` → `extc_rc_markSlots`：根表语义从"指针值数组"
    变成"槽地址数组"，在标记时 `*(void **)slot` 解引用（值更新，且完全不改健全性）；
    再复制 `extc_rc_collect` / `extc_rc_maybeCollect` → `...Slots`。
  · 每个调用点 `extc_rc_maybeCollect(区域, (void *[]){ A, B, ... }, N);`
    变成一段 `{ static void **tab; if (!tab) { 建一次 }; extc_rc_maybeCollectSlots(...); }`。

用法：latemark.py <in.c> <out.c>
"""
import re
import sys

SRC, DST = sys.argv[1], sys.argv[2]
s = open(SRC).read()


def take_func(src, header):
    """从 header 起按花括号配平截出整个函数文本。"""
    i = src.index(header)
    j = src.index("{", i)
    depth = 0
    k = j
    while True:
        c = src[k]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                break
        k += 1
    return src[i:k + 1], i, k + 1


# ---- 1. mark → markSlots ----------------------------------------------------
MARK_H = "static size_t extc_rc_mark(ExtcRcRegion *r, void *const *roots, size_t nroots) {"
mark, mi, mj = take_func(s, MARK_H)
old_loop = ("    for (size_t i = 0; i < nroots; i++)\n"
            "        if (roots[i]) extc_rc_markPush(r, roots[i]);\n")
assert old_loop in mark, "root loop not found in extc_rc_mark"
mark_slots = mark.replace(
    MARK_H, "static size_t extc_rc_markSlots(ExtcRcRegion *r, void *const *roots, size_t nroots) {"
).replace(
    old_loop,
    "    /* 根表 = 槽地址表：清扫这一刻才解引用 ⇒ 每个分配点不再 O(根项数) */\n"
    "    for (size_t i = 0; i < nroots; i++) {\n"
    "        void *o = *(void **)roots[i];\n"
    "        if (o) extc_rc_markPush(r, o);\n"
    "    }\n")
s = s[:mj] + "\n" + mark_slots + "\n" + s[mj:]

# ---- 2. collect → collectSlots ---------------------------------------------
COL_H = "static size_t extc_rc_collect(ExtcRcRegion *r, void *const *roots, size_t nroots) {"
col, ci, cj = take_func(s, COL_H)
col_slots = col.replace(COL_H, "static size_t extc_rc_collectSlots(ExtcRcRegion *r, void *const *roots, size_t nroots) {") \
               .replace("extc_rc_mark(r, roots, nroots)", "extc_rc_markSlots(r, roots, nroots)")
assert "markSlots" in col_slots
s = s[:cj] + "\n" + col_slots + "\n" + s[cj:]

MC_H = "static int extc_rc_maybeCollect(ExtcRcRegion *r, void *const *roots, size_t nroots) {"
mc, mci, mcj = take_func(s, MC_H)
mc_slots = mc.replace(MC_H, "static int extc_rc_maybeCollectSlots(ExtcRcRegion *r, void *const *roots, size_t nroots) {") \
             .replace("extc_rc_collect(r, roots, nroots)", "extc_rc_collectSlots(r, roots, nroots)")
assert "collectSlots" in mc_slots
s = s[:mcj] + "\n" + mc_slots + "\n" + s[mcj:]

# ---- 3. 调用点：值数组 → 建一次槽地址表 -------------------------------------
# 入口守卫 / `regionOf` 复用落地后，调用点是 `maybeCollect(__extc_xxxN, …)`（第一实参为块内临时量）；
# 交替项认它，替换后仍落在原来那个 `if (extc_rc_shouldSweep(__extc_rcgN))` 之下。
pat = re.compile(
    r"extc_rc_maybeCollect\(((?:extc_rc_regionOf\([^;]*?\))|(?:__extc_rcg\d+)), "
    r"\(void \*\[\]\)\{ ([^}]*?) \}, (\d+)\);")

state = {"i": 0}


def rep(m):
    i = state["i"]
    state["i"] += 1
    region, body, n = m.group(1), m.group(2), m.group(3)
    items = [x.strip() for x in body.split(",")]
    assert len(items) == int(n), (len(items), n)
    slots = []
    for it in items:
        mm = re.fullmatch(r"\(void \*\)\((.*)\)", it)
        assert mm, "unexpected root item: " + it
        slots.append("(void *)&(%s)" % mm.group(1))
    return (
        "{ static void **__extc_rs%(i)d; static size_t __extc_rn%(i)d;\n"
        "  if (!__extc_rs%(i)d) {\n"
        "      __extc_rs%(i)d = (void **)malloc((size_t)%(n)s * sizeof(void *));\n"
        "      void *__extc_rt%(i)d[] = { %(slots)s };\n"
        "      memcpy(__extc_rs%(i)d, __extc_rt%(i)d, (size_t)%(n)s * sizeof(void *));\n"
        "      __extc_rn%(i)d = (size_t)%(n)s;\n"
        "  }\n"
        "  extc_rc_maybeCollectSlots(%(region)s, (void *const *)__extc_rs%(i)d, __extc_rn%(i)d); }"
        % {"i": i, "n": n, "slots": ", ".join(slots), "region": region}
    )


s, cnt = pat.subn(rep, s)
assert cnt > 0, "no call sites patched"
print("late-root prototype: patched call sites =", cnt, file=sys.stderr)
open(DST, "w").write(s)
