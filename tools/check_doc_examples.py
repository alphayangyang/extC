#!/usr/bin/env python3
"""活文档里的 extC 示例必须真能编过 —— 没闸门的例子会先烂。

规则见 `docs/README.md` 维护规则 5 与 `docs/COMMENT-STYLE.md`：
`@code` / 围栏里的例子是**判据**，不是散文。

    tools/check_doc_examples.py            报告
    tools/check_doc_examples.py --gate     有失败就 exit 1（check.sh 调用）

判据（对 `docs/**/*.md`，不含 `archive/`、`manual/` 与 `changelog/`）：

* 围栏 ```` ```extc ```` 且**含 `fn main`** ⇒ 是一段完整程序：必须 `extc --check-c` 通过；
  能跑的还会 `--run` 一次（用 `--run` 的退出码当判据时，页面里要写明期望输出）。
* 围栏 ```` ```extc ```` 且**不含 `fn main`** ⇒ 是片段：**必须**在围栏上一行写
  `<!-- fragment -->`，否则算"未标注片段"——因为片段无法被自动检查，必须显式承认。
* ```sh / ``` 其它语言的块不检查。

临时文件写在 `/tmp/extc-docex/`。
"""
import re
import subprocess
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXTC = ROOT / "build" / "extc"
TMP = pathlib.Path("/tmp/extc-docex")
SKIP_PARTS = {"archive", "manual", "changelog"}
SKIP_NAMES = {"CHANGELOG.md"}

FENCE = re.compile(r"^```(\w*)\s*$")


def pages():
    for p in sorted(ROOT.glob("docs/**/*.md")):
        if SKIP_PARTS & set(p.parts) or p.name in SKIP_NAMES:
            continue
        yield p


def extc_blocks(text):
    """产出 (起始行号, 片段文本, 标注)。标注取 fragment / expect-error / ""。"""
    lines = text.split("\n")
    out, i = [], 0
    while i < len(lines):
        m = FENCE.match(lines[i].strip())
        if m and m.group(1) == "extc":
            start = i
            j = i + 1
            while j < len(lines) and not lines[j].strip().startswith("```"):
                j += 1
            body = "\n".join(lines[start + 1:j])
            mark = lines[start - 1] if start > 0 else ""
            if "<!-- fragment -->" in mark:
                tag = "fragment"
            elif "<!-- expect-error -->" in mark:
                tag = "expect-error"
            else:
                tag = ""
            out.append((start + 1, body, tag))
            i = j
        i += 1
    return out


def main():
    bad, checked, fragments, negative = [], 0, 0, 0
    TMP.mkdir(parents=True, exist_ok=True)
    for p in pages():
        rel = p.relative_to(ROOT).as_posix()
        text = p.read_text(encoding="utf-8")
        for n, body, tag in extc_blocks(text):
            if tag == "fragment":
                fragments += 1
                continue
            src = TMP / f"{p.stem}-{n}.extc"
            src.write_text(body if body.endswith("\n") else body + "\n", encoding="utf-8")
            r = subprocess.run([str(EXTC if EXTC.exists() else "build/extc"), "--check-c", str(src)],
                               capture_output=True, text=True)
            if tag == "expect-error":
                negative += 1
                if r.returncode == 0:
                    bad.append(f"{rel}:{n}: 标了 `<!-- expect-error -->`，但它编过了 ⇒ 要么改标注，要么改例子")
                continue
            if "fn main" not in body:
                bad.append(f"{rel}:{n}: extC 片段没有 `fn main`，也没有 `<!-- fragment -->` / "
                           f"`<!-- expect-error -->` 标注 —— 无法自动检查的片段必须显式承认")
                continue
            checked += 1
            if r.returncode != 0:
                tail = (r.stderr or r.stdout).strip().split("\n")[-3:]
                bad.append(f"{rel}:{n}: 示例编不过（extc --check-c rc={r.returncode}）\n      "
                           + "\n      ".join(tail))

    print(f"extC 示例：完整程序 {checked} 段已检 · 片段 {fragments} 段跳过 · 必须失败的 {negative} 段已核")
    if bad:
        print(f"\n失败 {len(bad)} 条：")
        for b in bad:
            print("  FAIL " + b)
        if "--gate" in sys.argv:
            sys.exit(1)
    else:
        print("ok  活文档里的 extC 完整示例全部编过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
