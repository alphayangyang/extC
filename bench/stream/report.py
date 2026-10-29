#!/usr/bin/env python3
"""bench/stream/report.py —— 从 raw.txt 生成 RESULTS.md（别手改 RESULTS.md）。"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RAW = os.path.join(HERE, "raw.txt")
OUT = os.path.join(HERE, "RESULTS.md")

PASSES = {1: 5, 2: 3, 3: 1, 4: 3, 5: 3, 6: 3}
MODENAME = {
    1: "materialize（tmp + out 两个数组）",
    2: "fused（一个循环写 out）",
    3: "stream（不落数组，边算边归约）",
    4: "par-fused（融合 + par，8 线程）",
    5: "fused + 软件预取（ctuned 专用）",
    6: "fused + 非临时写（ctuned 专用，省 RFO）",
}

def parse(path):
    header, rows = {}, []
    for line in open(path, encoding="utf-8"):
        parts = line.rstrip("\n").split("\t")
        if parts[0] == "data":
            _, arm, mode, n, ms, rss, cs = parts
            rows.append((arm, int(mode), int(n), float(ms), int(rss), int(cs)))
        elif len(parts) >= 2:
            header[parts[0]] = "\t".join(parts[1:])
    return header, rows

REPS = 1

def gbs(mode, n, ms):
    return REPS * PASSES[mode] * 8.0 * n / 1e9 / (ms / 1000.0)

def nsc(mode, n, ms):
    return ms * 1e6 / (n * REPS)

def main():
    header, rows = parse(RAW)
    global REPS
    REPS = int(header.get("reps", "1"))
    if not rows:
        sys.exit("raw.txt 里没有数据行")
    sizes = sorted({r[2] for r in rows})
    table = {(a, m, n): (ms, rss, cs) for a, m, n, ms, rss, cs in rows}

    L = []
    A = L.append
    A("# bench/stream —— 流式管线的 DRAM 代价（RESULTS.md）")
    A("")
    A("> 本文件由 `bash bench/stream/run.sh` 生成，**别手改**；原始记录在 `raw.txt`，口径与判据见 `SPEC.md`。")
    A("")
    A("## 机器与口径")
    A("")
    A("| 项 | 值 |")
    A("|---|---|")
    for k in ("machine", "kernel", "nproc", "caches", "gcc", "extc", "pin", "parpin", "threads", "reps", "runs", "sizes"):
        if k in header:
            A(f"| {k} | {header[k]} |")
    A(f"| 构建 | extC 臂：`{header.get('flags_O2','')}`；O3 臂：`{header.get('flags_O3','')}`；手写 C 同一组旗子 |")
    A("")
    A("## 流量模型（每元素 DRAM 字节 = 遍数 × 8）")
    A("")
    A("| mode | 写法 | 遍数 | 字节/元素 |")
    A("|---|---|---|---|")
    for m in sorted(PASSES):
        A(f"| {m} | {MODENAME[m]} | {PASSES[m]} | {PASSES[m]*8} |")
    A("")
    A("`stream` 不写中间数组也不写 `out`，所以只有 1 遍（读 `in`）；`materialize` 要把 `tmp` 和 `out`")
    A("各写一遍、各读一遍，再加最后 `Σ out` 的读，共 5 遍。")
    A("")

    for n in sizes:
        A(f"## 结果：n = {n}（数组 {n*8/2**20:.1f} MiB，三块共 {3*n*8/2**20:.1f} MiB）")
        A("")
        A("| 臂.mode | best ms | ns/cell | 有效 GB/s | RSS KB | cs |")
        A("|---|---|---|---|---|---|")
        for arm in sorted({r[0] for r in rows}):
            for mode in (1, 2, 3, 4, 5, 6):
                key = (arm, mode, n)
                if key not in table:
                    continue
                ms, rss, cs = table[key]
                A(f"| `{arm}`.{mode} | {ms:.2f} | {nsc(mode,n,ms):.2f} | {gbs(mode,n,ms):.1f} | {rss} | {cs} |")
        A("")

    A("## 比值（同一张表内比较）")
    A("")
    A("| n | fused/materialize | stream/materialize | stream/fused | par-fused/fused | ctuned.O2 ÷ c.O2 (mode2) | 非临时写 ÷ 普通写 (mode6/mode2, O2) | extc.O3 ÷ extc.O2 (mode2) |")
    A("|---|---|---|---|---|---|---|---|")
    for n in sizes:
        def get(arm, mode):
            k = (arm, mode, n)
            return table[k][0] if k in table else None
        def ratio(a, b):
            return f"{a/b:.2f}×" if (a and b) else "—"
        fused = get("extc.O2", 2); mat = get("extc.O2", 1); stream = get("extc.O2", 3)
        parf = get("extc.O3", 4) or get("extc.O2", 4)
        fused_o3 = get("extc.O3", 2)
        A(f"| {n} | {ratio(fused, mat)} | {ratio(stream, mat)} | {ratio(stream, fused)} | "
          f"{ratio(parf, fused)} | {ratio(get('ctuned.O2',2), get('c.O2',2))} | "
          f"{ratio(get('ctuned.O2',6), get('ctuned.O2',2))} | {ratio(fused_o3, fused)} |")
    A("")

    A("## 怎么读")
    A("")
    A("- `ns/cell` 是每元素**每遍**的墙钟时间（总时间 ÷ `n × reps`）；`有效 GB/s` = `reps × 遍数 × 8n ÷ 总时间`，"
  "按上表的模型算，不是硬件计数器。模型**不含** store 的写分配读（RFO）、`in` 的初始化与首次触页，"
  "所以这个 GB/s 偏低、只适合同表内相对比较。")
    A("- 三块数组共 `3 × 8n` 字节：n=131072 贴 L2/核（3 MiB），n=1048576 在 L3 内（24 MiB），")
    A("  n=4194304 已出 L3（96 MiB），最后一档就是 DRAM 带宽墙。")
    A("- 比值 < 1 表示更快。`fused/materialize` 衡量\"省掉一次 tmp 物化\"值多少；")
    A("  `stream/materialize` 衡量\"不落数组\"值多少；`par-fused/fused` 衡量多线程在这条内存受限管线上的收益；")
    A("  `ctuned.O2 ÷ c.O2` 是`restrict`+对齐+ivdep 这组 C 层假设能赢多少（< 1 表示赢了）；")
    A("- `非临时写 ÷ 普通写` 是 `_mm256_stream_pd` 相对普通 store 的比值：省掉写分配的 RFO 读，")
    A("  只在「写完不再读回」的形状上成立（本 bench 的 mode 6 就在循环里直接从寄存器求和）。")
    A("  `extc.O3 ÷ extc.O2` 是纯编译旗子的收益。")
    A("- 绝对值跨会话会漂（宿主 DRAM 带宽状态），结论读比值，别拿毫秒数和别的会话比。")
    A("- `cs` 每一行都必须相同：不同的只是减了多少遍内存流量，不是算法。")
    A("")

    A("## 一句话结论（自动算）")
    A("")
    big = sizes[-1]
    def get(arm, mode, n=big):
        k = (arm, mode, n)
        return table[k][0] if k in table else None
    mat = get("extc.O2", 1); fused = get("extc.O2", 2); stream = get("extc.O2", 3)
    if mat and fused:
        A(f"- n={big}：融合把 extC 的 {mat:.1f} ms 降到 {fused:.1f} ms（{(1-fused/mat)*100:.0f}%）；")
    if mat and stream:
        A(f"  流式归约降到 {stream:.1f} ms（{(1-stream/mat)*100:.0f}%），因为它连 `out` 都不写。")
    c2 = get("c.O2", 2); ct2 = get("ctuned.O2", 2); nt = get("ctuned.O2", 6); pf = get("ctuned.O2", 5)
    if c2 and ct2:
        A(f"- 同一份手写 C：`restrict`+64B 对齐+ivdep {'赢' if ct2 < c2 else '没赢'}"
          f" {c2:.1f} → {ct2:.1f} ms（{ct2/c2:.2f}×）。")
    if ct2 and nt:
        A(f"- 唯一赢的 C 层调整是非临时写：融合形状 {ct2:.1f} → {nt:.1f} ms（{nt/ct2:.2f}×，省 RFO）；")
    if ct2 and pf:
        A(f"  软件预取 {'有效' if pf < ct2 else '没用'}"
          f"（{ct2:.1f} → {pf:.1f} ms，{pf/ct2:.2f}×），因为顺序访存已有硬件预取。")
    A("")
    open(OUT, "w", encoding="utf-8").write("\n".join(L))
    print(f"wrote {OUT} ({len(rows)} 行)")

if __name__ == "__main__":
    main()
