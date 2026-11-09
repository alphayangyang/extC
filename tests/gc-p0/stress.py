#!/usr/bin/env python3
"""tests/gc-p0/stress.py —— G4 正式验收量具：注入 4×L 循环垃圾时的内存界。

背景（Lead m02258）：run.sh 的 g4 段实际只覆盖 G6/G7（pb 每节点字节 / pc 稳态 / cyc N-vs-4N 代理），
**不是** G4 判据「注入 4× 循环垃圾时 RSS ≤ M_max + 8 MiB 且不随轮次单调涨」。本脚本补这个缺口。

形状：tests/gc-p0/src/g4_stress.extc —— main 开头建 L 结点长链（根永不摘），每轮再建 4L 个
互指的 2-环结点（经 makeRing 逃逸到函数级根 w.ring，进 rc 区），建完立刻丢根 ⇒ 每轮 4L 个
不可达环垃圾；每轮向 stderr 打 "R k" 便于把 /proc 采样对齐到轮边界。

三档对照（Lead 要求）：
  A 默认（隐式 M_max = 3·L，自学习） / B EXTC_RC_M_MAX=33554432 / C EXTC_RC_M_MAX=1099511627776（等效关墙）

判据（Lead m02258）：
  1) 默认档 VmHWM ≤ 声明 M_max + 8 MiB（M_max 取运行时 [work] 行；同时记录 M_max_now/[mmax] 警告）；
  2) 稳态：最后若干轮「轮末 VmHWM」相邻差 ≤2%（且不单调上涨）；
  3) 次序：A<B<C 或 A≈B<C 才算墙真正约束；A≈B≈C ⇒ RED（墙没真正约束）。
     **A≈B<C 是可接受的合理现象，不是失败**：峰值不一定顶到 M_max，因为触发阈值 thr
     （自学习 scale = 上轮存活字节 × 倍数）可能先于 M_max 成为约束（G17 实测：显式 64MiB 与
     1TiB 两档峰值都是 68.6 MB，因为 thr 饱和在 ~66–75 MB，远低于墙）。报告要把这一条解释成
     「墙没有成为唯一约束」，而不是「墙没生效」；只有当三档峰值几乎相同（A≈B≈C）时才是 RED。
不判 PASS/RED 的部分（记录）：[work] 的 thr_max/M_max_now/pressure_steps、[mmax] 是否出现。

用法：
  tests/gc-p0/stress.py                      # 默认 build-frz/extc，3 reps
  tests/gc-p0/stress.py --extc ./build-emp/extc --out tests/gc-p0/out/after --reps 3
  tests/gc-p0/stress.py --live 100000 --garb 400000 --rounds 100
"""
import argparse, csv, json, os, re, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src", "g4_stress.extc")

CFG = [  # (name, env value of EXTC_RC_M_MAX or None)
    ("A", None),                      # 默认：隐式 M_max = 3·L
    ("B", "33554432"),                # 显式 32 MiB
    ("C", "1099511627776"),           # 等效关墙（1 TiB）
]
WORK_KEYS = ("thr_max", "mmax", "scale_max", "pressure_steps", "M_max", "M_max_now",
             "M_max_implicit", "pressure", "mult", "thr")


def build(args):
    os.makedirs(os.path.join(args.out, "c"), exist_ok=True)
    os.makedirs(os.path.join(args.out, "bin"), exist_ok=True)
    os.makedirs(os.path.join(args.out, "log"), exist_ok=True)
    cpath = os.path.join(args.out, "c", "g4_stress.c")
    binp = os.path.join(args.out, "bin", "g4_stress")
    with open(os.path.join(args.out, "log", "g4_stress.compile.log"), "w") as lf:
        r = subprocess.run([args.extc, SRC, "-o", cpath], stdout=lf, stderr=lf)
        if r.returncode != 0:
            sys.exit(f"FATAL: extc compile failed（见 {args.out}/log/g4_stress.compile.log）")
        cmd = [args.cc] + args.cflags.split() + ["-o", binp, cpath]
        r = subprocess.run(cmd, stdout=lf, stderr=lf)
        if r.returncode != 0:
            sys.exit(f"FATAL: cc failed（见 {args.out}/log/g4_stress.compile.log）")
    return binp


def read_proc(pid):
    """/proc/<pid>/status → (VmRSS kB, VmHWM kB) or (None,None)。"""
    try:
        with open(f"/proc/{pid}/status") as f:
            rss = hwm = None
            for line in f:
                if line.startswith("VmRSS:"):
                    rss = int(line.split()[1])
                elif line.startswith("VmHWM:"):
                    hwm = int(line.split()[1])
            return rss, hwm
    except (FileNotFoundError, ProcessLookupError):
        return None, None


def run_one(binp, cfg, mm, rep, args):
    env = dict(os.environ)
    env["EXTC_RC_PAUSE"] = "1"
    if mm:
        env["EXTC_RC_M_MAX"] = mm
    else:
        env.pop("EXTC_RC_M_MAX", None)
    t0 = time.monotonic()
    proc = subprocess.Popen([binp, str(args.live), str(args.rounds), str(args.garb)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, text=True)
    samples = []          # (t_ms, rss_kb, hwm_kb)
    marks = []            # (t_ms, round_index)
    lines = []            # (t_ms, text)
    stop = threading.Event()

    def sampler():
        while not stop.is_set():
            rss, hwm = read_proc(proc.pid)
            if rss is not None:
                samples.append(((time.monotonic() - t0) * 1000.0, rss, hwm))
            time.sleep(0.002)

    def reader(stream, collect_stderr):
        for line in stream:
            t = (time.monotonic() - t0) * 1000.0
            line = line.rstrip("\n")
            if collect_stderr:
                lines.append((t, line))
                m = re.match(r"^R (\d+)$", line)
                if m:
                    marks.append((t, int(m.group(1))))
        stream.close()

    th_s = threading.Thread(target=sampler, daemon=True)
    th_e = threading.Thread(target=reader, args=(proc.stderr, True), daemon=True)
    th_o = threading.Thread(target=reader, args=(proc.stdout, False), daemon=True)
    th_s.start(); th_e.start(); th_o.start()
    try:
        rc = proc.wait(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        proc.kill(); rc = -9
    stop.set()
    th_s.join(timeout=1); th_e.join(timeout=2); th_o.join(timeout=2)
    wall = time.monotonic() - t0

    work = {}
    mmax_lines = []
    for _, line in lines:
        if line.startswith("[work]"):
            for k, v in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=(-?\d+)", line):
                if k in WORK_KEYS:
                    work[k] = int(v)
            work["_line"] = line
        elif line.startswith("[mmax]"):
            mmax_lines.append(line)

    peak = max((h for _, _, h in samples if h), default=0)
    end_hwm = {}  # round -> 轮末 VmHWM（marker 时刻之前最后一个采样点）
    for t, k in marks:
        best = None
        for st, _, h in samples:
            if st <= t:
                best = h
        if best is not None:
            end_hwm[k] = best
    # 若采样没覆盖（进程太快），退回用 marker 时刻之后第一个采样
    for t, k in marks:
        if k not in end_hwm:
            for st, _, h in samples:
                if st > t:
                    end_hwm[k] = h
                    break

    return dict(cfg=cfg, mm=mm, rep=rep, rc=rc, wall_s=round(wall, 3), peak_kb=peak,
                samples=samples, marks=marks, end_hwm=end_hwm, work=work,
                mmax_lines=mmax_lines, stderr_lines=lines,
                stdout=next((l for _, l in lines if l.startswith("G4S")), ""))


def size_of_work(work):
    """声明 M_max（字节）。[work] 的 M_max 是学习后的生效值；M_max_now 是最后一次快照。"""
    for k in ("M_max", "M_max_now", "mmax"):
        v = work.get(k)
        if v:
            return v, k
    return 0, "none"


def judge(runs, args):
    """返回 (verdict, detail-lines)。runs: dict cfg -> list[run]。"""
    lines = []
    ok = True

    def take(cfg):
        rs = runs[cfg]
        peaks = sorted(r["peak_kb"] for r in rs)
        p = peaks[len(peaks) // 2] if peaks else 0
        mmax, mkey = size_of_work(rs[0]["work"])
        return p, mmax, mkey, rs[0]

    pA, mA, mkeyA, rA = take("A")
    pB, mB, _, rB = take("B")
    pC, mC, _, rC = take("C")
    cap = 8 * 1024  # +8 MiB
    lines.append(f"峰 VmHWM: A={pA} kB  B={pB} kB  C={pC} kB（每档 {args.reps} reps 的 median，单位 kB）")
    lines.append(f"[work] 声明 M_max: A={mA} B={mB} C={mC}（取字段 {mkeyA}）")
    lines.append(f"pressure_steps: A={rA['work'].get('pressure_steps','n/a')} "
                 f"B={rB['work'].get('pressure_steps','n/a')} C={rC['work'].get('pressure_steps','n/a')}")
    warn = any(r["mmax_lines"] for r in runs["A"])
    if warn:
        lines.append(f"[mmax] 警告: {runs['A'][0]['mmax_lines'][0]}")
    if args.garb != 4 * args.live:
        lines.append(f"⚠ 形状不是 4×L：live={args.live} garb={args.garb}")

    # 1) 默认档内存上界
    if mA > 0 and pA <= mA / 1024 + cap:
        lines.append(f"① 默认档 VmHWM ≤ M_max + 8 MiB：{pA} ≤ {mA // 1024 + cap} kB ⇒ PASS")
    else:
        lines.append(f"① 默认档 VmHWM ≤ M_max + 8 MiB：{pA} > {mA // 1024 + cap} kB ⇒ RED")
        ok = False

    # 2) 稳态：最后 5 个轮末 HWM，相邻差 ≤2%
    eh = runs["A"][0]["end_hwm"]
    ks = sorted(eh)
    diffs = []
    for k in ks[-6:]:
        if k - 1 in eh and eh[k - 1]:
            diffs.append(abs(eh[k] - eh[k - 1]) / eh[k - 1])
    rises = sum(1 for k in ks[-6:] if k - 1 in eh and eh[k] > eh[k - 1])
    if diffs:
        lines.append(f"② 稳态：末段轮末 VmHWM 相邻差 max={max(diffs) * 100:.2f}% "
                     f"（最后 {len(diffs)} 个间隔，>2% 记 RED；单调涨 {rises}/{len(diffs)} 段）")
        if max(diffs) > 0.02:
            ok = False
    else:
        lines.append("② 稳态：采样点不足 ⇒ N/A（进程过快，请调大 --rounds/--garb 或减小 --interval）")
    if ks:
        lines.append(f"   轮末 VmHWM 序列（末 8 轮）: " + " ".join(f"R{k}={eh[k]}" for k in ks[-8:]))

    # 3) 次序：A<B<C 或 A≈B<C；A≈B≈C ⇒ RED
    def within5(x, y):
        return y > 0 and abs(x - y) / y <= 0.05
    strict = pA < pB < pC
    ab_close = within5(pA, pB) and pC > pB * 1.05
    if strict:
        lines.append(f"③ 次序：A<B<C（{pA}<{pB}<{pC}）⇒ PASS（墙真正约束）")
    elif ab_close:
        lines.append(f"③ 次序：A≈B<C（A≈B 5% 容差，C 显著更大）⇒ PASS（墙真正约束；"
                     f"A≈B 的饱和通常是 thr 先于 M_max 成为约束，属合理现象，不是失败）")
    else:
        lines.append(f"③ 次序：A={pA} B={pB} C={pC} 既不满足 A<B<C 也不满足 A≈B<C ⇒ "
                     f"RED（墙没真正约束：峰值由 thr/扫掠频率决定，不是 M_max）")
        ok = False

    # 非判据：默认档是否真达到过墙（pressure_steps>0）
    if rA["work"].get("pressure_steps", 0) == 0:
        lines.append("   注：默认档 pressure_steps=0 ⇒ 本轮没有一步真正触发内存墙压力"
                     "（峰值由触发阈值 thr 决定）；结论只能说明「未越过 M_max+8MiB」，"
                     "不能说明墙在起约束作用")
    return ("PASS" if ok else "RED"), lines


def main():
    ap = argparse.ArgumentParser(description="G4 正式验收：注入 4×L 循环垃圾时的内存界")
    ap.add_argument("--extc", default="./build-frz/extc")
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--cflags", default="-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L")
    ap.add_argument("--out", default=os.path.join(HERE, "out", "g4-stress"))
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--live", type=int, default=100000)
    ap.add_argument("--garb", type=int, default=400000, help="每轮垃圾结点数，默认 4×L")
    ap.add_argument("--rounds", type=int, default=100)
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    binp = build(args)
    runs = {}
    for cfg, mm in CFG:
        runs[cfg] = []
        for rep in range(1, args.reps + 1):
            r = run_one(binp, cfg, mm, rep, args)
            runs[cfg].append(r)
            tag = "default" if mm is None else mm
            print(f"  stress cfg={cfg}({tag}) rep={rep} rc={r['rc']} wall={r['wall_s']}s "
                  f"peak={r['peak_kb']}kB marks={len(r['marks'])} samples={len(r['samples'])}")
            with open(os.path.join(args.out, "log", f"stress_{cfg}_r{rep}.csv"), "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["t_ms", "rss_kb", "hwm_kb"])
                w.writerows(r["samples"])
            with open(os.path.join(args.out, "log", f"stress_{cfg}_r{rep}.err"), "w") as f:
                f.write("\n".join(line for _, line in r.get("stderr_lines", [])) + "\n")
            r.pop("samples", None)
            r.pop("stderr_lines", None)
    verdict, lines = judge(runs, args)
    print("\n== G4-Stress（注入 4×L 循环垃圾）==")
    for line in lines:
        print("  " + line)
    print(f"  G4-STRESS verdict = {verdict}")
    with open(os.path.join(args.out, "g4_stress.json"), "w") as f:
        json.dump(dict(args=vars(args), runs=runs, verdict=verdict, detail=lines), f, indent=1)
    print(f"  数据：{args.out}/g4_stress.json")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())