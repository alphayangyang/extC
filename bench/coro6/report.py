#!/usr/bin/env python3
"""bench/coro6/report.py —— 把 raw.txt 变成 RESULTS.md（**别手改 RESULTS.md**）。"""
import sys, os, re
raw = sys.argv[1] if len(sys.argv) > 1 else 'bench/coro6/raw.txt'
NAME = {'extc':'extC','cpp':'C++20','rust':'Rust','go':'Go','java':'Java 25','python':'Python'}
MECH = {'extc':'`coroutine<i64>`（编译器状态机）','cpp':'`co_yield`（编译器状态机）',
        'rust':'`async/.await` + std-only executor（每值 2 次 poll）',
        'go':'goroutine + 无缓冲 channel（会合）','java':'虚拟线程 + `SynchronousQueue`',
        'python':'原生 generator'}
single, scale, rss = {}, {}, {}
for ln in open(raw, encoding='utf-8'):
    p = ln.split()
    if not p: continue
    if p[0] == 'single': single[p[1]] = (int(p[2]), int(p[3]), int(p[4]))
    elif p[0] == 'scale': scale[(p[1], int(p[2]))] = (int(p[3]), int(p[4]))
    elif p[0] == 'rss': rss[p[1]] = int(p[2])
out = ['# 六语言 × 协程 × 多进程（1/4/8）横评', '',
       '> **自动生成**：`bench/coro6/run.sh` 量出 `raw.txt`，`report.py` 出这张表。别手改。',
       '> 同一工作量、同一校验和（六语言逐位相同才算做了等价的工作）；',
       '> 每语言取各自最惯用的惰性生产者机制 —— **机制不同**，所以同时给"同语言纯循环"下界。', '',
       '## 单进程（每轮 10.256M 步）', '',
       '| 语言 | 机制 | 协程版 | 同语言纯循环 | 机制开销/次 |', '|---|---|---|---|---|']
base = None
for k in ('extc','rust','cpp','python','go','java'):
    if k not in single: continue
    c, l, n = single[k]
    ns = (c - l) * 1e6 / n
    out.append(f'| {NAME[k]} | {MECH[k]} | {c} ms | {l} ms | {ns:+.2f} ns |')
out += ['', '## 多进程扩展（吞吐 = 百万步/秒；每进程绑一个 P 核）', '',
        '| 语言 | 1 进程 | 4 进程 | 扩展 | 8 进程 | 扩展 | RSS/进程 |', '|---|---|---|---|---|---|---|']
for k in ('extc','cpp','rust','go','python','java'):
    if (k,1) not in scale: continue
    w1, s1 = scale[(k,1)]; w4, s4 = scale[(k,4)]; w8, s8 = scale[(k,8)]
    t1 = s1 * 1 / (w1/1000) / 1e6
    t4 = s4 * 4 / (w4/1000) / 1e6; t8 = s8 * 8 / (w8/1000) / 1e6
    out.append(f'| {NAME[k]} | {t1:.1f} | {t4:.1f} | {t4/t1:.2f}x | {t8:.1f} | {t8/t1:.2f}x | {rss.get(k,0)/1024:.0f} MB |')
out += ['', '> 扩展列是**吞吐**比（并行成功时墙钟几乎不变）；口径、坑与结论见 `RESULTS.md` 尾部的手写小节。', '']
dst = os.path.join(os.path.dirname(raw) or '.', 'RESULTS.md')
body = open(os.path.join(os.path.dirname(__file__) or '.', 'NOTES.md'), encoding='utf-8').read() if os.path.exists(os.path.join(os.path.dirname(__file__) or '.', 'NOTES.md')) else ''
open(dst, 'w', encoding='utf-8').write('\n'.join(out) + '\n' + body)
print(f'✓ 生成 {dst}')
