#!/usr/bin/env python3
"""tools/loc.py -- 行数/字符数统计的 **Python 参考实现**

它是 `tools/loc.extc` 的对拍基准，不是替代品：两份**独立实现**给出同一组数，
比任何一份自己跑绿都更能说明问题（这一对拍当场抓出过 extC 版一个真 bug：
把"块注释还开着"当成"末行还没结算"的判据）。

    git ls-files | python3 tools/loc.py            # 每个文件一行 + TOTAL
    python3 tools/loc.py src/check_top.c           # 也可以直接给文件

输出格式与 extC 版**逐字一致**：`<路径> <字节> <字符> <行> <代码> <注释> <空行>`。

分类规则（改一处就要改两处）：
  · 行首在块注释里 => 注释；离开块要看**本行**有没有 `*/`
  · 行首非空白是 `/*` => 注释（本行没有 `*/` 就进块）；`//` => 注释
  · 全空白 => 空行；否则 => 代码
  · 字符数 = UTF-8 码点数（续字节 10xxxxxx 不计）
"""
import sys


def count(data: bytes):
    """(bytes, chars, lines, code, comment, blank)"""
    txt = data.decode("utf-8", "replace")
    # 字符数按 UTF-8 规则数：续字节 10xxxxxx 不算一个字符
    chars = sum(1 for b in data if (b & 0xC0) != 0x80)
    lines = code = comment = blank = 0
    if txt:
        parts = txt.split("\n")
        if txt.endswith("\n"):
            parts = parts[:-1]          # 末尾换行不产生一条空行
        inblk = False
        for line in parts:
            s = line.strip()
            if inblk:
                comment += 1
                if "*/" in s:
                    inblk = False
            elif s.startswith("/*"):
                comment += 1
                if "*/" not in s:
                    inblk = True
            elif s.startswith("//"):
                comment += 1
            elif not s:
                blank += 1
            else:
                code += 1
            lines += 1
    return len(data), chars, lines, code, comment, blank


def main(argv):
    paths = argv[1:] if len(argv) > 1 else [l for l in sys.stdin.read().split("\n") if l]
    tot = [0] * 6
    files = bad = 0
    for path in paths:
        try:
            data = open(path, "rb").read()
        except OSError:
            print(f"!! {path}")
            bad += 1
            continue
        st = count(data)
        print(f"{path} {st[0]} {st[1]} {st[2]} {st[3]} {st[4]} {st[5]}")
        tot = [a + b for a, b in zip(tot, st)]
        files += 1
    print(f"TOTAL {tot[0]} {tot[1]} {tot[2]} {tot[3]} {tot[4]} {tot[5]} files={files} bad={bad}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
