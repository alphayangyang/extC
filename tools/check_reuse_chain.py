#!/usr/bin/env python3
"""Structural check for where the reuse chain stores its links, with mutations.

The reuse chain's "next slot" lives in the per-slot `tid[]` array (encoded as slot + 1,
0 = chain end) rather than in the slot payload. Both encodings are semantically identical,
so no behavioural test can notice the payload write creeping back -- and with it one cold
last-level write per freed slot (cachegrind, P5 N=2M: sweepStep DLmw 979,055 -> 2,326,
last-level misses 4,614,274 -> 3,545,405). This gate pins the storage decision:
  * freeSlot pushes onto the chain through tid[] and never touches the payload,
  * the alloc-time pop decodes tid[] (and freeSlot does not zero the link),
  * the uint16 encoding bound is declared and guarded at slab construction.
"""
import argparse
import ast
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]

LINK = 's->tid[i] = (uint16_t)(s->freelist + 1);'
POP = 's->freelist = (int64_t)s->tid[i] - 1;'
PAYLOAD_LINK = '*(int64_t *)p = s->freelist;'
TID_ZERO = 's->tid[i] = 0;'
LIMIT = '#define EXTC_RC_REUSE_MAX_SLOT 0xFFFEu'
GUARD = 'if (s->nslots > EXTC_RC_REUSE_MAX_SLOT)'
FREESLOT = 'static void extc_rc_freeSlot(ExtcRcSlab *s, size_t i) {'
FREESLOT_LINK = 'static void extc_rc_freeSlotLink(ExtcRcSlab *s, size_t i) {'
FREESLOT_WORD = 'static size_t extc_rc_freeSlotWord(ExtcRcSlab *s, size_t w, uint64_t dead) {'
DELEGATE = 'extc_rc_freeSlotLink(s, i);'


def emitter_text():
    source = (ROOT / 'src/back/reclaim.c').read_text()
    start = source.index('RECLAIM_RT[]')
    end = source.index('\n};', start)
    return ''.join(ast.literal_eval(m.group()) for m in re.finditer(r'"(?:[^"\\]|\\.)*"', source[start:end]))


def body_of(text, sig):
    start = text.find(sig)
    if start < 0:
        return None
    a = text.index('{', start)
    depth, b = 1, a + 1
    while b < len(text) and depth:
        depth += (text[b] == '{') - (text[b] == '}')
        b += 1
    return text[a:b]


def violations(text):
    out = []
    fs = body_of(text, FREESLOT)
    fsl = body_of(text, FREESLOT_LINK)
    fsw = body_of(text, FREESLOT_WORD)
    if fs is None or fsl is None or fsw is None:
        return ['freeSlot/freeSlotLink/freeSlotWord missing']
    if LINK not in fsl:
        out.append('freeSlotLink does not push onto the tid[] chain')
    if LINK not in fsw:
        out.append('freeSlotWord does not push onto the tid[] chain')
    if DELEGATE not in fs:
        out.append('freeSlot does not delegate the chain push')
    if 'extc_rc_unlinkChildren(s, ' not in fsw:
        out.append('freeSlotWord does not unlink children of the freed slots')
    for name, body in (('freeSlot', fs), ('freeSlotLink', fsl), ('freeSlotWord', fsw)):
        if PAYLOAD_LINK in body:
            out.append(name + ' writes the chain link into the slot payload')
        if TID_ZERO in body:
            out.append(name + ' zeroes tid[] (that would erase the chain link)')
        if 'EXTC_RC_SLAB_DATA(s)' in body:
            out.append(name + ' still derives the slot payload address')
    if POP not in text:
        out.append('alloc does not decode the tid[] chain')
    if LIMIT not in text:
        out.append('reuse-chain encoding bound not declared')
    if GUARD not in text:
        out.append('slabNew does not guard the slot count against the encoding bound')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    text = emitter_text()
    if args.selftest:
        assert not violations(text), violations(text)
        mutations = {
            'no push': text.replace(LINK, '/* injected */'),
            'no decode': text.replace(POP, '/* injected */'),
            'no bound': text.replace(LIMIT, '/* injected */'),
            'no guard': text.replace(GUARD, '/* injected */'),
            'payload link': text.replace(LINK, LINK + '\n    ' + PAYLOAD_LINK),
            'tid zeroed': text.replace(LINK, LINK + '\n    ' + TID_ZERO),
            'no delegate': text.replace(DELEGATE, '/* injected */'),
            'word does not unlink': text.replace('extc_rc_unlinkChildren(s, base + b);', '/* injected */'),
            'word skips chain': text.replace(LINK, LINK + '\n    ' + TID_ZERO),
        }
        for name, mutated in mutations.items():
            assert mutated != text, name
            bad = violations(mutated)
            assert bad, name
            print('ok red:', name, '=>', bad[0])
        return 0
    bad = violations(text)
    for item in bad:
        print('FAIL:', item)
    if not bad:
        print('ok reuse chain lives in tid')
    return bool(bad)


if __name__ == '__main__':
    raise SystemExit(main())
