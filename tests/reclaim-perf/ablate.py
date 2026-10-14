#!/usr/bin/env python3
"""**单项消融 / 单项原型**：每次只动生成物里回收运行期的一块，用来给"这一块值多少指令"定量。
全部只改 `/tmp` 里的生成物副本，`src/` 不动。

  guardfast   把 slab 登记表查询从"有序数组二分"换成"直接映射 + 校验"（命中跳过二分，
              未命中回落二分）。**语义不变**（保守回落），量的是**二分**那一段的成本。
  guardfirst  把 `extc_rc_maybeCollect(...)` 包进 `if (extc_rc_shouldSweep(r))`：
              没到压力线时**根本不物化根表**。语义与原来逐字等价（maybeCollect 的第一句
              就是同一个判据），量的是**根表物化**的总成本。
  nohold      拆掉持有者计数（holdUpd 直接返回）：计数税上界（**改内存行为**，只用来看上界）。
  noreg       拆掉**全部**跨区登记（storeRefOwned → storeRef）：**不健全**，只当上界。
  noreg0      只拆 `owner` 为空字面量的那些点（`regionOfOwn(0)` 恒为 0 ⇒ 本来就不登记）：
              **语义不变**，量的是这些点上白付的 regionOfOwn 链。
  padNNNN     把每个调用点的根表**用空指针补齐**到 NNNN 项（原根一个不少）：
              语义逐字不变（标记跳过空根），量的是**根表尺寸 → 指令数**的斜率。

为什么没有"直接拆掉 owns 守卫"那一档：`cyclic_pressure` 里 `w`（world 结构）是 arena 指针，
会作为 `owner` 走进 `storeRefOwned`，`extc_rc_unlinkChildren` 也会对非 rc 字段调
`regionOfOwn`。"假定所有指针都是 slab 指针"这条消融在本形状上**直接段错误**
（`gdb`：`extc_rc_regionOfOwn` ← `extc_rc_unlinkChildren` ← `extc_rc_regionDrop`），
这本身就是"守卫不是可选项"的证据。

用法：ablate.py <mode> <in.c> <out.c>，然后照常 cc 编译。
"""
import re
import sys

mode, SRC, DST = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(SRC).read()


def take_func(src, header):
    i = src.index(header)
    j = src.index("{", i)
    depth, k = 0, j
    while True:
        c = src[k]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                break
        k += 1
    return src[i:k + 1]


# 调用点：区域表达式里含内层括号，所以靠后面那个 `, (void *[]){` 字面量收口；
# 根项里不会出现 `}`（都是 `(void *)(表达式)`），所以用 `[^}]*?` 取项表最稳。
# 入口守卫 / 分片 / `regionOf` 复用**落地后**，调用点长成 `maybeCollect(__extc_xxxN, …)`
# （第一实参是块内临时量，名字随发射形态变）⇒ 交替项按"`__extc_` 前缀"认它，
# 各种发射形态都能跑。
CALL = re.compile(r"extc_rc_maybeCollect\(((?:extc_rc_regionOf\(.*?\))|(?:__extc_\w+)), \(void \*\[\]\)\{ ([^}]*?) \}, (\d+)\);", re.S)

if mode == "guardfast":
    # 1) 表与哈希函数，插在登记表变量之后。
    anchor = ("static ExtcRcSlab **extc_rc_ownTab;\n"
              "static size_t extc_rc_ownN, extc_rc_ownCap;\n")
    if anchor not in s:   # 生成物里带 __attribute__((unused)) 前缀
        anchor = anchor.replace("static ExtcRcSlab", "__attribute__((unused)) static ExtcRcSlab")
        anchor = anchor.replace("static size_t", "__attribute__((unused)) static size_t")
    assert anchor in s, "ownTab anchor not found"
    tab = ("\n/* 消融：直接映射登记表。命中（槽位里正好是 slab 基址）就跳过二分；"
           "未命中回落二分 ⇒ 结果与原来一模一样。 */\n"
           "#define EXTC_RC_OWNMAP_BITS 12\n"
           "#define EXTC_RC_OWNMAP_SIZE (1u << EXTC_RC_OWNMAP_BITS)\n"
           "static ExtcRcSlab *extc_rc_ownMap[EXTC_RC_OWNMAP_SIZE];\n"
           "static size_t extc_rc_ownHashOf(const void *base) {\n"
           "    return (size_t)(((uintptr_t)base >> EXTC_RC_SLAB_BITS) * 2654435761u)\n"
           "           & (EXTC_RC_OWNMAP_SIZE - 1);\n"
           "}\n")
    s = s.replace(anchor, anchor + tab, 1)

    # 2) 登记/注销时维护映射。
    add = take_func(s, "extc_rc_ownAdd(ExtcRcSlab *s) {")
    assert add.rstrip().endswith("}")
    newadd = add.rstrip()[:-1] + "    extc_rc_ownMap[extc_rc_ownHashOf(s)] = s;\n}"
    s = s.replace(add, newadd, 1)

    dele = take_func(s, "extc_rc_ownDel(ExtcRcSlab *s) {")
    newdel = dele.rstrip()[:-1] + ("    if (extc_rc_ownMap[extc_rc_ownHashOf(s)] == s)\n"
                                   "        extc_rc_ownMap[extc_rc_ownHashOf(s)] = (ExtcRcSlab *)0;\n}")
    s = s.replace(dele, newdel, 1)

    # 3) ownSlab 里：映射命中直接用，未命中才二分。
    #    当前发射形态带**超大块**的回落（`ownBig`），所以这里只把"取 s"这一步换成
    #    "映射命中即取、未命中回落二分"，后面的槽首校验与 `ownBig` 回落原样保留。
    old = ("    size_t i = extc_rc_ownLower(base);\n"
           "    if (i < extc_rc_ownN) {\n"
           "        ExtcRcSlab *s = extc_rc_ownTab[i];\n")
    assert s.count(old) == 1, "ownSlab lookup body not unique/found"
    new = ("    ExtcRcSlab *s = extc_rc_ownMap[extc_rc_ownHashOf(base)];\n"
           "    if ((void *)s != base) {              /* 未命中/碰撞：回落二分，语义不变 */\n"
           "        size_t i = extc_rc_ownLower(base);\n"
           "        s = (i < extc_rc_ownN) ? extc_rc_ownTab[i] : (ExtcRcSlab *)0;\n"
           "    }\n"
           "    if (s) {\n")
    s = s.replace(old, new, 1)

elif mode == "guardfirst":
    # 根表只在真的要扫时才物化：判据与 maybeCollect 的第一句相同 ⇒ 语义不变。
    i = [0]
    noop = [0]

    def rep(m):
        if m.group(1).startswith("__extc_"):
            # 入口守卫已在 codegen 里落地 ⇒ 这一档没有"未守卫的调用点"可消：
            # 原样保留，该变体 == on（`on-guardfirst` 的差值为 0，正是现状）。
            noop[0] += 1
            return m.group(0)
        n = i[0]
        i[0] += 1
        return ("{ ExtcRcRegion *__extc_pf%d = %s;\n"
                "  if (extc_rc_shouldSweep(__extc_pf%d))\n"
                "      extc_rc_maybeCollect(__extc_pf%d, (void *[]){ %s }, %s); }"
                % (n, m.group(1), n, n, m.group(2), m.group(3)))

    s, cnt = CALL.subn(rep, s)
    assert cnt > 0, "no call sites patched"
    print("guardfirst: patched", i[0], "sites;", noop[0], "already guarded (no-op)",
          file=sys.stderr)

elif mode == "nohold":
    h = "extc_rc_holdUpd(void *p, int delta) {"
    j = s.index(h) + len(h)
    s = s[:j] + "\n    (void)p; (void)delta; return;   /* 消融：不计数 */\n" + s[j:]

elif mode in ("noreg", "noreg0"):
    # 调用点形如 extc_rc_storeRefOwned((void **)&SLOT, VAL, OWNER)；
    # 定义那行的签名是 (void **slot, void *v, void *owner)，不会被这个模式命中。
    KEY = "extc_rc_storeRefOwned((void **)&"
    out, i, n = [], 0, 0
    while True:
        j = s.find(KEY, i)
        if j < 0:
            out.append(s[i:])
            break
        k = s.index("(", j + len("extc_rc_storeRefOwned"))
        depth, m = 0, k
        while True:
            c = s[m]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            m += 1
        inner = s[k + 1:m]
        args, d, cur = [], 0, ""
        for c in inner:
            if c == "(":
                d += 1
            elif c == ")":
                d -= 1
            if c == "," and d == 0:
                args.append(cur); cur = ""
            else:
                cur += c
        args.append(cur)
        assert len(args) == 3, args
        repl = None
        if mode == "noreg0":
            if args[2].strip() == "(void *)(0)":
                repl = "extc_rc_storeRef(%s, %s)" % (args[0].strip(), args[1].strip())
        else:
            repl = "extc_rc_storeRef(%s, %s)" % (args[0].strip(), args[1].strip())
        if repl is None:
            out.append(s[i:m + 1])
        else:
            out.append(s[i:j]); out.append(repl); n += 1
        i = m + 1
    s = "".join(out)
    assert n > 0, "no storeRefOwned call sites"
    print(mode + ": patched", n, "sites", file=sys.stderr)

elif mode.startswith("pad"):
    target = int(mode[3:])
    i = [0]

    def rep(m):
        body, n = m.group(2), int(m.group(3))
        assert target >= n, "pad target %d < existing %d" % (target, n)
        i[0] += 1
        return "extc_rc_maybeCollect(%s, (void *[]){ %s%s }, %d);" % (
            m.group(1), body, ", (void *)(0)" * (target - n), target)

    s, cnt = CALL.subn(rep, s)
    assert cnt > 0, "no call sites patched"
    print("pad%d: patched %d sites" % (target, cnt), file=sys.stderr)

else:
    sys.exit("unknown mode: " + mode)

open(DST, "w").write(s)
