#!/usr/bin/env python3
"""Structural smoke check for the regional allocation cache, with mutations.

The fallback path is pinned to the size-class "has room" lists
(extc_rc_classSlot / extc_rc_classPush): allocation must be O(1) in the number
of live slabs and must not fall back to scanning r->slabs.  The allocation body
is also pinned to be inlined at every allocation site (`always_inline` through
the `EXTC_RC_ALLOC_DECL` macro; `-DEXTC_RC_ALLOC_INLINE=0` swaps it to
`noinline` for size), because the per-allocation call and stack frame are a
third of the allocation cost.  This is a structure check
only; the scaling bound is enforced by gate-slab-scale.

The per-allocation epilogue guard is pinned at the call site: `extc_rc_finish`
starts with `!marking || !seedsDone || msp != 0` and the alloc path calls it on
every allocation while marking, so the caller tests the two remaining
conditions (`seedsDone && msp == 0`) and skips the call otherwise; the guard
inside finish stays as defence in depth.
"""
import argparse
import ast
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def emitter_text():
    source = (ROOT / 'src/back/reclaim.c').read_text()
    start = source.index('RECLAIM_RT[]')
    end = source.index('\n};', start)
    return ''.join(ast.literal_eval(m.group()) for m in re.finditer(r'"(?:[^"\\]|\\.)*"', source[start:end]))


def body(text, sig):
    start = text.find(sig)
    if start < 0:
        return ''
    a = text.index('{', start)
    depth = 1
    b = a + 1
    while b < len(text) and depth:
        depth += (text[b] == '{') - (text[b] == '}')
        b += 1
    return text[a:b]


def violations(text):
    alloc = body(text, 'static EXTC_RC_ALLOC_DECL void *extc_rc_alloc(')
    free_slot = body(text, 'static void extc_rc_freeSlot(')
    region_free = body(text, 'static void extc_rc_regionFree(')
    if not alloc:
        return ['allocation function missing']
    checks = {
        'cache field': 'ExtcRcSlab *allocHot[32];' in text,
        'cache read': 'ExtcRcSlab **hot = ac < 32 ? &r->allocHot[ac]' in alloc,
        # allocHot 的尺寸类不变式（唯一写入点见下） ⇒ 不再逐次验 s->ss；若有人把
        # "顺手校验"加回来，这里红。
        'no redundant size check': 's->ss != ss' not in alloc,
        'cache write single site': text.count('*hot = s;') == 1,
        'capacity check': 's->freelist < 0 && s->bump >= s->nslots' in alloc,
        'class lookup': 'ExtcRcClass *c = extc_rc_classSlot(r, ss);' in alloc,
        'class head': 's = c ? c->head : (ExtcRcSlab *)0;' in alloc,
        'lazy pop': 'c->head = s->availNext;' in alloc and 's->avail = 0;' in alloc,
        'no slab scan': 's = r->slabs;' not in alloc and 'while (s && (s->ss != ss' not in alloc,
        'new slab pushed': 'extc_rc_classPush(r, s);' in alloc,
        'cache write': 'if (hot) *hot = s;' in alloc,
        # 分配体在每个分配点内联：一次调用 + 进出栈约 13.5 Ir/分配（-O1 实测），内联后
        # 每分配省掉这一笔，还能让常量 n 折叠成尺寸类的立即数。开关保留给「分配点极多、
        # 不想按每点 ~2.7 KB 涨二进制」的程序。摘掉属性或开关就红。
        'alloc inline switch': all(t in text for t in (
            '#ifndef EXTC_RC_ALLOC_INLINE',
            '#define EXTC_RC_ALLOC_INLINE 1',
            '#if EXTC_RC_ALLOC_INLINE',
            '#define EXTC_RC_ALLOC_DECL __attribute__((noinline))')),
        'alloc always inlined': all(t in text for t in (
            '#define EXTC_RC_ALLOC_DECL __attribute__((always_inline)) inline',
            '__attribute__((unused)) static EXTC_RC_ALLOC_DECL void *extc_rc_alloc(')),
        'freeSlot re-registers room': 'if (!s->avail) extc_rc_classPush(s->region, s);' in free_slot,
        # 标记期每分配都调一次 finish，而它的首行就是「marking 且 seedsDone 且栈空」才真收尾
        # ⇒ 绝大多数调用是调用+返回+早退。守卫上提到调用点（这里是 marking 分支内，只剩两条），
        # 裸调用回潮就红。
        'finish guard hoisted': 'if (r->seedsDone && r->msp == 0) extc_rc_finish(r);' in alloc,
        'no bare finish call': not re.search(r'(?m)^\s*extc_rc_finish\(r\);', alloc),
        'class table freed': 'r->clsBkt[b] = c->next;' in region_free and 'free(c);' in region_free,
        'class helpers': all(name in text for name in (
            'extc_rc_classBucket(size_t ss)',
            'extc_rc_classNeed(ExtcRcRegion *r, size_t ss)',
            'extc_rc_classSlot(const ExtcRcRegion *r, size_t ss)',
            'extc_rc_classPush(ExtcRcRegion *r, ExtcRcSlab *s)')),
    }
    return [k for k, ok in checks.items() if not ok]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    text = emitter_text()
    if args.selftest:
        assert not violations(text), violations(text)
        for token in ['ExtcRcSlab *allocHot[32];', 'ExtcRcSlab **hot = ac < 32 ? &r->allocHot[ac]',
                      's->freelist < 0 && s->bump >= s->nslots',
                      'ExtcRcClass *c = extc_rc_classSlot(r, ss);',
                      's = c ? c->head : (ExtcRcSlab *)0;', 'c->head = s->availNext;',
                      'extc_rc_classPush(r, s);',
                      'if (!s->avail) extc_rc_classPush(s->region, s);',
                      'r->clsBkt[b] = c->next;']:
            assert token in text, token
            mutated = text.replace(token, '/* injected */')
            assert violations(mutated), token
            print('ok red:', token)
        # 线性扫描必须能被单独注入成红：把类链兜底整段换成线性扫描形状。
        old_scan = ('s = r->slabs;\n'
                    '        while (s && (s->ss != ss ||\n'
                    '                    (s->freelist < 0 && s->bump >= s->nslots))) s = s->next;')
        injected = text.replace('s = c ? c->head : (ExtcRcSlab *)0;', old_scan)
        injected = injected.replace('ExtcRcClass *c = extc_rc_classSlot(r, ss);', '')
        assert 'no slab scan' in violations(injected), violations(injected)
        print('ok red: linear slab scan reintroduced')
        # 把多余的尺寸校验加回来也必须红（allocHot 的尺寸类不变式）。
        redundant = text.replace('if (!s || (s->freelist < 0 && s->bump >= s->nslots)) {',
                                 'if (!s || s->ss != ss || (s->freelist < 0 && s->bump >= s->nslots)) {')
        assert 'no redundant size check' in violations(redundant), violations(redundant)
        print('ok red: redundant size re-validation')
        # 缓存写入点变成两处（比如复制到别的路径）也必须红。
        twice = text.replace('if (hot) *hot = s;', 'if (hot) *hot = s;\n    if (hot) *hot = s;')
        assert 'cache write single site' in violations(twice), violations(twice)
        print('ok red: second cache write site')
        # 分配体的内联属性被摘掉（回到每次分配付一次调用 + 进出栈）也必须红。
        noinline = text.replace(
            '#define EXTC_RC_ALLOC_DECL __attribute__((always_inline)) inline',
            '#define EXTC_RC_ALLOC_DECL')
        assert 'alloc always inlined' in violations(noinline), violations(noinline)
        print('ok red: allocation body not inlined')
        # 开关被删掉（没法用 -DEXTC_RC_ALLOC_INLINE=0 换体积）也必须红。
        noswitch = text.replace('#if EXTC_RC_ALLOC_INLINE', '#if 1')
        assert 'alloc inline switch' in violations(noswitch), violations(noswitch)
        print('ok red: allocation inline switch removed')
        # 体积出口那一档必须**明确** noinline（不是「赌编译器不内联」）。
        noglass = text.replace('#define EXTC_RC_ALLOC_DECL __attribute__((noinline))',
                               '#define EXTC_RC_ALLOC_DECL')
        assert 'alloc inline switch' in violations(noglass), violations(noglass)
        print('ok red: size escape hatch lost noinline')
        # 收尾守卫退回裸调用（每次分配白付一次调用+返回+早退）也必须红。
        bare = text.replace('if (r->seedsDone && r->msp == 0) extc_rc_finish(r);',
                            'extc_rc_finish(r);')
        assert 'finish guard hoisted' in violations(bare), violations(bare)
        assert 'no bare finish call' in violations(bare), violations(bare)
        print('ok red: finish guard not hoisted')
        return 0
    bad = violations(text)
    for item in bad:
        print('FAIL:', item)
    if not bad:
        print('ok allocation cache structure')
    return bool(bad)


if __name__ == '__main__':
    raise SystemExit(main())
