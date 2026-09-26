#!/usr/bin/env python3
"""bench/echo/report.py —— 把 results.txt 变成 RESULTS.md 里的表格。

results.txt 是 run.sh 的原始记录（每格一行，格式由 load.go 决定）。这里只做两件事：
  · 每格取 **req/s 最高的一次**（best-of-REPS，与 run.sh 的口径一致）；
  · 按 形态 × 载荷 分组，把 4 个服务器并排成表。
任何一行带 "数据校验失败" 的格子照原样标出来 —— 那种数字不能进结论。
"""
import re
import sys
from collections import defaultdict

ROW = re.compile(
    r'(?P<mode>\w+)\s+conns=(?P<conns>\d+)\s+payload=(?P<payload>\d+)\s+iters=(?P<iters>\d+)\s+'
    r'(?P<rps>[\d.]+) req/s\s+(?P<mbs>[\d.]+) MB/s\s+p50=\s*(?P<p50>[\d.]+)us\s+p99=\s*(?P<p99>[\d.]+)us\s+(?P<tail>.*)')
SERVERS = ['extC', 'Go(1核)', 'Go(默认)', 'sink']


def main(path):
    raw = open(path, encoding='utf-8', errors='replace').read().splitlines(True)
    # run.sh 用 `tee -a` 追加 ⇒ 文件里可能留着历史运行。只认**最后一段**（最后那次运行的标题之后）。
    last = 0
    for i, l in enumerate(raw):
        if l.startswith('echo server 压测'):
            last = i
    raw = raw[last:]

    best = {}          # (mode, payload, conns, server) -> (rps, p50, p99, tail)
    for line in raw:
        m = ROW.search(line)
        if not m:
            continue
        srv = None
        for s in SERVERS:
            if s in line:
                srv = s
                break
        if srv is None:
            continue
        key = (m.group('mode'), int(m.group('payload')), int(m.group('conns')), srv)
        rps = float(m.group('rps'))
        bad = '校验失败' in m.group('tail')
        prev = best.get(key)
        if prev is None or rps > prev[0]:
            best[key] = (rps, float(m.group('p50')), float(m.group('p99')), bad)

    keys = sorted({(mo, pa) for (mo, pa, _, _) in best})
    out = []
    for mode, payload in keys:
        conns = sorted({c for (mo, pa, c, _) in best if (mo, pa) == (mode, payload)})
        out.append('### %s · payload=%d B\n' % (mode, payload))
        out.append('| 连接 | ' + ' | '.join(SERVERS) + ' |')
        out.append('|---|' + '---|' * len(SERVERS))
        for c in conns:
            cells = []
            for s in SERVERS:
                v = best.get((mode, payload, c, s))
                if v is None:
                    cells.append('—')
                elif v[3]:
                    cells.append('**校验失败**')
                else:
                    cells.append('%.0f（p50 %.0fµs）' % (v[0], v[1]))
            out.append('| %d | %s |' % (c, ' | '.join(cells)))
        out.append('')
    sys.stdout.write('\n'.join(out))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'bench/echo/results.txt')
