#!/usr/bin/env python3
"""函数声明与函数定义必须有 Doxygen 说明 —— 棘轮，只许变好。

规范见 `docs/COMMENT-STYLE.md`：说明 · 参数 · 返回值 · 使用例。

    tools/check_doxygen.py             报告
    tools/check_doxygen.py --gate      未覆盖计数比基线涨了就失败（check.sh 调用）
    tools/check_doxygen.py --write-baseline

判据：`src/**/*.h` 里的函数声明、以及 `src/**/*.c` 里的函数定义，
其**紧邻上方的注释块**里要有 `@brief`。
基线 `tools/doxygen-baseline.txt` 按文件记未覆盖数，**只许变少**。

已知的计数口径（不是精确度量，是进度棘轮）：
* `.h` 只算**原型**（以 `;` 结尾、带行尾注释的先剥掉注释）；`static inline` 函数体不算。
* `.c` 只算**顶格或缩进的函数定义**（以 `{` 结尾或下一行是 `{`、且不以 `;` 结尾）。
* **跨多行**的声明算在它的最后一行上，因此其上方注释块可能找不到 —— 会偏保守（多算欠账）。
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASE = ROOT / "tools" / "doxygen-baseline.txt"

DECL = re.compile(r"^\s*(?:static\s+|extern\s+|inline\s+)*[A-Za-z_][\w \t*]*\s+\**\w+\s*\([^;{]*\)\s*;\s*$")


def blocks_and_decls(path):
    """返回 (声明总数, 有 @brief 的声明数)。看每条声明上方最近的注释块。"""
    lines = path.read_text(encoding="utf-8", errors="replace").split("\n")
    total = covered = 0
    for i, l in enumerate(lines):
        if CTRL.match(l.strip()):
            continue
        # 行尾注释不算声明的一部分（`bool f(int x);  /* 说明 */`）
        if not DECL.match(re.sub(r"/\*.*?\*/\s*$", "", l).rstrip()):
            continue
        total += 1
        # 往上找最近的 /** ... */ 或 /* ... */ 块
        j = i - 1
        while j >= 0 and lines[j].strip() == "":
            j -= 1
        if j < 0:
            continue
        # 若这一行是块的结尾，往回找到块的开始
        if lines[j].strip().endswith("*/"):
            k = j
            while k >= 0 and "/**" not in lines[k] and "/*" not in lines[k]:
                k -= 1
            chunk = "\n".join(lines[k:j + 1]) if k >= 0 else ""
        else:
            # 单行注释
            chunk = lines[j]
        if "@brief" in chunk:
            covered += 1
    return total, covered


CTRL = re.compile(r"^(?:static\s+|extern\s+|inline\s+)*(?:if|else|for|while|switch|do|return|case|default)\b")


DEF = re.compile(r"^\s*(?:static\s+|extern\s+|inline\s+)*[A-Za-z_][\w \t*]*\s+\**\w+\s*\([^;{]*\)\s*\{?\s*$")


def defs_in_c(path):
    """`.c` 里的函数定义：以 `{` 结尾（或下一行是 `{`）的签名行。"""
    lines = path.read_text(encoding="utf-8", errors="replace").split("\n")
    total = covered = 0
    for i, l in enumerate(lines):
        if l.strip().endswith(";") or CTRL.match(l.strip()) or not DEF.match(l):
            continue
        nxt = lines[i + 1].strip() if i + 1 < len(lines) else ""
        if not (l.rstrip().endswith("{") or nxt.startswith("{")):
            continue
        total += 1
        j = i - 1
        while j >= 0 and lines[j].strip() == "":
            j -= 1
        if j < 0:
            continue
        if lines[j].strip().endswith("*/"):
            k = j
            while k >= 0 and "/*" not in lines[k]:
                k -= 1
            chunk = "\n".join(lines[k:j + 1]) if k >= 0 else ""
            if "@brief" in chunk:
                covered += 1
    return total, covered


def scan():
    out = {}
    for h in sorted(ROOT.glob("src/**/*.h")):
        t, c = blocks_and_decls(h)
        if t:
            out[h.relative_to(ROOT).as_posix()] = (t, c)
    for c in sorted(ROOT.glob("src/**/*.c")):
        t, cv = defs_in_c(c)
        if t:
            k = c.relative_to(ROOT).as_posix()
            prev = out.get(k, (0, 0))
            out[k] = (prev[0] + t, prev[1] + cv)
    return out


def main():
    data = scan()
    miss = {f: t - c for f, (t, c) in data.items()}
    total = sum(t for t, _ in data.values())
    covered = sum(c for _, c in data.values())

    base = {}
    if BASE.exists():
        for l in BASE.read_text(encoding="utf-8").splitlines():
            if l.strip() and not l.startswith("#"):
                k, v = l.rstrip("\n").split("\t")
                base[k] = int(v)

    grown = sorted((f, m, base.get(f, 0)) for f, m in miss.items() if m > base.get(f, 0))
    fixed = sorted((f, base[f], miss.get(f, 0)) for f in base if base[f] > miss.get(f, 0))

    if "--write-baseline" in sys.argv:
        BASE.write_text(
            "# 未写 Doxygen 的声明数（棘轮：只许变少）\n"
            + "\n".join(f"{f}\t{miss[f]}" for f in sorted(miss)) + "\n", encoding="utf-8")
        print(f"基线已写：{sum(miss.values())} 条待补")
        return 0

    if "--gate" in sys.argv:
        bad = False
        if grown:
            for f, m, b in grown:
                print(f"  {f}: 未覆盖从 {b} 涨到 {m}")
            bad = True
        if fixed:
            print(f"有 {len(fixed)} 个文件的欠账已减少 ⇒ 请从 tools/doxygen-baseline.txt 下调：")
            for f, b, m in fixed[:10]:
                print(f"  {f}: {b} → {m}")
            bad = True
        if bad:
            sys.exit(1)
        print(f"ok  Doxygen 覆盖未退步（{covered}/{total} 个声明有 @brief，欠账 {sum(miss.values())}）")
        return 0

    print(f"声明 {total} 个 · 有 @brief {covered} 个 · 欠账 {sum(miss.values())} 个")
    for f, (t, c) in sorted(data.items(), key=lambda kv: -(kv[1][0] - kv[1][1]))[:10]:
        print(f"  {t - c:4d} 欠 / {t:4d}   {f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
