#!/usr/bin/env python3
"""tools/check_ownslab_body.py --- 守住「标记步专用的 ownSlab 副本」这两条结构事实。

## 为什么需要这个门禁

标记步（extc_rc_markStep）每弹出一个对象都要把地址翻成 slab：这个站点占全运行期指令的
两成上下，且它是**唯一**值得强制内联的那一个调用点。把 extc_rc_ownSlab 整体标成
always_inline 会让**所有**调用点都内联，实测 cross_region 这类形状直接慢两成（寄存器压力）。
所以运行期里复制了一份 extc_rc_ownSlabMark：函数体与 extc_rc_ownSlab 逐字相同，只多一个
always_inline，且只被 markStep 调用。

复制文件的代价是**两份体可能漂移**：改了一份忘了另一份，行为差异只会在最热的那个站点上
悄悄出现，语义判据和指令数棘轮都未必看得见。这条门禁：

1. 两份体逐字相同（含注释；只允许函数名不同），跳过标记数量也一致；
2. 副本带 always_inline、原件**不带**（带上就回到那条会拖慢别的形状的路）；
3. markStep 只调副本，不再调原件。

--selftest 把上面每一条各注入一次，要求判据变红。
"""
import argparse
import ast
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SIG_ORIG = 'static inline ExtcRcSlab *extc_rc_ownSlab(void *p) {'
SIG_COPY = 'static inline __attribute__((always_inline)) ExtcRcSlab *extc_rc_ownSlabMark(void *p) {'
MARK_STEP_SIG = 'static size_t extc_rc_markStep(ExtcRcRegion *r, size_t budget) {'
SKIP_TAGS = ['skip:own-hot', 'skip:lookup-probe']


def emitter_text():
    source = (ROOT / 'src/back/reclaim.c').read_text()
    start = source.index('RECLAIM_RT[]')
    end = source.index('\n};', start)
    return ''.join(ast.literal_eval(m.group())
                   for m in re.finditer(r'"(?:[^"\\]|\\.)*"', source[start:end]))


def body_of(text, needle):
    start = text.find(needle)
    if start < 0:
        return None
    a = text.index('{', start)
    depth = 1
    b = a + 1
    while b < len(text) and depth:
        depth += (text[b] == '{') - (text[b] == '}')
        b += 1
    return text[a:b]


def check(text):
    bad = []
    orig = body_of(text, SIG_ORIG)
    copy = body_of(text, SIG_COPY)
    if orig is None:
        bad.append('原 extc_rc_ownSlab 不见了（签名变了就同步本门禁）')
    if copy is None:
        bad.append('标记步副本 extc_rc_ownSlabMark 不见了')
    if orig is not None and copy is not None:
        if orig != copy:
            bad.append('两份 ownSlab 体不逐字相同（改了一份忘了另一份）')
        for tag in SKIP_TAGS:
            if orig.count(tag) != copy.count(tag):
                bad.append('%s 两份体数量不一致（原件 %d、副本 %d）'
                           % (tag, orig.count(tag), copy.count(tag)))
            elif orig.count(tag) == 0:
                bad.append('%s 在两份体里都不见了' % tag)
    if SIG_COPY not in text:
        bad.append('副本的 always_inline 签名不见了')
    if re.search(r'always_inline\)\)\s+(?:static\s+inline\s+)?ExtcRcSlab \*extc_rc_ownSlab\(',
                 text):
        bad.append('原件也被标成 always_inline（会在所有调用点内联，别的形状会变慢）')
    step = body_of(text, MARK_STEP_SIG)
    if step is None:
        bad.append('extc_rc_markStep 不见了（签名变了就同步本门禁）')
    else:
        if 'extc_rc_ownSlabMark(' not in step:
            bad.append('markStep 没调标记步副本')
        if re.search(r'extc_rc_ownSlab\s*\(', step):
            bad.append('markStep 还在调原件（副本白复制了）')
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    text = emitter_text()
    bad = check(text)
    if args.selftest:
        assert not bad, bad
        orig = body_of(text, SIG_ORIG)
        copy = body_of(text, SIG_COPY)
        assert orig and copy and orig == copy
        line = [l for l in copy.splitlines() if 'extc_rc_ownBig(p)' in l][0]
        cases = [
            ('copy body drifted', text.replace(copy, copy.replace(line, line + ' ', 1), 1)),
            ('markStep calls the original', text.replace('extc_rc_ownSlabMark(o)',
                                                         'extc_rc_ownSlab(o)', 1)),
            ('original marked always_inline',
             text.replace(SIG_ORIG, '__attribute__((always_inline)) ' + SIG_ORIG, 1)),
            ('skip tag deleted from the copy',
             text.replace(copy, copy.replace('skip:own-hot', 'skip:own-ho', 1), 1)),
        ]
        for label, mutated in cases:
            if not check(mutated):
                print('FAIL: red self-test did not fire for', label)
                return 1
            print('ok red:', label)
        return 0
    for item in bad:
        print('FAIL:', item)
    if not bad:
        print('ok ownSlab mark-step copy structure + red selftest')
    return bool(bad)


if __name__ == '__main__':
    raise SystemExit(main())
