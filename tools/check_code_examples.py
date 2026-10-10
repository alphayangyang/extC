#!/usr/bin/env python3
"""注释里的 `@code` 使用例必须能过编译 —— 免得例子先烂。

    tools/check_code_examples.py          报告
    tools/check_code_examples.py --gate   有例子烂掉就失败（check.sh 调用）

覆盖 `src/**/*.h`、`src/**/*.c`、`stdlib/**/*.extc` 注释里的 `@code … @endcode` 块。

判据：
* **extC 例子**（含 `fn main`）：用 `./build/extc --check-c` 编一遍，再用 `timeout 10 ./build/extc --run`
  真跑一遍，要求退出码 0。
* **extC 片段**（没有 `fn main`）：**包一层 `fn main() -> i32 { … return 0 }` 再编**。
  包着也编不过就判失败 —— 片段无法独立检查，要么写成完整程序，要么标 `!skip` 写明原因。
* **C 例子**：`cc -fsyntax-only`。

语言判定：`@code{.extc}` / `@code{.c}` 显式指定；没写就按内容猜（出现 `fn `、`use `、
`-> i32` 这类extC 特征算 extC，否则算 C）。

显式放行的写法：`@code` 块的第一行写 `!skip`（必须写明原因），闸门会跳过并打印出来。
"""
import re
import subprocess
import sys
import tempfile
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXTC = ROOT / "build" / "extc"

GLOBS = ["src/**/*.h", "src/**/*.c", "stdlib/**/*.extc"]
CODE = re.compile(r"@code(?:\{([^}]*)\})?(.*?)@endcode", re.S)


def blocks(text):
    """产出 (行号, 语言提示, 代码文本)。"""
    out = []
    for m in CODE.finditer(text):
        line = text[: m.start()].count("\n") + 1
        body = m.group(2)
        clean = []
        for l in body.split("\n"):
            l = re.sub(r"^\s*\*?\s?", "", l)          # 去掉注释前缀 ` * `
            clean.append(l)
        while clean and not clean[0].strip():
            clean.pop(0)
        while clean and not clean[-1].strip():
            clean.pop()
        if not clean:
            continue
        out.append((line, (m.group(1) or "").strip(), "\n".join(clean)))
    return out


def guess_lang(hint, body):
    if hint in ("extc", "extC"):
        return "extc"
    if hint in ("c", "C"):
        return "c"
    for sig in ("fn ", "use ", "-> i32", "io::cout", "var ", "let "):
        if sig in body:
            return "extc"
    return "c"


def check(rel, line, lang, body, tmp):
    src = tmp / f"{pathlib.Path(rel).stem}-{line}.{'extc' if lang == 'extc' else 'c'}"
    src.write_text(body + "\n", encoding="utf-8")
    if lang == "c":
        r = subprocess.run(["cc", "-fsyntax-only", str(src)], capture_output=True, text=True)
        if r.returncode != 0:
            tail = (r.stderr or "").strip().split("\n")[:3]
            return f"{rel}:{line}: C 例子过不了 -fsyntax-only\n      " + "\n      ".join(tail)
        return None
    r = subprocess.run([str(EXTC), "--check-c", str(src)], capture_output=True, text=True)
    if r.returncode != 0:
        tail = (r.stderr or r.stdout).strip().split("\n")[-3:]
        return f"{rel}:{line}: extC 例子过不了 --check-c\n      " + "\n      ".join(tail)
    if "fn main" not in body:
        # 片段：包一层 main 再试。包不上就说明它没法独立检查。
        wrapped = "fn main() -> i32 {\n" + body + "\n    return 0\n}\n"
        src.write_text(wrapped, encoding="utf-8")
        r = subprocess.run([str(EXTC), "--check-c", str(src)], capture_output=True, text=True)
        if r.returncode != 0:
            tail = (r.stderr or r.stdout).strip().split("\n")[-3:]
            return (f"{rel}:{line}: extC 片段包一层 `fn main` 也编不过 ⇒ 无法独立检查。"
                    f"要么写成完整程序，要么标 `!skip` 写明原因\n      " + "\n      ".join(tail))
        return None
    r = subprocess.run(["timeout", "10", str(EXTC), "--run", str(src)],
                       capture_output=True, text=True, cwd=str(tmp))
    if r.returncode != 0:
        tail = (r.stderr or r.stdout).strip().split("\n")[-3:]
        return f"{rel}:{line}: extC 例子跑不过（rc={r.returncode}）\n      " + "\n      ".join(tail)
    return None


def main():
    bad, checked, skipped = [], 0, 0
    with tempfile.TemporaryDirectory() as td:
        tmp = pathlib.Path(td)
        for pattern in GLOBS:
            for p in sorted(ROOT.glob(pattern)):
                rel = p.relative_to(ROOT).as_posix()
                text = p.read_text(encoding="utf-8", errors="replace")
                for line, hint, body in blocks(text):
                    first = body.split("\n")[0].strip()
                    if first.startswith("!skip"):
                        skipped += 1
                        print(f"  skip {rel}:{line} —— {first[5:].strip() or '（没写原因）'}")
                        continue
                    lang = guess_lang(hint, body)
                    checked += 1
                    msg = check(rel, line, lang, body, tmp)
                    if msg:
                        bad.append(msg)
    if bad:
        print(f"注释里的 @code 例子：检了 {checked} 段，失败 {len(bad)} 段")
        for m in bad:
            print("  FAIL " + m)
        return 1
    print(f"ok  注释里的 @code 例子全部通过（检了 {checked} 段，显式跳过 {skipped} 段）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
