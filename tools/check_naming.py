#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""命名与写法门禁：一个概念一个拼写。

为什么要有这条判据：CORE 的"一个需求一个答案、加第二种写法必须删一种"过去只靠自觉，
于是同一个概念长出了五种拼写（长度 len()/length()/count()/size()/used()、容量
capacity()/capOf()、收缩 shrink()/shrinkTo(k)、构造 new()/create()/withCap()），
find 还有两种返回约定（-1 哨兵 vs ?T）。人读代码时要为每一处多问一句"这是哪一种"，
而这问句是机制该消灭的。指南写在文档项目 guide/style.md 的命名表里，这条门禁把它
变成机械判据。

口径（客观、可机检）：
  R1 struct / type 声明名 = lowerCamel（首字母小写、不许下划线）
  R2 trait 声明名 = UpperCamel（首字母大写、不许下划线）
  R3 fn / 方法名 = lowerCamel（运算符重载不查：名字不是标识符）
  R4 const 名 = UPPER_SNAKE（全大写，数字与下划线合法）
  R5 顶层 let / var 名 = lowerCamel（顶层全局与函数同风格；整数常量走 const）
  R6 禁用同义方法名（BANNED，见下）：length/size/used/capOf/asMutSlice/create/setUsed
  R7 枚举变体名 = lowerCamel（type x = 后的 | variant 行）

扫描根：stdlib/ + examples/ + bench/（tests/ 故意不查：负例要写各种拼法，
外加外来语料；门禁只管我们交付的代码面）。只扫 .extc。

例外：tools/naming-allowlist.txt，每行 路径<TAB>类别<TAB>名字<TAB>理由。
过期条目也报错 —— 例外是欠账，不是长期免票。

用法：
  python3 tools/check_naming.py --check
  python3 tools/check_naming.py --root DIR --dirs stdlib,examples      # 自测/换根
"""

import argparse
import pathlib
import re
import sys

DEFAULT_DIRS = ("stdlib", "examples", "bench")
# 冻结快照与生成物：bench/**/stamped/ 由 bench/*/make_stamped*.py 生成，语义是"历史发射物"
# （用来复算旧结果），不是现行代码 -- 与 docs/archive 同理，不进命名规范。
SKIP_PARTS = {"stamped", "build"}

LOWER_CAMEL = re.compile(r"^[a-z][A-Za-z0-9]*$")
UPPER_CAMEL = re.compile(r"^[A-Z][A-Za-z0-9]*$")
UPPER_SNAKE = re.compile(r"^[A-Z][A-Z0-9_]*$")

# R6：同一个概念只许一种名字。左 = 不许再出现的名字，右 = 该写什么。
BANNED = {
    "length": "len()（网络报文的字节数叫 bodyLength）",
    "size": "len()（屏幕尺寸叫 dims()）",
    "used": "len()",
    "capOf": "capacity()",
    "asMutSlice": "viewMut()",
    "create": "new() / withCapacity()",
    "setUsed": "setLen()",
    "getSize": "len()",
}

RE_STRUCT = re.compile(r"^\s*(?:pub\s+)?struct\s+([A-Za-z_]\w*)")
RE_TYPE = re.compile(r"^\s*(?:pub\s+)?type\s+([A-Za-z_]\w*)")
RE_TRAIT = re.compile(r"^\s*(?:pub\s+)?trait\s+([A-Za-z_]\w*)")
RE_FN = re.compile(r"^\s*(?:pub\s+)?fn\s+([A-Za-z_]\w*)")
RE_FN_ANY = re.compile(r"\bfn\s+([A-Za-z_]\w*)")  # 同一行里的方法（如 impl 体压成一行）也要查禁用名
RE_CONST = re.compile(r"^\s*(?:pub\s+)?const\s+([A-Za-z_]\w*)")
RE_GLOBAL = re.compile(r"^(?:pub\s+)?(let|var)\s+([A-Za-z_]\w*)")
RE_VARIANT = re.compile(r"^\s*\|\s*([A-Za-z_]\w*)")
RE_ENUM_OPEN = re.compile(r"^\s*(?:pub\s+)?type\s+([A-Za-z_]\w*)[^=]*=\s*$")
RE_ENUM_INLINE = re.compile(r"^\s*(?:pub\s+)?type\s+[A-Za-z_]\w*[^=]*=.*\|")
RE_INLINE_VARIANT = re.compile(r"\|\s*([A-Za-z_]\w*)")


def strip_comment(line: str) -> str:
    """去掉行尾 // 注释；引号内的 // 不算（字符串里常有 http://）。"""
    out = []
    quote = ""
    i = 0
    while i < len(line):
        ch = line[i]
        if quote:
            out.append(ch)
            if ch == "\\" and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if ch == quote:
                quote = ""
            i += 1
            continue
        if ch in "\"'":
            quote = ch
            out.append(ch)
            i += 1
            continue
        if ch == "/" and i + 1 < len(line) and line[i + 1] == "/":
            break
        out.append(ch)
        i += 1
    return "".join(out)


def camel_ok(name: str, upper: bool) -> bool:
    return bool(UPPER_CAMEL.match(name)) if upper else bool(LOWER_CAMEL.match(name))


def scan_file(path: pathlib.Path):
    """返回 (声明数, [(行号, 类别, 名字, 期望)])"""
    text = path.read_bytes().decode("utf-8", "surrogateescape")
    decls = 0
    bad = []
    in_enum = False
    for lineno, raw in enumerate(text.split("\n"), 1):
        line = strip_comment(raw)
        if not line.strip():
            continue
        if in_enum:
            m = RE_VARIANT.match(line)
            if m:
                decls += 1
                name = m.group(1)
                if not camel_ok(name, False):
                    bad.append((lineno, "variant", name, "lowerCamel（枚举变体）"))
                continue
            in_enum = False
        if RE_ENUM_INLINE.match(line):
            for name in RE_INLINE_VARIANT.findall(line.split("=", 1)[-1]):
                decls += 1
                if not camel_ok(name, False):
                    bad.append((lineno, "variant", name, "lowerCamel（枚举变体）"))
        elif RE_ENUM_OPEN.match(line):
            in_enum = True
        m = RE_STRUCT.match(line) or RE_TYPE.match(line)
        if m:
            decls += 1
            name = m.group(1)
            if not camel_ok(name, False):
                bad.append((lineno, "type", name, "lowerCamel（类型名首字母小写）"))
            continue
        m = RE_TRAIT.match(line)
        if m:
            decls += 1
            name = m.group(1)
            if not camel_ok(name, True):
                bad.append((lineno, "trait", name, "UpperCamel（trait 名首字母大写）"))
            continue
        m = RE_FN.match(line)
        if m:
            decls += 1
            name = m.group(1)
            if name in BANNED:
                bad.append((lineno, "member", name, BANNED[name]))
            elif not camel_ok(name, False):
                bad.append((lineno, "fn", name, "lowerCamel（函数/方法名）"))
            continue
        for name in RE_FN_ANY.findall(line):
            if name in BANNED and (lineno, "member", name, BANNED[name]) not in bad:
                bad.append((lineno, "member", name, BANNED[name]))
        m = RE_CONST.match(line)
        if m:
            decls += 1
            name = m.group(1)
            if not UPPER_SNAKE.match(name):
                bad.append((lineno, "const", name, "UPPER_SNAKE（const 名全大写）"))
            continue
        m = RE_GLOBAL.match(line)  # 只匹配列 0 ⇒ 顶层
        if m:
            decls += 1
            name = m.group(2)
            if not camel_ok(name, False):
                bad.append((lineno, "global", name, "lowerCamel（顶层 let/var 名）"))
    return decls, bad


def load_allow(path: pathlib.Path):
    entries = {}
    if not path.is_file():
        return entries
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 4:
            print(f"tools/naming-allowlist.txt:{lineno}: 格式应为 路径<TAB>类别<TAB>名字<TAB>理由")
            sys.exit(2)
        entries[tuple(parts[:3])] = parts[3]
    return entries


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="（默认行为；为电池里的统一写法保留）")
    ap.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parent.parent))
    ap.add_argument("--dirs", default=",".join(DEFAULT_DIRS))
    args = ap.parse_args()
    root = pathlib.Path(args.root)
    allow = load_allow(root / "tools" / "naming-allowlist.txt")

    files = 0
    decls = 0
    violations = []
    for d in [x for x in args.dirs.split(",") if x]:
        base = root / d
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*.extc")):
            rel = p.relative_to(root).as_posix()
            if SKIP_PARTS & set(p.relative_to(root).parts):
                continue
            files += 1
            n, bad = scan_file(p)
            decls += n
            violations.extend((rel, lineno, kind, name, expect) for lineno, kind, name, expect in bad)

    kept = []
    for v in violations:
        rel, lineno, kind, name, expect = v
        if (rel, kind, name) in allow:
            kept.append(v)
        else:
            print("  %s:%d: %s `%s` 应为 %s" % (rel, lineno, kind, name, expect))
    used_keys = {(rel, kind, name) for rel, _, kind, name, _ in kept}
    expired = [k for k in allow if k not in used_keys]
    for rel, kind, name in expired:
        print("  tools/naming-allowlist.txt: 过期例外（不再命中违规）: %s %s `%s`" % (rel, kind, name))
    total = len(violations) - len(kept) + len(expired)
    if total:
        print(f"命名：{total} 处违规（{decls} 个声明 / {files} 个文件）")
        return 1
    print(f"ok 命名：{decls} 个声明 / {files} 个文件，0 处违规")
    return 0


if __name__ == "__main__":
    sys.exit(main())
