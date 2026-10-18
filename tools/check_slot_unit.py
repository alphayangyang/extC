#!/usr/bin/env python3
"""Structural gate for the slot unit (8-byte load rounding) and its premise.

The premise: 8 bytes is a **sufficient** granularity because no extC type needs a
larger natural alignment. That premise lives in the type system (built-ins are at
most 64 bits wide, and every composite is built from them), so it is checked here
at the source; if a wider type ever appears this gate is the thing that must go red.

What it buys: the slot unit decides internal fragmentation, hence peak RSS on
allocation-heavy shapes. It does not establish a bound by itself -- the exhaustive
index check is tests/reclaim-perf/slotindex.c (gate-reclaim-perf), and the numbers
are recorded in docs/CHANGELOG.md.

Injection self-test runs on every invocation: each rule is deleted in turn and the
run must turn red.
"""
import argparse
import ast
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]

# Slot granularity, its floor (the 8-byte free-list head must fit) and the
# wraparound guard. Changing the unit means changing all three.
SLOT_RULES = [
    ('slot rounding', 'size_t s = (n + 7) & ~(size_t)7;'),
    ('slot floor', 'return s > 16 ? s : 16;'),
    ('slot overflow guard', 'extc_rc_oops("slot size overflow")'),
]
# Size-class cache: class = slot unit / 8, table covers units 16..256 bytes.
CACHE_RULES = [
    ('cache field', 'ExtcRcSlab *allocHot[32];'),
    ('cache read', 'ExtcRcSlab **hot = ac < 32 ? &r->allocHot[ac]'),
    ('cache class unit', 'size_t ac = ss / 8;'),
]
# The exhaustive index check must enumerate the *real* granularity, otherwise
# "exhaustive over the domain" would be circular.
INDEX_RULE = ('index domain step', 'ss += 8')
# Alignment premise: exactly these built-ins, nothing wider than 64 bits.
KNOWN_BUILTINS = ['i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64', 'bool']
WIDE = re.compile(r'(128|256|512|f80|f128|__m128|simd|vec)', re.I)


def emitter_text():
    source = (ROOT / 'src/back/reclaim.c').read_text()
    start = source.index('RECLAIM_RT[]')
    end = source.index('\n};', start)
    return ''.join(ast.literal_eval(m.group()) for m in re.finditer(r'"(?:[^"\\]|\\.)*"', source[start:end]))


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


def builtin_names():
    src = (ROOT / 'src/types/types.c').read_text()
    start = src.index('BUILTIN_NAMES[]')
    return re.findall(r'"([^"]+)"', src[start:src.index('};', start)])


def type_kinds():
    src = (ROOT / 'src/ast/ast.h').read_text()
    start = src.index('typedef enum {')
    body = src[start:src.index('} TypeKind;', start)]
    # Only the declarators matter: a comment may legitimately say "no vector types".
    return '\n'.join(l for l in body.splitlines()
                     if '/*' not in l and not l.strip().startswith('*'))


def violations(text, builtins, kinds, index_src):
    slot_body = body_of(text, 'static size_t extc_rc_slotSize(')
    alloc_body = body_of(text, 'static EXTC_RC_ALLOC_DECL void *extc_rc_alloc(')
    bad = []
    if slot_body is None:
        bad.append('slot size function missing')
        slot_body = ''
    if alloc_body is None:
        bad.append('allocation function missing')
        alloc_body = ''
    for name, token in SLOT_RULES:
        if token not in slot_body:
            bad.append(name)
    for name, token in CACHE_RULES:
        if token not in (text if name == 'cache field' else alloc_body):
            bad.append(name)
    name, token = INDEX_RULE
    if token not in index_src:
        bad.append(name)
    if not builtins:
        bad.append('builtin table missing')
    else:
        if builtins != KNOWN_BUILTINS:
            bad.append('builtin set changed (re-review the 8-byte alignment premise)')
        if any(WIDE.search(b) for b in builtins):
            bad.append('builtin wider than 64 bits')
    if not kinds:
        bad.append('type kinds missing')
    elif WIDE.search(kinds):
        bad.append('type kind wider than 64 bits')
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    text = emitter_text()
    builtins = builtin_names()
    kinds = type_kinds()
    index_src = (ROOT / 'tests/reclaim-perf/slotindex.c').read_text()
    bad = violations(text, builtins, kinds, index_src)
    if args.selftest:
        assert not bad, bad
    # Red self-test: delete each rule in turn, plus both alignment-premise halves.
    mutations = [(token, text) for _, token in SLOT_RULES + CACHE_RULES]
    mutations.append((INDEX_RULE[1], index_src))
    for token, where in mutations:
        assert token in where, token
        if where is text:
            mutated = violations(text.replace(token, '/* injected */'), builtins, kinds, index_src)
        else:
            mutated = violations(text, builtins, kinds, index_src.replace(token, '/* injected */'))
        if not mutated:
            print('FAIL: red self-test did not fire for', token)
            return 1
        print('ok red:', token)
    # Alignment premise: a wide built-in or a wide type kind must turn the gate red.
    premise = [('wide builtin', [b for b in builtins] + ['i128'], kinds),
               ('wide type kind', builtins, kinds + '\n    TY_VEC,')]
    for label, mut_builtins, mut_kinds in premise:
        if not violations(text, mut_builtins, mut_kinds, index_src):
            print('FAIL: red self-test did not fire for', label)
            return 1
        print('ok red:', label)
    if args.selftest:
        return 0
    for item in bad:
        print('FAIL:', item)
    if not bad:
        print('ok slot unit structure + red selftest')
    return bool(bad)


if __name__ == '__main__':
    raise SystemExit(main())
