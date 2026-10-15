#!/usr/bin/env python3
"""Structural smoke check for the mark-both barrier split, with mutations.

买的是**本地槽屏障的省法**：无计数档把「每次本地槽写都着色」换成「每次堆边删除给旧值着色」。
没有删除屏障，这一省就不健全（从还没扫到的对象上读到白对象、那条边随后在堆上被删掉、
白对象只剩本地槽这一个来源 ⇒ 标记完成时会被判死）；没有这一省，P6 的几亿次本地槽着色省不掉。

判据都在 src/back/reclaim.c 的运行期文本 + 发射器文本里：
  S1 删除屏障 extc_rc_shadeOld 存在，且只在无计数档动作（计数档 no-op，不碰老路径）
  S2 删除屏障先看不飞标记：wb == 0 时连 *slot 都不读
  S3 四个堆写入口都调删除屏障（storeRef / SameRegion / OwnedReg / Owned）
  S4 initRef / initRefNoHold 的着色被 #if !EXTC_RC_NOCOUNT 把门
  S5 storeRefLocal 不再着色（它只写本地槽，保护由删除屏障承担）
每条都能红：--selftest 逐 token 注入缺口，确认 violations 真的变红。
"""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEL_SITES = 4


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
    checks['old barrier defined'] = 'static inline void extc_rc_shadeOld(void *const *slot)' in text
    old = body(text, 'extc_rc_shadeOld(void *const *slot)')
    checks['old barrier no-count only'] = ('#if EXTC_RC_NOCOUNT' in old and '#else' in old
                                           and '(void)slot;' in old)
    checks['old barrier off-path'] = 'if (!extc_rc_wb || !slot) return;' in old
    checks['old barrier shades'] = 'extc_rc_shade(*slot);' in old
    checks['old barrier call sites'] = text.count('extc_rc_shadeOld(slot);') == DEL_SITES
    ir = body(text, 'extc_rc_initRef(void **slot, void *v)')
    checks['local init guarded'] = ('#if !EXTC_RC_NOCOUNT' in ir and 'extc_rc_shade(v);' in ir
                                    and '#endif' in ir)
    nh = body(text, 'extc_rc_initRefNoHold(void **slot, void *v)')
    checks['local nohold guarded'] = ('#if !EXTC_RC_NOCOUNT' in nh and 'extc_rc_shade(v);' in nh
                                      and '#endif' in nh)
    lc = body(text, 'extc_rc_storeRefLocal(void **slot, void *v)')
    checks['local store unbarriered'] = bool(lc) and 'extc_rc_shade' not in lc and '*slot = v;' in lc
    return [k for k, ok in checks.items() if not ok]


INJECTIONS = [
    ('static inline void extc_rc_shadeOld(void *const *slot)', 'first'),
    ('if (!extc_rc_wb || !slot) return;', 'first'),
    ('extc_rc_shade(*slot);', 'first'),
    ('(void)slot;', 'first'),
    ('extc_rc_shadeOld(slot);', 'all'),
    ('#if !EXTC_RC_NOCOUNT', 'all'),
    ('extc_rc_storeRefLocal(void **slot, void *v) {', 'add-shade'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    text = source_text()
    if args.selftest:
        assert not violations(text), violations(text)
        for token, mode in INJECTIONS:
            assert token in text, token
            if mode == 'add-shade':
                mutated = text.replace(token, token + ' extc_rc_shade(v);', 1)
            elif mode == 'all':
                mutated = text.replace(token, '/* injected */')
            else:
                mutated = text.replace(token, '/* injected */', 1)
            assert violations(mutated), token
            print('ok red:', token)
        return 0
    bad = violations(text)
    for item in bad:
        print('FAIL:', item)
    if not bad:
        print('ok mark-both barrier structure')
    return bool(bad)


if __name__ == '__main__':
    raise SystemExit(main())
