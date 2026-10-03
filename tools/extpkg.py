#!/usr/bin/env python3
"""extpkg.py —— 包的第一件工具：`verify`（契约一致性验证）。

    tools/extpkg.py verify <包目录> [--extc build/extc]

**为什么是 verify 而不是 install**：包管理器里 90% 是机械活（取头、生成绑定、拼链接行），
机械活这里先不做；真正需要设计的是**验证**——`effects` 是信任不是证明（`C-ABI.md` §9.21）。
`verify` 把包里**有判据**的声明跑一遍，输出三态：

    通过 / 被证伪 / 不可验证

包目录的形状（第一个包见 `~/qqbot-extc/vendor/sqlite/`）：

    <包>/extpkg.toml     声明台账：每条声明 + 它的状态 + 判据文件名
    <包>/<名字>.extc     绑定（契约就在这里：extern! + effects）
    <包>/<名字>.link     链接需求（lib / pkgconfig / ccflag）—— driver 直接读，见 C-ABI §9.20.1
    <包>/<C 源>          包自带的 C 侧（extC 表达不了的那些形状）
    <包>/tests/*.extc    判据；文件头 `// expect: <标记>` 声明期望输出里的标记

判据怎么跑：`extc -w -I <包> [--ccflag <包>/<C 源>] --run <包>/tests/<判据>.extc`，
工作目录在临时目录里（`--run` 会建 `build/`，不要污染调用者的仓库）。rc 必须是 0，
且输出里必须出现那个标记——被证伪的判据（硬缺页 rc=139、或打印 falsified）都算失败。

退出码：0 = 所有**可验证**的声明都通过；1 = 有声明被证伪或判据跑不起来。
「不可验证」「不适用」不是失败，但它们会被原样打印出来——台账的意义就在这。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXTC = ROOT / "build" / "extc"


def load_manifest(pkg: Path) -> dict:
    path = pkg / "extpkg.toml"
    if not path.is_file():
        sys.exit(f"extpkg: `{path}` 不存在（包目录里要有 extpkg.toml）")
    with path.open("rb") as f:
        return tomllib.load(f)


def expected_marker(test: Path) -> str | None:
    """判据文件头的 `// expect: <标记>`。没有就只判退出码。"""
    for line in test.read_text(encoding="utf-8").splitlines()[:8]:
        m = re.match(r"\s*//\s*expect:\s*(.+?)\s*$", line)
        if m:
            return m.group(1)
    return None


def run_claim(extc: Path, pkg: Path, sources: list[str], claim: dict, workdir: Path) -> tuple[bool, str]:
    test = pkg / "tests" / f"{claim['test']}.extc"
    if not test.is_file():
        return False, f"判据文件不存在：{test}"
    argv = [str(extc), "-w", "-I", str(pkg)]
    for src in sources:
        argv += ["--ccflag", str(pkg / src)]
    argv += ["--run", str(test)]
    proc = subprocess.run(argv, cwd=workdir, capture_output=True, text=True, timeout=300)
    out = (proc.stdout + proc.stderr).strip()
    want = expected_marker(test)
    if proc.returncode != 0:
        tail = next((l for l in reversed(out.splitlines()) if l.strip()), "")
        if proc.returncode == 255:
            tail = f"被信号杀死（例如隔离页上的硬缺页）{('：' + tail) if tail else ''}"
        else:
            tail = f"rc={proc.returncode}{('：' + tail) if tail else ''}"
        return False, tail
    if want and want not in out:
        return False, f"缺标记「{want}」：{out.splitlines()[-1][:70] if out else ''}"
    return True, want or "rc=0"


def check_libs(pkg: Path, name: str) -> list[str]:
    """`.link` 里声明的库在本机找不找得到（找不到不是失败，但要说出来）。"""
    link = pkg / f"{name}.link"
    if not link.is_file():
        return [f"没有 {name}.link（链接需求未声明）"]
    notes = []
    for line in link.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line.startswith("lib "):
            continue
        lib = line[4:].strip()
        probe = subprocess.run(["ldconfig", "-p"], capture_output=True, text=True)
        found = lib.lstrip(":") in probe.stdout
        notes.append(f"{lib}：{'本机有' if found else '**本机找不到**'}")
    return notes


def main() -> int:
    ap = argparse.ArgumentParser(prog="extpkg", description="extC 包工具（第一件：verify）")
    ap.add_argument("command", choices=["verify"])
    ap.add_argument("package", help="包目录（含 extpkg.toml）")
    ap.add_argument("--extc", default=str(DEFAULT_EXTC))
    a = ap.parse_args()

    extc = Path(a.extc)
    if not extc.is_file():
        sys.exit(f"extpkg: 编译器不在 `{extc}`（先 make，或用 --extc 指路）")
    pkg = Path(a.package).resolve()
    man = load_manifest(pkg)
    meta = man.get("package", {})
    name = meta.get("name", pkg.name)
    version = meta.get("version", "?")
    sources = man.get("sources", {}).get("c", [])
    claims = man.get("claim", [])

    print(f"包 {name} {version} · 契约验证（{len(claims)} 条声明）")
    for note in check_libs(pkg, name):
        print(f"  链接  {note}")
    print()

    passed = falsified = unverifiable = napp = 0
    bad = False
    work = Path(tempfile.mkdtemp(prefix="extpkg-"))
    try:
        for claim in claims:
            cid = claim.get("id", "?")
            state = claim.get("state", "unverifiable")
            if state != "verifiable":
                count = "不可验证" if state == "unverifiable" else "不适用"
                if state == "unverifiable":
                    unverifiable += 1
                else:
                    napp += 1
                print(f"  {count}  {cid:<26} {claim.get('statement', '')}")
                if claim.get("reason"):
                    print(f"            ↳ {claim['reason']}")
                continue
            if not claim.get("test"):
                print(f"  被证伪    {cid:<26} 声明为可验证，却没有判据")
                bad = True
                falsified += 1
                continue
            ok, detail = run_claim(extc, pkg, sources, claim, work)
            if ok:
                passed += 1
                print(f"  通过    {cid:<26} {detail}")
            else:
                falsified += 1
                bad = True
                print(f"  被证伪    {cid:<26} {detail}")
                print(f"            ↳ 声明：{claim.get('statement', '')}")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print()
    print(f"{len(claims)} 条声明：通过 {passed} · 被证伪 {falsified} · 不可验证 {unverifiable} · 不适用 {napp}")
    if unverifiable or napp:
        print("（「不可验证」不是失败：它表示这条声明**没有可观察的失效形态**，或判据还没写；")
        print("  台账的意义就是把这三态摆在一起，而不是把'没验'说成'验过'。）")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
