#!/usr/bin/env python3
"""Structural smoke check for the regional allocation cache, with mutations.

The fallback path is pinned to the size-class "has room" lists
(extc_rc_classSlot / extc_rc_classPush): allocation must be O(1) in the number
of live slabs and must not fall back to scanning r->slabs.  This is a
structure check only; the scaling bound is enforced by gate-slab-scale.
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
    alloc = body(text, 'static void *extc_rc_alloc(')
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
        'freeSlot re-registers room': 'if (!s->avail) extc_rc_classPush(s->region, s);' in free_slot,
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
        return 0
    bad = violations(text)
    for item in bad:
        print('FAIL:', item)
    if not bad:
        print('ok allocation cache structure')
    return bool(bad)


if __name__ == '__main__':
    raise SystemExit(main())
