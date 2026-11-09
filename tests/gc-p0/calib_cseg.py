#!/usr/bin/env python3
"""c_seg 标定（Lead m04755 C / task-29 项4）：

对 pd_bigobj 与 pd_pointer_dense 各跑 3 reps（PAUSE=1 + STEP_CPU=1、与 after 同 pin/同 args），
从生成物里注入的 [probe-seg] 探针取 seg>0 步的精确 p99.9（15 桶直方图只有桶界，做不到 34.08us
这类精度；task-26 的 P1-RQ3-CHUNK.md 用的是同一个探针），取 3 reps 中位算

    c_seg_ns = p99.9_us * 1000 / chunk        (chunk = 生成物 #define EXTC_RC_SCAN_CHUNK)

并把 9 列写回 tests/gc-p0/bseg-cal.tsv（shape/chunk/c_seg_ns/p99.9_us/p99.9_min_us/
p99.9_max_us/run_id/src_md5/extc_md5），spread=3 reps 的 min..max。

口径（写进 MEASUREMENTS-P1.md §1）：
  - 标定 run 独立于 after 判据（Lead m04755 C），产物与日志落 --out（默认
    tests/gc-p0/out/p1-cal），不写 p1-after；
  - pin：taskset -c 6（与 after 的 G20 同 pin）；
  - 时钟：EXTC_RC_STEP_CPU=1（cpu 口径；[probe-seg] 的 _d 与 [step] 同源）；
  - 每 rep 一次独立进程；p99.9 取该 rep 全量 seg>0 步（探针上限 4e6 步）。

用法：
  python3 tests/gc-p0/calib_cseg.py --extc ./build-emp3/extc \
      --src-md5 reclaim.c=...,codegen.c=... --build-md5 $(md5sum build-emp3/extc | cut -d' ' -f1)
  python3 tests/gc-p0/calib_cseg.py --selftest
"""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEFAULT_CFLAGS = "-O2 -std=c11 -fwrapv -D_POSIX_C_SOURCE=200809L"
DEFAULT_CAL = os.path.join(HERE, "bseg-cal.tsv")
DEFAULT_OUT = os.path.join(HERE, "out", "p1-cal")
SHAPES = {"pd_bigobj": "pd_bigobj.extc", "pd_pointer_dense": "pd_pointer_dense.extc"}
ROUNDS = 200
REPS = 3
PIN = 6

PROBE_GLOB = r'''
/* probe: exact distribution of resumed-segment (seg>0) step durations */
static long long extc_probe_d[4000000];
static long long extc_probe_n;
static int extc_probe_cmp(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return x < y ? -1 : (x > y);
}
static void extc_probe_dump(void) {
    long long i, n = extc_probe_n, o5 = 0, o10 = 0, o50 = 0, o1ms = 0;
    for (i = 0; i < n; i++) {
        if (extc_probe_d[i] > 5000) o5++;
        if (extc_probe_d[i] > 10000) o10++;
        if (extc_probe_d[i] > 50000) o50++;
        if (extc_probe_d[i] > 1000000) o1ms++;
    }
    fprintf(stderr, "[probe-seg] n=%lld over5us=%lld over10us=%lld over50us=%lld over1ms=%lld",
            n, o5, o10, o50, o1ms);
    if (n > 0) {
        long long *t = (long long *)malloc((size_t)n * sizeof *t);
        if (t) {
            memcpy(t, extc_probe_d, (size_t)n * sizeof *t);
            qsort(t, (size_t)n, sizeof *t, extc_probe_cmp);
            fprintf(stderr, " p50=%.3f p90=%.3f p99=%.3f p999=%.3f max=%.3f",
                    t[n/2]/1000.0, t[n*9/10]/1000.0, t[(long long)(n*0.99)]/1000.0,
                    t[(long long)(n*0.999)]/1000.0, t[n-1]/1000.0);
            free(t);
        }
    }
    fprintf(stderr, "\n");
}
__attribute__((destructor)) static void extc_probe_fini(void) { extc_probe_dump(); }
'''

ANCHOR = "        extc_rc_stepNote(_d);\nif (seg > 0) {"
INJECT = ("        extc_rc_stepNote(_d);\n"
          "if (seg > 0 && extc_probe_n < 4000000) extc_probe_d[extc_probe_n++] = _d;\n"
          "if (seg > 0) {")
DECL_ANCHOR = "__attribute__((unused)) static inline void extc_rc_stepNote(long long d) {"


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, **kw)


def inject_probe(cpath):
    """把 [probe-seg] 探针注入生成物（只在 /tmp 的副本上改，不碰 src/）。"""
    s = open(cpath, encoding="utf-8", errors="surrogateescape").read()
    if s.count(ANCHOR) != 1:
        raise SystemExit("probe anchor 命中 %d 次（期望 1）；生成物结构变了，请重新核对 markStep"
                         % s.count(ANCHOR))
    if s.count(DECL_ANCHOR) != 1:
        raise SystemExit("probe decl anchor 命中 %d 次（期望 1）" % s.count(DECL_ANCHOR))
    s = s.replace(DECL_ANCHOR, PROBE_GLOB + "\n" + DECL_ANCHOR, 1)
    s = s.replace(ANCHOR, INJECT, 1)
    out = cpath[:-2] + ".probe.c"
    open(out, "w", encoding="utf-8", errors="surrogateescape").write(s)
    return out


def product_chunk(cpath):
    s = open(cpath, encoding="utf-8", errors="surrogateescape").read()
    m = re.search(r"#define\s+EXTC_RC_SCAN_CHUNK\s+(\d+)", s)
    if not m:
        raise SystemExit("生成物里找不到 EXTC_RC_SCAN_CHUNK")
    return int(m.group(1))


def parse_probe_line(txt):
    m = re.search(r"\[probe-seg\]\s+(.*)", txt or "")
    if not m:
        return None
    d = {}
    for k, v in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([-0-9.]+)", m.group(1)):
        d[k] = float(v) if "." in v else int(v)
    if "n" not in d:
        return None
    return d


def cseg_from_p999(p999_us, chunk):
    return float(p999_us) * 1000.0 / float(chunk)


def median(vals):
    return float(statistics.median(vals)) if vals else None


def read_cal(path):
    """读 bseg-cal.tsv：返回 (headers, rows, order)；rows 为 dict。"""
    headers, rows, order = [], {}, []
    if not os.path.exists(path):
        return headers, rows, order
    for line in open(path, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            headers.append(line.rstrip("\n"))
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 3:
            continue
        shape = parts[0]
        row = {"shape": shape}
        keys = ["shape", "chunk", "c_seg_ns", "p99.9_us", "p99.9_min_us",
                "p99.9_max_us", "run_id", "src_md5", "extc_md5"]
        for i, k in enumerate(keys):
            row[k] = parts[i] if i < len(parts) else ""
        if shape not in rows:
            order.append(shape)
        rows[shape] = row
    return headers, rows, order


def write_cal(path, headers, rows, order):
    lines = list(headers) if headers else []
    for shape in order:
        r = rows[shape]
        lines.append("\t".join(str(r.get(k, "")) for k in
                     ["shape", "chunk", "c_seg_ns", "p99.9_us", "p99.9_min_us",
                      "p99.9_max_us", "run_id", "src_md5", "extc_md5"]))
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, path)


def fmt_cseg(x):
    return "%.3f" % x


def fmt_us(x):
    return "%.2f" % x


def calibrate_shape(extc, cc, cflags, shape, outdir, reps, pin, chunk=None):
    cdir = os.path.join(outdir, "c")
    bdir = os.path.join(outdir, "bin")
    os.makedirs(cdir, exist_ok=True)
    os.makedirs(bdir, exist_ok=True)
    cpath = os.path.join(cdir, "cal_%s.c" % shape)
    r = run([extc, "-w", "--no-line-map", "-I", os.path.join(ROOT, "stdlib"),
             os.path.join(HERE, "src", SHAPES[shape]), "-o", cpath])
    if r.returncode != 0:
        raise SystemExit("FAIL extc %s: %s" % (shape, (r.stderr or r.stdout)[-800:]))
    chunk_prod = product_chunk(cpath)
    if chunk is not None and chunk != chunk_prod:
        raise SystemExit("--chunk=%d 与生成物 EXTC_RC_SCAN_CHUNK=%d 不一致" % (chunk, chunk_prod))
    probec = inject_probe(cpath)
    binp = os.path.join(bdir, "cal_%s" % shape)
    r = run([cc] + cflags.split() + ["-o", binp, probec])
    if r.returncode != 0:
        raise SystemExit("FAIL cc %s: %s" % (shape, (r.stderr or r.stdout)[-800:]))
    env = dict(os.environ)
    env["EXTC_RC_PAUSE"] = "1"
    env["EXTC_RC_STEP_CPU"] = "1"
    reps_out = []
    for i in range(1, reps + 1):
        cmd = [binp, str(ROUNDS)]
        if pin is not None and pin >= 0:
            taskset = shutil.which("taskset")
            if taskset:
                cmd = [taskset, "-c", str(pin)] + cmd
        t0 = time.time()
        p = run(cmd, env=env)
        wall = time.time() - t0
        pr = parse_probe_line(p.stderr)
        if p.returncode != 0 or pr is None:
            raise SystemExit("FAIL run %s rep%d rc=%s probe=%r stderr_tail=%s"
                             % (shape, i, p.returncode, pr, (p.stderr or "")[-400:]))
        if pr.get("n", 0) < 100000:
            print("  ⚠ %s rep%d: seg 步 n=%s < 1e5（p99.9 样本不足，记 N/A）"
                  % (shape, i, pr.get("n")), file=sys.stderr)
        reps_out.append({"rep": i, "probe": pr, "wall_s": round(wall, 3), "rc": p.returncode})
        with open(os.path.join(outdir, "log", "cal_%s_r%d.err" % (shape, i)), "w") as f:
            f.write(p.stderr)
    p999 = [x["probe"]["p999"] for x in reps_out]
    med = median(p999)
    cseg = cseg_from_p999(med, chunk_prod)
    return {"shape": shape, "chunk": chunk_prod, "reps": reps_out,
            "p999_us": med, "p999_min_us": min(p999), "p999_max_us": max(p999),
            "c_seg_ns": cseg}


def _selftest():
    bad = []

    def ck(ok, msg):
        if not ok:
            bad.append(msg)

    txt = ("[probe-seg] n=25507 over5us=17584 over10us=15000 over50us=0 over1ms=0 "
           "p50=1.000 p90=3.000 p99=10.000 p999=34.080 max=157.100")
    d = parse_probe_line(txt)
    ck(d["n"] == 25507 and d["over5us"] == 17584 and d["over1ms"] == 0, "S1 n/over 解析错: %r" % (d,))
    ck(abs(d["p999"] - 34.08) < 1e-9, "S1 p999 解析错: %r" % (d.get("p999"),))
    ck(parse_probe_line("no probe here") is None, "S2 缺 [probe-seg] 应返回 None")
    ck(parse_probe_line("[probe-seg] n=0")["n"] == 0, "S2b n=0 应可解析")
    ck(abs(cseg_from_p999(34.08, 8192) - 4.160) < 1e-3, "S3 c_seg 计算错: %r" % (cseg_from_p999(34.08, 8192),))
    ck(abs(cseg_from_p999(56.40, 8192) - 6.885) < 1e-3, "S3b dense c_seg 计算错: %r" % (cseg_from_p999(56.40, 8192),))
    ck(median([34.08, 47.53, 40.0]) == 40.0, "S4 median 错: %r" % (median([34.08, 47.53, 40.0]),))
    # 9 列读写往返
    td = tempfile.mkdtemp()
    p = os.path.join(td, "cal.tsv")
    with open(p, "w") as f:
        f.write("# header line one\n# header line two\n")
        f.write("pd_bigobj\t8192\t4.160\t34.08\t34.08\t47.53\trun-old\tunknown\tunknown\n")
    h, rows, order = read_cal(p)
    ck(len(h) == 2 and order == ["pd_bigobj"], "S5 读标定表错: %r %r" % (h, order))
    rows["pd_pointer_dense"] = {"shape": "pd_pointer_dense", "chunk": 8192, "c_seg_ns": "6.885",
                                "p99.9_us": "56.40", "p99.9_min_us": "56.40",
                                "p99.9_max_us": "65.05", "run_id": "cal-test",
                                "src_md5": "aa", "extc_md5": "bb"}
    order.append("pd_pointer_dense")
    write_cal(p, h, rows, order)
    h2, rows2, order2 = read_cal(p)
    ck(len(h2) == 2 and order2 == ["pd_bigobj", "pd_pointer_dense"], "S6 写回后顺序/表头错: %r %r" % (h2, order2))
    ck(rows2["pd_pointer_dense"]["p99.9_max_us"] == "65.05", "S6b 写回丢列: %r" % (rows2["pd_pointer_dense"],))
    ck(rows2["pd_bigobj"]["src_md5"] == "unknown", "S6c 旧行应保留: %r" % (rows2["pd_bigobj"],))
    # 产品宏 chunk 解析
    tdf = os.path.join(td, "x.c")
    with open(tdf, "w") as f:
        f.write("#define EXTC_RC_SCAN_CHUNK 8192\n")
    ck(product_chunk(tdf) == 8192, "S7 chunk 解析错")
    if bad:
        print("SELFTEST FAIL: " + "; ".join(bad))
        return 1
    print("S1 [probe-seg] 解析 / S2 缺失与 n=0 / S3 c_seg 公式 / S4 median")
    print("S5-S6 9 列标定表读写往返（表头保留、旧行保留、新行追加） / S7 chunk 宏解析")
    print("⇒ SELFTEST PASS")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="c_seg 标定（Lead m04755 C）")
    ap.add_argument("--extc", default=os.path.join(ROOT, "build-emp3", "extc"))
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--cflags", default=DEFAULT_CFLAGS)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--cal", default=DEFAULT_CAL)
    ap.add_argument("--shapes", default="pd_bigobj,pd_pointer_dense")
    ap.add_argument("--reps", type=int, default=REPS)
    ap.add_argument("--pin", type=int, default=PIN, help="-1 = 不 pin")
    ap.add_argument("--chunk", type=int, default=None, help="期望的生成物 chunk（不一致即报错）")
    ap.add_argument("--src-md5", default="")
    ap.add_argument("--build-md5", default="")
    ap.add_argument("--no-update", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return _selftest()
    os.makedirs(os.path.join(a.out, "log"), exist_ok=True)
    results = {}
    for shape in [s.strip() for s in a.shapes.split(",") if s.strip()]:
        if shape not in SHAPES:
            raise SystemExit("未知形状 %r（可选 %s）" % (shape, ",".join(SHAPES)))
        r = calibrate_shape(a.extc, a.cc, a.cflags, shape, a.out, a.reps, a.pin, a.chunk)
        results[shape] = r
        print("  %-17s chunk=%d  p99.9=%s..%s us（中位 %s）  c_seg=%s ns/word  n=%s"
              % (shape, r["chunk"], fmt_us(r["p999_min_us"]), fmt_us(r["p999_max_us"]),
                 fmt_us(r["p999_us"]), fmt_cseg(r["c_seg_ns"]),
                 ",".join(str(x["probe"]["n"]) for x in r["reps"])))
    meta = {"ts": time.strftime("%Y-%m-%dT%H:%M:%S"), "extc": os.path.abspath(a.extc),
            "extc_md5": a.build_md5, "src_md5": a.src_md5, "cflags": a.cflags,
            "reps": a.reps, "pin": a.pin, "rounds": ROUNDS, "chunk": None, "results": results}
    with open(os.path.join(a.out, "calib_cseg.json"), "w") as f:
        json.dump(meta, f, indent=1)
    if not a.no_update:
        h, rows, order = read_cal(a.cal)
        for shape, r in results.items():
            row = {"shape": shape, "chunk": r["chunk"], "c_seg_ns": fmt_cseg(r["c_seg_ns"]),
                   "p99.9_us": fmt_us(r["p999_us"]), "p99.9_min_us": fmt_us(r["p999_min_us"]),
                   "p99.9_max_us": fmt_us(r["p999_max_us"]),
                   "run_id": "cal:%s:reps=%d:pin=%s" % (meta["ts"], a.reps,
                                                        "none" if a.pin < 0 else a.pin),
                   "src_md5": a.src_md5 or "unknown", "extc_md5": a.build_md5 or "unknown"}
            if shape in rows:
                rows[shape] = row
            else:
                rows[shape] = row
                order.append(shape)
        write_cal(a.cal, h, rows, order)
        print("  → 已写 %s（9 列，spread=%s..%s）" % (a.cal, "min..max", ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
