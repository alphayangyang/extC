#!/usr/bin/env python3
"""Summarise one callgrind profile (and optionally a no-GC reference profile).

Events with --cache-sim=yes: Ir Dr Dw I1mr D1mr D1mw ILmr DLmr DLmw.
Everything here is *deterministic instruction / cache-simulation* data: no timing.
Wall-clock and GC-time columns are passed in via --wall-ms / --gc-ms and are
labelled as "reported" values, never measured here (see docs/analysis/ALLOC-COST.md §1).

Usage:
  parse_callgrind.py PROFILE [--off PROFILE] [--allocs N] [--steps N]
                              [--wall-ms X] [--gc-ms X] [--json]
  parse_callgrind.py --annot FILE ...      # parse a cached 'callgrind_annotate' output

A cached annotation can be produced with:
  callgrind_annotate --auto=no --show-percs=no --threshold=99.99 PROFILE
"""
import argparse, json, subprocess, sys

EVENTS = ('Ir', 'Dr', 'Dw', 'I1mr', 'D1mr', 'D1mw', 'ILmr', 'DLmr', 'DLmw')


def _num(t):
    return t == '.' or t.replace(',', '').isdigit()


def parse_annot(text):
    """-> (totals, [(function_name, [9 event ints])]) sorted by Ir desc."""
    totals, rows = None, []
    for ln in text.splitlines():
        toks = ln.strip().split()
        if not toks:
            continue
        if toks[-2:] == ['PROGRAM', 'TOTALS']:
            nums = [t.replace(',', '') for t in toks[:-2]]
            if len(nums) == len(EVENTS) and all(_num(t) for t in nums):
                totals = [int(t) if t != '.' else 0 for t in nums]
            continue
        if totals is None or len(toks) < len(EVENTS) + 1:
            continue
        nums = [t.replace(',', '') for t in toks[:len(EVENTS)]]
        if not all(_num(t) for t in nums):
            continue
        rows.append((' '.join(toks[len(EVENTS):]),
                     [int(t) if t != '.' else 0 for t in nums]))
    rows.sort(key=lambda r: -r[1][0])
    return totals, rows


def annotate(profile):
    return subprocess.run(['callgrind_annotate', '--auto=no', '--show-percs=no',
                           '--threshold=99.99', profile],
                          capture_output=True, text=True, check=True).stdout


def find(rows, *patterns):
    for pat in patterns:
        for name, v in rows:
            if pat in name:
                return name, v
    return None, None


def disp(name):
    """'path/file.c:fn [/path/bin]' -> 'fn  (file.c)' for the printed table."""
    n = name.split(' [')[0]
    if ':' in n:
        f, fn = n.rsplit(':', 1)
        return '%s  (%s)' % (fn, f.rsplit('/', 1)[-1])
    return n


def gc_totals(rows):
    """Self cost of every runtime extc_rc_* function (they are not inlined here)."""
    ir = d1 = dll = 0
    for name, v in rows:
        if 'extc_rc_' in name:
            ir += v[0]; d1 += v[4]; dll += v[7]
    return ir, d1, dll


def summarize(profile, off=None):
    totals, rows = parse_annot(annotate(profile))
    if totals is None:
        sys.exit('no PROGRAM TOTALS in %s' % profile)
    out = {'profile': profile, 'totals': dict(zip(EVENTS, totals))}
    fns = {}
    for key, pats in (('main', (':main',)), ('markStep', ('extc_rc_markStep',)),
                      ('sweepStep', ('extc_rc_sweepStep',)),
                      ('shadeSlow', ('extc_rc_shadeSlow',)),
                      ('ownSlab', ('extc_rc_ownSlab',)),
                      ('memset', ('memset',))):
        name, v = find(rows, *pats)
        fns[key] = v if v else [0] * len(EVENTS)
        fns[key + '_name'] = name
    out['functions'] = fns
    ir = totals[0]
    out['gc'] = dict(zip(('ir', 'd1mr', 'dlmr'), gc_totals(rows)))
    out['gc_ir_share'] = out['gc']['ir'] / ir
    out['main_ir_share'] = fns['main'][0] / ir
    if off:
        otot, orows = parse_annot(annotate(off))
        if otot is None:
            sys.exit('no PROGRAM TOTALS in %s' % off)
        oname, ov = find(orows, ':main')
        aname, av = find(orows, 'extc_arena_alloc')
        out['off'] = {
            'profile': off,
            'totals': dict(zip(EVENTS, otot)),
            'main_ir': ov[0] if ov else 0,
            'arena_ir': av[0] if av else 0,
        }
    out['top'] = [{'function': n, 'ir': v[0], 'd1mr': v[4], 'dlmr': v[7]}
                  for n, v in rows[:12]]
    return out


def derive(res, allocs=None, steps=None, wall_ms=None, gc_ms=None):
    t = res['totals']
    d = {'ir_per_alloc': t['Ir'] / allocs if allocs else None,
         'd1mr_per_alloc': t['D1mr'] / allocs if allocs else None,
         'dlmr_per_alloc': t['DLmr'] / allocs if allocs else None,
         'gc_ir_per_alloc': res['gc']['ir'] / allocs if allocs else None,
         'main_ir_per_alloc': res['functions']['main'][0] / allocs if allocs else None}
    if 'off' in res and allocs:
        d['off_ir_per_alloc'] = res['off']['totals']['Ir'] / allocs
        d['off_main_ir_per_alloc'] = res['off']['main_ir'] / allocs
        d['off_arena_ir_per_alloc'] = res['off']['arena_ir'] / allocs
        d['inline_rc_ir_per_alloc'] = (d['main_ir_per_alloc']
                                       - d['off_main_ir_per_alloc'])
    if steps:
        d['ir_per_step'] = t['Ir'] / steps
        d['d1mr_per_step'] = t['D1mr'] / steps
        d['dlmr_per_step'] = t['DLmr'] / steps
        d['main_ir_per_step'] = res['functions']['main'][0] / steps
        d['gc_ir_per_step'] = res['gc']['ir'] / steps
        d['gc_dlmr_per_step'] = res['gc']['dlmr'] / steps
        if 'off' in res:
            d['off_main_ir_per_step'] = res['off']['main_ir'] / steps
    if wall_ms and allocs:
        d['ns_per_alloc_reported'] = wall_ms * 1e6 / allocs
    if gc_ms and allocs:
        d['gc_ns_per_alloc_reported'] = gc_ms * 1e6 / allocs
    if wall_ms and steps:
        d['ns_per_step_reported'] = wall_ms * 1e6 / steps
    return {k: v for k, v in d.items() if v is not None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile', nargs='?')
    ap.add_argument('--off')
    ap.add_argument('--allocs', type=int)
    ap.add_argument('--steps', type=int)
    ap.add_argument('--wall-ms', type=float)
    ap.add_argument('--gc-ms', type=float)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    if not a.profile:
        ap.error('PROFILE is required')
    res = summarize(a.profile, a.off)
    res['derived'] = derive(res, a.allocs, a.steps, a.wall_ms, a.gc_ms)
    if a.json:
        print(json.dumps(res, indent=1))
        return
    t, g = res['totals'], res['gc']
    print('%s  Ir=%d  D1mr=%d  DLmr=%d' % (a.profile, t['Ir'], t['D1mr'], t['DLmr']))
    print('  GC(extc_rc_*): Ir=%.1f%%  D1mr=%.1f%%  DLmr=%.1f%%'
          % (100 * g['ir'] / t['Ir'], 100 * g['d1mr'] / t['D1mr'],
             100 * g['dlmr'] / t['DLmr']))
    print('  main self: %.1f%% of Ir' % (100 * res['main_ir_share']))
    print('  %-44s %7s %9s %10s %9s' % ('function', 'share', 'Ir', 'D1mr', 'DLmr'))
    for f in res['top']:
        print('  %-44s %6.2f%% %9d %10d %9d'
              % (disp(f['function']), 100 * f['ir'] / t['Ir'], f['ir'],
                 f['d1mr'], f['dlmr']))
    if 'off' in res:
        o = res['off']
        print('  off(no-GC) Ir=%d  (on/off=%.2fx)'
              % (o['totals']['Ir'], t['Ir'] / o['totals']['Ir']))
    for k, v in res['derived'].items():
        print('  %-28s %.4g' % (k, v))


if __name__ == '__main__':
    main()
