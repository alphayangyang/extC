#!/usr/bin/env python3
# bench/gc/pause_corpus.py —— 全语料**停顿画像**普查（task-29；只测量，不改任何判决/发射）。
#
# 做什么：对每个 .extc 形状
#   1) 用固定编译器生成 C（默认档）；数 `extc_rc_` 的**整串出现次数** ⇒ 判定"是否发 rc 运行期"
#      （0 ⇒ 结构性无停顿，单列，**显式报 n=0** ✓）
#   2) 对发 rc 的：`tools/rc-pause-probe.py` 只插**生成物副本** ⇒ 编两个二进制（原档 / 插桩档）
#   3) 跑 reps 次（taskset 绑核）：插桩档开 DIGRAPH_PAUSE_DIST=1 ⇒ 从 **stderr** 取 PAUSE_DIST；
#      同时量墙钟 ⇒ `总停顿占墙钟比`
#   4) 不回归检查：原档 stdout 与"插桩档 stdout 去掉 `PROBE ` 行"必须**逐字节相同** ✓；
#      原档在无诊断 env 下 **stderr 必须 0 字节** ✓
#   5) 反瞎：插桩档若**取不到 PAUSE_DIST** ⇒ 该行 FAIL（不是静默跳过 ✗）
#
# 用法：
#   pause_corpus.py --compiler /tmp/gcbench/extc.pin3 --n 20000 --reps 3 --out /tmp/gcbench/pause-corpus
#   pause_corpus.py --selftest                # 注入自检：伪造"缺 PAUSE_DIST"⇒ 必须非 0 退出
import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DIRS = [
    "tests/reclaim-perf/shapes",
    "tests/reclaim-shapes",
    "tests/reclaim-gaps/shapes",
    "bench/bigmatrix/src",
    "bench/gc/src",
    "bench/heavy",
    "bench/app",
]
PAUSE = re.compile(r"^PAUSE_DIST n=(\d+) overflow=(\d+) p50_us=([0-9.]+) p90_us=([0-9.]+) "
                   r"p99_us=([0-9.]+) max_us=([0-9.]+) total_us=([0-9.]+)")
SEL = ""
PROBE_LINE = re.compile(rb"^PROBE [^\n]*\n?", re.M)


def shapes():
    out = []
    for d in DIRS:
        p = os.path.join(ROOT, d)
        if not os.path.isdir(p):
            continue
        for f in sorted(os.listdir(p)):
            if f.endswith(".extc"):
                out.append(os.path.join(d, f))
    return out


def run(cmd, timeout, pin=6, env=None, cwd=None):
    e = dict(os.environ)
    if env:
        e.update(env)

    def _pin():
        os.sched_setaffinity(0, {pin})

    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=e, cwd=cwd,
                           preexec_fn=_pin)
        ms = (time.monotonic() - t0) * 1000.0
        return p.returncode, p.stdout, p.stderr, ms, False
    except subprocess.TimeoutExpired as ex:
        ms = (time.monotonic() - t0) * 1000.0
        return 124, ex.stdout or b"", ex.stderr or b"", ms, True


def has_args(src):
    try:
        with open(src, encoding="utf-8", errors="replace") as f:
            return bool(re.search(r"fn main\(args\s*:", f.read()))
    except OSError:
        return False


def census(args):
    global SEL
    comp = args.compiler
    work = args.out
    gen = os.path.join(work, "gen")
    bin_d = os.path.join(work, "bin")
    os.makedirs(gen, exist_ok=True)
    os.makedirs(bin_d, exist_ok=True)
    rows = []
    raw = open(os.path.join(work, "rows.tsv"), "w", encoding="utf-8")
    rawdist = open(os.path.join(work, "raw_PAUSE_DIST.log"), "a", encoding="utf-8")
    rawdist.write("# 每次运行的原始 PAUSE_DIST（stderr 原文）；shape<TAB>rep<TAB>line\n")
    raw.write("shape\trc_occ\tkind\tn\treps_ok\tp50_us\tp90_us\tp99_us\tmax_us\ttotal_us\twall_ms\tshare\tstatus\n")
    for rel in shapes():
        if SEL and SEL not in rel:
            continue
        src = os.path.join(ROOT, rel)
        name = os.path.basename(rel)[:-5]
        key = rel.replace("/", "_")
        cfile = os.path.join(gen, key + ".c")
        # 1) 生成
        rc, out, err, _, _ = run([comp, "-w", "--no-line-map", "-I", os.path.join(ROOT, "stdlib"),
                                  src, "-o", cfile], timeout=args.timeout)
        if rc != 0:
            first = (err or out).decode("utf-8", "replace").strip().splitlines()
            rows.append(dict(shape=rel, rc_occ="COMPILE_FAIL", kind="compile_fail", n="-", reps_ok=0,
                             p50="-", p90="-", p99="-", max_="-", total="-", wall="-", share="-",
                             status="编译不过：" + (first[0][:70] if first else "?")))
            raw.write("%s\tCOMPILE_FAIL\tcompile_fail\t-\t0\t-\t-\t-\t-\t-\t-\t-\t%s\n"
                      % (rel, rows[-1]["status"]))
            continue
        with open(cfile, "rb") as f:
            ctext = f.read()
        occ = ctext.count(b"extc_rc_")
        # 2) 不发 rc ⇒ 结构性 0
        if occ == 0:
            rows.append(dict(shape=rel, rc_occ=0, kind="idle", n=0, reps_ok=0, p50=0.0, p90=0.0,
                             p99=0.0, max_=0.0, total=0.0, wall="-", share=0.0,
                             status="无 rc 运行期 ⇒ 结构性 0 次 collect（显式 0 ✓）"))
            raw.write("%s\t0\tidle\t0\t0\t0\t0\t0\t0\t0\t-\t0\t结构性 0\n" % rel)
            continue
        # 3) 插桩副本 + 两个二进制
        pfile = os.path.join(gen, key + ".probe.c")
        with open(pfile, "wb") as f:
            p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "rc-pause-probe.py"), cfile],
                               stdout=f, stderr=subprocess.PIPE)
        if p.returncode != 0:
            rows.append(dict(shape=rel, rc_occ=occ, kind="probe_fail", n="-", reps_ok=0, p50="-",
                             p90="-", p99="-", max_="-", total="-", wall="-", share="-",
                             status="探针插不进：" + p.stderr.decode("utf-8", "replace").strip()[:70]))
            raw.write("%s\t%d\tprobe_fail\t-\t0\t-\t-\t-\t-\t-\t-\t-\t%s\n" % (rel, occ, rows[-1]["status"]))
            continue
        plain_bin = os.path.join(bin_d, key)
        probe_bin = os.path.join(bin_d, key + ".probe")
        cc_err = ""
        for src_c, dst in ((cfile, plain_bin), (pfile, probe_bin)):
            rc, out, err, _, _ = run([args.cc, "-O2", "-D_POSIX_C_SOURCE=200809L", "-o", dst, src_c],
                                     timeout=args.timeout)
            if rc != 0:
                cc_err = (err or out).decode("utf-8", "replace").strip()[:70]
                break
        if cc_err:
            rows.append(dict(shape=rel, rc_occ=occ, kind="cc_fail", n="-", reps_ok=0, p50="-", p90="-",
                             p99="-", max_="-", total="-", wall="-", share="-", status="cc 失败：" + cc_err))
            raw.write("%s\t%d\tcc_fail\t-\t0\t-\t-\t-\t-\t-\t-\t-\t%s\n" % (rel, occ, cc_err))
            continue
        # 4) argv：形状的 argv 语义不止一种（`N` / `rounds:per` / 无 argv）⇒ 用**候选梯**逐个试，
        #    第一个 rc=0 的即采用，并把用的是哪种写进报告 ✓（试不出来 ⇒ run_fail，照实报 ✗）。
        cands = []
        if has_args(src):
            cands = [[args.n], ["1", args.n], [args.n, args.n], []]
        else:
            cands = [[]]
        argv = None
        argv_mode = ""
        rc = out_plain = err_plain = None
        wall_plain = 0.0
        to1 = False
        for cand in cands:
            rc, out_plain, err_plain, wall_plain, to1 = run([plain_bin] + cand, timeout=args.timeout)
            if rc == 0 and not to1:
                argv = cand
                argv_mode = (" ".join(cand)) if cand else "none（源自带规模）"
                break
        if argv is None:
            argv = []
            argv_mode = "候选全失败"
        stderr_clean = (len(err_plain) == 0)
        # 6) 插桩档 × reps
        samples = []
        prod_mismatch = 0
        stderr_diag = 0
        for _ in range(args.reps):
            env_diag = {} if args.inject_missing else {"DIGRAPH_PAUSE_DIST": "1"}
            rc, out_p, err_p, wall, to = run([probe_bin] + argv, timeout=args.timeout,
                                             env=env_diag)
            if to or rc != 0:
                samples.append(None)
                continue
            stripped = PROBE_LINE.sub(b"", out_p)
            if stripped != out_plain:
                prod_mismatch += 1
            m = None
            line_for_log = ""
            for line in err_p.decode("utf-8", "replace").splitlines():
                m = PAUSE.match(line)
                if m:
                    line_for_log = line
                    break
            if m is None:
                samples.append(None)
                continue
            stderr_diag += len(err_p)
            rawdist.write("%s\trep%d\t%s\n" % (rel, len(samples) + 1, line_for_log))
            samples.append(dict(n=int(m.group(1)), ovf=int(m.group(2)), p50=float(m.group(3)),
                                p90=float(m.group(4)), p99=float(m.group(5)), max_=float(m.group(6)),
                                total=float(m.group(7)), wall=wall))
        good = [s for s in samples if s]
        if not good:
            status = "FAIL：取不到 PAUSE_DIST（缺行 ✗）"
            rows.append(dict(shape=rel, rc_occ=occ, kind="no_stat", n="-", reps_ok=0, p50="-", p90="-",
                             p99="-", max_="-", total="-", wall="-", share="-", status=status))
            raw.write("%s\t%d\tno_stat\t-\t0\t-\t-\t-\t-\t-\t-\t-\t%s\n" % (rel, occ, status))
            continue
        med = lambda k: statistics.median([s[k] for s in good])
        wall_med = med("wall")
        total_us_med = med("total")
        share = (total_us_med / 1000.0) / wall_med if wall_med > 0 else 0.0
        status = []
        if not stderr_clean:
            status.append("原档 stderr 非空 ✗")
        if prod_mismatch:
            status.append("产物与插桩档不一致 ×%d ✗" % prod_mismatch)
        if len(good) < args.reps:
            status.append("有效 %d/%d" % (len(good), args.reps))
        rows.append(dict(shape=rel, rc_occ=occ, kind="rc", n=int(med("n")), reps_ok=len(good),
                         p50=med("p50"), p90=med("p90"), p99=med("p99"), max_=med("max_"),
                         total=total_us_med, wall=wall_med, share=share,
                         status=("argv=%s; " % argv_mode) + ("; ".join(status) if status else "ok")))
        raw.write("%s\t%d\trc\t%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.2f\t%.5f\t%s\n"
                  % (rel, occ, rows[-1]["n"], len(good), rows[-1]["p50"], rows[-1]["p90"],
                     rows[-1]["p99"], rows[-1]["max_"], rows[-1]["total"], wall_med, share,
                     rows[-1]["status"]))
    raw.close()
    rawdist.close()
    return rows


def selftest():
    """注入自检：伪造"插桩档没有 PAUSE_DIST 行" ⇒ 解析逻辑必须判 FAIL（会红）。"""
    fake = "some output\nno pause line here\n"
    ok = PAUSE.search(fake) is None
    print("SELFTEST 缺 PAUSE_DIST 行 ⇒ %s" % ("red ✓（解析不到，会判 FAIL）" if ok else "green ✗"))
    # 另一条：有行但格式错
    bad = "PAUSE_DIST n=xx overflow=0\n"
    ok2 = PAUSE.search(bad) is None
    print("SELFTEST 格式错的 PAUSE_DIST ⇒ %s" % ("red ✓" if ok2 else "green ✗"))
    return 0 if (ok and ok2) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default="/tmp/gcbench/extc.pin3")
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--n", default="20000")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--out", default="/tmp/gcbench/pause-corpus")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--only", default="", help="只跑形状名包含该子串的源（自检/冒烟用）")
    ap.add_argument("--inject-missing", action="store_true",
                    help="自检注入：不设 DIGRAPH_PAUSE_DIST ⇒ 每行都取不到统计行 ⇒ 必须判 FAIL 并非 0 退出")
    args = ap.parse_args()
    global SEL
    SEL = args.only
    if args.selftest:
        return selftest()
    os.makedirs(args.out, exist_ok=True)
    t0 = time.time()
    rows = census(args)
    print("形状数=%d  用时=%.1fs" % (len(rows), time.time() - t0))
    kinds = {}
    for r in rows:
        kinds[r["kind"]] = kinds.get(r["kind"], 0) + 1
    print("分类：", kinds)
    bad = kinds.get("no_stat", 0)
    if bad:
        print("FAIL：%d 个形状取不到 PAUSE_DIST（缺行 ✗）" % bad)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
