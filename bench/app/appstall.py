#!/usr/bin/env python3
"""appstall.py —— bench/app 三形状的"用户视角停顿"驱动器（extC/C++/Go，全默认档）。

口径（写死在 bench/app/STALL.md §1；本文件是它的实现）：

- **停顿只由应用侧单调戳定义**：每个业务单位边界打一次戳（戳打在**形状源码**里，由
  `bench/app/make_stamped_app.py` 从 `bench/app/*.{extc,cpp,go}` 派生，每处替换断言锚点唯一），
  单位延迟 = 相邻戳之差；戳数 = 单位数 + 1。
- **GC 侧计时一律不参与任何统计**：`extc_rc_*` / Go `gctrace` / `runtime.MemStats` 的 Pause
  本轮**没有**被用来定义停顿，也没有被用来判"命中"。`EXTC_RECLAIM` 只作**编译期**对照臂
  （它是代码生成开关，不是运行期开关）。
- **全默认档**：不加调优旗标、**不 pin 核**、不设堆上限、不开关 GC。计时轮的命令原文进日志。
  唯一显式传入的是 argv（工作量规模 + 戳文件路径，三语言逐字一致），且 canon 轮与插桩轮
  **argv 完全相同**。
- 异常值**不剔除**；`max` 报 5 次里最大的那次，另给 5 次中位的 max。

用法：
  python3 bench/app/appstall.py --build --reps 5
  python3 bench/app/appstall.py --reps 1 --shapes session     # 冒烟
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, "bench", "app")
WORK = "/tmp/appstall"
BIN = os.path.join(WORK, "bin")
GEN = os.path.join(WORK, "gen")
LOGS = os.path.join(WORK, "logs")
OUTS = os.path.join(WORK, "out")
ERRS = os.path.join(WORK, "err")
STAMPS = os.path.join(WORK, "stamps")
EXTC = os.path.join(ROOT, "build", "extc")
EXTC_MD5_EXPECTED = "57f6b8ccd7c16100d12e5b51b26849e8"
RUNNER_SRC = os.path.join(ROOT, "bench", "gc", "digraph_run.c")
RUNNER = os.path.join(BIN, "appstall_run")
CC = "cc"
CXX = "g++"
GO = "go"

BUDGETS_NS = [100_000, 1_000_000, 16_700_000]      # 0.1 ms / 1 ms / 16.7 ms
MAGIC = b"USAPPST1"
HDR = 40

# ------------------------------------------------------------------ 形状表
# argv：**程序自带的默认规模**（源码头注释里的默认值），canon 与插桩轮逐字一致。
SHAPES = {
    "session": dict(
        argv=["2000000", "65536"],
        units=lambda a: int(a[0]),                       # 主循环一次迭代 = 一次会话事件
        unit_name="一次会话事件（查/建更/断连，主循环一次迭代）",
        # 供 route 用的相位切分（其余形状为 None）
        phases=None,
    ),
    "route": dict(
        argv=["2000000"],
        units=lambda a: int(a[0]) * 2 + int(a[0]) // 4 + int(a[0]) // 2,
        unit_name="一次路由表操作（插入/前驱查找/区间扫描/删除 四次循环各一次迭代）",
        phases=lambda a: [("insert", int(a[0])), ("lookup", int(a[0])),
                          ("scan", int(a[0]) // 4), ("delete", int(a[0]) // 2)],
    ),
    "log": dict(
        argv=["2000000"],
        units=lambda a: int(a[0]),                       # 主循环一次迭代 = 一次请求
        unit_name="一次请求处理（追加 24B + 可选 find + 可选截断，主循环一次迭代）",
        phases=None,
    ),
}

# 每条臂：`cenv` 只加在**编译** extc 时（EXTC_RECLAIM 是代码生成开关），`renv` 只加在**运行**时。
# `default=True` = 出厂默认档；其余是**显式标注的非默认对照臂**。
# 二进制名：extc 按 arm 名区分；cpp/go 按语言（GOGC 是运行期开关 ⇒ 与 go 共用同一份二进制）。
ARMS = [
    dict(name="extc_def",   lang="extc", cenv={},                    renv={},                  default=True,  label="extC 默认"),
    dict(name="extc_norec", lang="extc", cenv={"EXTC_RECLAIM": "0"}, renv={},                  default=False, label="extC EXTC_RECLAIM=0（非默认档）"),
    dict(name="extc_force", lang="extc", cenv={"EXTC_RECLAIM": "1"}, renv={},                  default=False, label="extC EXTC_RECLAIM=1（非默认档·强制发射）"),
    dict(name="cpp",        lang="cpp",  cenv={},                    renv={},                  default=True,  label="C++（无回收器）"),
    dict(name="go",         lang="go",   cenv={},                    renv={},                  default=True,  label="Go 默认（GOGC=100）"),
    dict(name="go_nogc",    lang="go",   cenv={},                    renv={"GOGC": "off"},     default=False, label="Go GOGC=off（非默认档·关 GC）"),
]
ALL_ARMS = list(ARMS)          # config 报表永远列全部臂，与 --arms 过滤无关

BUILD_LOG = []


def sh(cmd, env=None, cwd=None, timeout=None):
    p = subprocess.run(cmd, capture_output=True, env=env, cwd=cwd, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def blog(line):
    BUILD_LOG.append(line)
    print("[build] " + line, flush=True)


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(65536), b""):
            h.update(b)
    return h.hexdigest()


def loadavg():
    with open("/proc/loadavg") as f:
        return f.read().split()[0]


def clean_env(extra=None):
    drop = {"EXTC_RECLAIM", "EXTC_RC_SHARDS", "EXTC_RC_NOCOUNT", "EXTC_DBG_M", "EXTC_DBG_IMPL",
            "EXTC_DBG_HOME", "EXTC_DBG_OWNER", "EXTC_DUMP_EFFECTS", "EXTC_NO_LEVELPASS",
            "EXTC_SELFCHECK", "GOGC", "GOMEMLIMIT", "GOMAXPROCS", "GOFLAGS",
            "JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "GODEBUG"}
    e = {k: v for k, v in os.environ.items() if k not in drop}
    if extra:
        e.update(extra)
    return e


# ------------------------------------------------------------------ 构建
def build_all(reps):
    for d in (BIN, GEN, LOGS, OUTS, ERRS, STAMPS):
        os.makedirs(d, exist_ok=True)
    rc, _, e = sh([CC, "-O2", "-o", RUNNER, RUNNER_SRC])
    blog(f"rc={rc} {CC} -O2 -o {RUNNER} {RUNNER_SRC}")
    if rc != 0:
        sys.exit("runner 构建失败：" + e)

    if md5(EXTC) != EXTC_MD5_EXPECTED:
        sys.exit(f"extC 编译器 md5={md5(EXTC)} != 绿点 {EXTC_MD5_EXPECTED} ⇒ 拒绝出数")

    for name, sp in SHAPES.items():
        for tag, src in (("canon", os.path.join(APP, name)),
                         ("us", os.path.join(APP, "stamped", "extc", name + "_us"))):
            for arm in ARMS:
                if arm["lang"] != "extc":
                    continue
                oc = os.path.join(GEN, f"{name}_{tag}_{arm['name']}.c")
                rc, _, e = sh([EXTC, src + ".extc", "-o", oc], env=clean_env(arm["cenv"]))
                blog(f"rc={rc} {' '.join([EXTC, src + '.extc', '-o', oc])} cenv={arm['cenv'] or '{}'}")
                if rc != 0:
                    sys.exit(f"extc 编译失败 {src}.extc [{arm['name']}]：{e}")
                ob = os.path.join(BIN, f"{name}_{tag}_{arm['name']}")
                rc, _, e = sh([CC, "-O2", "-std=c11", "-fwrapv", oc, "-o", ob])
                blog(f"rc={rc} {CC} -O2 -std=c11 -fwrapv {oc} -o {ob}")
                if rc != 0:
                    sys.exit(f"cc 编译失败 {oc}：{e}")
        for tag, src in (("canon", os.path.join(APP, name + ".cpp")),
                         ("us", os.path.join(APP, "stamped", "cpp", name + "_us.cpp"))):
            ob = os.path.join(BIN, f"{name}_{tag}_cpp")
            rc, _, e = sh([CXX, "-O2", "-std=c++17", src, "-o", ob])
            blog(f"rc={rc} {CXX} -O2 -std=c++17 {src} -o {ob}")
            if rc != 0:
                sys.exit(f"g++ 编译失败 {src}：{e}")
        for tag, src in (("canon", os.path.join(APP, name + ".go")),
                         ("us", os.path.join(APP, "stamped", "go", name + "_us.go"))):
            ob = os.path.join(BIN, f"{name}_{tag}_go")
            rc, _, e = sh([GO, "build", "-o", ob, src], cwd=ROOT, env=clean_env())
            blog(f"rc={rc} {GO} build -o {ob} {src}")
            if rc != 0:
                sys.exit(f"go build 失败 {src}：{e}")
    # 配置自证小工具（Go 运行期自己报的 GOMAXPROCS/GOGC/GOMEMLIMIT）
    cfg = os.path.join(GEN, "cfg.go")
    with open(cfg, "w") as f:
        f.write('package main\n\nimport (\n\t"fmt"\n\t"runtime"\n\t"runtime/debug"\n)\n\n'
                'func main() {\n'
                '\tfmt.Printf("GOMAXPROCS=%d NumCPU=%d GOGC=%d GOMEMLIMIT=%d\\n",\n'
                '\t\truntime.GOMAXPROCS(0), runtime.NumCPU(), debug.SetGCPercent(-1), debug.SetMemoryLimit(-1))\n'
                '}\n')
    rc, _, e = sh([GO, "build", "-o", os.path.join(BIN, "cfg_go"), cfg], cwd=ROOT, env=clean_env())
    blog(f"rc={rc} {GO} build -o {BIN}/cfg_go {cfg}")
    if rc != 0:
        sys.exit("cfg.go 构建失败：" + e)
    # 二进制可执行自检
    for f in sorted(os.listdir(BIN)):
        p = os.path.join(BIN, f)
        if os.path.isfile(p) and not os.access(p, os.X_OK):
            sys.exit(f"二进制不可执行：{p}")


# ------------------------------------------------------------------ 配置打印
def config_report(path):
    lines = []

    def w(s):
        lines.append(s)
        print(s, flush=True)

    w("# ---- 配置打印（出厂默认档核对；与计时轮分开，日志 " + path + "）----")
    w(f"# 采集时间 {time.strftime('%F %T %z')}  load1={loadavg()}  nproc={os.cpu_count()}")
    with open("/proc/cpuinfo") as f:
        m = re.search(r"model name\s*:\s*(.+)", f.read())
    w(f"# CPU: {m.group(1).strip() if m else '?'}  nproc={os.cpu_count()}")
    with open("/proc/meminfo") as f:
        w("# " + [l.strip() for l in f if l.startswith("MemTotal")][0])
    w(f"# kernel: {os.uname().release}")
    w("")
    w("[extC] compiler=" + EXTC)
    w(f"[extC] md5={md5(EXTC)}  （绿点要求 {EXTC_MD5_EXPECTED}）"
      f"  {'核对通过' if md5(EXTC) == EXTC_MD5_EXPECTED else '核对失败'}")
    w("[extC] 编译开关（与 bench/app/run.sh 逐字同一套）：")
    w(f"        extc bench/app/<shape>.extc -o <gen.c> ; cc -O2 -std=c11 -fwrapv <gen.c> -o <bin>")
    w("        （run.sh 用的就是这两条；`-fwrapv` 是 extC 的既定编译契约，docs/MANUAL.md §8）")
    w("        EXTC_RECLAIM 是**代码生成开关**（编译期 getenv），只在对照臂的 extc 调用上设：")
    w("          extc_def   env -u EXTC_RECLAIM      （出厂默认）")
    w("          extc_norec EXTC_RECLAIM=0           （非默认档）")
    w("          extc_force EXTC_RECLAIM=1           （非默认档·强制发射）")
    rc, out, err = sh([os.path.join(BIN, "cfg_go")], env=clean_env())
    w(f"[Go]   {sh([GO, 'version'])[1].strip()}")
    w(f"[Go]   cmd={BIN}/cfg_go rc={rc}  →  {out.strip()}")
    w("[Go]   运行期自报（程序自己读 runtime/debug）：GOMAXPROCS 必须 == NumCPU 才说明没被外部压过")
    w("[Go]   出厂默认档：env GOGC/GOMEMLIMIT/GOMAXPROCS/GOFLAGS/GODEBUG 全部未设置；`go build` 无任何旗标")
    w("[Go]   非默认对照臂 `go_nogc`：**运行期**加 `GOGC=off`（关 GC）。与 `go` 共用同一份二进制。")
    w("       ⚠️ 关 GC 会让堆无界增长 ⇒ 「无 GC 暂停」与「占用更多内存」两个因素无法分开。")
    w(f"[C++]  {sh([CXX, '--version'])[1].splitlines()[0].strip()}")
    w("[C++]  语言/运行期**没有回收器**（无 GC、无 RC 运行期）⇒ 天然的「零回收成本」基线")
    w("       编译开关（与 run.sh 逐字同一套）：g++ -O2 -std=c++17 bench/app/<shape>.cpp -o <bin>")
    w("")
    w("[生成物] 回收运行期是否发射（判据 = 生成 C 里 `extc_rc_collect` 的出现次数；0 = 结构性不发射）：")
    for name in SHAPES:
        row = []
        for arm in ALL_ARMS:
            if arm["lang"] != "extc":
                continue
            for tag in ("canon", "us"):
                g = os.path.join(GEN, f"{name}_{tag}_{arm['name']}.c")
                n = open(g, encoding="utf-8", errors="replace").read().count("extc_rc_collect") if os.path.exists(g) else -1
                row.append(f"{arm['name']}/{tag}={n}")
        same = "产物逐字节相同" if md5(os.path.join(GEN, f"{name}_canon_extc_def.c")) == \
            md5(os.path.join(GEN, f"{name}_canon_extc_norec.c")) else "产物不同"
        w(f"        {name}: " + " · ".join(row) + f"   [默认 vs EXTC_RECLAIM=0：{same}]")
    w("        正对照（bench/gc/src/digraph.extc，有受管分配点）：默认 4 处、EXTC_RECLAIM=0 0 处、产物不同")
    w("        ⇒ 本判据**能红**；上面三个形状全 0 是真实的结构性结论，不是量具坏了。")
    w("")
    w("[环境] 计时轮用 env 白名单：EXTC_*/GOGC/GOMEMLIMIT/GOMAXPROCS/GOFLAGS/GODEBUG/JAVA_* 一律剔除")
    for k in ("EXTC_RECLAIM", "GOGC", "GOMEMLIMIT", "GOMAXPROCS", "GOFLAGS"):
        w(f"        {k}={'<unset>' if k not in os.environ else os.environ[k]}")
    w("[量具] bench/gc/digraph_run（fork→exec→wait4；**不传 --pin**，未 pin 核）")
    w("       墙钟 = CLOCK_MONOTONIC 包住被测程序；峰值 RSS = wait4.ru_maxrss（内核值，KB）")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


# ------------------------------------------------------------------ 单次运行
def run_once(shape, arm, kind, rep, argv):
    """跑一次；返回 dict。stdout/stderr/戳文件落盘。"""
    tag = f"{shape}_{kind}_{arm['name']}_{rep}"
    outfile = os.path.join(OUTS, tag + ".out")
    errfile = os.path.join(ERRS, tag + ".err")
    stampfile = os.path.join(STAMPS, tag + ".bin")
    exe = os.path.join(BIN, f"{shape}_{kind}_{arm['name'] if arm['lang'] == 'extc' else arm['lang']}")
    cmd = [RUNNER, "--out", outfile, "--err", errfile, "--", exe] + argv + [stampfile]
    pre = loadavg()
    t0 = time.time()
    rc, so, se = sh(cmd, env=clean_env(arm["renv"]))
    dt = time.time() - t0
    rec = dict(shape=shape, arm=arm["name"], lang=arm["lang"], kind=kind, rep=rep,
               cmd=" ".join(cmd), load_pre=pre, load_post=loadavg(), wall_py_ms=dt * 1000.0,
               outfile=outfile, errfile=errfile, stampfile=stampfile, valid=False)
    if rc != 0:
        rec["why"] = f"runner rc={rc}"
        return rec
    m = re.search(r"ms=([0-9.]+) rss_kb=(\d+) rc=(-?\d+)", so)
    if not m:
        rec["why"] = "垫片无输出"
        return rec
    rec["wall_ms"] = float(m.group(1))
    rec["rss_kb"] = int(m.group(2))
    rec["child_rc"] = int(m.group(3))
    rec["runner_rc"] = rc
    if rec["child_rc"] != 0:
        rec["why"] = f"子进程 rc={rec['child_rc']}"
        return rec
    if os.path.getsize(errfile) != 0:
        rec["why"] = "stderr 非空（插桩不该写 stderr）"
        return rec
    rec["valid"] = True
    return rec


def decode_stamps(path):
    """→ (nstamps, t0, nneg, novf, deltas)；严格：魔数/长度/字节消耗全核对。"""
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < HDR:
        raise ValueError(f"戳文件太短：{len(blob)}")
    if blob[:8] != MAGIC:
        raise ValueError(f"魔数不符：{blob[:8]!r}")
    n, t0, nneg, novf = struct.unpack_from("<QqQQ", blob, 8)
    if n == 0:
        raise ValueError("nstamps=0")
    deltas = []
    i = HDR
    for _ in range(n - 1):
        shift = 0
        v = 0
        while True:
            if i >= len(blob):
                raise ValueError("varint 流提前结束")
            c = blob[i]
            i += 1
            v |= (c & 127) << shift
            if c < 128:
                break
            shift += 7
            if shift > 63:
                raise ValueError("varint 过长")
        deltas.append(v)
    if i != len(blob):
        raise ValueError(f"戳文件有 {len(blob) - i} 个多余字节（缓冲溢出会这样）")
    return n, t0, nneg, novf, deltas


def fmt_us(ns):
    """ns → µs（3 位小数）。表里只有这一处换算，避免把 ns 标成 µs。"""
    return f"{ns / 1000.0:.3f}"


def pct(sorted_vals, p):
    n = len(sorted_vals)
    if n == 0:
        return None
    if n == 1:
        return float(sorted_vals[0])
    idx = p * (n - 1)
    lo = int(math.floor(idx))
    hi = min(lo + 1, n - 1)
    return sorted_vals[lo] * (1.0 - (idx - lo)) + sorted_vals[hi] * (idx - lo)


def med(xs):
    s = sorted(x for x in xs if x is not None)
    if not s:
        return None
    n = len(s)
    return float(s[n // 2]) if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2.0


def dist(vals_ns):
    s = sorted(vals_ns)
    return dict(n=len(s), p50=pct(s, 0.50), p90=pct(s, 0.90), p99=pct(s, 0.99),
                p999=pct(s, 0.999), max=float(s[-1]) if s else None,
                mean=(sum(s) / len(s)) if s else None,
                over={b: sum(1 for v in vals_ns if v > b) for b in BUDGETS_NS})


# ------------------------------------------------------------------ 主流程
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--shapes", default="session,route,log")
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--arms", default="", help="只跑这些臂（逗号分隔）；空 = 全部（默认档 + 非默认对照臂）")
    args = ap.parse_args()
    shapes = [s.strip() for s in args.shapes.split(",") if s.strip()]
    global ARMS
    if args.arms.strip():
        keep = [x.strip() for x in args.arms.split(",") if x.strip()]
        ARMS = [a for a in ARMS if a["name"] in keep]
        if not ARMS:
            sys.exit("--arms 没匹配到任何臂")

    os.makedirs(LOGS, exist_ok=True)
    if args.build:
        build_all(args.reps)
    with open(os.path.join(LOGS, "build.log"), "w") as f:
        f.write("\n".join(BUILD_LOG) + "\n")
    config_report(os.path.join(LOGS, "config.txt"))

    runs = []
    results = {}
    for name in shapes:
        sp = SHAPES[name]
        argv = sp["argv"]
        units = sp["units"](argv)
        expected = units + 1
        print(f"[run] {name} argv={argv} units={units} 期望戳数={expected}", flush=True)
        arm_res = {}
        stdout_blobs = {}
        for arm in ARMS:
            arm_res[arm["name"]] = dict(arm=arm["name"], lang=arm["lang"], label=arm["label"],
                                        default=arm["default"], argv=argv, units=units,
                                        expected_stamps=expected, canon=[], us=[])
        # **轮转（rep 主序）**：同一个 rep 里把所有臂紧挨着跑完 ⇒ 外部负载在各臂之间均摊。
        # （runA 是 arm 主序：extC 跑到 load≈3.4、Go 跑到 load≈1.0 ⇒ 尾部被负载混杂，已作废。）
        for rep in range(args.reps):
            for arm in ARMS:
                res = arm_res[arm["name"]]
                for kind in ("canon", "us"):
                    r = run_once(name, arm, kind, rep, argv)
                    if kind == "us" and r.get("valid"):
                        try:
                            n, t0, nneg, novf, deltas = decode_stamps(r["stampfile"])
                        except ValueError as ex:
                            r["valid"] = False
                            r["why"] = f"戳解码失败：{ex}"
                        else:
                            r["nstamps"] = n
                            r["t0"] = t0
                            r["nneg"] = nneg
                            r["novf"] = novf
                            r["ndelta"] = len(deltas)
                            if n != expected:
                                r["valid"] = False
                                r["why"] = f"戳数 {n} != 期望 {expected}"
                            elif novf != 0:
                                r["valid"] = False
                                r["why"] = f"novf={novf}（戳缓冲溢出）"
                            elif nneg != 0:
                                r["valid"] = False
                                r["why"] = f"nneg={nneg}（单调钟倒退）"
                            else:
                                r["lat"] = deltas
                                r["dist"] = dist(deltas)
                                if sp["phases"]:
                                    ph = {}
                                    off = 0
                                    for pname, cnt in sp["phases"](argv):
                                        seg = deltas[off:off + cnt]
                                        ph[pname] = dist(seg)
                                        off += cnt
                                    r["phase_dist"] = ph
                    if r.get("valid"):
                        blob = open(r["outfile"], "rb").read()
                        stdout_blobs.setdefault(blob, []).append(f"{name}/{arm['name']}/{kind}/{rep}")
                    runs.append(r)
                    res[kind].append(r)
        results[name] = dict(units=units, expected_stamps=expected, argv=argv,
                             unit_name=sp["unit_name"], arms=arm_res,
                             stdout_variants=len(stdout_blobs),
                             stdout_lines=[b.decode("utf-8", "replace").strip()
                                           for b in sorted(stdout_blobs, key=lambda b: b)][:4],
                             stdout_groups={k.decode("utf-8", "replace").strip(): v
                                            for k, v in stdout_blobs.items()})
        print(f"[run] {name} 完成；stdout 变体数={len(stdout_blobs)}（1 = 全部逐字节一致）", flush=True)

    summary_text, tables = summarize(results, args.reps)
    # JSON：不塞逐单位原始时长（几十 MB）；原始戳文件在 /tmp/appstall/stamps/ 可复算
    for name, R in results.items():
        for an, res in R["arms"].items():
            for kind in ("canon", "us"):
                for r in res[kind]:
                    r.pop("lat", None)
                    r.pop("dist", None)
                    r.pop("phase_dist", None)
    with open(os.path.join(WORK, "results.json"), "w") as f:
        json.dump(dict(reps=args.reps, shapes=shapes, results=results,
                       summary_text=summary_text, tables=tables), f, indent=1, sort_keys=True)
    with open(os.path.join(LOGS, "runs.tsv"), "w") as f:
        f.write("shape\tarm\tlang\tkind\trep\tvalid\tchild_rc\trunner_rc\twall_ms\trss_kb\t"
                "load_pre\tload_post\tnstamps\tnneg\tnovf\twhy\toutfile\tstampfile\n")
        for r in runs:
            f.write("\t".join(str(x) for x in [
                r["shape"], r["arm"], r["lang"], r["kind"], r["rep"], int(bool(r.get("valid"))),
                r.get("child_rc", ""), r.get("runner_rc", ""), f"{r.get('wall_ms', 0):.3f}",
                r.get("rss_kb", ""), r.get("load_pre", ""), r.get("load_post", ""),
                r.get("nstamps", ""), r.get("nneg", ""), r.get("novf", ""), r.get("why", ""),
                r.get("outfile", ""), r.get("stampfile", "")]) + "\n")
    with open(os.path.join(LOGS, "summary.txt"), "w") as f:
        f.write(summary_text)
    # 孤儿进程自检
    orphans = count_orphans()
    with open(os.path.join(LOGS, "orphans.txt"), "w") as f:
        f.write(f"orphans={len(orphans)}\n" + "\n".join(orphans) + "\n")
    print(summary_text)
    print(f"[selfcheck] 孤儿进程={len(orphans)} {'OK' if not orphans else 'FAIL'}")
    print(f"[done] results.json / runs.tsv / summary.txt / orphans.txt 在 {WORK}/")
    # 任一自检不过 ⇒ 非零退出
    return 0 if summary_ok(results, args.reps) and not orphans else 1


def count_orphans():
    out = []
    me = os.getpid()
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == me:
            continue
        try:
            with open(f"/proc/{pid}/stat", "rb") as f:
                data = f.read().decode("utf-8", "replace")
            # comm 可能含空格/括号 ⇒ 取最后一个 ')' 之后的字段
            rest = data[data.rfind(")") + 2:].split()
            ppid = int(rest[1])
            if ppid != 1:
                continue
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                cmd = f.read().decode("utf-8", "replace").replace("\0", " ")
        except (OSError, IndexError, ValueError):
            continue
        if WORK in cmd:
            out.append(f"pid={pid} ppid=1 cmd={cmd.strip()}")
    return out


def summary_ok(results, reps):
    ok = True
    for name, R in results.items():
        if len(R["stdout_groups"]) != 1:
            ok = False
        for an, res in R["arms"].items():
            for kind in ("canon", "us"):
                good = [r for r in res[kind] if r.get("valid")]
                if len(good) != reps:
                    ok = False
                for r in res[kind]:
                    if r.get("child_rc") not in (None, 0):
                        ok = False
            for r in res["us"]:
                if r.get("nstamps") not in (None, R["expected_stamps"]):
                    ok = False
    return ok


def summarize(results, reps):
    L = []
    W = L.append
    tables = {}
    W("== bench/app 用户视角停顿（单位=ns→µs；墙钟/ RSS 来自 bench/gc/digraph_run，未 pin 核）==")
    W(f"reps={reps}  预算档 = 0.1ms / 1ms / 16.7ms（判据 `单位延迟 > 预算`）")
    W("")

    # ---- 0. 自检 ----
    W("-- 自检 --")
    for name, R in results.items():
        exp = R["expected_stamps"]
        W(f"  [{name}] argv={R['argv']} 单位={R['unit_name']} 单位数={R['units']} 期望戳数={exp}"
          f"  stdout 变体数={R['stdout_variants']}（1=三语言×canon×插桩×{reps} 次逐字节一致）")
        for an, res in R["arms"].items():
            good = [r for r in res["us"] if r.get("valid")]
            cnt = [r.get("nstamps") for r in res["us"]]
            bad = [f"rep{r['rep']}:{r.get('why')}" for r in res["us"] + res["canon"] if not r.get("valid")]
            ok = (len(good) == reps) and all(c == exp for c in cnt)
            W(f"    {an:11s} 有效插桩 {len(good)}/{reps} 戳数={cnt} 期望={exp} "
              f"{'OK' if ok else 'FAIL ' + '; '.join(bad)}")
    W("")

    # ---- 1. 分布表 ----
    W("-- 单位延迟分布（µs；逐 rep 算分位后取 5 次**中位**；max(中位)=5 次中位的 max，"
      "最长=5 次里最大的那一次；异常值不剔除）--")
    W("\t".join(["形状", "臂", "档", "单位数", "p50(µs)", "p90", "p99", "p99.9", "max(中位)", "最长(5次最大)",
                 "最长/p50", ">0.1ms(中位)", ">1ms(中位)", ">16.7ms(中位)",
                 ">0.1ms(逐次)", ">1ms(逐次)", ">16.7ms(逐次)"]))
    tab = {}
    for name, R in results.items():
        for an, res in R["arms"].items():
            ds = [r["dist"] for r in res["us"] if r.get("valid") and "dist" in r]
            if not ds:
                continue
            row = dict(shape=name, arm=an, lang=res["lang"], label=res["label"],
                       default=res["default"], units=R["units"])
            for q in ("p50", "p90", "p99", "p999", "max", "mean"):
                row[q] = med([d[q] for d in ds])
            row["max_abs"] = max(d["max"] for d in ds)
            row["max_med"] = row["max"]
            row["ratio"] = row["max_abs"] / row["p50"] if row["p50"] else None
            for b in BUDGETS_NS:
                row[f"over_{b}"] = med([d["over"][b] for d in ds])
                row[f"over_{b}_per"] = [d["over"][b] for d in ds]
                row[f"over_{b}_max"] = max(d["over"][b] for d in ds)
            row["wall_canon_med"] = med([r["wall_ms"] for r in res["canon"] if r.get("valid")])
            row["wall_us_med"] = med([r["wall_ms"] for r in res["us"] if r.get("valid")])
            row["rss_canon_max"] = max([r["rss_kb"] for r in res["canon"] if r.get("valid")], default=None)
            row["rss_us_max"] = max([r["rss_kb"] for r in res["us"] if r.get("valid")], default=None)
            # 插桩的现场代价：单位工作量上多花的 ns（canon vs us 墙钟差 / 单位数）
            if row["wall_canon_med"] and row["wall_us_med"]:
                row["us_cost_ns_per_unit"] = (row["wall_us_med"] - row["wall_canon_med"]) * 1e6 / R["units"]
            else:
                row["us_cost_ns_per_unit"] = None
            tab[(name, an)] = row
            W("\t".join(str(x) for x in [
                name, an, "默认" if res["default"] else "非默认", R["units"],
                fmt_us(row['p50']), fmt_us(row['p90']), fmt_us(row['p99']), fmt_us(row['p999']),
                fmt_us(row['max_med']), fmt_us(row['max_abs']),
                f"{row['ratio']:.1f}x" if row["ratio"] else "-",
                int(row["over_100000"]), int(row["over_1000000"]), int(row["over_16700000"]),
                row["over_100000_per"], row["over_1000000_per"], row["over_16700000_per"]]))
    tables["dist"] = {f"{k[0]}/{k[1]}": v for k, v in tab.items()}
    W("")

    # ---- 2. 墙钟 / RSS / 插桩代价 ----
    W("-- 墙钟（ms，5 次中位）/ 峰值 RSS（MiB，5 次最大）/ 插桩现场代价 --")
    W("\t".join(["形状", "臂", "档", "墙钟 canon", "墙钟 us", "RSS canon", "RSS us",
                 "us−canon ms", "每单位 +ns"]))
    for name, R in results.items():
        for an, res in R["arms"].items():
            row = tab.get((name, an))
            if not row:
                continue
            d = (row["wall_us_med"] - row["wall_canon_med"]) if (row["wall_us_med"] and row["wall_canon_med"]) else None
            W("\t".join(str(x) for x in [
                name, an, "默认" if res["default"] else "非默认",
                f"{row['wall_canon_med']:.1f}" if row["wall_canon_med"] else "-",
                f"{row['wall_us_med']:.1f}" if row["wall_us_med"] else "-",
                f"{row['rss_canon_max']/1024:.1f}" if row["rss_canon_max"] else "-",
                f"{row['rss_us_max']/1024:.1f}" if row["rss_us_max"] else "-",
                f"{d:+.1f}" if d is not None else "-",
                f"{row['us_cost_ns_per_unit']:.1f}" if row["us_cost_ns_per_unit"] is not None else "-"]))
    W("")

    # ---- 3. extC 默认 vs 不回收 ----
    W("-- extC 对照：默认 vs EXTC_RECLAIM=0（非默认档）/ vs EXTC_RECLAIM=1（非默认档·强制发射）--")
    W("\t".join(["形状", "对照", "Δp50", "Δp99", "Δp99.9", "Δmax(最长)", "Δ>0.1ms(中位)", "Δ>1ms",
                 "生成物 md5 相同？", "回收运行期符号数"]))
    for name, R in results.items():
        base = tab.get((name, "extc_def"))
        if not base:
            continue
        for other in ("extc_norec", "extc_force"):
            o = tab.get((name, other))
            if not o:
                continue
            W("\t".join(str(x) for x in [
                name, other,
                f"{o['p50'] - base['p50']:+.2f}", f"{o['p99'] - base['p99']:+.2f}",
                f"{o['p999'] - base['p999']:+.2f}", f"{o['max_abs'] - base['max_abs']:+.2f}",
                f"{int(o['over_100000']) - int(base['over_100000']):+d}",
                f"{int(o['over_1000000']) - int(base['over_1000000']):+d}",
                "见 config.txt", "见 config.txt"]))
    W("")

    # ---- 4. route 逐相位 ----
    for name, R in results.items():
        if name != "route":
            continue
        W("-- route 逐相位分布（µs；相位按单位序号切；注意：相位边界那一笔（最后一个 insert 的起点→"
          "第一个 lookup 的起点）算在**前一个相位**里）--")
        W("\t".join(["臂", "相位", "单位数", "p50(µs)", "p90", "p99", "p99.9", "max(中位)", "最长(5次最大)"]))
        for an, res in R["arms"].items():
            pds = [r["phase_dist"] for r in res["us"] if r.get("valid") and "phase_dist" in r]
            if not pds:
                continue
            for pname in ("insert", "lookup", "scan", "delete"):
                row = {}
                for q in ("p50", "p90", "p99", "p999", "max"):
                    row[q] = med([d[pname][q] for d in pds])
                row["max_abs"] = max(d[pname]["max"] for d in pds)
                W("\t".join(str(x) for x in [
                    an, pname, pds[0][pname]["n"],
                    fmt_us(row['p50']), fmt_us(row['p90']), fmt_us(row['p99']),
                    fmt_us(row['p999']), fmt_us(row['max']), fmt_us(row['max_abs'])]))
        W("")

    # ---- 5. 总判 ----
    ok = summary_ok(results, reps)
    W(f"自检总判：{'全部通过' if ok else '有不通过项（见上）'}")
    return "\n".join(L) + "\n", tables


if __name__ == "__main__":
    sys.exit(main())
