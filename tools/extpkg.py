#!/usr/bin/env python3
"""extpkg.py —— extC 的包工具（v0）。

    tools/extpkg.py verify <包目录>                契约验证（三态：通过 / 被证伪 / 不可验证）
    tools/extpkg.py fetch  [--project DIR] [--offline]
    tools/extpkg.py vendor [--project DIR]
    tools/extpkg.py build  [--project DIR] [--offline] [--extc PATH]

**边界先说清**：联网只发生在 `fetch`（以及会顺手 fetch 的 `build`）里。编译器 `extc` 永远不联网，
它只读本地的 `-I` 目录与 `<module>.link`（`C-ABI.md` §9.20.1）。理由：编译器联网 = 不可复现构建 +
隐式副作用 + 离线/生产机器构建不了。

**三层**：

    packages.toml   你写的需求（精确版本 + 来源）
    extc.lock       解析结果（sha256 + 解出来的文件清单）—— 生成物，不要手改
    ~/.cache/extpkg 下载缓存（`$EXTPKG_CACHE` 可改）
    <项目>/vendor/  落地到项目里的包（可提交 ⇒ 断网机器 clone 即可构建）

**包的形状**（第一个真包见 `~/qqbot-extc/vendor/sqlite/`）：

    <包>/extpkg.toml     元数据 + 声明台账（每条声明的状态 + 判据名）
    <包>/<名字>.extc     绑定（契约就在这里：`extern!` + `effects`）
    <包>/<名字>.link     链接需求（lib / pkgconfig / ccflag）—— driver 直接读，见 C-ABI §9.20.1
    <包>/<C 源>          包自带的 C 侧（extC 表达不了的那些形状）
    <包>/tests/*.extc    判据；文件头 `// expect: <标记>` 声明期望输出里的标记

**判据（`tests/extpkg/run.sh` 钉住）**：首次 fetch 写 lock · 第二次 `--offline` 命中缓存 ·
改 lock 里一个字节必须红 · 空缓存 + `--offline` 必须红且消息可执行 · `vendor` 后清空缓存仍能
`build` · 两次干净 fetch 的 lock 逐字节相同。

**明确不做（v0）**：semver 求解（只写精确版本）、传递依赖（包是叶子）、开放 registry
（策展：包是我们自己写/审的）——下载来的包是**可执行材料**（C 源码 + `.link` 里的 ccflag），
把它交给陌生人等于把构建链交出去。
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXTC = ROOT / "build" / "extc"


def cache_dir() -> Path:
    d = Path(os.environ.get("EXTPKG_CACHE", Path.home() / ".cache" / "extpkg"))
    d.mkdir(parents=True, exist_ok=True)
    return d


# ---------------------------------------------------------------- 读

def load_toml(path: Path) -> dict:
    if not path.is_file():
        sys.exit(f"extpkg: `{path}` 不存在")
    with path.open("rb") as f:
        return tomllib.load(f)


def pkg_source(proj: Path, spec: dict) -> str:
    """`path = "vendor/x"` = 本地包（不下载）；否则 `source` 可以是 http(s)://、
    file://，也可以是一个路径（相对项目根）。"""
    if spec.get("path"):
        return "path:" + spec["path"]
    src = spec.get("source")
    if not src:
        sys.exit(f"extpkg: 依赖 `{spec.get('name', '?')}` 既没有 `path` 也没有 `source`")
    if src.startswith(("http://", "https://", "file://")):
        return src
    return (proj / src).resolve().as_uri()


def dir_digest(base: Path) -> tuple[str, list[str]]:
    """一个目录的内容指纹（路径 + 每个文件的 sha256），给本地包用。
    有序、无时间戳 ⇒ 同样的内容永远同样的指纹。"""
    files = sorted(str(p.relative_to(base)) for p in base.rglob("*") if p.is_file())
    h = hashlib.sha256()
    for rel in files:
        h.update(rel.encode() + b"\0")
        h.update(sha256_file(base / rel).encode() + b"\n")
    return h.hexdigest(), files


def expected_marker(test: Path) -> str | None:
    for line in test.read_text(encoding="utf-8").splitlines()[:8]:
        m = re.match(r"\s*//\s*expect:\s*(.+?)\s*$", line)
        if m:
            return m.group(1)
    return None


# ---------------------------------------------------------------- 锁

def lock_text(entries: list[dict]) -> str:
    """手写 TOML（不引第三方依赖）。字段顺序固定 ⇒ 同样的输入逐字节同样的锁。"""
    out = ["# extc.lock —— extpkg 生成，不要手改。`extpkg fetch` 会重写它。", "version = 1", ""]
    for e in sorted(entries, key=lambda x: x["name"]):
        out.append("[[package]]")
        out.append(f"name = {json.dumps(e['name'])}")
        out.append(f"version = {json.dumps(e['version'])}")
        out.append(f"source = {json.dumps(e['source'])}")
        out.append(f"sha256 = {json.dumps(e['sha256'])}")
        out.append(f"root = {json.dumps(e['root'])}")
        files = ", ".join(json.dumps(f) for f in e["files"])
        out.append(f"files = [{files}]")
        out.append("")
    return "\n".join(out)


def read_lock(proj: Path) -> dict:
    path = proj / "extc.lock"
    if not path.is_file():
        return {}
    data = load_toml(path)
    lock = {}
    for p in data.get("package", []):
        p = dict(p)
        if "version" in p:
            p["version"] = str(p["version"])
        lock[p["name"]] = p
    return lock


# ---------------------------------------------------------------- 下载 / 解包

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dst: Path) -> None:
    with urllib.request.urlopen(url, timeout=120) as r, dst.open("wb") as f:
        shutil.copyfileobj(r, f)


def safe_extract(archive: Path, dst: Path) -> str:
    """解包并返回顶层目录名。拒绝绝对路径与 `..`（归档是外来数据）。"""
    with tarfile.open(archive, "r:*") as tf:
        names = tf.getnames()
        for n in names:
            p = Path(n)
            if p.is_absolute() or ".." in p.parts:
                sys.exit(f"extpkg: 归档里的路径不安全：`{n}`")
        roots = {Path(n).parts[0] for n in names if n.strip()}
        if len(roots) != 1:
            sys.exit(f"extpkg: 归档必须只有一个顶层目录，实得 {sorted(roots)}")
        tf.extractall(dst)
    return roots.pop()


# ---------------------------------------------------------------- fetch

def cmd_fetch(proj: Path, offline: bool) -> int:
    man = load_toml(proj / "packages.toml")
    deps = man.get("dependencies", {})
    if not deps:
        sys.exit("extpkg: packages.toml 里没有 [dependencies]")
    old = read_lock(proj)
    cache = cache_dir()
    entries = []
    for name in sorted(deps):
        spec = deps[name]
        version = str(spec.get("version", "?"))
        url = pkg_source(proj, {"name": name, **spec})
        if url.startswith("path:"):
            local = (proj / url[5:]).resolve()
            if not local.is_dir():
                sys.exit(f"extpkg: 依赖 `{name}` 的 path 不存在：{local}")
            digest, files = dir_digest(local)
            want = old.get(name, {}).get("sha256")
            if want and want != digest:
                sys.exit(f"extpkg: 本地包 `{name}` 的内容变了！\n"
                         f"        锁里：{want}\n        实得：{digest}\n"
                         f"        （本地包也要可复现：确认改动是有意的，再删掉 extc.lock 重跑。）")
            entries.append({"name": name, "version": version, "source": url,
                            "sha256": digest, "root": ".", "files": files})
            print(f"  local  {name} {version}：{len(files)} 个文件（{local}）")
            continue

        archive = cache / f"{name}-{version}.download"
        unpacked = cache / f"{name}-{version}"

        if not archive.is_file():
            if offline:
                sys.exit(f"extpkg: --offline 但缓存里没有 `{name}-{version}`"
                         f"（先跑一次 `extpkg fetch`；vendor 过的项目不用再下）")
            print(f"  fetch  {name} {version} <- {url}")
            tmp = archive.with_suffix(".part")
            download(url, tmp)
            tmp.rename(archive)

        got = sha256_file(archive)
        want = old.get(name, {}).get("sha256")
        if want and want != got:
            sys.exit(f"extpkg: `{name}` 的哈希不符！\n"
                     f"        锁里：{want}\n"
                     f"        实得：{got}\n"
                     f"        （来源变了，或者缓存被改过。确认无误就删掉 extc.lock 重跑一次。）")
        if not want:
            print(f"  首次获取 {name} {version}：sha256={got}")
            print(f"          （已写进 extc.lock；之后所有构建按这个哈希校验）")

        if not unpacked.is_dir():
            shutil.rmtree(unpacked, ignore_errors=True)
            unpacked.mkdir(parents=True)
            root = safe_extract(archive, unpacked)
        else:
            roots = [d.name for d in unpacked.iterdir() if d.is_dir()]
            root = roots[0] if len(roots) == 1 else "."
        if root != ".":
            # 归档的顶层目录只是打包习惯（`tar czf x.tar.gz honest/`）：摊平，
            # 让 `cache/<name>-<version>/` 与 `vendor/<name>/` 里**就是包本身**。
            top = unpacked / root
            for child in sorted(top.iterdir()):
                child.rename(unpacked / child.name)
            top.rmdir()
        base = unpacked          # 摊平之后，缓存目录里就是包本身
        files = sorted(str(p.relative_to(base)) for p in base.rglob("*") if p.is_file())
        entries.append({"name": name, "version": version, "source": url,
                        "sha256": got, "root": root, "files": files})
        print(f"  ok     {name} {version}：{len(files)} 个文件，缓存 {unpacked}")

    (proj / "extc.lock").write_text(lock_text(entries), encoding="utf-8")
    print(f"  lock   {proj / 'extc.lock'}（{len(entries)} 个包）")
    return 0


# ---------------------------------------------------------------- vendor

def cmd_vendor(proj: Path) -> int:
    lock = read_lock(proj)
    if not lock:
        sys.exit("extpkg: 没有 extc.lock（先跑 `extpkg fetch`）")
    cache = cache_dir()
    for name in sorted(lock):
        e = lock[name]
        if str(e.get("source", "")).startswith("path:"):
            print(f"  local  {name}：本地包，不用 vendor（就在 {e['source'][5:]}）")
            continue
        src = cache / f"{name}-{e['version']}"
        if not src.is_dir():
            sys.exit(f"extpkg: 缓存里没有 `{name}-{e['version']}`（先跑 `extpkg fetch`）")
        dst = proj / "vendor" / name
        shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(src, dst)
        (dst / ".extpkg-source").write_text(
            f"{e['source']}\nsha256={e['sha256']}\n", encoding="utf-8")
        print(f"  vendor {name} {e['version']} -> {dst}")
    print(f"  ok     {len(lock)} 个包进 vendor/（可提交；提交之后断网机器也能构建）")
    return 0


# ---------------------------------------------------------------- build

def cmd_build(proj: Path, offline: bool, extc: Path) -> int:
    man = load_toml(proj / "packages.toml")
    if not (proj / "extc.lock").is_file():
        cmd_fetch(proj, offline)
    lock = read_lock(proj)
    needs_vendor = any(not str(lock[n].get("source", "")).startswith("path:")
                       and not (proj / "vendor" / n).is_dir() for n in lock)
    if needs_vendor:
        cmd_vendor(proj)

    build = man.get("build", {})
    entry = build.get("entry")
    if not entry:
        sys.exit("extpkg: packages.toml 的 [build] 里没有 `entry`")
    out = Path(build.get("out", f"build/{man.get('project', {}).get('name', 'app')}"))
    if not out.is_absolute():
        out = proj / out
    out.parent.mkdir(parents=True, exist_ok=True)

    argv = [str(extc), "-w", "--build", "-o", str(out)]
    for name in sorted(lock):
        src = str(lock[name].get("source", ""))
        pkg_dir = (proj / src[5:]).resolve() if src.startswith("path:") else proj / "vendor" / name
        argv += ["-I", str(pkg_dir)]
        meta = pkg_dir / "extpkg.toml"
        if meta.is_file():
            for src in load_toml(meta).get("sources", {}).get("c", []):
                argv += ["--ccflag", str(pkg_dir / src)]
    argv.append(str(proj / entry))
    print("  build  " + " ".join(argv[1:]))
    rc = subprocess.run(argv, cwd=proj).returncode
    if rc != 0:
        return rc
    print(f"  ok     {out}")
    return 0


# ---------------------------------------------------------------- verify

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


def check_libs(pkg: Path) -> list[str]:
    """包目录里每个 `.link` 声明的库，在本机找不找得到（找不到不是失败，但要说出来）。
    按目录扫而不是按包名拼：包名与模块名不必相同（`honest` 包里的模块可以叫 `demo`）。"""
    links = sorted(pkg.glob("*.link"))
    if not links:
        return ["没有 .link（链接需求未声明）"]
    notes = []
    probe = subprocess.run(["ldconfig", "-p"], capture_output=True, text=True)
    for link in links:
        for line in link.read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if not line.startswith("lib "):
                continue
            lib = line[4:].strip()
            found = lib.lstrip(":") in probe.stdout
            notes.append(f"{link.name}: {lib}：{'本机有' if found else '**本机找不到**'}")
    return notes


def cmd_verify(pkg: Path, extc: Path) -> int:
    man = load_toml(pkg / "extpkg.toml")
    meta = man.get("package", {})
    name = meta.get("name", pkg.name)
    version = meta.get("version", "?")
    sources = man.get("sources", {}).get("c", [])
    claims = man.get("claim", [])

    print(f"包 {name} {version} · 契约验证（{len(claims)} 条声明）")
    for note in check_libs(pkg):
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


# ---------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(prog="extpkg", description="extC 包工具（verify / fetch / vendor / build）")
    ap.add_argument("command", choices=["verify", "fetch", "vendor", "build"])
    ap.add_argument("target", nargs="?", help="verify: 包目录；其余: 项目目录（默认 .）")
    ap.add_argument("--project", default=".", help="项目目录（默认当前目录）")
    ap.add_argument("--offline", action="store_true", help="绝不联网（缓存里没有就报错）")
    ap.add_argument("--extc", default=str(DEFAULT_EXTC))
    a = ap.parse_args()

    extc = Path(a.extc)
    if not extc.is_file():
        sys.exit(f"extpkg: 编译器不在 `{extc}`（先 make，或用 --extc 指路）")

    if a.command == "verify":
        if not a.target:
            sys.exit("extpkg: verify 需要一个包目录")
        return cmd_verify(Path(a.target).resolve(), extc)
    proj = Path(a.target or a.project).resolve()
    if a.command == "fetch":
        return cmd_fetch(proj, a.offline)
    if a.command == "vendor":
        return cmd_vendor(proj)
    return cmd_build(proj, a.offline, extc)


if __name__ == "__main__":
    sys.exit(main())
