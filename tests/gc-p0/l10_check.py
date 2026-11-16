#!/usr/bin/env python3
"""L10-b / L10-c：线性泄漏用例（回收门禁 G-L10）。

两例都是「同一形状跑到 50k 与 800k 轮」：泄漏时 VmHWM 随轮数线性涨、`rs_live`（跨区入边
净数）也随轮数线性涨；修好之后两者都必须与轮数无关。判据（只许变严，`tests/reclaim-idle/`
记台账的规矩同此）：

  (1) 两次运行退出码 0；
  (2) stdout 逐字等于基准（形状的确定性输出，含 sum 校验）；
  (3) VmHWM(800k) / VmHWM(50k) <= 1.25（泄漏形状实测 3.5x / 13.8x）；
  (4) 两次运行的 rs_live <= 8（泄漏形状实测 50 000 / 800 000）。

VmHWM 优先取 sample.py 的 `vmhwm_kb`，退回 `peak_vmrss_kb`、再退回 `maxrss_kb`
（WSL2 上 ru_maxrss 会被父进程污染，见 tests/gc-p0/sample.py 的头注释）。

用法：
  python3 tests/gc-p0/l10_check.py --dir tests/gc-p0/out/run-XXX
  python3 tests/gc-p0/l10_check.py --selftest
"""
import argparse
import json
import os
import re
import sys
import tempfile

RATIO_MAX = 1.25
RS_LIVE_MAX = 8
CASES = {
    "l10b": {
        "name": "L10-b",
        "expect": {50000: "L10B n=50000 done=50000", 800000: "L10B n=800000 done=800000"},
    },
    "l10c": {
        "name": "L10-c",
        "expect": {50000: "L10C n=50000 sum=1249975000", 800000: "L10C n=800000 sum=319999600000"},
    },
}


def read_runs(out):
    path = os.path.join(out, "runs.jsonl")
    recs = []
    if not os.path.exists(path):
        return recs
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                recs.append(json.loads(line))
            except ValueError:
                continue
    return recs


def timemax(out):
    """`/usr/bin/time -f '%M %e'` 的读数：键是 (case, args) ⇒ 子进程自己的 ru_maxrss。

    短进程（50k 轮约 4 ms）用 2 ms 采样读不到峰值：采样从进程启动后才开始，窗口结束时进程
    已经进入后半段，于是同一形状的 50k 读数比 800k 小一半（实测 1 632 kB vs 3 908 kB），ratio
    判据量到的是采样窗口而不是泄漏。time 的 ru_maxrss 由内核在子进程退出时给出，与时长无关。
    """
    path = os.path.join(out, "timemax.txt")
    tab = {}
    for line in read_text(path).splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ("50000", "800000"):
            try:
                tab[(parts[0], parts[1])] = int(parts[2])
            except ValueError:
                continue
    return tab


def hwm(rec):
    for k in ("vmhwm_kb", "peak_vmrss_kb", "maxrss_kb"):
        v = rec.get(k)
        if isinstance(v, int) and v > 0:
            return v
    return None


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def rs_live(err_text):
    m = re.search(r"rs_live=(-?\d+)", err_text)
    return int(m.group(1)) if m else None


def judge(out):
    recs = read_runs(out)
    tm = timemax(out)
    report = {"cases": {}, "verdicts": {}}
    ok = True
    for case, spec in CASES.items():
        rows = {}
        for rec in recs:
            if rec.get("case") != case:
                continue
            args = (rec.get("args") or "").strip()
            if args in ("50000", "800000"):
                rows[int(args)] = rec
        res = {"name": spec["name"], "checks": [], "numbers": {}}
        got = {}
        for n in (50000, 800000):
            rec = rows.get(n)
            if rec is None:
                res["checks"].append(f"{n}: 缺记录（未跑）")
                ok = False
                continue
            h = tm.get((case, str(n))) or hwm(rec)
            rc_ = rec.get("rc")
            out_txt = read_text(os.path.join(out, "log", f"{case}_r{rec.get('rep')}.out")).strip()
            err_txt = read_text(os.path.join(out, "log", f"{case}_r{rec.get('rep')}.err"))
            live = rs_live(err_txt)
            got[n] = {"hwm_kb": h, "rc": rc_, "rs_live": live, "stdout": out_txt}
            if rc_ != 0:
                res["checks"].append(f"{n}: 退出码 {rc_} ≠ 0")
                ok = False
            if out_txt != spec["expect"][n]:
                res["checks"].append(f"{n}: stdout 不符：{out_txt!r} ≠ {spec['expect'][n]!r}")
                ok = False
            if live is None:
                res["checks"].append(f"{n}: stderr 里没有 rs_live（EXTC_RC_STATS=1 没生效）")
                ok = False
            elif live > RS_LIVE_MAX:
                res["checks"].append(f"{n}: rs_live={live} > {RS_LIVE_MAX}（跨区边不撤销）")
                ok = False
        if 50000 in got and 800000 in got and got[50000]["hwm_kb"] and got[800000]["hwm_kb"]:
            ratio = got[800000]["hwm_kb"] / got[50000]["hwm_kb"]
            res["numbers"]["ratio"] = round(ratio, 3)
            res["numbers"]["hwm_50k_kb"] = got[50000]["hwm_kb"]
            res["numbers"]["hwm_800k_kb"] = got[800000]["hwm_kb"]
            if ratio > RATIO_MAX:
                res["checks"].append(f"VmHWM 800k/50k = {ratio:.2f}x > {RATIO_MAX}x（随轮数增长）")
                ok = False
        else:
            ok = False
        for n in (50000, 800000):
            if n in got:
                res["numbers"][f"rs_live_{n}"] = got[n]["rs_live"]
        res["verdict"] = "PASS" if not res["checks"] else "RED"
        report["cases"][case] = res
    report["verdict"] = "PASS" if ok else "RED"
    return report


def print_report(report):
    print(f"== L10：线性泄漏用例（判据 VmHWM 800k/50k <= {RATIO_MAX}x 且 rs_live <= {RS_LIVE_MAX}）==")
    for case in sorted(report["cases"]):
        res = report["cases"][case]
        nums = res.get("numbers", {})
        detail = ""
        if "ratio" in nums:
            detail = (f"50k={nums['hwm_50k_kb']} kB / 800k={nums['hwm_800k_kb']} kB"
                      f"  ratio={nums['ratio']:.2f}x"
                      f"  rs_live={nums.get('rs_live_50000')}/{nums.get('rs_live_800000')}")
        print(f"  {res['verdict']:4s} {res['name']}  {detail}")
        for c in res["checks"]:
            print(f"        · {c}")
    print(f"  ⇒ {report['verdict']}")


def selftest():
    def mk(root, ratios, lives, stdout_ok=True):
        os.makedirs(os.path.join(root, "log"), exist_ok=True)
        recs = []
        for case, spec in CASES.items():
            for rep, n in ((1, 50000), (2, 800000)):
                h = 3600 * (ratios if n == 800000 else 1.0)
                recs.append({"case": case, "rep": rep, "rc": 0, "args": str(n),
                             "vmhwm_kb": int(h)})
                txt = spec["expect"][n] if stdout_ok else "garbage"
                with open(os.path.join(root, "log", f"{case}_r{rep}.out"), "w") as f:
                    f.write(txt + chr(10))
                with open(os.path.join(root, "log", f"{case}_r{rep}.err"), "w") as f:
                    f.write(f"extc_rc_stats rs_inc=1 rs_dec=1 rs_live={lives} rs_hwm=1" + chr(10))
        with open(os.path.join(root, "runs.jsonl"), "w") as f:
            for r in recs:
                f.write(json.dumps(r) + chr(10))

    bad = 0
    with tempfile.TemporaryDirectory() as d:
        mk(d, 1.0, 1)
        if judge(d)["verdict"] != "PASS":
            print("S1 失败：干净数据应 PASS"); bad += 1
    with tempfile.TemporaryDirectory() as d:
        mk(d, 3.5, 1)
        if judge(d)["verdict"] != "RED":
            print("S2 失败：RSS 3.5x 应 RED"); bad += 1
    with tempfile.TemporaryDirectory() as d:
        mk(d, 1.0, 50000)
        if judge(d)["verdict"] != "RED":
            print("S3 失败：rs_live=50000 应 RED"); bad += 1
    with tempfile.TemporaryDirectory() as d:
        mk(d, 1.0, 1, stdout_ok=False)
        if judge(d)["verdict"] != "RED":
            print("S4 失败：stdout 不符应 RED"); bad += 1
    print(f"selftest: {'ok' if bad == 0 else str(bad) + ' 项失败'}")
    return bad == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir")
    ap.add_argument("--json")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return 0 if selftest() else 1
    if not a.dir:
        ap.error("--dir or --selftest")
    report = judge(a.dir)
    print_report(report)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=1)
    return 0 if report["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())