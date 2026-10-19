#!/usr/bin/env python3
"""tools/check_retreat.py --- 守住「空清扫退场倍数」的结构事实。

## 为什么需要这个门禁

触发线 = max(2×上一轮存活, 下限) × scale，其中 scale 只在「一次清扫一无所获」时增长
（学到「这里没有环」）。退场倍数原本是 2：空扫一次线到 4× 活集。对「活集大、垃圾少」
的负载（bench/gc/v2 的 chase：48 MB 活集 + 每 256 步 4 个即抛节点）这不够 —— 第一轮
空扫后线只到 32 MiB，于是又付了一轮「标记 32 MiB 活集、只换回约 3 MB 垃圾」的整堆标记。

把倍数改成 4（空扫一次线直接到 8× 活集）就跳过那一轮：chase 的回收轮数 2 → 1、
指令数 464 780 738 → 385 414 942（−17.1%）、峰值 RSS 反而降 4 MB（少一轮标记栈/页）。
它只影响「空扫 + 活集继续涨」的负载：垃圾多的形状下一轮就收到东西 ⇒ scale 复位 0。

这条门禁钉住四件事，否则「倍数悄悄退回 2」「退场改用字面量（绕开宏）」「丢掉溢出夹紧」
这类退化只能靠某个 bench 的墙钟慢慢发现：

1. 默认值是 4（#define EXTC_RC_SCALE_MUL ((int64_t)4)）；
2. 退场那一行用宏、不是字面量；
3. 没有残留的 * 2 退场（旧实现）；
4. 退场之后仍有 EXTC_RC_SCALE_MAX 夹紧（倍数只会涨，不夹会溢出）。

--selftest 把上面每一条各注入一次，要求判据变红。
"""
import argparse
import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DEFINE = '#define EXTC_RC_SCALE_MUL ((int64_t)4)'
RETREAT = 'r->scale = (r->scale > 0 ? r->scale : 1) * EXTC_RC_SCALE_MUL;'
OLD_RETREAT = 'r->scale = (r->scale > 0 ? r->scale : 1) * 2;'
CLAMP = 'if (r->scale > EXTC_RC_SCALE_MAX) r->scale = EXTC_RC_SCALE_MAX;'


def emitter_text():
    source = (ROOT / 'src/back/reclaim.c').read_text()
    start = source.index('RECLAIM_RT[]')
    end = source.index('\n};', start)
    return ''.join(ast.literal_eval(m.group())
                   for m in re.finditer(r'"(?:[^"\\]|\\.)*"', source[start:end]))


def check(text):
    bad = []
    if DEFINE not in text:
        m = re.search(r'#define EXTC_RC_SCALE_MUL ([^\n]*)', text)
        bad.append('退场倍数默认值不是 4：%r（活集大垃圾少的负载会多付一轮整堆标记）'
                   % (m.group(1) if m else '找不到 #define EXTC_RC_SCALE_MUL'))
    n = text.count(RETREAT)
    if n != 1:
        bad.append('退场那一行不是恰好一处 %r（找到 %d 处）' % (RETREAT, n))
    if OLD_RETREAT in text:
        bad.append('仍有旧的 * 2 退场（退场倍数被改回去了？）')
    # 所有对 r->scale 的赋值必须是：清零、退场（宏）、夹紧到 SCALE_MAX。
    for m in re.finditer(r'r->scale = ([^;]+);', text):
        rhs = m.group(1).strip()
        if rhs in ('0', 'EXTC_RC_SCALE_MAX'):
            continue
        if rhs == '(r->scale > 0 ? r->scale : 1) * EXTC_RC_SCALE_MUL':
            continue
        bad.append('意外的 r->scale 赋值：%r（退场只能走宏 / 清零 / 夹紧）' % rhs)
    if CLAMP not in text:
        bad.append('退场后丢了 EXTC_RC_SCALE_MAX 夹紧（倍数只涨，不夹会溢出）')
    return bad


def selftest():
    base = emitter_text()
    cases = [
        ('默认值退回 2', [(DEFINE, '#define EXTC_RC_SCALE_MUL ((int64_t)2)')]),
        ('退场改用字面量', [(RETREAT, OLD_RETREAT)]),
        ('丢掉溢出夹紧', [(CLAMP + '\n', '')]),
        ('另起一处写 scale', [(CLAMP, 'r->scale = 7;\n' + CLAMP)]),
    ]
    fails = 0
    for name, subs in cases:
        text = base
        for old, new in subs:
            if old not in text:
                print('  FAIL 自测「%s」：注入锚点不在运行期文本里（判据会瞎）' % name)
                fails += 1
                break
            text = text.replace(old, new, 1)
        else:
            bad = check(text)
            if bad:
                print('  ok   红自测「%s」：%s' % (name, bad[0]))
            else:
                print('  FAIL 红自测「%s」：注入后判据没变红' % name)
                fails += 1
    if check(base):
        print('  FAIL 基线文本就判红：%s' % check(base)[0])
        fails += 1
    else:
        print('  ok   基线文本判绿')
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true', help='注入式自测：每条判据都要能变红')
    args = ap.parse_args()
    text = emitter_text()
    if args.selftest:
        fails = selftest()
        print()
        if fails:
            print('失败 %d' % fails)
            return 1
        print('  ok   退场倍数自测：4 条注入全部变红')
        return 0
    bad = check(text)
    for b in bad:
        print('  FAIL %s' % b)
    if bad:
        return 1
    print('  ok   退场倍数结构：默认 4 / 退场走宏 / 无旧 *2 / 溢出夹紧都在')
    return 0


if __name__ == '__main__':
    sys.exit(main())
