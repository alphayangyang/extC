#!/usr/bin/env python3
"""bench/gc/v2/report.py - 把 run.sh bench 的 TSV 变成 REPORT.md 里的表与图。

用法： python3 bench/gc/v2/report.py <in.tsv> [out.md] [out.png]
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
    L.append('所有臂都经 `bench/gc/v2/shim.c`（`wait4` 内核值，best-of-REPS 取 ms 最小），单核绑核，stdout 与 extC 参考逐字节相同（`run.sh align`）。')
    L.append('')
    for s in shapes:
        rs = [r for r in rows if r['shape'] == s]
        rs.sort(key=lambda r: r['ms_min'])
        base = next((r for r in rs if r['arm'].startswith('extc')), None)
        L.append(f'## {s}')
        L.append('')
        L.append('| 臂 | 墙钟 ms | ×extC | RSS MB | CPU ms | 次缺页 | 非自愿切换 |')
        L.append('|---|---:|---:|---:|---:|---:|---:|')
        for r in rs:
            rel = (r['ms_min'] / base['ms_min']) if base else float('nan')
            cpu = r['utime_ms'] + r['stime_ms']
            L.append('| `%s` | %s | %.2f× | %s | %s | %s | %s |' % (
                r['arm'], fmt(r['ms_min']), rel, fmt(r['rss_kb'] / 1024.0), fmt(cpu),
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
    L.append('每语言取该形状最快的一档（`extC:off` / `Go:off` 是 GC 关档对照，不计入）；extC 一列即参考臂。')
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
    pause_tsv = sys.argv[4] if len(sys.argv) > 4 else os.path.join(os.path.dirname(out_md) or '.', 'pause.tsv')
    if os.path.exists(pause_tsv):
        L.append('## extC 侧的停顿形状（`EXTC_RC_PAUSE=1`，由 `bench/gc/v2/pause.sh` 产出的 `pause.tsv`）')
        L.append('')
        L.append('切片 = `extc_rc_stepNote` 记下的每个有界标记/清扫步（`[step]`）；`[pause]` 只计起轮与销账入口，不等于全部回收时间（口径见 `docs/RECLAMATION.md` 与 `docs/TODO.md`）。')
        L.append('')
        L.append('| 形状 | 切片数 | 切片均值 µs | 切片 max µs | ≥16µs | ≥64µs | ≥1ms | collects | `[pause]` 总 µs |')
        L.append('|---|---:|---:|---:|---:|---:|---:|---:|---:|')
        with open(pause_tsv, newline='') as fp:
            next(fp, None)
            for line in fp:
                c = line.rstrip('\n').split('\t')
                if len(c) < 11:
                    continue
                L.append('| %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
                    c[0], f'{int(c[1]):,}', c[3], c[4], c[5], c[6], c[8], c[9], c[10]))
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