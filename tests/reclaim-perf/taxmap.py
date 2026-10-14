#!/usr/bin/env python3
"""当前构建的回收成本总表 —— 谁发射 rc、为什么、付多少、买到了什么。

三个阶段各自独立、互不依赖，产物全是 TSV（原始数据入 `out/`，生成的 C 只落 /tmp）：

    python3 tests/reclaim-perf/taxmap.py census   # 全语料：默认档 vs EXTC_RECLAIM=0 是否逐字节相同
    python3 tests/reclaim-perf/taxmap.py reasons  # 每个"发 rc"的形状：挡路理由（EXTC_DBG_RCIDLE）
    python3 tests/reclaim-perf/taxmap.py probe    # 代表性形状：on/off 墙钟与峰值 RSS（两规模）
    python3 tests/reclaim-perf/taxmap.py report   # 汇总成 markdown（stdout）

纪律（与 `tests/reclaim-*/README.md` 一致）：
  · 只读 `src/`；生成的 C 与二进制全部写 `/tmp/taxmap/`；TSV 写 `tests/reclaim-perf/out/`。
  · 每个数字绑定 `build/extc` 的 md5 与 `git rev-parse HEAD`，写进每个 TSV 的表头注释。
  · **不用 `timeout(1)` 计时**：这台机器上 `timeout 90 /bin/true` 自身要 ~104 ms，
    会把毫秒级的形状全部抬高成同一个常数。计时走 `fork`+`wait4`（同时拿精确峰值 RSS），
    超时由父进程轮询后 `SIGKILL`（自带看门狗）。

环境：`build/extc` 已构建；`cc` 可用。
"""
import hashlib
import os
import signal
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT = os.path.join(REPO, "build", "extc")
STDLIB = os.path.join(REPO, "stdlib")
OUT = os.path.join(REPO, "tests", "reclaim-perf", "out")
TMP = "/tmp/taxmap"

CC_FLAGS = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-fwrapv", "-O2"]

# 代表性形状：引用图原型（拓扑 × 变更方式）+ 真实基准形状。
# 值 = (小规模, 大规模)。平方形状只能用小数（它们在默认档上是 O(N²)，见报告）。
PROBE = [
    ("tests/reclaim-perf/shapes/refgraph/rg_walk_chain.extc", 1000000, 4000000),
    ("tests/reclaim-perf/shapes/refgraph/rg_walk_buckets.extc", 1000000, 4000000),
    ("tests/reclaim-perf/shapes/refgraph/rg_walk_tree.extc", 1000000, 4000000),
    ("tests/reclaim-perf/shapes/refgraph/rg_walk_array.extc", 1000000, 4000000),
    ("tests/reclaim-perf/shapes/refgraph/rg_scan_parent.extc", 1000000, 4000000),
    ("tests/reclaim-perf/shapes/refgraph/rg_scan_graph.extc", 10000, 40000),
    ("tests/reclaim-perf/shapes/refgraph/rg_grow_tail.extc", 10000, 40000),
    ("tests/reclaim-perf/shapes/refgraph/rg_insert_hash.extc", 1000000, 4000000),
    ("tests/reclaim-perf/shapes/refgraph/rg_relink_delete.extc", 100000, 400000),
    ("tests/reclaim-perf/shapes/refgraph/rg_overwrite_ring.extc", 1000000, 4000000),
    ("tests/reclaim-perf/shapes/refgraph/rg_evict_cache.extc", 1000000, 4000000),
    ("tests/reclaim-perf/shapes/refgraph/rg_drop_component.extc", 100000, 400000),
    ("bench/gc/src/spectrum_p2.extc", 1000000, 4000000),
    ("bench/gc/src/spectrum_p4.extc", 5000, 20000),
    ("bench/gc/src/spectrum_p5.extc", 300000, 1200000),
    ("bench/gc/src/digraph.extc", 100000, 400000),
]

CENSUS_ROOTS = ("examples", "tests", "bench")
CENSUS_SKIP_DIRS = {"archive", "build", "in", "in-small", "out", "stamped", "__pycache__"}


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_id():
    head = subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
                          capture_output=True, text=True).stdout.strip()
    return f"build/extc md5={md5(EXT)} HEAD={head}"


def gen(src, dst, reclaim_on):
    env = dict(os.environ)
    if not reclaim_on:
        env["EXTC_RECLAIM"] = "0"
    return subprocess.run([EXT, "-w", "--no-line-map", "-I", STDLIB, src, "-o", dst],
                          capture_output=True, text=True, env=env, timeout=120)


def run_once(binary, n, limit_s, reclaim_on):
    """fork+exec，父进程轮询看门狗；返回 (rc, 墙钟 ms, 峰值 RSS KB, 是否超时)。"""
    t0 = time.monotonic()
    pid = os.fork()
    if pid == 0:
        devnull = os.open(os.devnull, os.O_WRONLY)
        os.dup2(devnull, 1)
        os.dup2(devnull, 2)
        if not reclaim_on:
            os.environ["EXTC_RECLAIM"] = "0"
        try:
            os.execv(binary, [binary, str(n)])
        finally:
            os._exit(127)
    killed = False
    while True:
        wpid, status, ru = os.wait4(pid, os.WNOHANG)
        if wpid == pid:
            break
        if time.monotonic() - t0 > limit_s:
            os.kill(pid, signal.SIGKILL)
            killed = True
        time.sleep(0.002)
    wall_ms = (time.monotonic() - t0) * 1000.0
    if killed:
        return (124, wall_ms, ru.ru_maxrss, True)
    rc = os.waitstatus_to_exitcode(status)
    return (rc, wall_ms, ru.ru_maxrss, False)


def find_census_sources():
    out = []
    for root in CENSUS_ROOTS:
        for dirpath, dirnames, filenames in os.walk(os.path.join(REPO, root)):
            dirnames[:] = [d for d in dirnames if d not in CENSUS_SKIP_DIRS]
            for fn in sorted(filenames):
                if fn.endswith(".extc"):
                    out.append(os.path.join(dirpath, fn))
    return sorted(out)


def stage_census():
    os.makedirs(TMP, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    rows = []
    sources = find_census_sources()
    for i, src in enumerate(sources):
        rel = os.path.relpath(src, REPO)
        stem = rel.replace("/", "_")[:-5]
        on = os.path.join(TMP, stem + ".on.c")
        off = os.path.join(TMP, stem + ".off.c")
        p = gen(src, on, True)
        if p.returncode != 0:
            err = (p.stderr or p.stdout).strip().splitlines()
            rows.append((rel, "GENFAIL", 0, 0, "", err[0] if err else ""))
            continue
        gen(src, off, False)
        same = md5(on) == md5(off)
        rows.append((rel, "ZERO" if same else "PAYS",
                     os.path.getsize(on), os.path.getsize(off), md5(on)[:8], ""))
        if i % 200 == 0:
            print(f"  … {i}/{len(sources)}", file=sys.stderr)
    path = os.path.join(OUT, "census.tsv")
    with open(path, "w") as f:
        f.write(f"# {build_id()}\n")
        f.write("path\tclass\tsize_on\tsize_off\tmd5_on\terror\n")
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")
    print(f"wrote {path} ({len(rows)} sources)")


def stage_reasons():
    os.makedirs(OUT, exist_ok=True)
    census = os.path.join(OUT, "census.tsv")
    pays = []
    with open(census) as f:
        for line in f:
            if line.startswith("#") or line.startswith("path\t"):
                continue
            cols = line.rstrip("\n").split("\t")
            if len(cols) > 1 and cols[1] == "PAYS":
                pays.append(cols[0])
    path = os.path.join(OUT, "reasons.tsv")
    with open(path, "w") as f:
        f.write(f"# {build_id()}\n")
        f.write("path\tfunctions_blocked\treason\tsites\n")
        for rel in pays:
            env = dict(os.environ, EXTC_DBG_RCIDLE="1")
            p = subprocess.run([EXT, "-w", "--no-line-map", "-I", STDLIB,
                                os.path.join(REPO, rel), "-o", os.path.join(TMP, "r.c")],
                               capture_output=True, text=True, env=env, timeout=120)
            text = p.stderr + p.stdout
            reasons = {}
            for line in text.splitlines():
                if "idle=0" not in line or "reason=" not in line:
                    continue
                body = line.split("reason=", 1)[1]
                body = body.split(":", 1)[1].strip() if body.startswith("line ") else body
                reasons[body] = reasons.get(body, 0) + 1
            for reason, n in sorted(reasons.items(), key=lambda kv: -kv[1]):
                f.write(f"{rel}\t{sum(reasons.values())}\t{reason}\t{n}\n")
    print(f"wrote {path} ({len(pays)} shapes)")


def stage_probe():
    os.makedirs(TMP, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "probe.tsv")
    with open(path, "w") as f:
        f.write(f"# {build_id()}\n")
        f.write("# 计量：fork+wait4，2 次取「最小墙钟 / 最大 RSS」；不用 timeout(1)（它自身 ~104ms）\n")
        f.write("path\tn\tmode\trc\twall_ms\trss_kb\ttimeout\n")
        for rel, n_small, n_large in PROBE:
            src = os.path.join(REPO, rel)
            stem = rel.replace("/", "_")[:-5]
            bins = {}
            for mode, reclaim_on in (("on", True), ("off", False)):
                c = os.path.join(TMP, f"{stem}.{mode}.c")
                b = os.path.join(TMP, f"{stem}.{mode}")
                if gen(src, c, reclaim_on).returncode != 0:
                    print(f"GENFAIL {rel}", file=sys.stderr)
                    break
                cc = subprocess.run(["cc"] + CC_FLAGS + ["-o", b, c],
                                    capture_output=True, text=True)
                if cc.returncode != 0:
                    print(f"CCFAIL {rel}: {cc.stderr.splitlines()[:1]}", file=sys.stderr)
                    break
                bins[mode] = b
            if len(bins) != 2:
                continue
            for n in (n_small, n_large):
                limit = 90 if n < 1000000 else 180
                for mode in ("on", "off"):
                    best_wall, best_rss, rc, to = None, 0, 0, False
                    for _ in range(2):
                        rc, wall, rss, to = run_once(bins[mode], n, limit, mode == "on")
                        best_wall = wall if best_wall is None else min(best_wall, wall)
                        best_rss = max(best_rss, rss)
                        if to:
                            break
                    f.write(f"{rel}\t{n}\t{mode}\t{rc}\t{best_wall:.1f}\t{best_rss}\t{int(to)}\n")
                    f.flush()
                    print(f"  {rel} N={n} {mode}: rc={rc} {best_wall:.1f}ms {best_rss}KB"
                          + (" TIMEOUT" if to else ""), file=sys.stderr)
    print(f"wrote {path}")


def load_tsv(name):
    rows = []
    with open(os.path.join(OUT, name)) as f:
        for line in f:
            if line.startswith("#") or line.startswith("path\t"):
                continue
            rows.append(line.rstrip("\n").split("\t"))
    return rows


def stage_report():
    census = load_tsv("census.tsv")
    classes = {}
    for r in census:
        classes[r[1]] = classes.get(r[1], 0) + 1
    print("## 语料普查（全仓 *.extc）\n")
    print(f"源文件 {len(census)}：ZERO {classes.get('ZERO',0)} · PAYS {classes.get('PAYS',0)}"
          f" · GENFAIL {classes.get('GENFAIL',0)}\n")
    reasons = {}
    for r in load_tsv("reasons.tsv"):
        reasons[r[2]] = reasons.get(r[2], 0) + 1
    print("## 挡路理由（发 rc 的形状，按出现次数）\n")
    for reason, n in sorted(reasons.items(), key=lambda kv: -kv[1]):
        print(f"- {n}× {reason}")
    print("\n## 代表性形状：on/off 代价\n")
    probe = load_tsv("probe.tsv")
    by = {}
    for r in probe:
        by.setdefault(r[0], {})[(int(r[1]), r[2])] = r
    print("| 形状 | N | on ms | off ms | 税× | on RSS KB | off RSS KB | RSS 比 |")
    print("|---|---|---|---|---|---|---|---|")
    for rel, _s, _l in PROBE:
        sizes = sorted({k[0] for k in by.get(rel, {})})
        for n in sizes:
            on = by[rel].get((n, "on"))
            off = by[rel].get((n, "off"))
            if not on or not off:
                continue
            wt = float(on[4]) / max(float(off[4]), 0.01)
            rs = int(on[5]) / max(int(off[5]), 1)
            print(f"| {rel} | {n} | {on[4]} | {off[4]} | {wt:.2f}× | {on[5]} | {off[5]} | {rs:.2f}× |")
    print()


def main():
    stage = sys.argv[1] if len(sys.argv) > 1 else "report"
    {"census": stage_census, "reasons": stage_reasons,
     "probe": stage_probe, "report": stage_report}[stage]()


if __name__ == "__main__":
    main()
