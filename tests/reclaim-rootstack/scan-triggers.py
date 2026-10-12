#!/usr/bin/env python3
"""全量语料上的「清扫触发点」普查：修前(pre) vs 修后(post)。

用途（对应 RECON.md 的问题 3）：19fd567 的 `rcFrameOwnsArena` 让「不是本帧拥有的区域」
一个触发都不发。本脚本逐个 `.extc` 编译两遍（修前的 `extc` 与修后的 `extc`），

  * 数每个程序里 `extc_rc_maybeCollect` 的**触发点**个数（运行期定义那一行不算）；
  * 对"修前有触发、修后一个都不剩"的程序，把修前每个触发点按
    「区域表达式 + 所在函数是不是 main」分类。

触发点的字面形状（codegen.c `rcEmitTrigger`）：
    { ExtcRcRegion *__extc_rcgN = extc_rc_regionOf(<区域表达式>, <lvl>);
      if (extc_rc_shouldSweep(__extc_rcgN)) extc_rc_maybeCollect(__extc_rcgN, <根表>, <n>); }

用法：
    tests/reclaim-rootstack/scan-triggers.py --pre <修前 extc> --post <修后 extc> \
        --root <repo> [--out <csv>] [--limit N]

输出：stdout 是汇总表；`--out` 写逐程序 CSV（程序、pre 数、post 数、分类计数）。
只读既有文件、只写 /tmp 与 --out。
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

RE_DEF = re.compile(r'^[A-Za-z_][\w \t\*]*\s\*?([A-Za-z_]\w*)\s*\([^;{]*\)\s*\{\s*$')
RE_SITE = re.compile(r'extc_rc_maybeCollect\(__extc_rcg(\d+),')
RE_REGION = re.compile(r'ExtcRcRegion \*__extc_rcg\d+ = extc_rc_regionOf\((.+?), (-?\d+)\);')


def arena_kind(expr, lvl):
    """把触发点的区域表达式归到一档。"""
    if 'extc_tls_arena' in expr:
        return 'tls'
    if '->home' in expr:
        return 'owcell'
    if '__extc_home' in expr:
        return 'home'
    if '__extc_a[' in expr:
        return 'block'
    if '->arena' in expr:
        return 'coro_frame'
    return 'other:' + expr


def enclosing_func(lines, idx):
    """idx 之前最后一条 col-0 的函数定义头；返回 (名字, 是否 main)。"""
    for j in range(idx, -1, -1):
        m = RE_DEF.match(lines[j])
        if m:
            name = m.group(1)
            # main 的判据：函数体内有 codegen 发的家 arena 自指声明。
            is_main = name in ('main',)
            return name, is_main
    return None, False


def scan_c(text):
    """返回每个触发点的 (arena_kind, lvl, 函数名, 是否 main, 是否自递归)。"""
    lines = text.split('\n')
    sites = []
    # 自递归：函数体（到下一个 col-0 定义头为止）里出现自己的名字 + `(`
    starts = [i for i in range(len(lines)) if RE_DEF.match(lines[i])]
    rec = set()
    for a, b in zip(starts, starts[1:] + [len(lines)]):
        nm = RE_DEF.match(lines[a]).group(1)
        for ln in lines[a + 1:b]:
            if re.search(r'\b' + re.escape(nm) + r'\s*\(', ln):
                rec.add(nm)
                break
    for i, ln in enumerate(lines):
        if not RE_SITE.search(ln):
            continue
        m = RE_REGION.search(ln)
        if not m:
            sites.append(('unparsed', 0, None, False, False))
            continue
        expr, lvl = m.group(1), int(m.group(2))
        fn, is_main = enclosing_func(lines, i)
        sites.append((arena_kind(expr, lvl), lvl, fn, is_main, fn in rec))
    return sites


def compile_one(extc, root, src, out):
    try:
        p = subprocess.run(
            [extc, '-I', os.path.join(root, 'stdlib'), src, '-o', out],
            env={**os.environ, 'EXTC_RECLAIM': '1'},
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)
    except subprocess.TimeoutExpired:
        return False
    return p.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pre', required=True)
    ap.add_argument('--post', required=True)
    ap.add_argument('--root', required=True)
    ap.add_argument('--out', default=None)
    ap.add_argument('--sites', default=None, help='逐触发点明细 CSV（只记被移除的程序）')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--dirs', default='tests,bench,examples,stdlib')
    a = ap.parse_args()

    srcs = []
    for d in a.dirs.split(','):
        base = os.path.join(a.root, d)
        for dp, _, fs in os.walk(base):
            for f in sorted(fs):
                if f.endswith('.extc'):
                    srcs.append(os.path.join(dp, f))
    srcs.sort()
    if a.limit:
        srcs = srcs[:a.limit]

    tmp = tempfile.mkdtemp(prefix='rs-scan-')
    rows = []
    stat = {'prog': 0, 'both_ok': 0, 'pre_trig_prog': 0, 'removed_prog': 0,
            'partial_prog': 0, 'pre_sites': 0, 'post_sites': 0, 'removed_sites': 0}
    cls = {}
    site_rows = []
    for k, src in enumerate(srcs):
        rel = os.path.relpath(src, a.root)
        pre_c = os.path.join(tmp, 'pre.c')
        post_c = os.path.join(tmp, 'post.c')
        stat['prog'] += 1
        if not compile_one(a.pre, a.root, src, pre_c):
            continue
        if not compile_one(a.post, a.root, src, post_c):
            continue
        stat['both_ok'] += 1
        pre = scan_c(open(pre_c, encoding='utf-8', errors='replace').read())
        post = scan_c(open(post_c, encoding='utf-8', errors='replace').read())
        stat['pre_sites'] += len(pre)
        stat['post_sites'] += len(post)
        if pre:
            stat['pre_trig_prog'] += 1
        if pre and not post:
            stat['removed_prog'] += 1
            stat['removed_sites'] += len(pre)
            for kind, lvl, fn, is_main, is_rec in pre:
                bucket = 'main' if is_main else ('recursive' if is_rec else 'nonmain')
                key = (kind, bucket)
                cls[key] = cls.get(key, 0) + 1
                site_rows.append((rel, kind, lvl, fn, is_main, is_rec))
        elif pre and post and len(post) < len(pre):
            stat['partial_prog'] += 1
            stat['removed_sites'] += len(pre) - len(post)
        rows.append((rel, len(pre), len(post)))
        if (k + 1) % 200 == 0:
            print(f'  ... {k+1}/{len(srcs)}', file=sys.stderr)

    print('== 触发点普查（EXTC_RECLAIM=1）==')
    print(f'语料目录       {a.dirs}')
    print(f'文件数         {stat["prog"]}')
    print(f'两版都编过     {stat["both_ok"]}')
    print(f'修前有触发的程序 {stat["pre_trig_prog"]}')
    print(f'触发全被移除的程序 {stat["removed_prog"]}（含触发点 {stat["removed_sites"]} 个）')
    print(f'部分移除的程序  {stat["partial_prog"]}')
    print(f'触发点总数     修前 {stat["pre_sites"]} → 修后 {stat["post_sites"]}')
    print('-- 被移除的触发点分类（修前形状 × 所在函数）--')
    for key in sorted(cls):
        print(f'   {key[0]:12s} {key[1]:10s} {cls[key]}')
    if a.out:
        with open(a.out, 'w') as fh:
            fh.write('program,pre,post\n')
            for rel, p, q in rows:
                fh.write(f'{rel},{p},{q}\n')
        print(f'逐程序 CSV → {a.out}')
    if a.sites:
        with open(a.sites, 'w') as fh:
            fh.write('program,arena_kind,level,function,is_main,is_recursive\n')
            for r in site_rows:
                fh.write(','.join(str(x) for x in r).replace('True', 'yes').replace('False', 'no') + '\n')
        print(f'逐触发点 CSV → {a.sites}')
    subprocess.run(['rm', '-rf', tmp])


if __name__ == '__main__':
    main()
