#!/usr/bin/env python3
"""对「修前有触发、修后触发被全部移除」的 110 个程序，量**开档峰值 RSS 的回归**。

对应 RECON.md 的问题 3：触发被 `rcFrameOwnsArena` 移除之后，哪些程序的可测峰值真的变差了。

做法（每个程序）：
  1. 修前的 `extc` 与修后的 `extc` 各生成一份 C（`EXTC_RECLAIM=1`）；
  2. 同一把尺子编译：`cc -std=c11 -fwrapv -O1`；
  3. `/usr/bin/time -v` 跑一遍，取 `Maximum resident set size`（KB）；
  4. 有参数需求的形状（`bench/*`）用 `--arg` 显式给（默认不给，退回 0 参数）。
带 `--timeout`（默认 20s）防止个别重形状挂住；rc!=0 的对照记下来但不计入 Δ。

用法：
  tests/reclaim-rootstack/peak-regression.py --pre <修前 extc> --post <修后 extc> \
      --root <repo> --list <逐程序 CSV（scan-triggers.py --out）> [--out out.csv]
只读仓库，只写 /tmp 与 --out。
"""
import argparse
import csv
import os
import re
import subprocess
import sys
import tempfile

RSS_RE = re.compile(r'Maximum resident set size \(kbytes\): (\d+)')


def sh(cmd, timeout=120, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        class R:
            returncode = 124
            stdout = stderr = ''
        return R()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pre', required=True)
    ap.add_argument('--post', required=True)
    ap.add_argument('--root', required=True)
    ap.add_argument('--list', required=True)
    ap.add_argument('--out', default=None)
    ap.add_argument('--timeout', type=float, default=20.0)
    ap.add_argument('--cc', default='cc')
    a = ap.parse_args()

    progs = []
    with open(a.list) as fh:
        for row in csv.DictReader(fh):
            if int(row['pre']) > 0 and int(row['post']) == 0:
                progs.append(row['program'])

    tmp = tempfile.mkdtemp(prefix='rs-peak-')
    out_rows = []
    print(f'{"程序":52s} {"修前 MB":>9s} {"修后 MB":>9s} {"Δ MB":>8s}  rc')
    for rel in progs:
        src = os.path.join(a.root, rel)
        res = {}
        for tag, extc in (('pre', a.pre), ('post', a.post)):
            c = os.path.join(tmp, tag + '.c')
            b = os.path.join(tmp, tag)
            g = sh([extc, '-I', os.path.join(a.root, 'stdlib'), src, '-o', c], timeout=60,
                   env={**os.environ, 'EXTC_RECLAIM': '1'})
            if g.returncode != 0:
                res[tag] = None
                continue
            cc = sh([a.cc, '-std=c11', '-fwrapv', '-O1', c, '-o', b], timeout=120)
            if cc.returncode != 0:
                res[tag] = None
                continue
            r = sh(['/usr/bin/time', '-v', b], timeout=a.timeout)
            m = RSS_RE.search(r.stderr or '')
            res[tag] = (int(m.group(1)) if m else None, r.returncode)
        pre, post = res.get('pre'), res.get('post')
        pre_mb = pre[0] / 1024.0 if pre and pre[0] is not None else None
        post_mb = post[0] / 1024.0 if post and post[0] is not None else None
        rc = f'{pre[1] if pre else "-"}/{post[1] if post else "-"}'
        delta = (post_mb - pre_mb) if (pre_mb is not None and post_mb is not None) else None
        print(f'{rel:52s} {pre_mb if pre_mb is None else f"{pre_mb:9.2f}"} '
              f'{post_mb if post_mb is None else f"{post_mb:9.2f}"} '
              f'{delta if delta is None else f"{delta:8.2f}"}  {rc}')
        out_rows.append((rel, pre_mb, post_mb, delta, rc))

    if a.out:
        with open(a.out, 'w') as fh:
            fh.write('program,pre_mb,post_mb,delta_mb,rc_pre_rc_post\n')
            for r in out_rows:
                fh.write(','.join('' if x is None else str(x) for x in r) + '\n')
        print(f'CSV → {a.out}')
    subprocess.run(['rm', '-rf', tmp])


if __name__ == '__main__':
    main()
