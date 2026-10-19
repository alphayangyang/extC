#!/usr/bin/env python3
"""bench/gc/v2/report.py - 把 run.sh bench 的 TSV 变成 REPORT.md 里的表与图。

用法： python3 bench/gc/v2/report.py <in.tsv> [out.md] [out.png] [pause_v2.tsv]
TSV 列（制表符）： shape arm ms_min ms_med ms_max rss_kb utime_ms stime_ms minflt majflt nvcsw nivcsw

口径：所有臂都经 bench/gc/v2/shim.c 垫片（wait4 内核值），best-of-REPS 取 ms 最小；
      其余列取「ms 最小的那一次」的采样（避免把噪声混进 CPU/缺页数）。
"""
import sys, os, csv

CJK = {
    "extc": "extC", "go": "Go", "java": "Java", "rust": "Rust", "node": "Node", "cpp": "C++",
}

def read_tsv(path):
    rows = []
    with open(path, newline='') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line or line.startswith('#'):
                continue
            c = line.split('\t')
            if len(c) < 12 or c[0] == 'shape':
                continue
            rows.append({
                "shape": c[0], "arm": c[1],
                "ms_min": float(c[2]), "ms_med": float(c[3]), "ms_max": float(c[4]),
                "rss_kb": int(c[5]), "utime_ms": float(c[6]), "stime_ms": float(c[7]),
                "minflt": int(c[8]), "majflt": int(c[9]),
                "nvcsw": int(c[10]), "nivcsw": int(c[11]),
            })
    return rows

def fmt(x, n=1):
    return f'{x:,.{n}f}'

def main():
    src = sys.argv[1]
    out_md = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(src)[0] + '.md'
    out_png = sys.argv[3] if len(sys.argv) > 3 else None
    rows = read_tsv(src)
    shapes = []
    for r in rows:
        if r['shape'] not in shapes:
            shapes.append(r['shape'])
    L = []
    L.append('# GC 大横评 v2 —— 四形状六语言')
    L.append('')
    L.append('所有臂都经 `bench/gc/v2/shim.c`（`wait4` 内核值，best-of-REPS 取 ms 最小），单核绑核，stdout 与 extC 参考逐字节相同（`run.sh align`）；每个形状内 REPS 轮**交错**跑（一轮里每个臂各一次），噪声突发不会整段压在某一个臂上。')
    L.append('')
    for s in shapes:
        rs = [r for r in rows if r['shape'] == s]
        rs.sort(key=lambda r: r['ms_min'])
        base = next((r for r in rs if r['arm'].startswith('extc')), None)
        L.append(f'## {s}')
        L.append('')
        L.append('| 臂 | 墙钟 ms（min） | 中位 ms | ×extC | RSS MB | CPU ms | 次缺页 | 非自愿切换 |')
        L.append('|---|---:|---:|---:|---:|---:|---:|---:|')
        for r in rs:
            rel = (r['ms_min'] / base['ms_min']) if base else float('nan')
            cpu = r['utime_ms'] + r['stime_ms']
            L.append('| `%s` | %s | %s | %.2f× | %s | %s | %s | %s |' % (
                r['arm'], fmt(r['ms_min']), fmt(r['ms_med']), rel, fmt(r['rss_kb'] / 1024.0), fmt(cpu),
                f"{r['minflt']:,}", f"{r['nivcsw']:,}"))
        L.append('')
    # 跨形状概览：每个语言最快臂
    L.append('## 概览（每语言每形状最快臂 / extC 之比）')
    L.append('')
    langs = []
    for r in rows:
        lg = r['arm'].split(':')[0]
        if lg not in langs:
            langs.append(lg)
    L.append('每语言按 **min** 取该形状最快的一档（`extC:off` / `Go:off` 是 GC 关档对照，不计入）；extC 一列即参考臂。')
    L.append('min 对噪声突发敏感（这台 VM 上同一臂两次可差 30%），所以上一节同时给出中位；两者结论不一致时以中位为准。')
    L.append('')
    L.append('| 形状 | ' + ' | '.join(CJK.get(lg, lg) for lg in langs) + ' |')
    L.append('|---|' + '---:|' * len(langs))
    for s in shapes:
        rs = [r for r in rows if r['shape'] == s]
        base = next((r for r in rs if r['arm'] == 'extc:default'), None)
        cells = []
        for lg in langs:
            cand = [r for r in rs if r['arm'].split(':')[0] == lg and not r['arm'].endswith(':off')]
            if not cand:
                cells.append('-')
                continue
            b = min(cand, key=lambda r: r['ms_min'])
            tag = b['arm'].split(':')[1] if ':' in b['arm'] else ''
            rel = (b['ms_min'] / base['ms_min']) if base else float('nan')
            cells.append(f"{fmt(b['ms_min'])} ({rel:.2f}×, {tag})")
        L.append('| %s | %s |' % (s, ' | '.join(cells)))
    L.append('')
    pause_tsv = sys.argv[4] if len(sys.argv) > 4 else os.path.join(os.path.dirname(out_md) or '.', 'pause_v2.tsv')
    if os.path.exists(pause_tsv):
        L.append('## 跨语言停顿分布（`pause_v2.tsv`；每个臂的口径与完整讨论见 `PAUSE.md`）')
        L.append('')
        L.append('事件 = 各语言**自己运行期承认的停顿**（extC 是有界回收步、Go 是两段 STW、Java 是每条 GC 暂停、Node 是每次 GC），')
        L.append('定义逐臂不同，读表前先看 `PAUSE.md` 的「事件来源」栏。extC 行的切片数与 `collects` 就是 `pause_v2.tsv` 的同名列。')
        L.append('')
        L.append('| 形状 | 臂 | 事件数 | p50 | p90 | p99 | p99.9 | max | 占墙钟 |')
        L.append('|---|---|---:|---:|---:|---:|---:|---:|---:|')
        with open(pause_tsv, newline='') as fp:
            head = next(fp, '').rstrip('\n').split('\t')
            idx = {k: i for i, k in enumerate(head)}

            def cell(c, k):
                return c[idx[k]] if k in idx and idx[k] < len(c) else '-'

            def us(v):
                try:
                    f = float(v)
                except ValueError:
                    return v
                return '%.2f ms' % (f / 1000.0) if f >= 1000 else '%.2f µs' % f
            order = {s: i for i, s in enumerate(shapes)}
            rows2 = [ln.rstrip('\n').split('\t') for ln in fp if ln.strip()]
            def p99key(c):
                try:
                    return float(cell(c, 'p99_us'))
                except ValueError:
                    return 0.0

            rows2.sort(key=lambda c: (order.get(cell(c, 'shape'), 9), p99key(c)))
            for c in rows2:
                try:
                    share = '%.2f%%' % float(cell(c, 'share_pct'))
                except ValueError:
                    share = cell(c, 'share_pct')
                L.append('| %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
                    cell(c, 'shape'), cell(c, 'arm'), f"{int(cell(c, 'events') or 0):,}",
                    us(cell(c, 'p50_us')), us(cell(c, 'p90_us')), us(cell(c, 'p99_us')),
                    us(cell(c, 'p999_us')), us(cell(c, 'max_us')), share))
        L.append('')
        L.append('### Sigma GC time（Σ GC 归因时间；口径见 `PAUSE.md` 第 1.2 节）')
        L.append('')
        L.append('上表是**停顿**（定义逐臂不同）；这里是同一次运行里可观测到的**全部 GC 时间**：')
        L.append('extC 没有独立 GC 线程 ⇒ 停顿步就是全部 GC 工作；Go/Java/Node 的并发阶段不进停顿、但进这里。')
        L.append('占比高低要配本文件上面的峰值 RSS 读：extC 占比高换来 7.4–7.5 MB 峰值；')
        L.append('Java `cycleratio` 0 事件 / 0.0% 占比，代价是 104 MB；低占比既可能是「回收便宜」也可能是「还没回收」。')
        L.append('')
        L.append('| 形状 | 臂 | Σ 停顿 | 停顿/墙钟 | Σ GC 工作 | GC/墙钟 |')
        L.append('|---|---|---:|---:|---:|---:|')
        for c in rows2:
            try:
                tg = float(cell(c, 'total_gc_us'))
            except ValueError:
                continue
            if tg <= 0:
                continue
            try:
                gs = '%.2f%%' % float(cell(c, 'gc_share_pct'))
            except ValueError:
                gs = '-'
            try:
                ps = '%.2f%%' % float(cell(c, 'share_pct'))
            except ValueError:
                ps = '-'
            L.append('| %s | %s | %s | %s | %s | %s |' % (
                cell(c, 'shape'), cell(c, 'arm'), us(cell(c, 'total_us')), ps, us(cell(c, 'total_gc_us')), gs))
        L.append('')
    with open(out_md, 'w') as f:
        f.write('\n'.join(L))
    print('wrote', out_md, f'({len(rows)} 行 / {len(shapes)} 形状)')
    if out_png:
        try:
            import matplotlib
            matplotlib.use('Agg')
            import matplotlib.pyplot as plt
        except Exception as e:
            print('skip chart:', e)
            return
        n = len(shapes)
        fig, axes = plt.subplots(1, n, figsize=(4.2 * n, 4.0))
        if n == 1:
            axes = [axes]
        for ax, s in zip(axes, shapes):
            rs = sorted([r for r in rows if r['shape'] == s], key=lambda r: r['ms_min'])
            ax.barh([r['arm'] for r in rs], [r['ms_min'] for r in rs], color='#4f81bd')
            ax.set_xscale('log')
            ax.set_title(s)
            ax.set_xlabel('ms (log)')
            ax.tick_params(axis='y', labelsize=7)
        fig.tight_layout()
        fig.savefig(out_png, dpi=130)
        print('wrote', out_png)

if __name__ == '__main__':
    main()