#!/usr/bin/env python3
"""userstall.py —— "用户视角的停顿"横评驱动器（task-87）。

口径（写死在 bench/gc/USER-STALL.md §1，本文件是它的实现）：
- **停顿只由应用侧时间戳定义**：每个业务单位边界打一次戳（形状源码里打，见 stamped/make_stamped.py），
  单位延迟 = 相邻戳之差；停顿 = 单位延迟的长尾（> p99 或 > 预算）。
- GC 侧计时（`extc_rc_collect`、Go gctrace、JVM -Xlog:gc）**只用于事后解释/对照**，不参与命中判定，
  也不参与"停顿时长"，本驱动器不读它们来做任何统计。
- **全默认档**：不加任何调优旗标、不 pin 核、不设堆上限、不开关 GC。所有命令**原文**写进日志。
  唯一显式传入的是 argv（工作量规模），且同形状三语言完全一致。

用法：
  python3 bench/gc/userstall.py --build --reps 5          # 全流程
  python3 bench/gc/userstall.py --reps 1 --shapes P5      # 冒烟
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time

GC = os.path.dirname(os.path.abspath(__file__))          # bench/gc
ROOT = os.path.dirname(os.path.dirname(GC))              # repo root
STAMPED = os.path.join(GC, "stamped")
WORK = "/tmp/userstall"
BIN = os.path.join(WORK, "bin")
CLASSES_CANON = os.path.join(WORK, "classes_canon")
CLASSES_US = os.path.join(WORK, "classes_us")
LOGS = os.path.join(WORK, "logs")
OUTS = os.path.join(WORK, "out")
STAMPS = os.path.join(WORK, "stamps")
EXTC = os.path.join(ROOT, "build", "extc")
STDLIB = os.path.join(ROOT, "stdlib")
RUNNER = os.path.join(BIN, "digraph_run")
JAVA = "java"
JAVAC = "javac"
GO = "go"
CC = "cc"

# ---------------- 形状表 ----------------
# argv：**同形状三语言完全一致**（默认档只约束运行期旗标，不约束工作量；
# 规模按 bench/gc/APP-STALL.md §0 的单位数取，写进报告）。
SHAPES = {
    "P1": dict(argv=["200000", "20261013"],
               extc="spectrum_p1", go="spectrum_p1", java=("SpectrumP1",),
               units=lambda a: int(a[0]),                       # 一次分配 = 补充口径（§0：P1 无跨单位结构）
               note="单位=一次分配（补充口径；APP-STALL §0 说 P1 无业务边界，只作整段对照）"),
    "P2": dict(argv=["1000000", "20261013"],
               extc="spectrum_p2", go="spectrum_p2", java=("SpectrumP2",),
               units=lambda a: 0 if int(a[0]) == 0 else (int(a[0]) + 255) // 256,
               note="单位=一个追加块(256 次 append)"),
    "P3": dict(argv=["5000", "20261013"],
               extc="spectrum_p3", go="spectrum_p3", java=("SpectrumP3",),
               units=lambda a: int(a[0]),
               note="单位=一轮（建链+遍历+丢）"),
    "P4": dict(argv=["2000", "20261013"],
               extc="spectrum_p4", go="spectrum_p4", java=("SpectrumP4",),
               units=lambda a: max(int(a[0]), 8),
               note="单位=一个编译单元"),
    "P5": dict(argv=["200000", "20261013"],
               extc="spectrum_p5", go="spectrum_p5", java=("SpectrumP5",),
               units=lambda a: int(a[0]),
               note="单位=单次操作；另并列 批256（floor(ops/256) 个整批）"),
    "P6": dict(argv=["200000", "20261013"],
               extc="digraph", go="digraph", java=("digraph",),
               units=lambda a: 16 + int(a[0]) // 4096,
               note="单位=一轮（digraph 完整构建/遍历轮）"),
}
RUNTIMES = ("extc", "go", "java")
BUDGETS_NS = [100_000, 1_000_000, 16_700_000]      # 0.1ms / 1ms / 16.7ms


def sh(cmd, **kw):
    """跑一条命令，返回 (rc, stdout, stderr, 原文)。原文进日志。"""
    p = subprocess.run(cmd, capture_output=True, **kw)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace"), " ".join(cmd)


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(65536), b""):
            h.update(b)
    return h.hexdigest()


def loadavg():
    with open("/proc/loadavg") as f:
        return f.read().split()[0]


def pct(sorted_vals, p):
    n = len(sorted_vals)
    if n == 0:
        return None
    if n == 1:
        return float(sorted_vals[0])
    idx = p * (n - 1)
    lo = int(math.floor(idx))
    hi = min(lo + 1, n - 1)
    frac = idx - lo
    return sorted_vals[lo] * (1.0 - frac) + sorted_vals[hi] * frac


# ---------------- 构建 ----------------
BUILD_LOG = []


def blog(line):
    BUILD_LOG.append(line)
    print("[build] " + line)


def build_all(need_build):
    os.makedirs(BIN, exist_ok=True)
    for d in (LOGS, OUTS, STAMPS, CLASSES_CANON, CLASSES_US):
        os.makedirs(d, exist_ok=True)
    if not need_build:
        return
    # 垫片（只读复用 bench/gc/digraph_run.c，不改它）
    rc, _, e, cmd = sh([CC, "-O2", "-o", RUNNER, os.path.join(GC, "digraph_run.c")])
    blog(f"rc={rc} {cmd}")
    if rc != 0:
        sys.exit("digraph_run 构建失败: " + e)
    for name, sp in SHAPES.items():
        src = os.path.join(GC, "src", sp["extc"] + ".extc")
        us = os.path.join(STAMPED, "extc", sp["extc"] + "_us.extc")
        for tag, s, out in (("canon", src, f"{sp['extc']}_c"), ("us", us, f"{sp['extc']}_us_c")):
            cpath = os.path.join(WORK, out + ".c")
            rc, _, e, cmd = sh([EXTC, "-w", "--no-line-map", "-I", STDLIB, s, "-o", cpath])
            blog(f"rc={rc} {cmd}")
            if rc != 0:
                sys.exit(f"extc 编译失败 {s}: {e}")
            rc, _, e, cmd = sh([CC, "-O2", "-o", os.path.join(BIN, out), cpath])
            blog(f"rc={rc} {cmd}")
            if rc != 0:
                sys.exit(f"cc 编译失败 {cpath}: {e}")
        src = os.path.join(GC, "src", sp["go"] + ".go")
        us = os.path.join(STAMPED, "go", sp["go"] + "_us.go")
        for tag, s, out in (("canon", src, f"{sp['go']}_go"), ("us", us, f"{sp['go']}_us_go")):
            rc, _, e, cmd = sh([GO, "build", "-o", os.path.join(BIN, out), s], cwd=ROOT)
            blog(f"rc={rc} {cmd}")
            if rc != 0:
                sys.exit(f"go build 失败 {s}: {e}")
    # Java：canon = stamped/src 的五个副本 + bench/gc/src/digraph.java；us = stamped/java
    rc, _, e, cmd = sh([JAVAC, "-d", CLASSES_CANON] +
                       [os.path.join(STAMPED, "src", f"SpectrumP{i}.java") for i in range(1, 6)] +
                       [os.path.join(GC, "src", "digraph.java")])
    blog(f"rc={rc} {cmd}")
    if rc != 0:
        sys.exit("javac canon 失败: " + e)
    us_java = sorted(os.path.join(STAMPED, "java", f) for f in os.listdir(os.path.join(STAMPED, "java")))
    rc, _, e, cmd = sh([JAVAC, "-d", CLASSES_US] + us_java)
    blog(f"rc={rc} {cmd}")
    if rc != 0:
        sys.exit("javac us 失败: " + e)
    build_cfg_programs()


def build_cfg_programs():
    go_cfg = os.path.join(STAMPED, "cfg.go")
    rc, _, e, cmd = sh([GO, "build", "-o", os.path.join(BIN, "cfg_go"), go_cfg], cwd=ROOT)
    blog(f"rc={rc} {cmd}")
    if rc != 0:
        sys.exit("cfg.go 构建失败: " + e)


# ---------------- 配置打印（硬要求） ----------------
def config_report(fh):
    def w(s):
        fh.write(s + "\n")
        print(s)
    w("# ---- 配置打印（出厂默认档核对；本段命令与计时轮**分开**，日志留 /tmp/userstall/logs/config.txt）----")
    w(f"# 采集时间 {time.strftime('%F %T %z')}   load1={loadavg()}  nproc={os.cpu_count()}")
    with open("/proc/cpuinfo") as f:
        cpu = re.search(r"model name\s*:\s*(.+)", f.read())
    w(f"# CPU: {cpu.group(1).strip() if cpu else '?'}  nproc={os.cpu_count()}")
    with open("/proc/meminfo") as f:
        w("# " + [l.strip() for l in f if l.startswith("MemTotal")][0])
    # extC
    w(f"[extC] compiler={EXTC} md5={md5(EXTC)} mtime={time.strftime('%F %T', time.localtime(os.path.getmtime(EXTC)))}")
    w("[extC] 编译开关（原文，逐形状同一套）：" + f"{EXTC} -w --no-line-map -I {STDLIB} <src> -o <gen.c> ; cc -O2 -o <bin> <gen.c>")
    w("[extC] 环境开关：EXTC_* **未设置**（env -u 语义；本驱动不导出任何 EXTC_*）")
    for name, sp in SHAPES.items():
        gen = os.path.join(WORK, f"{sp['extc']}_us_c.c")
        if os.path.exists(gen):
            txt = open(gen, encoding="utf-8", errors="replace").read()
            w(f"[extC] {name}: 生成物 {os.path.basename(gen)} extc_rc_collect 出现 {txt.count('extc_rc_collect')} 处；"
              f"是否发射回收运行期={'是' if 'extc_rc_collect' in txt else '否（结构性 0）'}")
    # Go
    rc, out, err, cmd = sh([os.path.join(BIN, "cfg_go")])
    w(f"[Go] cmd={cmd} rc={rc}")
    w("[Go] " + out.strip().replace("\n", " | "))
    w(f"[Go] env GOGC={'<unset>' if 'GOGC' not in os.environ else os.environ['GOGC']}  "
      f"GOMEMLIMIT={'<unset>' if 'GOMEMLIMIT' not in os.environ else os.environ['GOMEMLIMIT']}  "
      f"GOMAXPROCS={'<unset>' if 'GOMAXPROCS' not in os.environ else os.environ['GOMAXPROCS']}")
    # Java
    rc, out, err, cmd = sh([JAVA, "-XX:+PrintCommandLineFlags", "-version"])
    w(f"[JVM] cmd={cmd} rc={rc}（仅打印旗标，不计时）")
    for line in (out + "\n" + err).splitlines():
        if line.startswith("-XX"):
            w("[JVM] " + line)
    rc, out, err, cmd = sh([JAVA, "-Xlog:gc:stderr", "-cp", CLASSES_CANON, "SpectrumP1", "2000", "20261013"])
    using = [l for l in err.splitlines() if "Using" in l]
    w(f"[JVM] cmd={cmd} rc={rc}（仅取 Using 行，输出丢弃）")
    for l in using:
        w("[JVM] " + l)
    w("[JVM] 环境开关：JAVA_TOOL_OPTIONS/_-JAVA_OPTIONS/_JAVA_OPTIONS 未设置；无 -Xmx/-XX:*GC*")


# ---------------- 单次运行 ----------------
def run_once(cmdline, kind, arm, rep):
    """跑一次；返回 dict。stdout/stderr 落盘。"""
    outfile = os.path.join(OUTS, f"{arm}_{kind}_{rep}.out")
    errfile = os.path.join(STAMPS, f"{arm}_{kind}_{rep}.err")
    pre = loadavg()
    runner = [RUNNER, "--out", outfile]
    if kind == "us":
        runner += ["--err", errfile]
    full = runner + ["--"] + cmdline
    t0 = time.time()
    rc, so, se, cmdstr = sh(full)
    dt = time.time() - t0
    if rc != 0:
        return dict(valid=False, why=f"runner rc={rc}", cmd=cmdstr, stderr=se[-400:])
    m = re.search(r"ms=([0-9.]+) rss_kb=(\d+) rc=(-?\d+)", so)
    if not m:
        return dict(valid=False, why="垫片无输出", cmd=cmdstr, stderr=so[-400:])
    wall_ms, rss_kb, child_rc = float(m.group(1)), int(m.group(2)), int(m.group(3))
    d = dict(valid=child_rc == 0, wall_ms=wall_ms, rss_kb=rss_kb, child_rc=child_rc,
             load_pre=pre, load_post=loadavg(), outfile=outfile,
             errfile=errfile if kind == "us" else None, cmd=cmdstr,
             runner_rc=rc)
    if child_rc != 0:
        d["why"] = f"子进程 rc={child_rc}"
    return d


def parse_stamps(path, expected):
    """返回 (values, meta, nlines, err)。只认 `US <int>` 与 `USMETA ...`。"""
    vals = []
    meta = {}
    if not os.path.exists(path):
        return None, None, 0, "戳文件不存在"
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("US "):
                try:
                    vals.append(int(line[3:]))
                except ValueError:
                    return None, None, len(vals), "戳行无法解析: " + line[:80]
            elif line.startswith("USMETA "):
                m = re.match(r"^USMETA n=(\d+)(?: gc=(.*))?\s*$", line)
                if m:
                    meta["n"] = m.group(1)
                    meta["gc"] = m.group(2) if m.group(2) is not None else ""
    if "n" in meta and int(meta["n"]) != len(vals):
        return None, meta, len(vals), f"USMETA n={meta['n']} != 实际戳行数 {len(vals)}"
    if expected is not None and len(vals) != expected:
        return None, meta, len(vals), f"戳数 {len(vals)} != 期望 {expected}"
    return vals, meta, len(vals), None


def latencies(vals):
    return [vals[i + 1] - vals[i] for i in range(len(vals) - 1)]


def batch_latencies(lat, bsz):
    k = len(lat) // bsz
    return [sum(lat[i * bsz:(i + 1) * bsz]) for i in range(k)]


def dist_summary(vals_ns):
    s = sorted(vals_ns)
    return dict(n=len(s),
                p50=pct(s, 0.50), p90=pct(s, 0.90), p99=pct(s, 0.99), p999=pct(s, 0.999),
                max=float(s[-1]) if s else None,
                mean=(sum(s) / len(s)) if s else None,
                over={str(b): sum(1 for v in vals_ns if v > b) for b in BUDGETS_NS})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--shapes", default="P1,P2,P3,P4,P5,P6")
    ap.add_argument("--runtimes", default="extc,go,java")
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--no-canon", action="store_true", help="跳过 canon 臂（只测插桩臂）")
    args = ap.parse_args()

    shapes = [s.strip() for s in args.shapes.split(",") if s.strip()]
    runtimes = [r.strip() for r in args.runtimes.split(",") if r.strip()]

    build_all(args.build)
    os.makedirs(LOGS, exist_ok=True)

    cfg_path = os.path.join(LOGS, "config.txt")
    with open(cfg_path, "w") as fh:
        config_report(fh)
    with open(os.path.join(LOGS, "build.log"), "w") as fh:
        fh.write("\n".join(BUILD_LOG) + "\n")

    runs = []
    results = {}
    for name in shapes:
        sp = SHAPES[name]
        argv = sp["argv"]
        units = sp["units"](argv)
        expected = units + 1
        arm_res = {}
        canon_stdout = {}
        for rt in runtimes:
            cls = sp["java"][0]
            if rt == "extc":
                canon = [os.path.join(BIN, f"{sp['extc']}_c")]
                us = [os.path.join(BIN, f"{sp['extc']}_us_c")]
            elif rt == "go":
                canon = [os.path.join(BIN, f"{sp['go']}_go")]
                us = [os.path.join(BIN, f"{sp['go']}_us_go")]
            else:
                canon = [JAVA, "-cp", CLASSES_CANON, cls]
                us = [JAVA, "-cp", CLASSES_US, cls]
            for c in ([us] if args.no_canon else [canon, us]):
                exe = c[0] if os.path.sep in c[0] else shutil.which(c[0])
                if not exe or not os.access(exe, os.X_OK):
                    sys.exit(f"二进制不可执行: {c[0]}")
            res = dict(shape=name, runtime=rt, units=units, expected_stamps=expected,
                       argv=argv, note=sp["note"], canon=[], us=[])
            kinds = ["us"] if args.no_canon else ["canon", "us"]
            for kind in kinds:
                cmdline = (canon if kind == "canon" else us) + argv
                for rep in range(args.reps):
                    arm = f"{name}_{rt}"
                    r = run_once(cmdline, kind, arm, rep)
                    r.update(shape=name, runtime=rt, kind=kind, rep=rep,
                             load=loadavg())
                    if kind == "us" and r.get("valid"):
                        vals, meta, nl, err = parse_stamps(r["errfile"], expected)
                        r["stamps"] = nl
                        r["usmeta"] = meta
                        if err:
                            r["valid"] = False
                            r["why"] = "戳自检失败: " + err
                        else:
                            lat = latencies(vals)
                            r["lat"] = lat
                            # span = 首戳到末戳（形状自身的墙钟，**不含**进程启动与末尾落盘）
                            r["span_ms"] = (vals[-1] - vals[0]) / 1e6
                            r["sum_lat_ms"] = sum(lat) / 1e6
                            if name == "P5":
                                r["lat_batch"] = batch_latencies(lat, 256)
                    runs.append(r)
                    res[kind].append(r)
            # 逐 runtime 的 stdout 一致性（canon vs us）
            for kind, group in (("canon", res["canon"]), ("us", res["us"])):
                outs = {os.path.basename(r["outfile"]) for r in group if r.get("valid")}
                if len(outs) > 1:
                    base = open(os.path.join(OUTS, sorted(outs)[0]), "rb").read()
                    for o in sorted(outs)[1:]:
                        if open(os.path.join(OUTS, o), "rb").read() != base:
                            res.setdefault("problems", []).append(f"{kind} stdout 逐次不一致: {o}")
            if res["canon"] and res["us"]:
                a = open(res["canon"][0]["outfile"], "rb").read()
                b = open(res["us"][0]["outfile"], "rb").read()
                res["canon_eq_us_stdout"] = (a == b)
                canon_stdout[rt] = a
            arm_res[rt] = res
        # 跨 runtime 逐字节一致
        ref_rt = None
        for rt, blob in canon_stdout.items():
            if ref_rt is None:
                ref_rt, ref_blob = rt, blob
            elif blob != ref_blob:
                arm_res[rt].setdefault("problems", []).append(f"stdout 与 {ref_rt} 不一致")
        results[name] = dict(units=units, expected_stamps=expected, argv=argv,
                             note=sp["note"], arms=arm_res)
        print(f"[run] {name} units={units} 期望戳={expected} 完成")

    # ---------------- 汇总 ----------------
    summary = summarize(results, args.reps)
    # JSON 里不放逐单位原始时长（几十 MB）；原始戳文件在 /tmp/userstall/stamps/ 可复算
    for name, R in results.items():
        for rt, res in R["arms"].items():
            for kind in ("canon", "us"):
                for r in res.get(kind, []):
                    r.pop("lat", None)
                    r.pop("lat_batch", None)
    with open(os.path.join(WORK, "results.json"), "w") as fh:
        json.dump(dict(config=cfg_path, reps=args.reps, shapes=shapes, runtimes=runtimes,
                       results=results, summary_text=summary["text"],
                       table={f"{k[0]}/{k[1]}": v for k, v in summary["table"].items()}),
                  fh, indent=1, sort_keys=True)
    # runs.tsv：每次一行（只含 exit code 0 的标记 valid=1）
    with open(os.path.join(LOGS, "runs.tsv"), "w") as fh:
        fh.write("shape\truntime\tkind\trep\tvalid\tchild_rc\trunner_rc\twall_ms\trss_kb\tload_pre\tload_post\tstamps\twhy\toutfile\terrfile\n")
        for r in runs:
            fh.write("\t".join(str(x) for x in [
                r["shape"], r["runtime"], r["kind"], r["rep"], int(bool(r.get("valid"))),
                r.get("child_rc", ""), r.get("runner_rc", ""),
                f"{r.get('wall_ms', float('nan')):.3f}" if r.get("wall_ms") is not None else "",
                r.get("rss_kb", ""), r.get("load_pre", r.get("load", "")), r.get("load_post", ""),
                r.get("stamps", ""), r.get("why", ""), r.get("outfile", ""), r.get("errfile") or ""]) + "\n")
    with open(os.path.join(LOGS, "summary.txt"), "w") as fh:
        fh.write(summary["text"])
    print(summary["text"])
    print(f"[done] results.json / runs.tsv / summary.txt 在 {WORK}/")


def med(xs):
    s = sorted(x for x in xs if x is not None)
    if not s:
        return None
    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2.0


def summarize(results, reps):
    lines = []
    W = lines.append
    W("== 横评汇总（单位=ns→µs；墙钟=5 次中位；RSS=5 次最大；分布=逐 rep 算后取 5 次中位）==")
    tab = {}
    for name, R in results.items():
        for rt, res in R["arms"].items():
            canon = [r for r in res.get("canon", []) if r.get("valid")]
            us = [r for r in res.get("us", []) if r.get("valid")]
            row = dict(shape=name, runtime=rt, units=R["units"], expected=R["expected_stamps"],
                       note=R["note"])
            row["wall_canon_ms"] = med([r["wall_ms"] for r in canon]) if canon else None
            row["wall_us_ms"] = med([r["wall_ms"] for r in us]) if us else None
            row["span_ms"] = med([r.get("span_ms") for r in us]) if us else None
            row["rss_canon_kb"] = max((r["rss_kb"] for r in canon), default=None)
            row["rss_us_kb"] = max((r["rss_kb"] for r in us), default=None)
            row["valid_canon"] = f"{len(canon)}/{reps}"
            row["valid_us"] = f"{len(us)}/{reps}"
            row["canon_eq_us_stdout"] = res.get("canon_eq_us_stdout")
            row["problems"] = res.get("problems", [])
            row["usmeta"] = [r.get("usmeta") for r in us]
            row["stamp_counts"] = [r.get("stamps") for r in us]
            for key, lkey in (("single", "lat"), ("batch", "lat_batch")):
                groups = [r[lkey] for r in us if lkey in r]
                if not groups:
                    continue
                # 逐 rep 的分布量 → 取 5 次中位
                ds = [dist_summary(g) for g in groups]
                row[f"{key}_n_units"] = ds[0]["n"]
                for q in ("p50", "p90", "p99", "p999", "max", "mean"):
                    row[f"{key}_{q}_us"] = med([d[q] / 1000.0 for d in ds])
                for b in BUDGETS_NS:
                    row[f"{key}_over_{b}"] = med([d["over"][str(b)] for d in ds])
                    row[f"{key}_over_{b}_max"] = max(d["over"][str(b)] for d in ds)
                    row[f"{key}_over_{b}_per_rep"] = [d["over"][str(b)] for d in ds]
                # 最长一次停滞 = 5 次里最大的一次；倍数对 p50（5 次中位）
                row[f"{key}_max_over_reps_us"] = max(d["max"] for d in ds) / 1000.0
                p50us = row[f"{key}_p50_us"]
                row[f"{key}_longest_ratio"] = (row[f"{key}_max_over_reps_us"] / p50us) if p50us else None
            tab[(name, rt)] = row

    def show(keys, title):
        W("")
        W("-- " + title + " --")
        hdr = ["形状", "运行时", "单位数", "墙钟canon ms", "墙钟us ms", "形状自身span ms", "RSScanon MiB", "RSSus MiB",
               "有效canon", "有效us", "戳数", "canon==us stdout"]
        W("\t".join(hdr))
        for k in keys:
            r = tab[k]
            W("\t".join(str(x) for x in [
                r["shape"], r["runtime"], r["units"],
                f"{r['wall_canon_ms']:.1f}" if r["wall_canon_ms"] else "-",
                f"{r['wall_us_ms']:.1f}" if r["wall_us_ms"] else "-",
                f"{r['span_ms']:.2f}" if r.get("span_ms") else "-",
                f"{r['rss_canon_kb']/1024:.1f}" if r["rss_canon_kb"] else "-",
                f"{r['rss_us_kb']/1024:.1f}" if r["rss_us_kb"] else "-",
                r["valid_canon"], r["valid_us"],
                ",".join(str(x) for x in r["stamp_counts"]),
                r["canon_eq_us_stdout"]]))

    keys = [(n, rt) for n in results for rt in results[n]["arms"]]
    show(keys, "墙钟 / RSS / 自检")
    W("")
    W("-- 单位延迟分布（µs；逐 rep 算后取 5 次中位；max=5 次中位的 max，最长=5 次里最大的那一次）--")
    W("\t".join(["形状", "运行时", "口径", "单位数", "p50", "p90", "p99", "p99.9", "max(中位)",
                 "最长", "最长/典型", ">0.1ms", ">1ms", ">16.7ms"]))
    for k in keys:
        r = tab[k]
        for key in ("single", "batch"):
            if f"{key}_p50_us" not in r:
                continue
            W("\t".join(str(x) for x in [
                r["shape"], r["runtime"], key, r[f"{key}_n_units"],
                f"{r[f'{key}_p50_us']:.2f}", f"{r[f'{key}_p90_us']:.2f}",
                f"{r[f'{key}_p99_us']:.2f}", f"{r[f'{key}_p999_us']:.2f}",
                f"{r[f'{key}_max_us']:.2f}",
                f"{r[f'{key}_max_over_reps_us']:.2f}",
                f"{r[f'{key}_longest_ratio']:.1f}x" if r.get(f"{key}_longest_ratio") else "-",
                r[f"{key}_over_100000"], r[f"{key}_over_1000000"], r[f"{key}_over_16700000"]]))
    W("")
    W("-- 自检汇总 --")
    ok = True
    for name, R in results.items():
        for rt, res in R["arms"].items():
            r = tab[(name, rt)]
            bad = []
            exp = R["expected_stamps"]
            for c in r["stamp_counts"]:
                if c != exp:
                    bad.append(f"戳数 {c} != {exp}")
            if r["valid_us"].split("/")[0] != str(reps):
                bad.append(f"us 有效 {r['valid_us']}")
            if not res.get("canon_eq_us_stdout", True):
                bad.append("canon != us stdout")
            bad += r["problems"]
            st = "OK" if not bad else "FAIL: " + "; ".join(bad)
            if bad:
                ok = False
            W(f"  {name}/{rt}: 戳数期望={exp} 实测={r['stamp_counts']} canon==us={res.get('canon_eq_us_stdout')} {st}")
    W("")
    W(f"自检总判：{'全部通过' if ok else '有不通过项（见上）'}")
    return dict(text="\n".join(lines) + "\n", table=tab, ok=ok)


if __name__ == "__main__":
    main()
