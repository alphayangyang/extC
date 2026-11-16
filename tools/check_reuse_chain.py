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

P1 split the free path in two modes (both still keep the link in tid[]):
  * default (EXTC_RC_LAZY_FREE=0): freeSlotWord links every dead slot as it
    sweeps, through tid[] (slot index `base + b`),
  * lazy reuse (EXTC_RC_LAZY_FREE=1): the sweep only counts the holes
    (`s->nfree += n`), and extc_rc_alloc rebuilds the chain from the occupancy
    bitmap through tid[] when it next needs a slot in that block,
so the payload-storage invariant is pinned on both modes.
"""
import argparse
import ast
from pathlib import Path
import re
import reclaim_source

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
WORD_LINK = 's->tid[idx] = (uint16_t)(s->freelist + 1);'
WORD_POP = 's->freelist = (int64_t)idx;'
LAZY_HOLES = 's->nfree += (uint32_t)n;'
ALLOC = 'static EXTC_RC_ALLOC_DECL void *extc_rc_alloc('
LAZY_IF = '#if EXTC_RC_LAZY_FREE'


def emitter_text():
    source = reclaim_source.text()
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


def lazy_variant(text, lazy):
    """Resolve every `#if EXTC_RC_LAZY_FREE` down to the branch a real build
    compiles: lazy=True keeps the #if branch, lazy=False keeps the #else branch
    (nothing when there is no #else).  The preprocessor text carries both
    branches; the alloc body must be inspected one branch at a time, or its
    raw brace count runs past the function."""
    lines = text.splitlines(True)
    out, i = [], 0
    while i < len(lines):
        if lines[i].strip().startswith(LAZY_IF):
            depth, j, spans, start = 1, i + 1, [], i + 1
            while j < len(lines):
                s = lines[j].strip()
                if s.startswith('#if'):
                    depth += 1
                elif s.startswith('#endif'):
                    depth -= 1
                    if depth == 0:
                        spans.append((start, j))
                        break
                elif depth == 1 and (s.startswith('#else') or s.startswith('#elif')):
                    spans.append((start, j))
                    start = j + 1
                j += 1
            if not spans:
                raise AssertionError('unterminated ' + LAZY_IF)
            if lazy:
                a, b = spans[0]
            else:
                a, b = spans[-1] if len(spans) > 1 else (0, 0)
            out.append(lazy_variant(''.join(lines[a:b]), lazy))
            i = j + 1
            continue
        out.append(lines[i])
        i += 1
    return ''.join(out)


def violations(text):
    out = []
    fs = body_of(text, FREESLOT)
    fsl = body_of(text, FREESLOT_LINK)
    fsw = body_of(text, FREESLOT_WORD)
    alloc_alloc = body_of(lazy_variant(text, False), ALLOC)
    alloc_lazy = body_of(lazy_variant(text, True), ALLOC)
    if fs is None or fsl is None or fsw is None or alloc_alloc is None or alloc_lazy is None:
        return ['freeSlot/freeSlotLink/freeSlotWord/alloc missing']
    if LINK not in fsl:
        out.append('freeSlotLink does not push onto the tid[] chain')
    # 默认档：扫描时逐个把空洞挂到 tid[]（idx = base + b）。
    if WORD_LINK not in fsw:
        out.append('freeSlotWord does not push onto the tid[] chain')
    # 懒复用档：扫描只数空洞，不挂链。
    if LAZY_HOLES not in fsw:
        out.append('freeSlotWord lazy-reuse branch does not count the holes')
    # 懒复用档的链由 alloc 从占用位图重建，也必须写 tid[]。
    if WORD_LINK not in alloc_lazy or WORD_POP not in alloc_lazy:
        out.append('alloc does not rebuild the lazy-reuse chain through tid[]')
    # 默认档的分配弹出仍要解码 tid[]（值存 tid，载荷不动）。
    if POP not in alloc_alloc:
        out.append('default-build alloc does not decode the tid[] chain')
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
            # 默认档 freeSlotWord 跳过挂链（链只能靠载荷或干脆没有）
            'word skips chain': text.replace(WORD_LINK, '/* injected */'),
            # 懒复用档 freeSlotWord 不数空洞（链会漏挂，alloc 重建时槽被当满）
            'word lazy does not count holes': text.replace(LAZY_HOLES, '/* injected */'),
            # 懒复用档 alloc 重建链时不再写 tid[]（退回载荷/无链）
            'alloc rebuild loses tid link': text.replace(WORD_POP, '/* injected */'),
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
