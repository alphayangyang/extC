#!/usr/bin/env python3
"""文档里写的 API 名必须真的存在 —— 免得文档先烂。

    tools/check_doc_facts.py           报告
    tools/check_doc_facts.py --gate    有找不到的名字就失败（check.sh 调用）

扫 `docs/usage/*.md` 里两种写法：

* `` `mod::name` `` —— `mod` 必须是**真模块**（`std` / `stl`，或 stdlib 里存在的模块名）；
  示意用的假模块（文档里讲错误信息时的 `alpha::open2` 那种）会被跳过。
* `` `Type.method()` `` —— `Type` 必须是源码里出现过的名字。

判据：这些名字必须在 `src/` + `stdlib/` + `tests/` + `examples/` 里出现过。
"""
import re
import subprocess
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
DOCS = sorted((ROOT / "docs" / "usage").glob("*.md"))


def corpus():
    out = subprocess.run(["git", "grep", "-h", "-oE", r"[A-Za-z_][A-Za-z0-9_]*", "--",
                          "src", "stdlib", "tests", "examples"],
                         capture_output=True, text=True, cwd=ROOT).stdout
    return set(out.split())


def real_modules():
    """真模块名：`std` / `stl` 本身，加上 stdlib 下每条 `.extc` 的模块名。"""
    mods = {"std", "stl"}
    for p in (ROOT / "stdlib").rglob("*.extc"):
        mods.add(p.stem)
    return mods


def check():
    names, mods = corpus(), real_modules()
    bad = []
    for p in DOCS:
        t = p.read_text(encoding="utf-8")
        for m in re.finditer(r"`([A-Za-z_][\w]*(?:::[A-Za-z_]\w*)+)`", t):
            full = m.group(1)
            head, last = full.split("::")[0], full.split("::")[-1]
            if head not in mods:
                continue                      # 示意用的假模块
            if last not in names and full not in names:
                bad.append((p.name, full, "模块成员找不到"))
        for m in re.finditer(r"`([A-Za-z_]\w*)\.([a-z]\w*)\(\)`", t):
            ty, meth = m.group(1), m.group(2)
            if ty not in names:
                continue                      # 类型本身不是真名字（示意）
            if meth not in names:
                bad.append((p.name, f"{ty}.{meth}()", "方法名找不到"))
    return bad


def main():
    bad = check()
    if bad:
        print(f"文档里声称存在、源码里找不到的名字：{len(bad)}")
        for f, n, why in bad:
            print(f"  {f}: {n} —— {why}")
        return 1
    print(f"ok  文档里的 API 名都能在源码里找到（扫了 {len(DOCS)} 页）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
