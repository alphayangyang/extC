# 驱动开关

本页由编译器自身的 `--help` 生成（`tools/gen_flags.py`），因此与实现同步：`check.sh` 在本页与 `extc --help` 不一致时会失败。

开关共 22 个：`--build` · `--ccflag` · `--cflags` · `--check-c` · `--dump-effects` · `--dump-tokens` · `--help` · `--libs` · `--no-line-map` · `--pkg-config` · `--run` · `-I` · `-L` · `-O0` · `-O2` · `-O3` · `-fsyntax-only` · `-h` · `-l` · `-march` · `-o` · `-w`

```text
extC compiler

usage: build/extc [options] <file.extc>

options:
  -o <file>        write the generated C to this file (default: stdout)
  --run            write generated C to build/<name>.c, compile it, run it
                   (uses $CC, default `cc`; creates ./build/ in the CWD)
  --build          same, but stop before running: the binary is `-o <file>` if
                   given, otherwise build/<name> (this is what a package's build
                   step wants: link flags from <module>.link, no execution)
  --check-c        syntax-check the generated C with `$CC -fsyntax-only`
  -w               suppress warnings
  -I <dir>         add a module search directory (for `use a::b`)
  -l <name>        link with `-l<name>` (repeatable; e.g. `-l z`)
  -L <dir>         add a library search directory (repeatable)
  --ccflag <flag>  append one raw flag to the C compiler command (repeatable)
                   escape hatch, e.g. `--ccflag -l:libsqlite3.so.0` (a soname,
                   which `-l` cannot spell) or `--ccflag -I/usr/include/cairo`
  --pkg-config <name>  add `pkg-config --cflags --libs <name>` (repeatable)
  -O0 .. -O3       optimisation level for the generated C (default: -O2)
  -march=native    allow host-specific instructions (faster, less portable)
  --dump-tokens    lex only; print the token table
  --dump-effects   print each function's effect summary (Addr/Cont, arena rule)
  --no-line-map    do not emit `#line` directives (default: emit them)

debug switches -- diagnostics only, **except the two marked below**:
  EXTC_DBG_ARENA=1     check the arena level the checker computed vs codegen
  EXTC_DBG_QN=1        trace how a qualified name (a::b::c) is parsed/resolved
  EXTC_DBG_M=1         print the per-module renamed-declaration counts
  EXTC_DBG_IMPL=1      print each `impl` block and the type it attaches to
  EXTC_DBG_HOME=1      print each function's home/zone flags and its arena sites
  EXTC_DBG_OWNER=1     report each result slot first reached from another body
  EXTC_DUMP_EFFECTS=1  print each function's effect summary
  EXTC_NO_LEVELPASS=1  skip the arena level pass -- **CHANGES THE OUTPUT**: it moves arena
                       placement in 2 of the 414 golden programs
  EXTC_SELFCHECK=1     run the checker's self-check -- **CAN FAIL THE BUILD** (exit 1)
  EXTC_DBG_PRIMSCAN=1  decide the runtime primitive block by the old line-shape scan
                       instead of the emission-time registry (rollback switch for the
                       codegen worklist refactor; the output is meant to be identical)

  -h, --help       show this help
```

## 环境变量与调试开关

带 `EXTC_` 前缀的开关只用于诊断，不改变输出；它们的含义以编译器自身打印的说明为准（见上）。
