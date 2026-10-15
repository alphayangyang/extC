#!/usr/bin/env python3
"""Structural smoke check for the regional allocation cache, with mutations.
This does not establish a complexity bound or replace runtime tests.
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


def violations(text):
    start = text.find('static void *extc_rc_alloc(')
    if start < 0:
        return ['allocation function missing']
    a = text.index('{', start)
    depth = 1
    b = a + 1
    while b < len(text) and depth:
        depth += (text[b] == '{') - (text[b] == '}')
        b += 1
    alloc = text[a:b]
    checks = {
        'cache field': 'ExtcRcSlab *allocHot[16];' in text,
        'cache read': 'ExtcRcSlab **hot = ac < 16 ? &r->allocHot[ac]' in alloc,
        'size check': 's->ss != ss' in alloc,
        'capacity check': 's->freelist < 0 && s->bump >= s->nslots' in alloc,
        'fallback': 's = r->slabs;' in alloc and 'while (s &&' in alloc,
        'cache write': 'if (hot) *hot = s;' in alloc,
    }
    return [k for k, ok in checks.items() if not ok]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    text = emitter_text()
    if args.selftest:
        assert not violations(text)
        for token in ['ExtcRcSlab *allocHot[16];', 'ExtcRcSlab **hot = ac < 16 ? &r->allocHot[ac]',
                      's->ss != ss', 's->freelist < 0 && s->bump >= s->nslots',
                      's = r->slabs;', 'if (hot) *hot = s;']:
            assert token in text, token
            mutated = text.replace(token, '/* injected */')
            assert violations(mutated), token
            print('ok red:', token)
        return 0
    bad = violations(text)
    for item in bad:
        print('FAIL:', item)
    if not bad:
        print('ok allocation cache structure')
    return bool(bad)


if __name__ == '__main__':
    raise SystemExit(main())
