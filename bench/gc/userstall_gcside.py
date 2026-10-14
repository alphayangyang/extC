#!/usr/bin/env python3
"""userstall_gcside.py —— **只做解释/对照**的两件事（task-87）：

(A) 各运行期**自己的** GC 账（Go `GODEBUG=gctrace=1`、JVM `-Xlog:gc`）：
    只报周期数 / 总停顿 / 最大停顿。**这些数绝不用于"命中判定"或"停顿时长"**
    （本横评的停顿定义只用应用侧时间戳；见 USER-STALL.md §1 口径）。

(B) extC 的 **默认 vs `EXTC_RECLAIM=0`** 对照 —— 用**同一套应用侧时间戳**（stamped/extc/*_us.extc）
    各测 5 次，报单位延迟分布之差。这是"GC 真让业务慢了多少"的正确量法（不需要任何 GC 内部计时）。
    注意：`EXTC_RECLAIM=0` 是**对照臂**，不是"出厂默认档"。

输出：/tmp/userstall/gcside.json + /tmp/userstall/logs/gcside.txt
"""
import json
import math
import os
import re
import subprocess
import sys

GC = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(GC))
WORK = "/tmp/userstall"
BIN = os.path.join(WORK, "bin")
CLASSES_CANON = os.path.join(WORK, "classes_canon")
CLASSES_US = os.path.join(WORK, "classes_us")
LOGS = os.path.join(WORK, "logs")
STAMPS = os.path.join(WORK, "stamps")
EXTC = os.path.join(ROOT, "build", "extc")
STDLIB = os.path.join(ROOT, "stdlib")
sys.path.insert(0, GC)
from userstall import SHAPES, sh, pct, latencies, dist_summary  # noqa: E402

REPS = 5
STW = re.compile(r"^gc \d+ @\S+ \d+%: ([0-9.]+)\+([0-9.]+)\+([0-9.]+) ms clock")
JPAUSE = re.compile(r"Pause\s+\w+\s+.*?([0-9.]+)ms")


def run(cmd, env=None, errfile=None):
    e = dict(os.environ)
    if env:
        for k, v in env.items():
            if v is None:
                e.pop(k, None)
            else:
                e[k] = v
    with open(errfile or os.devnull, "wb") as ef:
        p = subprocess.run(cmd, env=e, stdout=subprocess.PIPE, stderr=ef)
    return p.returncode


def parse_stamps(path, expected):
    vals = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("US "):
                vals.append(int(line[3:]))
    if len(vals) != expected:
        return None
    return vals


def main():
    out = {"gcside": {}, "reclaim_diff": {}}
    lines = []
    W = lines.append
    W("# GC 侧账本（**仅解释/对照**：不用于命中判定，也不用于停顿时长）")
    for name, sp in SHAPES.items():
        argv = sp["argv"]
        out["gcside"][name] = {}
        # ---- Go：GODEBUG=gctrace=1 ----
        ef = os.path.join(WORK, f"gcside_{name}_go.err")
        rc = run([os.path.join(BIN, f"{sp['go']}_go")] + argv, env={"GODEBUG": "gctrace=1"}, errfile=ef)
        stws = []
        n = 0
        for line in open(ef, errors="replace"):
            m = STW.match(line)
            if m:
                n += 1
                stws.append(float(m.group(1)) + float(m.group(3)))   # STW = a + c
        out["gcside"][name]["go"] = dict(rc=rc, cycles=n, total_ms=sum(stws),
                                         max_ms=max(stws) if stws else 0.0)
        W(f"[GC 侧] {name} Go     : rc={rc} 周期={n} ΣSTW={sum(stws):.3f}ms max={max(stws) if stws else 0:.3f}ms")
        # ---- Java：-Xlog:gc:stderr ----
        cls = sp["java"][0]
        ef = os.path.join(WORK, f"gcside_{name}_java.err")
        rc = run(["java", "-Xlog:gc:stderr", "-cp", CLASSES_CANON, cls] + argv, errfile=ef)
        pauses = []
        for line in open(ef, errors="replace"):
            if "Pause" in line:
                m = re.findall(r"([0-9.]+)ms", line)
                if m:
                    pauses.append(float(m[-1]))
        # JIT 编译事件（解释 Java 长尾用；**与 GC 无关**，同样是事后对照）
        efjit = os.path.join(WORK, f"gcside_{name}_javajit.err")
        rcj = run(["java", "-Xlog:jit+compilation:stderr", "-cp", CLASSES_CANON, cls] + argv, errfile=efjit)
        rc2, jout, jerr, jcmd = sh(["python3", os.path.join(GC, "jit_stats.py"), "java-jit", efjit])
        jits = jout.strip().replace("\n", " | ") if rc2 == 0 else f"jit_stats rc={rc2}: {jerr.strip()[:120]}"
        ncol = sum(1 for l in open(ef, errors="replace") if "[gc]" in l)
        out["gcside"][name]["java"] = dict(rc=rc, gc_log_lines=ncol, pause_events=len(pauses),
                                           total_ms=sum(pauses), max_ms=max(pauses) if pauses else 0.0)
        W(f"[GC 侧] {name} Java   : rc={rc} gc 日志行={ncol} Pause 事件={len(pauses)} "
          f"ΣPause={sum(pauses):.3f}ms max={max(pauses) if pauses else 0:.3f}ms")
        W(f"[对照] {name} Java JIT: rc={rcj} {jits}")
        out["gcside"][name].setdefault("java", {})["jit"] = jits
        # ---- extC：结构性 0 / rc 运行期 + 应用侧对照 ----
        gen = os.path.join(WORK, f"{sp['extc']}_us_c.c")
        marks = open(gen, errors="replace").read().count("extc_rc_collect") if os.path.exists(gen) else 0
        W(f"[GC 侧] {name} extC   : 生成物 extc_rc_collect={marks} 处 ⇒ "
          f"{'结构性 0（不发射回收运行期）' if marks == 0 else '有回收运行期（用 (B) 的应用侧对照）'}")
        out["gcside"][name]["extc"] = dict(collect_callsites=marks)
        if marks == 0:
            continue
        # ---- (B) extC 默认 vs EXTC_RECLAIM=0：同一套应用侧戳 ----
        src = os.path.join(GC, "stamped", "extc", sp["extc"] + "_us.extc")
        cfile = os.path.join(WORK, f"{sp['extc']}_us_nr.c")
        bfile = os.path.join(BIN, f"{sp['extc']}_us_nr_c")
        env = dict(os.environ)
        env["EXTC_RECLAIM"] = "0"
        p = subprocess.run([EXTC, "-w", "--no-line-map", "-I", STDLIB, src, "-o", cfile], env=env,
                           capture_output=True)
        assert p.returncode == 0, p.stderr.decode()
        p = subprocess.run(["cc", "-O2", "-o", bfile, cfile], capture_output=True)
        assert p.returncode == 0, p.stderr.decode()
        marks_nr = open(cfile, errors="replace").read().count("extc_rc_collect")

        units = sp["units"](argv)
        expected = units + 1
        res = {}
        for tag, cmd in (("default", [os.path.join(BIN, f"{sp['extc']}_us_c")]),
                         ("noreclaim", [bfile])):
            dists, walls = [], []
            for rep in range(REPS):
                errf = os.path.join(STAMPS, f"gcside_{name}_{tag}_{rep}.err")
                env2 = dict(os.environ)
                r = subprocess.run([os.path.join(BIN, "digraph_run"), "--out", "/dev/null",
                                    "--err", errf, "--"] + cmd + argv, env=env2,
                                   capture_output=True)
                ms = re.search(r"ms=([0-9.]+)", r.stdout.decode())
                vals = parse_stamps(errf, expected)
                assert vals is not None, f"{name}/{tag} 戳数不符"
                dists.append(dist_summary(latencies(vals)))
                walls.append(float(ms.group(1)))
            res[tag] = dict(wall_ms=sorted(walls)[len(walls) // 2],
                            p50_us=sorted(d["p50"] for d in dists)[REPS // 2] / 1000.0,
                            p99_us=sorted(d["p99"] for d in dists)[REPS // 2] / 1000.0,
                            max_us=max(d["max"] for d in dists) / 1000.0,
                            over_100us=sorted(d["over"]["100000"] for d in dists)[REPS // 2])
        res["collect_callsites_noreclaim"] = marks_nr
        res["delta_p50_us"] = res["default"]["p50_us"] - res["noreclaim"]["p50_us"]
        res["delta_p99_us"] = res["default"]["p99_us"] - res["noreclaim"]["p99_us"]
        res["delta_max_us"] = res["default"]["max_us"] - res["noreclaim"]["max_us"]
        out["reclaim_diff"][name] = res
        W(f"[应用侧对照] {name} extC 默认/不回收：p50 {res['default']['p50_us']:.3f}/{res['noreclaim']['p50_us']:.3f} "
          f"p99 {res['default']['p99_us']:.3f}/{res['noreclaim']['p99_us']:.3f} "
          f"max {res['default']['max_us']:.2f}/{res['noreclaim']['max_us']:.2f} µs；"
          f">0.1ms {res['default']['over_100us']}/{res['noreclaim']['over_100us']}（中位）；"
          f"不回收档 collect 记号={marks_nr}")
    with open(os.path.join(WORK, "gcside.json"), "w") as f:
        json.dump(out, f, indent=1)
    with open(os.path.join(LOGS, "gcside.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"[done] {WORK}/gcside.json")


if __name__ == "__main__":
    main()
