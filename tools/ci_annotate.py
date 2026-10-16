#!/usr/bin/env python3
# tools/ci_annotate.py —— 把失败的验收项**讲给 API 听**。
#
# 为什么要有这个脚本：CI 的失败现场只能用 `::error::` 注解读 —— 注解对公开仓库免认证
# （`/commits/<sha>/check-runs` → annotations），而跑 CI 的机器上没有 gh，读不到 Actions 日志。
# 注解有两个坑：
#   ① **一步最多留 10 条**，塞满就等于没说；
#   ② 闸门脚本会为**已知欠账**（棘轮基线里的行）打印 `  FAIL …`，`grep -a FAIL 日志` 抓到的
#      多半是这些噪声，真正的失败项被挤在 10 条之外。
#
# 所以这里的权威清单只有一份：`tools/check.py` 写的耗时表（`$SEC_TIMES`，默认
# `/tmp/extc-check-times.tsv`，列 `name group seconds status time`）里 status == FAIL 的项。
# 日志只负责给每个失败项摘"像原因的那几行"：该项表头 `== <名字>（<组>）==` 到下个项表头之间，
# 取那里最后一条 FAIL 行（= check.py 对该项的判决行）与它前面的现场行。
#
# 用法：
#   python3 tools/ci_annotate.py --log /tmp/quick.log --times /tmp/extc-check-times.tsv
#   python3 tools/ci_annotate.py --log … --max 6          # 少留几条
# 输出：若干 `::error::…` 行（GitHub 读），每条都短到人也能一眼看完。
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

ANSI = re.compile(r"\x1b\[[0-9;]*m")
HEADER = re.compile(r"^== (?P<name>.+)（(?P<group>[^（）]*)）==\s*$")
SUMMARY = re.compile(r"^通过 .*失败")
# 判据脚本自己判"这项不行"的行：闸门的 FAIL、棘轮的 新增/该删/转绿、崩了的栈。
REASON = re.compile(r"FAIL|Traceback|Error|error:|新增|该删|转绿|失败|超|不含|未|不成立|跳过")


def read_lines(path: str) -> list:
    try:
        return pathlib.Path(path).read_text(encoding="utf-8", errors="replace").split("\n")
    except OSError:
        return []


def rows_of(times_lines: list) -> list:
    """耗时表里的每一行 (name, group, status)。表里有哪些项 = 日志里哪些表头算项边界。"""
    out = []
    for ln in times_lines:
        if not ln.strip() or ln.startswith("#"):
            continue
        p = ln.split("\t")
        if len(p) >= 4:
            out.append((p[0].strip(), p[1].strip(), p[3].strip()))
    return out


def item_headers(log: list, names: set) -> list:
    """日志里属于**验收项**的表头行号（判据脚本自己的小节表头不算）。"""
    out = []
    for i, ln in enumerate(log):
        m = HEADER.match(ANSI.sub("", ln))
        if m and m.group("name") in names:
            out.append((i, m.group("name"), m.group("group")))
    return out


def block_of(log: list, heads: list, idx: int) -> list:
    """第 idx 个头对应的项正文：到下一个**项表头**为止。"""
    end = len(log)
    for j, _name, _group in heads:
        if j > heads[idx][0]:
            end = j
            break
    return [ANSI.sub("", x) for x in log[heads[idx][0] + 1:end]]


def clean(s: str, cap: int = 200) -> str:
    s = ANSI.sub("", s).rstrip()
    s = s.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    return s.strip()[:cap]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="把验收失败的项讲成 GitHub 注解")
    ap.add_argument("--log", default="/tmp/quick.log", help="check.sh 的日志")
    ap.add_argument("--times", default="/tmp/extc-check-times.tsv", help="check.py 写的耗时表")
    ap.add_argument("--max", type=int, default=10, help="最多留几条注解（GitHub 一步上限 10）")
    args = ap.parse_args(argv)

    log = read_lines(args.log)
    rows = rows_of(read_lines(args.times))
    fails = [(n, g) for n, g, st in rows if st == "FAIL"]
    out = []

    summary = [ln for ln in log if SUMMARY.match(ANSI.sub("", ln))]
    if summary:
        out.append("全套：" + clean(summary[-1], 80))
    if fails:
        out.append("失败 %d 项：" % len(fails)
                   + clean("、".join("%s（%s）" % (n, g) for n, g in fails), 320))

    if fails:
        heads = item_headers(log, {n for n, _g, _st in rows})
        for name, group in fails:
            idx = next((k for k, (_i, n, g) in enumerate(heads) if n == name and g == group), None)
            if idx is None:
                continue
            body = [x for x in block_of(log, heads, idx) if x.strip()]
            reasons = [x for x in body if REASON.search(x)]
            pick = reasons[-3:] if reasons else body[-3:]
            if pick:
                out.append("项 %s（%s）：%s" % (name, group, clean(pick[-1], 200)))
                for extra in pick[:-1]:
                    out.append("  ↳ " + clean(extra, 200))
            else:
                out.append("项 %s（%s）：日志里没有输出" % (name, group))
    if not fails and not summary:
        out.append("耗时表里没有失败项，日志里也没有汇总行：%s / %s" % (args.times, args.log))

    for line in out[:max(1, args.max)]:
        print("::error::" + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())