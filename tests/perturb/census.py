#!/usr/bin/env python3
"""扰动普查：用 `MALLOC_PERTURB_` 把"未初始化堆读"这一类缺陷从猜测变成一个数。

为什么要专门查这一类
    ASan/UBSan **照不到**"读了没写过的堆内存"（那是 MSan 的领域），可它有两条真实危害：
    ① 崩溃（读到的垃圾当指针用）；② **更坏的**：垃圾值被当成分析结论 ⇒ 误编译。
    把堆初值设成确定值（`MALLOC_PERTURB_=165`）能把"偶发"变成"必现"，于是可以按语料清算。

三档对照
    `plain`（不设）/ `42` / `165` —— 同一个源三档都跑。判据不是"扰动下崩了"就算 bug，而是
    **arm 之间不一致**：
      · `crash`：编译器自己被信号打死（`rc < 0`），且只在扰动档出现 ⇒ 未初始化读的强信号；
      · `discordant`：退出码在档位间不同（例如 plain 0 / 165 1）⇒ **疑似误编译**，比崩溃更值得看；
      · `reject`：三档一致的编译期拒绝 ⇒ 语料本来就是反例，正常；
      · `program`：编译器没报错、但被编译出来的程序非零退出（需要 argv/stdin 的源多在这里）
        ⇒ 按任务书标注"只编译/需要特殊输入"，不计入编译器缺陷。
    "崩在编译器还是被编译的程序"就是靠这一条分的：`--run` 下程序崩了，`extc` 自己只会返回 1
    且**没有编译期诊断**；`extc` 自己崩了，父进程看到的就是信号。

归属
    每个 `crash`：同档重跑 2 次（2/2 可复现）+ `gdb -batch -ex run -ex bt` 取前几帧 +
    与固定副本（pin）逐一对照同一源 ⇒ 既存 / 本次引入。根因按**栈顶 src/ 帧**去重。

用法
    python3 tests/perturb/census.py                     # 全语料三档（重活：先确认机器空）
    python3 tests/perturb/census.py --limit 3 --filter generic    # 脚本自检（轻）
    python3 tests/perturb/census.py --check             # 与 known-bad.txt 比：新增/过期都算红
"""
import argparse
import difflib
import json
import os
import re
import signal
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
EXTC = ROOT / "build" / "extc"
HERE = Path(__file__).resolve().parent
KNOWN_BAD = HERE / "known-bad.txt"
RUN_ROOT = Path(os.environ.get("EXTC_PERTURB_RUN_ROOT") or "/tmp/extc-perturb-runs")
# 每次 `--run` 一个私有目录（见 EXTC_RUN_DIR）。`EXTC_PERTURB_RUN_ROOT` 让调用方换一个**私有**
# 根：`tests/perturb/gate-orphans.sh` 用它把自己那次有界普查与别的普查（bench 档的
# `gate-perturb-corpus` 过全语料）分开，否则"跑之前该前缀下必须 0 进程"会被邻居的正常运行踩红。
# `None`/空串都退回默认根（`or` 而不是 `get(..., default)`：env 设成空串也要退回）。

# 进程组 + 登记那一套**复用** `tools/parrun.py` 的现成实现（每个子进程一个**新会话**、登记到
# `/tmp/extc-case-sessions`、到点杀整个组）：同一条规则只留一份。
# 为什么不直接用 `parrun.sh()`：它把 stderr 并进 stdout（`stderr=STDOUT`），而普查**必须**分开
# 两者 —— "疑似误编译"要拿被编译程序的 stdout 对拍，"编译器有没有诊断"要读 stderr。所以这里只用
# 它的三件原语：`register_session` / `unregister_session` / `kill_group`（见 `_spawn_case`）。
sys.path.insert(0, str(ROOT / "tools"))
import parrun  # noqa: E402
# 档位 = 额外加哪些环境变量。`afill` 是我们自己分配器的填图案档（`EXTC_ARENA_FILL`）：
# 这一档不依赖 glibc 的 `MALLOC_PERTURB_`，所以"读了没写过的 arena 内存"这一类靠它就照得到。
ARMS = {"plain": None,
        "42": {"MALLOC_PERTURB_": "42"},
        "165": {"MALLOC_PERTURB_": "165"},
        "afill": {"EXTC_ARENA_FILL": "165"}}
# 固定副本：与 task-10 同法，用来判"既存 / 本次引入"。缺哪个就跳过哪个（报告里注明）。
DEFAULT_PINS = ["/tmp/extc.pin3", "/tmp/extc.pin-prehoist", "/tmp/extc.pinR1", "/tmp/extc.pinR2"]
SIGNAME = {getattr(signal, n): n for n in dir(signal) if n.startswith("SIG") and n != "SIG_ATOMIC"}


def corpus():
    out = []
    for pat in ("tests/**/*.extc", "bench/**/*.extc", "examples/**/*.extc"):
        for f in sorted(ROOT.glob(pat)):
            if "tests/perturb" in str(f):
                continue
            out.append(f)
    return out


def norm(text, rundir):
    """把私有 run 目录抹成 `<RUN>`：它在各档之间必然不同，不抹掉就会"人人 stderr 都不同"。"""
    return (text or "").replace(str(rundir), "<RUN>").replace(str(RUN_ROOT), "<RUN>")


def _spawn_case(cmd, env, cwd, merge=False):
    """在**自己的会话**里起一个子进程并登记；返回 Popen。

    `extc --run` 会 fork 出被编译的程序（`tests/coro/echo_server.extc` 这类还会再 fork 后代）。
    只杀编译器就会把这些程序留给 init 当孤儿：实测一次有界普查（2 个源、1 档、`--timeout 2`）
    跑完留下 **9 个** `/tmp/extc-perturb-runs/...` 进程，`echo_server_fork` 的还在不停再生。
    所以每次 `--run` 都在独立会话里跑，超时才能连**整个进程组**一起收掉。

    登记（`parrun.register_session`）是为了本进程被 SIGKILL 时，之后的 `parrun.reap_orphans()`
    仍能认出这些会话是自己的后代、只收自己人。

    @param cmd    argv。
    @param env    环境变量。
    @param cwd    工作目录（每档一个私有目录，见 `run_capture`）。
    @param merge  真 = stderr 并进 stdout（gdb 用；普查本体不用，理由见文件头的 import 注释）。
    @return       Popen。
    """
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                         stderr=(subprocess.STDOUT if merge else subprocess.PIPE),
                         text=True, env=env, cwd=cwd, start_new_session=True)
    parrun.register_session(p.pid)
    return p


def _communicate_group(p, timeout):
    """收 `_spawn_case` 起的进程；到点杀**整个进程组**。返回 (out, err, rc, timed_out)。

    `start_new_session=True` ⇒ `p.pid` 同时是会话号与进程组号，所以超时直接 `killpg(p.pid)`：
    就算编译器自己已经先退、组里只剩它拉起的程序，这一枪照样能打着。
    """
    try:
        out, err = p.communicate(timeout=timeout)
        return out, err, p.returncode, False
    except subprocess.TimeoutExpired:
        try:
            os.killpg(p.pid, signal.SIGKILL)      # p.pid = 会话/组号（start_new_session）
        except OSError:
            parrun.kill_group(p)                  # 退回复用它那份（含 p.kill 兜底）
        try:
            out, err = p.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            out, err = "", ""
        return out, err, -1000, True
    finally:
        parrun.unregister_session(p.pid)


def run_capture(extc, src, arm, timeout, extra_env=None):
    """跑一次 `--run`：返回 {rc, kind, tail, out}。`rc < 0` = 被信号打死（-signum）。

    `out` 是**被编译程序的 stdout** —— 判"疑似误编译"必须比对它：两档都 rc==0、输出不同
    就是**静默错**，比崩溃更坏，所以这一栏不能省。
    """
    env = dict(os.environ)
    d = RUN_ROOT / arm / re.sub(r"[^A-Za-z0-9_.-]", "_", str(src.relative_to(ROOT)))
    env["EXTC_RUN_DIR"] = str(d)
    os.makedirs(d, exist_ok=True)          # `--run` 只 mkdir 一级；父级得由调用方建
    if ARMS[arm]:
        env.update(ARMS[arm])
    if extra_env:
        env.update(extra_env)
    try:
        # cwd 也钉到私有目录：`--run` 的程序继承调用方的 CWD，有些语料会往 CWD 写文件
        # （`tests/fs/*`），不隔离的话两档会互相踩 ⇒ 假 discordant
        p = _spawn_case([str(extc), "--run", str(src)], env, str(d))
    except OSError as e:
        return {"rc": -1001, "kind": "spawn-error", "tail": str(e), "out": "", "err": ""}
    out_raw, err_raw, rc, timed = _communicate_group(p, timeout)
    if timed:
        # 超时杀的是**整个进程组**（编译器 + 它拉起的程序及后代），不是只杀编译器。
        return {"rc": -1000, "kind": "timeout", "tail": f"(超过 {timeout}s 被杀：整个进程组)",
                "out": "", "err": ""}
    err = norm(err_raw or "", d)
    out = norm(out_raw or "", d)
    tail = "\n".join(err.strip().splitlines()[-6:])
    if rc < 0:
        return {"rc": rc, "kind": "crash", "tail": tail, "out": out, "err": err}
    if rc == 0:
        return {"rc": rc, "kind": "ok", "tail": tail, "out": out, "err": err}
    # 退了非零：有**错误**诊断 = 语料本身就是反例（`note:` 不算 —— 弃用警告的尾巴上就有 `note:`）；
    # 没有 = 编译器"没说不行"，是程序自己没跑好（需要 argv/stdin 的多在这里）
    if re.search(r":\s*error(\[|:)|(^|\n)\s*error(\[|:)", err):
        return {"rc": rc, "kind": "reject", "tail": tail, "out": out, "err": err}
    if re.search(r"(^|\n)extc:", err):     # 驱动器自己的抱怨（建目录/写文件…）：不是被编译程序的锅
        return {"rc": rc, "kind": "driver", "tail": tail, "out": out, "err": err}
    return {"rc": rc, "kind": "program", "tail": tail, "out": out, "err": err}


def run_one(extc, src, arm, timeout, extra_env=None):
    """给崩溃/归属用的一行版：只关心 rc 与分类，不搬 stdout。"""
    r = run_capture(extc, src, arm, timeout, extra_env)
    return r["rc"], r["kind"], r["tail"]


def out_diff(a, b, n=14):
    """两档 stdout 的差异（截断）：判"静默误编译"的直接证据。"""
    d = list(difflib.unified_diff(a.splitlines(), b.splitlines(),
                                  "plain", "perturbed", lineterm="", n=1))
    return "\n".join(d[:n]) if d else "(stdout 相同)"


def gdb_frames(extc, src, arm, timeout=120):
    """同一源同档在 gdb 下跑，取前 6 帧（只给 crash 用）。"""
    env = dict(os.environ)
    d = RUN_ROOT / arm / "gdb"
    env["EXTC_RUN_DIR"] = str(d)
    os.makedirs(d, exist_ok=True)
    if ARMS[arm]:
        env.update(ARMS[arm])
    try:
        p = _spawn_case(["gdb", "-batch", "-ex", "run", "-ex", "bt 6",
                         "--args", str(extc), "--run", str(src)],
                        env, str(d), merge=True)
    except OSError:
        return []
    out_raw, _err, _rc, timed = _communicate_group(p, timeout)
    if timed:
        return []
    out = out_raw or ""
    return re.findall(r"^#\d+\s+(?:0x[0-9a-f]+ in )?(\S+)\s+\(.*?\)\s+at\s+(src/\S+:\d+)",
                      out, re.M)


def root_of(frames):
    """栈顶那个 src/ 帧 = 根因指纹（没有就退化成 gdb 里的第一个函数名）。"""
    for fn, loc in frames:
        return f"{fn} @ {loc}"
    if frames:
        return frames[0][0]
    return "(unknown)"


def load_known_bad():
    keys = set()
    if KNOWN_BAD.exists():
        for line in KNOWN_BAD.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            keys.add(line.split("\t")[0])
    return keys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arms", default="plain,42,165")
    ap.add_argument("--limit", type=int, default=0, help="只跑前 N 个源（脚本自检用）")
    ap.add_argument("--filter", default="", help="只跑路径含这个子串的源")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--gdb", type=int, default=64,
                    help="最多给几个崩溃条目取 gdb 背栈（去重靠它，别设小了：漏了会并进 (unknown)）")
    ap.add_argument("--pins", default="", help="逗号分隔的固定副本；默认那四个 /tmp/extc.pin*")
    ap.add_argument("--extc", default=str(EXTC),
                    help="用哪个编译器跑普查（默认 build/extc；给 pin 时脚本自动带上 EXTC_STD）")
    ap.add_argument("--out", default=str(Path("/tmp/extc-perturb")))
    ap.add_argument("--check", action="store_true",
                    help="与 known-bad.txt 比：新增烂源或清单过期都返回 1（常驻门禁用）")
    ap.add_argument("--report", action="store_true", help="把 report.md 写进 --out")
    args = ap.parse_args()

    arms = [a.strip() for a in args.arms.split(",") if a.strip()]
    for a in arms:
        if a not in ARMS:
            print(f"census: 未知档位 `{a}`（可选 {', '.join(ARMS)}）", file=sys.stderr)
            return 2
    extc = Path(args.extc)
    if not extc.exists():
        print(f"census: {extc} 不存在 —— 先 make", file=sys.stderr)
        return 2
    # 副本住在 /tmp 时它按自己所在目录找 stdlib ✗ ⇒ 显式指回仓库（默认搜索路径本来就是这里）
    std_env = {"EXTC_STD": str(ROOT / "stdlib")} if str(extc).startswith("/tmp") else None

    srcs = [f for f in corpus() if args.filter in str(f.relative_to(ROOT))]
    if args.limit:
        srcs = srcs[:args.limit]
    jobs = args.jobs or min(4, os.cpu_count() or 1)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    RUN_ROOT.mkdir(parents=True, exist_ok=True)

    t0 = time.monotonic()
    print(f"扰动普查：{len(srcs)} 个源 × {len(arms)} 档 = {len(srcs) * len(arms)} 次 `--run`"
          f"（jobs={jobs}，timeout={args.timeout:g}s，编译器={extc}）", flush=True)

    results = {}
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {(str(f), a): ex.submit(run_capture, extc, f, a, args.timeout, std_env)
                for f in srcs for a in arms}
        for key, fut in futs.items():
            results[key] = fut.result()

    # ---- 汇总 ----
    crashes, discordant, stderr_discordant, rejects, programs, drivers = [], [], [], [], [], []
    for f in srcs:
        rel = str(f.relative_to(ROOT))
        by_arm = {a: results[(str(f), a)] for a in arms}
        crashes += [(rel, a) for a in arms if by_arm[a]["kind"] == "crash"]
        if "plain" in arms:
            base = by_arm["plain"]
            for a in arms:
                if a == "plain":
                    continue
                r = by_arm[a]
                # 疑似误编译：分类/退出码变了，或两档都 rc==0 而 stdout 不同（**静默错**）
                if (r["kind"], r["rc"]) != (base["kind"], base["rc"]) or \
                   (r["kind"] == "ok" and base["kind"] == "ok" and r["out"] != base["out"]):
                    discordant.append({
                        "src": rel, "arm": a,
                        "plain": (base["kind"], base["rc"]), "perturbed": (r["kind"], r["rc"]),
                        "stdout": out_diff(base["out"], r["out"]),
                        "tail": r["tail"],
                    })
                elif (r["kind"], r["rc"]) == (base["kind"], base["rc"]) and r["err"] != base["err"]:
                    # 行为一样、**诊断不同**：分析结论可能变了（静默错的前兆），也可能只是措辞受
                    # 扰动影响 ⇒ 单独计数、单列，**先量后定**，不并入判红口径。
                    stderr_discordant.append({
                        "src": rel, "arm": a,
                        "plain": (base["kind"], base["rc"]), "perturbed": (r["kind"], r["rc"]),
                        "stderr": out_diff(base["err"], r["err"]),
                    })
        if all(by_arm[a]["kind"] == "reject" for a in arms):
            rejects.append(rel)
        if all(by_arm[a]["kind"] == "program" for a in arms):
            programs.append(rel)
        if any(by_arm[a]["kind"] == "driver" for a in arms):
            drivers.append(rel)

    # ---- 每个崩：2/2 可复现 + 背栈 + 归属 ----
    n_need = args.gdb
    pins = [p for p in (args.pins.split(",") if args.pins else DEFAULT_PINS) if p and Path(p).exists()]
    findings = {}
    for rel, arm in crashes:
        src = ROOT / rel
        rep = [run_one(extc, src, arm, args.timeout, std_env)[0] for _ in range(2)]
        frames = gdb_frames(extc, src, arm) if n_need > 0 else []
        if frames:
            n_need -= 1
        root = root_of(frames)
        key = f"{rel}|{arm}"
        rc, _kind, tail = run_one(extc, src, arm, args.timeout, std_env)
        findings[key] = {
            "src": rel, "arm": arm, "rc": rc, "signal": SIGNAME.get(-rc, str(-rc)),
            "repro": rep, "tail": tail,
            "frames": [f"{fn} @ {loc}" for fn, loc in frames],
            "root": root,
            "pins": {Path(p).name: (run_one(p, src, arm, args.timeout,
                                           {"EXTC_STD": str(ROOT / "stdlib")})[0] < 0) for p in pins},
        }

    # ---- 疑似差异的杂质分类 + 四 pin 归属（同一源、两个档各跑一遍）----
    # 判据是"**对照**判断这个差异属不属于编译器"：
    #   · pin 也复现同样的**行为**差异 ⇒ 既存缺陷（与崩溃同类，不是本次引入的误编译）；
    #   · pin 两档一致、只有当前编译器变 ⇒ **真信号**（`suspect_miscompile` / `suspect_text`）；
    #   · pin 的**文本**也照抖（pid、MB/s、gcc 临时文件名 `/tmp/ccXXXX.o`）⇒ `noise`，语料自身
    #     不确定，不是编译器信号。
    for d in discordant + stderr_discordant:
        src = ROOT / d["src"]
        arm = d["arm"]
        cur_beh = {"plain": d["plain"], arm: d["perturbed"]}
        d["pins"] = {Path(p).name: {a: run_capture(p, src, a, args.timeout,
                                                   {"EXTC_STD": str(ROOT / "stdlib")})
                                    for a in ("plain", arm)} for p in pins}
        beh_diff = cur_beh["plain"] != cur_beh[arm]
        same_armdiff = all((v["plain"]["kind"], v["plain"]["rc"]) != (v[arm]["kind"], v[arm]["rc"])
                           for v in d["pins"].values()) if d["pins"] else None
        pin_repro = d["pins"] and all((v["plain"]["kind"], v["plain"]["rc"]) == cur_beh["plain"]
                                      and (v[arm]["kind"], v[arm]["rc"]) == cur_beh[arm]
                                      for v in d["pins"].values())
        any_pin_beh = any((v["plain"]["kind"], v["plain"]["rc"]) != (v[arm]["kind"], v[arm]["rc"])
                          for v in d["pins"].values()) if d["pins"] else None
        text_jit = any(v["plain"]["out"] != v[arm]["out"] or v["plain"]["err"] != v[arm]["err"]
                       for v in d["pins"].values()) if d["pins"] else None
        if not d["pins"]:
            d["class"] = "unknown(无 pin)"
        elif beh_diff:
            d["class"] = "preexisting" if (same_armdiff and pin_repro) else "suspect_miscompile"
        else:
            d["class"] = "noise" if text_jit else "suspect_text"

    # ---- 根因去重 ----
    by_root = {}
    for key, f in findings.items():
        by_root.setdefault(f["root"], []).append(key)

    # ---- 打印 ----
    timeouts = sum(1 for r in results.values() if r["kind"] == "timeout")
    print(f"\n== 总体（{time.monotonic() - t0:.1f}s）==")
    # 超时用例是"进程组被杀"这条路径的**证据**：为 0 说明这一轮没走到那条路（判据会空转）。
    print(f"  超时被杀（整个进程组）: {timeouts} 次")
    print(f"  编译器崩溃（rc<0）: {len(crashes)} 次 / {len({c[0] for c in crashes})} 个源")
    only_pert = [c for c in crashes
                 if "plain" in arms and results[(str(ROOT / c[0]), "plain")]["kind"] != "crash"]
    print(f"  其中**只在扰动档**崩: {len({c[0] for c in only_pert})} 个源")
    def tally(lst):
        c = {}
        for d in lst:
            c[d.get("class", "?")] = c.get(d.get("class", "?"), 0) + 1
        return "、".join(f"{k}={v}" for k, v in sorted(c.items())) or "-"
    alarm = [d for d in discordant if d.get("class") in ("suspect_miscompile", "suspect_text")]
    print(f"  档位间不一致（疑似误编译）: {len(discordant)} 条"
          f"（分类: {tally(discordant)}）" +
          ("  ⚠️ **有真信号：先停下单独报**" if alarm else ""))
    for d in discordant[:10]:
        print(f"    {d['src']} [{d['arm']}] plain={d['plain']} → perturbed={d['perturbed']}")
        if d.get("stdout") and d["stdout"] != "(stdout 相同)":
            print("      stdout diff:")
            for ln in d["stdout"].splitlines()[:8]:
                print(f"        {ln}")
        if d.get("tail"):
            print(f"      stderr 尾: {d['tail'].splitlines()[-1][:120]}")
        for k, v in (d.get("pins") or {}).items():
            a = d["arm"]
            print(f"      pin {k}: plain={v['plain']['kind']}/{v['plain']['rc']} "
                  f"{a}={v[a]['kind']}/{v[a]['rc']}"
                  f" 文本差异={'是' if (v['plain']['out'] != v[a]['out'] or v['plain']['err'] != v[a]['err']) else '否'}"
                  f"  分类={d.get('class')}")
    print(f"  仅 stderr 不同（诊断变了、行为一样）: {len(stderr_discordant)} 条"
          f"（分类: {tally(stderr_discordant)}）—— **先量后定**，不判红")
    for d in stderr_discordant[:8]:
        print(f"    {d['src']} [{d['arm']}] {d['plain']} 行为相同、诊断不同")
        for ln in d["stderr"].splitlines()[:6]:
            print(f"        {ln}")
        for k, v in (d.get("pins") or {}).items():
            print(f"        pin {k}: {v}")
    print(f"  三档一致的反例（拒绝）: {len(rejects)} 个源；需要 argv/stdin 的 program-fail: {len(programs)} 个源")
    print(f"  驱动器自己的报错（建目录/写文件/非法 .link 指令…，需人工判读）: "
          f"{len(drivers)} 个源" + (f" 如 {drivers[:3]}" if drivers else ""))
    print(f"  去重后根因: {len(by_root)} 个")
    for root, keys in sorted(by_root.items()):
        f = findings[keys[0]]
        uniq = sorted({k.split("|")[0] for k in keys})
        who = "、".join(f"{k}={('崩' if v else '不崩')}" for k, v in f["pins"].items()) or "(无 pin 可比)"
        print(f"    {root}")
        print(f"      {len(keys)} 条崩溃 / {len(uniq)} 个源（如 {f['src']}，档 {f['arm']}，"
              f"{f['signal']}，2/2={f['repro']}）")
        print(f"      pin 对照: {who}")
        for fr in f["frames"][:4]:
            print(f"      {fr}")

    summary = {
        "sources": len(srcs), "arms": arms,
        "timeouts": timeouts,
        "crashes": len(crashes), "crash_sources": sorted({c[0] for c in crashes}),
        "only_perturb_sources": sorted({c[0] for c in only_pert}),
        "drivers": drivers,
        "discordant": discordant,
        "stderr_discordant": stderr_discordant,
        "roots": {r: sorted(ks) for r, ks in by_root.items()},
        "findings": findings,
    }
    (out / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"\n  summary.json → {out / 'summary.json'}")

    if args.report:
        lines = [f"# 扰动普查报告（{time.strftime('%Y-%m-%d %H:%M')}）", "",
                 f"- 语料 {len(srcs)} 个源 × 档位 {', '.join(arms)}",
                 f"- 编译器崩溃 {len(crashes)} 次 / {len(summary['crash_sources'])} 个源；"
                 f"其中只在扰动档 {len(summary['only_perturb_sources'])} 个",
                 f"- 档位间退出码不一致 {len(discordant)} 条（疑似误编译）",
                 f"- 去重根因 {len(by_root)} 个", ""]
        for root, keys in sorted(by_root.items()):
            f = findings[keys[0]]
            lines += [f"## {root}", "",
                      f"- 源 {len(keys)} 个：{', '.join(sorted(k.split('|')[0] for k in keys))}",
                      f"- 代表：`{f['src']}` 档 `{f['arm']}` 信号 `{f['signal']}`，重跑 {f['repro']}",
                      f"- pin 对照：" + "、".join(f"`{k}`={'崩' if v else '不崩'}" for k, v in f["pins"].items()),
                      "", "```"]
            lines += ["  " + x for x in f["frames"][:6]] + ["```", ""]
        (out / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"  report.md → {out / 'report.md'}")

    if args.check:
        bad = set(summary["crash_sources"])
        known = load_known_bad()
        new, stale = sorted(bad - known), sorted(known - bad)
        print(f"\n== 与 known-bad.txt 比（{len(known)} 条）==")
        for s in new:
            print(f"  NEW   {s}")
        for s in stale:
            print(f"  STALE {s}（修好了 ⇒ 从清单里删掉）")
        if new or stale:
            print(f"扰动普查：新增 {len(new)}、过期 {len(stale)} ⇒ 红")
            return 1
        print(f"扰动普查：崩的源 {len(bad)} 个，全在清单里 ⇒ 绿")
        return 0
    return 0 if not crashes else 0      # 普查本身不是门禁；门禁看 --check


if __name__ == "__main__":
    sys.exit(main())
