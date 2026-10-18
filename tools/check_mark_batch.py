#!/usr/bin/env python3
"""Structural smoke check for the batched mark walk, with mutations.

This does not establish a latency bound; it only keeps the shape that buys the
latency (pop a batch, prefetch each popped object as it lands in the batch, then
scan the batch) from being silently refactored back into a serial
one-object-at-a-time chase. It also pins the prefetch where it pays: inside the
fill loop, not re-split into a second pass over the batch, and pins the hoisted
batch limit (min of batch width / budget / work quota / stack depth computed
once) instead of re-testing four conditions on every pop.

The scan body lives in one file-scope macro (EXTC_RC_MARK_SCAN) shared by two
call sites: the batch loop (deep stack) and a shallow-stack fast path that pops
and scans objects without assembling a batch, draining the shallow stack in a
loop of its own. The fast path exists because the batch machinery (array / limit
min / fill loop / scan loop) is pure fixed cost when the mark stack holds only a
handful of objects; the batch is a prefetch-lead-time mechanism, so it only pays
when enough objects are queued. The drain loop pins one more thing: once we are
in the shallow path, re-entering the outer loop to re-test the walk/budget/
shallow conditions for every single object costs about as much as the scan
so the shallow path keeps popping until the stack grows past the threshold.
Both properties are pinned below -- measurement: callgrind on cyclic_pressure
200000 drops 94,587,483 -> 89,675,120 Ir once the shallow path is in.
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
    checks = {
        'scan macro defined': '#define EXTC_RC_MARK_SCAN(_rawIn) do { ' in text,
        'scan macro takes raw': '    uintptr_t _raw = (_rawIn); ' in text,
        'scan body shared once': text.count('uintptr_t _raw = (_rawIn);') == 1,
        'scan body not inlined twice': 'uintptr_t _raw = _batch[_b];' not in text,
        'shallow threshold defined': ('#ifndef EXTC_RC_MARK_SHALLOW' in text and
                                      '#define EXTC_RC_MARK_SHALLOW' in text),
        'shallow fast path': ('if ((size_t)r->msp <= (size_t)EXTC_RC_MARK_SHALLOW && budget > 0' in text and
                              'void *_one = r->mstk[--r->msp];' in text and
                              'EXTC_RC_MARK_SCAN((uintptr_t)_one);' in text),
        # 浅栈路径要一次把浅栈排空：每弹一个就回外层重判 while/浅栈条件，比扫描体本身还显眼
        'shallow drain loop': ('} while (r->msp > 0 && (size_t)r->msp <= (size_t)EXTC_RC_MARK_SHALLOW && budget > 0' in text),
        'batch constant': '#define EXTC_RC_MARK_BATCH_POP' in text,
        'batch array': 'uintptr_t _batch[EXTC_RC_MARK_BATCH_POP];' in text,
        'limit hoisted': ('size_t _lim = EXTC_RC_MARK_BATCH_POP;' in text and
                          'if (budget < _lim) _lim = budget;' in text and
                          'if (_sp < _lim) _lim = _sp;' in text),
        'stack head hoisted': 'void **_stk = r->mstk; size_t _sp = r->msp;' in text,
        'pop from local head': '_batch[_nb] = (uintptr_t)_stk[--_sp];' in text,
        'stack head restored': 'r->msp = _sp;' in text,
        'pop loop': 'while (_nb < _lim) {' in text,
        'budget accounting': 'budget -= _nb; pops += _nb;' in text,
        'prefetch in fill loop': re.search(r'_batch\[_nb\] = \(uintptr_t\)_stk\[--_sp\];\s*\n\s*__builtin_prefetch\(\(void \*\)\(_batch\[_nb\]', text) is not None,
        'no separate prefetch sweep': 'for (size_t _pf = 0' not in text,
        'scan loop calls macro': 'for (size_t _b = 0; _b < _nb; _b++) { EXTC_RC_MARK_SCAN(_batch[_b]); }' in text,
        'width ceiling': re.search(r'#if EXTC_RC_MARK_BATCH_POP > EXTC_RC_MARK_BATCH_MAX\s*\n#error', text) is not None,
        # 扫描体：子对象循环走「指针步进」而不是每次重读 t->nptr/t->off[k]（-O1 下实测省 0.68%）；
        # 推进计数器的分派点改成内联（-O1 不跨调用点内联 ⇒ 每趟一次调用/返回）。
        'child loop pointer walk': ('const uint32_t *_op = t->off; const uint32_t *_oend = _op + t->nptr;' in text and
                                    'for (; _op < _oend; ++_op) {' in text and
                                    'void *child = *(void **)((char *)o + *_op);' in text),
        'mark advance inlined': '__attribute__((unused)) static inline __attribute__((always_inline)) void extc_rc_markAdvance(' in text,
    }
    return [k for k, ok in checks.items() if not ok]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    text = emitter_text()
    if args.selftest:
        assert not violations(text)
        for token in ['#define EXTC_RC_MARK_SCAN(_rawIn) do { ',
                      '    uintptr_t _raw = (_rawIn); ',
                      '#define EXTC_RC_MARK_SHALLOW',
                      'if ((size_t)r->msp <= (size_t)EXTC_RC_MARK_SHALLOW && budget > 0',
                      '} while (r->msp > 0 && (size_t)r->msp <= (size_t)EXTC_RC_MARK_SHALLOW && budget > 0',
                      '#define EXTC_RC_MARK_BATCH_POP',
                      'uintptr_t _batch[EXTC_RC_MARK_BATCH_POP];',
                      'size_t _lim = EXTC_RC_MARK_BATCH_POP;',
                      'if (budget < _lim) _lim = budget;',
                      'void **_stk = r->mstk; size_t _sp = r->msp;',
                      'if (_sp < _lim) _lim = _sp;',
                      'r->msp = _sp;',
                      'while (_nb < _lim) {',
                      'budget -= _nb; pops += _nb;',
                      'for (size_t _b = 0; _b < _nb; _b++) { EXTC_RC_MARK_SCAN(_batch[_b]); }',
                      '__builtin_prefetch((void *)(_batch[_nb] & ~(uintptr_t)1)',
                      '#if EXTC_RC_MARK_BATCH_POP > EXTC_RC_MARK_BATCH_MAX',
                      'const uint32_t *_op = t->off;',
                      '__attribute__((unused)) static inline __attribute__((always_inline)) void extc_rc_markAdvance(']:
            assert token in text, token
            mutated = text.replace(token, '/* injected */')
            assert violations(mutated), token
            print('ok red:', token)
        # 单独一趟预取循环不许回来：那会把预取推后（攒完再走一遍），也白多一趟循环
        split = text.replace('budget -= _nb; pops += _nb;',
                             'for (size_t _pf = 0; _pf < _nb; _pf++) __builtin_prefetch((void *)(_batch[_pf] & ~(uintptr_t)1), 0, 1);\n        budget -= _nb; pops += _nb;')
        assert 'no separate prefetch sweep' in violations(split)
        print('ok red: separate prefetch sweep')
        # 对象处理体抄回扫描循环里（宏只剩一处调用点）⇒ 两份体迟早漂
        dup = text.replace('        for (size_t _b = 0; _b < _nb; _b++) { EXTC_RC_MARK_SCAN(_batch[_b]); }',
                           '        for (size_t _b = 0; _b < _nb; _b++) {\n            uintptr_t _raw = _batch[_b];\n        }')
        assert 'scan body not inlined twice' in violations(dup)
        print('ok red: batch body inlined again')
        return 0
    bad = violations(text)
    for item in bad:
        print('FAIL:', item)
    if not bad:
        print('ok batched mark walk structure')
    return bool(bad)


if __name__ == '__main__':
    raise SystemExit(main())
