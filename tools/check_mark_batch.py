#!/usr/bin/env python3
"""Structural smoke check for the batched mark walk, with mutations.

This does not establish a latency bound; it only keeps the shape that buys the
latency (pop a batch, prefetch each popped object as it lands in the batch, then
scan the batch) from being silently refactored back into a serial
one-object-at-a-time chase. It also pins the prefetch where it pays: inside the
fill loop, not re-split into a second pass over the batch, and pins the hoisted
batch limit (min of batch width / budget / work quota / stack depth computed
once) instead of re-testing four conditions on every pop -- measurement:
callgrind 130,453,175 -> 128,473,122 Ir on cyclic_pressure 200000.
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
    start = text.find('static size_t extc_rc_markStep(ExtcRcRegion *r, size_t budget) {')
    if start < 0:
        return ['markStep missing']
    a = text.index('{', start)
    depth = 1
    b = a + 1
    while b < len(text) and depth:
        depth += (text[b] == '{') - (text[b] == '}')
        b += 1
    body = text[a:b]
    checks = {
        'batch constant': '#define EXTC_RC_MARK_BATCH_POP' in text,
        'batch array': 'uintptr_t _batch[EXTC_RC_MARK_BATCH_POP];' in body,
        'limit hoisted': ('size_t _lim = EXTC_RC_MARK_BATCH_POP;' in body and
                          'if (budget < _lim) _lim = budget;' in body and
                          'if (_sp < _lim) _lim = _sp;' in body),
        'stack head hoisted': 'void **_stk = r->mstk; size_t _sp = r->msp;' in body,
        'pop from local head': '_batch[_nb] = (uintptr_t)_stk[--_sp];' in body,
        'stack head restored': 'r->msp = _sp;' in body,
        'pop loop': 'while (_nb < _lim) {' in body,
        'budget accounting': 'budget -= _nb; pops += _nb;' in body,
        'prefetch in fill loop': re.search(r'_batch\[_nb\] = \(uintptr_t\)_stk\[--_sp\];\s*\n\s*__builtin_prefetch\(\(void \*\)\(_batch\[_nb\]', body) is not None,
        'no separate prefetch sweep': 'for (size_t _pf = 0' not in body,
        'scan loop': 'for (size_t _b = 0; _b < _nb; _b++) {' in body and 'uintptr_t _raw = _batch[_b];' in body,
        'width ceiling': re.search(r'#if EXTC_RC_MARK_BATCH_POP > EXTC_RC_MARK_BATCH_MAX\s*\n#error', text) is not None,
    }
    return [k for k, ok in checks.items() if not ok]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    text = emitter_text()
    if args.selftest:
        assert not violations(text)
        for token in ['#define EXTC_RC_MARK_BATCH_POP', 'uintptr_t _batch[EXTC_RC_MARK_BATCH_POP];',
                      'size_t _lim = EXTC_RC_MARK_BATCH_POP;',
                      'if (budget < _lim) _lim = budget;',
                      'void **_stk = r->mstk; size_t _sp = r->msp;',
                      'if (_sp < _lim) _lim = _sp;',
                      'r->msp = _sp;',
                      'while (_nb < _lim) {',
                      'budget -= _nb; pops += _nb;', 'uintptr_t _raw = _batch[_b];',
                      '__builtin_prefetch((void *)(_batch[_nb] & ~(uintptr_t)1)',
                      '#if EXTC_RC_MARK_BATCH_POP > EXTC_RC_MARK_BATCH_MAX']:
            assert token in text, token
            mutated = text.replace(token, '/* injected */')
            assert violations(mutated), token
            print('ok red:', token)
        # 单独一趟预取循环不许回来：那会把预取推后（攒完再走一遍），也白多一趟循环
        split = text.replace('budget -= _nb; pops += _nb;',
                             'for (size_t _pf = 0; _pf < _nb; _pf++) __builtin_prefetch((void *)(_batch[_pf] & ~(uintptr_t)1), 0, 1);\n        budget -= _nb; pops += _nb;')
        assert 'no separate prefetch sweep' in violations(split)
        print('ok red: separate prefetch sweep')
        return 0
    bad = violations(text)
    for item in bad:
        print('FAIL:', item)
    if not bad:
        print('ok batched mark walk structure')
    return bool(bad)


if __name__ == '__main__':
    raise SystemExit(main())
