#!/usr/bin/env python3
# tools/check_sugar_manual.py —— 语法糖手册的双向门禁。
#
# 为什么要有这条判据（而不是靠记性）：手册与用例会各走一边 —— 表里写着一条糖，
# 却没有用例在跑它；或者 tests/sugar/ 新加了一个用例，手册没提。任何一侧漏了，
# 读者拿到的就是一份「看着全、其实过时」的清单。所以两侧必须互相点名，缺一即红。
#
# 口径（客观、可机检）：
#   ① 手册里的每张表，表头必须是 写法 / 展开成什么 / 求值次数与边界 / 用例；
#      每条数据行四格都不许空，「用例」格至少点名一个 tests/sugar/<名字>.extc。
#   ② 点名的用例必须真的在 tests/sugar/ 下，且文件里有 // expect: 判据行。
#   ③ 反过来：tests/sugar/ 下的每个 .extc 都必须被手册点名（没被点名 = 没人知道它存在）。
#   ④ 用例名必须以 decl_ / assign_ / sugar_ 开头 —— tests/sugar/run.sh 按这三个前缀跑，
#      名字不对的用例根本不会被执行，等于没写。
#
# 用法：
#   python3 tools/check_sugar_manual.py            报告（不过也 exit 0）
#   python3 tools/check_sugar_manual.py --gate     任一侧缺失 ⇒ exit 1
#   python3 tools/check_sugar_manual.py --selftest 证明闸门有牙（每个反例都要被抓到）
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANUAL = ROOT / 'docs' / 'usage' / 'SUGAR.md'
CASES = ROOT / 'tests' / 'sugar'

COLUMNS = ('写法', '展开成什么', '求值次数与边界', '用例')
CASE_REF = re.compile(r'tests/sugar/([A-Za-z0-9_-]+\.extc)')
PREFIXES = ('decl_', 'assign_', 'sugar_')
SEPARATOR = re.compile(r'^\|[\s:|-]+\|$')
PIPE = chr(124)
BS = chr(92)


def split_row(line):
    '把一行表格切成格子；转义的竖线（反斜线 + 竖线）算单元格内容。'
    body = line.strip()
    if body.startswith(PIPE):
        body = body[1:]
    if body.endswith(PIPE) and not body.endswith(BS + PIPE):
        body = body[:-1]
    cells, cur, i = [], [], 0
    while i < len(body):
        ch = body[i]
        if ch == BS and i + 1 < len(body) and body[i + 1] == PIPE:
            cur.append(PIPE)
            i += 2
            continue
        if ch == PIPE:
            cells.append(''.join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(ch)
        i += 1
    cells.append(''.join(cur).strip())
    return cells


def tables(lines):
    '把连续的竖线行分组成 (起始行号, 行列表) 的表格块。'
    out, i = [], 0
    while i < len(lines):
        if lines[i].strip().startswith(PIPE):
            start, block = i, []
            while i < len(lines) and lines[i].strip().startswith(PIPE):
                block.append(lines[i])
                i += 1
            out.append((start + 1, block))
        else:
            i += 1
    return out


def analyze(text, cases):
    '''cases: {文件名: 有判据行吗}。返回 (findings, 数据行数, 被点名的用例集合)。'''
    findings, rows, seen = [], 0, set()
    for lineno, block in tables(text.split(chr(10))):
        if len(block) < 3:
            findings.append('第 %d 行：表格只有表头，没有数据行' % lineno)
            continue
        head = split_row(block[0])
        if tuple(head) != COLUMNS:
            findings.append('第 %d 行：表头是 %s，应为 %s'
                            % (lineno, ' / '.join(head), ' / '.join(COLUMNS)))
            continue
        if not SEPARATOR.match(block[1].strip()):
            findings.append('第 %d 行：表头下面缺少分隔行' % (lineno + 1))
        for k, line in enumerate(block[2:], start=2):
            cells = split_row(line)
            row = lineno + k
            if len(cells) != len(COLUMNS):
                findings.append('第 %d 行：有 %d 格，表头是 %d 格（格子里的竖线要转义）'
                                % (row, len(cells), len(COLUMNS)))
                continue
            rows += 1
            for name, cell in zip(COLUMNS, cells):
                if not cell:
                    findings.append('第 %d 行：%s 这一格空着' % (row, name))
            refs = CASE_REF.findall(cells[3])
            if not refs:
                findings.append('第 %d 行：用例格没有点名任何 tests/sugar/<名字>.extc' % row)
            for name in refs:
                seen.add(name)
                if name not in cases:
                    findings.append('第 %d 行：点名的用例不存在：tests/sugar/%s' % (row, name))
                    continue
                if not name.startswith(PREFIXES):
                    findings.append('用例名不以 decl_ / assign_ / sugar_ 开头，run.sh 跑不到：tests/sugar/%s' % name)
                if not cases[name]:
                    findings.append('用例没有判据行（// expect:）：tests/sugar/%s' % name)
    for name in sorted(set(cases) - seen):
        findings.append('手册缺条目：tests/sugar/%s 没有被任何一行点名' % name)
    return findings, rows, seen


def collect_cases():
    out = {}
    for p in sorted(CASES.glob('*.extc')):
        out[p.name] = 'expect' in p.read_text(encoding='utf-8', errors='replace')
    return out


def selftest():
    '每个反例都必须被 analyze 抓到，否则这条闸门只是装饰。'
    good_case = {'sugar_x.extc': True}
    good = chr(10).join([
        '| 写法 | 展开成什么 | 求值次数与边界 | 用例 |',
        '|---|---|---|---|',
        '| a = b, b = a | 先读后写 | 只接受等号 | tests/sugar/sugar_x.extc |',
    ])
    bad = []
    def case(name, text, cases, needle):
        findings, _rows, _seen = analyze(text, cases)
        if any(needle in f for f in findings):
            print('  ok   ' + name)
        else:
            print('  FAIL ' + name + '（没抓到「' + needle + '」）')
            bad.append(name)
    if analyze(good, good_case)[0]:
        print('  FAIL 正例被判红：' + str(analyze(good, good_case)[0]))
        bad.append('good')
    else:
        print('  ok   正例通过')
    case('反例·用例没被点名', good, {'sugar_x.extc': True, 'sugar_y.extc': True}, '手册缺条目')
    case('反例·点名了不存在的用例',
         good.replace('sugar_x.extc', 'sugar_y.extc'), good_case, '点名的用例不存在')
    case('反例·用例没有判据行', good, {'sugar_x.extc': False}, '没有判据行')
    case('反例·表头不对', good.replace('| 展开成什么 |', '| 怎么展开 |'), good_case, '表头是')
    case('反例·格子数不对', good + chr(10) + '| a | b | c |', good_case, '有 3 格')
    case('反例·展开了没写',
         good.replace('| 先读后写 |', '|  |'), good_case, '展开成什么 这一格空着')
    case('反例·用例格空着',
         good.replace('| tests/sugar/sugar_x.extc |', '|  |'), good_case, '用例格没有点名')
    case('反例·用例名 run.sh 跑不到',
         good.replace('tests/sugar/sugar_x.extc', 'tests/sugar/mine.extc'),
         {'mine.extc': True}, 'run.sh 跑不到')
    if bad:
        print('selftest: %d 项失败' % len(bad))
        return 1
    print('selftest: 9/9 通过（1 正例 + 8 反例）')
    return 0


def main(argv):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('--gate', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not MANUAL.exists():
        print('没有手册：docs/usage/SUGAR.md')
        return 1
    cases = collect_cases()
    findings, rows, seen = analyze(MANUAL.read_text(encoding='utf-8', errors='replace'), cases)
    if findings:
        print('语法糖手册与用例失去同步：%d 处' % len(findings))
        for x in findings:
            print('  ' + x)
        return 1 if a.gate else 0
    print('ok: %d 条糖 / %d 个用例双向点名（手册 -> 用例、用例 -> 手册）'
          % (rows, len(cases)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))