#!/usr/bin/env python3
"""tools/check_module_tramp.py --- 守住 @module 分发器的形状与零税。

## 为什么需要这个门禁

一个二进制、一个运行期：C 的 main 由编译器生成，是分发器；每个 @module 入口编成 static 的
__extc_entry_<模块>$<函数>，表 extc_module_entries[] 记名字、描述、函数指针。
./a -m <name> args... 查表、把命令行原地压紧（argv[k - 2] = argv[k]）后调入口；不带 -m 跑根
入口 main；--help 列名字与描述；未知模块 / -m 缺名字 exit 2。

四件事只在「有 @module」的程序里存在，坏掉都不会让普通测试变红：入口助手退回外部链接
（两个程序链接时符号撞车）、分发器丢掉 -m 分支（命令行静默跑根入口）、表丢一行（-m 少了
一个模块）、普通程序里混进分发器（零税失守）。所以这里对真实发射断言结构，再断
src/back/codegen.c 里那条发射规则，--selftest 做 10 条注入、每条必须变红。
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTC = Path(os.environ.get("EXTC", str(ROOT / "build" / "extc")))
MOD = ROOT / "tests/module/main.extc"
PLAIN = ROOT / "tests/ops/conv_int_float.extc"
CODEGEN = ROOT / "src/back/codegen.c"

TABLE_DEF = ("typedef struct ExtcModuleEntry { const char *name; const char *desc; "
             "int (*fn)(int, char **); } ExtcModuleEntry;")
ENTRIES = "static const ExtcModuleEntry extc_module_entries[] = {"
ROW_DEMO = '{ "demo", "prints how many arguments it got, then exits \\"7\\"", __extc_entry_demo$run },'
ROW_CALC = '{ "calc", "", __extc_entry_calc$run },'
ENTRY_DEF = "static int __extc_entry_demo$run(int argc, char **argv) {"
ENTRY_MAIN = "static int __extc_entry_main(int argc, char **argv) {"
C_MAIN = "int main(int argc, char **argv) {"
M_FLAG = 'strcmp(argv[1], "-m") == 0'
HELP_FLAG = 'strcmp(argv[1], "--help") == 0'
SHIFT = "for (int k = 3; k < argc; k++) argv[k - 2] = argv[k];"
CALL = "return extc_module_entries[i].fn(argc - 2, argv);"
ROOT_CALL = "return __extc_entry_main(argc, argv);"
USAGE = "extc_module_usage(argv[0]);"
MISSING = '"%s: -m needs a module name\\n"'
UNKNOWN = '"%s: unknown module \'%s\'\\n"'

SRC = {
    "cgIsEntry": "static bool cgIsEntry(CG *g, const FuncDef *f) {",
    "cgEntryCName": "if (g->needModuleTramp && cgIsEntry(g, f))",
    "param": 'bufPuts(&sig, (g->needModuleTramp || f->params.len) ? "int argc, char **argv" : "void");',
    "isEntry": "bool isEntry = isMain || (f && f->isModuleEntry);",
    "noArena": "if (isEntry && planUsesHome(f)) g->noArena = false;",
    "scan": "static void scanModuleEntries(CG *g, Module *m) {",
    "tramp": "static void emitModuleTramp(CG *g) {",
    "scan_call": "scanModuleEntries(&g, m);",
    "tramp_call": "if (g->needModuleTramp) emitModuleTramp(g);",
    "keep": "!f->isModuleEntry && !f->isExport",
    "same_module": '"module %s already has an @module entry"',
    "dup_name": '"duplicate module entry name %s"',
    "flag": "g->needModuleTramp = g->moduleEntries.len > 0;",
}


def emit(src, tag, work):
    out = work / (tag + ".c")
    env = dict(os.environ)
    env["EXTC_RECLAIM"] = "1"
    r = subprocess.run([str(EXTC), "-w", "--no-line-map", str(src), "-o", str(out)],
                       cwd=str(ROOT), env=env, capture_output=True, text=True, timeout=180)
    if r.returncode != 0:
        raise RuntimeError("emit %s failed (exit %d): %s"
                           % (tag, r.returncode, (r.stderr or r.stdout).strip()[:400]))
    if not out.exists():
        raise RuntimeError("emit %s wrote no output" % tag)
    return out.read_text()


def check(mod, plain, codegen):
    bad = []
    if TABLE_DEF not in mod:
        bad.append("the module program has no ExtcModuleEntry table type")
    if ENTRIES not in mod:
        bad.append("the module program has no extc_module_entries[] table")
    for row, name in ((ROW_DEMO, "demo"), (ROW_CALC, "calc")):
        if row not in mod:
            bad.append("the table has no row for %s (a compiled-in entry is unreachable)" % name)
    n = mod.count("static int __extc_entry_")
    if n != 6:
        bad.append("expected 6 internal-linkage entry occurrences (3 prototypes + 3 definitions),"
                   " found %d (an entry that is not static collides when two programs link)" % n)
    if ENTRY_DEF not in mod or ENTRY_MAIN not in mod:
        bad.append("entry helpers are not emitted as static functions with the C entry signature")
    if C_MAIN not in mod:
        bad.append("C main(int argc, char **argv) is missing (the dispatcher is not the entry)")
    for needle, why in ((M_FLAG, "does not recognise -m on the command line"),
                        (HELP_FLAG, "does not recognise --help"),
                        (SHIFT, "does not compact argv in place before calling the entry"),
                        (CALL, "does not call the table row with (argc - 2, argv)"),
                        (USAGE, "does not print the usage list on the error paths")):
        if needle not in mod:
            bad.append("the dispatcher %s" % why)
    if ROOT_CALL not in mod:
        bad.append("the dispatcher does not fall back to the program own entry without -m")
    if MISSING not in mod or UNKNOWN not in mod:
        bad.append("the dispatcher does not report -m without a name / an unknown module")
    if mod.count("return 2;") < 2:
        bad.append("the two usage errors do not exit 2")
    if "ExtcModuleEntry" in plain or "__extc_entry_" in plain or "extc_module_usage" in plain:
        bad.append("plain: a program with no @module pays for the dispatcher (zero-tax broken)")
    src = codegen.read_text() if codegen.exists() else ""
    for key, needle in SRC.items():
        if needle not in src:
            bad.append("src/back/codegen.c: the %s rule is missing" % key)
    return bad


SELFTEST = [
    ("del-row", "mod", ROW_CALC, ""),
    ("main-void", "mod", C_MAIN, "int main(void) {"),
    ("no-m-flag", "mod", M_FLAG, "0"),
    ("no-shift", "mod", SHIFT, ""),
    ("entry-extern", "mod", ENTRY_DEF, ENTRY_DEF.replace("static ", "", 1)),
    ("plain-tax", "plain", "int main(void) {",
     "typedef struct ExtcModuleEntry { int x; } ExtcModuleEntry;\nint main(void) {"),
    ("src-no-scan", "src", SRC["scan_call"], ";"),
    ("src-no-entry", "src", SRC["isEntry"], "bool isEntry = isMain;"),
    ("src-no-keep", "src", SRC["keep"], "!f->isExport"),
    ("src-no-param", "src", SRC["param"], 'bufPuts(&sig, "int argc, char **argv");'),
]


def selftest(mod, plain, codegen):
    bad = []
    src = codegen.read_text() if codegen.exists() else ""
    for name, which, old, new in SELFTEST:
        base = {"mod": mod, "plain": plain, "src": src}[which]
        if old not in base:
            bad.append("injection %s anchor not found (sync this gate when emission changes)" % name)
            continue
        mutated = base.replace(old, new)
        if which == "mod":
            got = check(mutated, plain, codegen)
        elif which == "plain":
            got = check(mod, mutated, codegen)
        else:
            tmp = Path(tempfile.mkdtemp()) / "codegen.c"
            tmp.write_text(mutated)
            got = check(mod, plain, tmp)
        if not got:
            bad.append("injection %s does not turn the check red (check is a no-op)" % name)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if not EXTC.exists():
        print("FAIL compiler not found: %s (run make first)" % EXTC)
        return 1
    with tempfile.TemporaryDirectory() as td:
        work = Path(td)
        try:
            mod = emit(MOD, "mod", work)
            plain = emit(PLAIN, "plain", work)
        except RuntimeError as e:
            print("FAIL %s" % e)
            return 1
    bad = check(mod, plain, CODEGEN)
    if args.selftest:
        bad += selftest(mod, plain, CODEGEN)
    if bad:
        print("FAIL module dispatcher structure:")
        for b in bad:
            print("  - " + b)
        return 1
    print("ok module tramp（C main 是分发器：-m 查表原地压紧 argv、--help 列名字与描述、两个用法错"
          " exit 2、入口 static；没有 @module 的程序零税）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

