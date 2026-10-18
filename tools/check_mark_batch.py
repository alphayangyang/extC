#!/usr/bin/env python3
"""Structural smoke check for the batched mark walk, with mutations.

This does not establish a latency bound; it only keeps the shape that buys the
latency (pop a batch, prefetch each popped object as it lands in the batch, then
scan the batch) from being silently refactored back into a serial
one-object-at-a-time chase. It also pins the prefetch where it pays: inside the
fill loop, not re-split into a second pass over the batch.
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
        'pop loop': 'while (_nb < EXTC_RC_MARK_BATCH_POP && budget > 0 && r->msp' in body,
        'budget accounting': 'budget--; pops++;' in body,
        'prefetch in fill loop': re.search(r'_batch\[_nb\+\+\] = \(uintptr_t\)r->mstk\[--r->msp\];\s*\n\s*__builtin_prefetch\(\(void \*\)\(_batch\[_nb - 1\]', body) is not None,
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
                      'while (_nb < EXTC_RC_MARK_BATCH_POP && budget > 0 && r->msp',
                      'budget--; pops++;', 'uintptr_t _raw = _batch[_b];',
                      '__builtin_prefetch((void *)(_batch[_nb - 1]',
                      '#if EXTC_RC_MARK_BATCH_POP > EXTC_RC_MARK_BATCH_MAX']:
            assert token in text, token
            mutated = text.replace(token, '/* injected */')
            assert violations(mutated), token
            print('ok red:', token)
        # 单独一趟预取循环不许回来：那会把预取推后（攒完再走一遍），也白多一趟循环
        split = text.replace('budget--; pops++;',
                             'for (size_t _pf = 0; _pf < _nb; _pf++) __builtin_prefetch((void *)(_batch[_pf] & ~(uintptr_t)1), 0, 1);\n            budget--; pops++;')
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
