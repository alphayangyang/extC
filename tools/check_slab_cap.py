#!/usr/bin/env python3
"""Structural smoke check for the per-size-class slab capacity, with mutations.

买的是**峰值 RSS**：一块 slab 发出去的容量（cap × 槽大小）按槽大小定，而不是一律发满
1 MiB —— 固定 1 MiB 时每个尺寸类最多白留 1 MiB（P6 上 cap 8.4 MB / 峰值占用 5.2 MB），
而 slab 一旦开出来只增不减（唯一的块级释放是区域整个归还），白留的容量就一直是峰值 RSS 的
一部分。容量变细之后清扫更早更频繁（P5 collects 40→58、峰值占用 62→33 MB），代价是那几档
的墙钟（P5 +4%）。

前提没动，也必须在判据里钉住：对齐仍是 EXTC_RC_SLAB_SIZE（1 MiB），因为"由对象指针找 slab"
是一次按位与；分配仍是一次 aligned_alloc(1 MiB, 1 MiB)，改的只是发出去几个槽。

  S1 两个常量在：EXTC_RC_SLAB_CHUNK_MIN = 64 KiB、EXTC_RC_SLAB_SLOTS_MIN = 1024
  S2 块大小从 CHUNK_MIN 起按 2 的幂向上取、顶到 EXTC_RC_SLAB_SIZE 为止，且至少装 SLOTS_MIN 个槽
  S3 cap 由 chunk 推出；固定 1 MiB 的旧公式不得残留
  S4 超大块判定仍由 cap 推（int big = (cap < 2)），没被"块大小固定"这条假设带偏
  S5 对齐前提：掩码仍按 EXTC_RC_SLAB_SIZE、分配仍是同尺寸的 aligned_alloc
  S6 容量记账按**已发放的槽**（每槽 ss），不是按块大小
  S7 文档注释与常量同步（slabNew 的注释里提到 CHUNK_MIN）
  S8 块大小被收回块大小上限以内 —— CHUNK_MIN 是**配置无关**的 64 KiB，而
     EXTC_RC_SLAB_BITS 可配置（< 16 时块 < 64 KiB）；不夹这一下，chunk 会大于
     EXTC_RC_SLAB_SIZE，cap 按块大小算就会写出界（ASan 实测 16 B 写越界）
每条都能红：--selftest 逐 token 注入缺口，确认 violations 真的变红。
"""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHUNK_DEF = '#define EXTC_RC_SLAB_CHUNK_MIN ((size_t)64 * 1024)'
SLOTS_DEF = '#define EXTC_RC_SLAB_SLOTS_MIN ((size_t)1024)'
OLD_CAP = 'size_t cap = (EXTC_RC_SLAB_SIZE - EXTC_RC_SLAB_HDR) / slotSize;'
NEW_CAP = 'size_t cap = (chunk - EXTC_RC_SLAB_HDR) / slotSize;'
LOOP = 'while (chunk < EXTC_RC_SLAB_SIZE && chunk / slotSize < EXTC_RC_SLAB_SLOTS_MIN)'
ALLOC = 'aligned_alloc(EXTC_RC_SLAB_SIZE, EXTC_RC_SLAB_SIZE)'
MASK = '(uintptr_t)p & ~(uintptr_t)(EXTC_RC_SLAB_SIZE - 1)'
CLAMP = 'if (chunk > EXTC_RC_SLAB_SIZE) chunk = EXTC_RC_SLAB_SIZE;'


def source_text():
    return (ROOT / 'src/back/reclaim.c').read_text()


def body(text, sig):
    start = text.find(sig)
    if start < 0:
        return ''
    a = text.index('{', start)
    depth, b = 1, a + 1
    while b < len(text) and depth:
        depth += (text[b] == '{') - (text[b] == '}')
        b += 1
    return text[a:b]


def violations(text):
    checks = {}
    checks['chunk min defined'] = CHUNK_DEF in text
    checks['slots min defined'] = SLOTS_DEF in text
    sn = body(text, 'extc_rc_slabNew(ExtcRcRegion *r, size_t slotSize)')
    checks['chunk starts at min'] = 'size_t chunk = EXTC_RC_SLAB_CHUNK_MIN;' in sn
    checks['chunk doubles while small'] = LOOP in sn and 'chunk <<= 1;' in sn
    checks['cap from chunk'] = NEW_CAP in sn
    checks['chunk clamped to slab size'] = CLAMP in sn
    checks['fixed 1 MiB cap gone'] = OLD_CAP not in text
    checks['big by cap'] = 'int big = (cap < 2);' in sn
    checks['alignment mask kept'] = MASK in text
    checks['aligned allocation kept'] = ALLOC in text
    checks['capacity counts emitted slots'] = 'r->bytes += (int64_t)ss;' in text
    doc = text[:text.find('extc_rc_slabNew(ExtcRcRegion *r, size_t slotSize)')]
    checks['doc mentions chunk rule'] = 'EXTC_RC_SLAB_CHUNK_MIN' in doc and 'EXTC_RC_SLAB_SLOTS_MIN' in doc
    return [k for k, ok in checks.items() if not ok]


INJECTIONS = [
    (CHUNK_DEF, 'drop-literal'),
    (SLOTS_DEF, 'drop-literal'),
    (LOOP, 'loop-drop-slots'),
    (NEW_CAP, 'restore-old-cap'),
    ('int big = (cap < 2);', 'drop-literal'),
    (MASK, 'drop-literal'),
    (ALLOC, 'drop-literal'),
    ('r->bytes += (int64_t)ss;', 'drop-literal'),
    (CLAMP, 'drop-literal'),
    ('EXTC_RC_SLAB_SLOTS_MIN', 'drop-in-doc'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    text = source_text()
    if args.selftest:
        base = violations(text)
        assert not base, base
        for token, mode in INJECTIONS:
            assert token in text, token
            if mode == 'drop-literal':
                mutated = text.replace(token, '/* injected */')
            elif mode == 'loop-drop-slots':
                mutated = text.replace(token, 'while (chunk < EXTC_RC_SLAB_SIZE)', 1)
            elif mode == 'restore-old-cap':
                mutated = text.replace(token, OLD_CAP, 1)
            elif mode == 'drop-in-doc':
                cut = text.find('extc_rc_slabNew(ExtcRcRegion *r, size_t slotSize)')
                mutated = text[:cut].replace(token, '/* injected */') + text[cut:]
            assert violations(mutated), token
            print('ok red:', token)
        return 0
    bad = violations(text)
    for item in bad:
        print('FAIL:', item)
    if not bad:
        print('ok slab cap structure')
    return bool(bad)


if __name__ == '__main__':
    raise SystemExit(main())
