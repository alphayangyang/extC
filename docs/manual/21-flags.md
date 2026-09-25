# 驱动开关

本页由编译器自身的 `--help` 生成（`tools/gen_flags.py`），因此与实现同步：`check.sh` 在本页与 `extc --help` 不一致时会失败。

开关共 15 个：`--check-c` · `--dump-effects` · `--dump-tokens` · `--help` · `--no-line-map` · `--run` · `-I` · `-O0` · `-O2` · `-O3` · `-fsyntax-only` · `-h` · `-march` · `-o` · `-w`

```text
extC compiler

usage: build/extc [options] <file.extc>

options:
  -o <file>        write the generated C to this file (default: stdout)
  --run            write generated C to build/<name>.c, compile it, run it
                   (uses $CC, default `cc`; creates ./build/ in the CWD)
  --check-c        syntax-check the generated C with `$CC -fsyntax-only`
  -w               suppress warnings
  -I <dir>         add a module search directory (for `use a::b`)
  -O0 .. -O3       optimisation level for the generated C (default: -O2)
  -march=native    allow host-specific instructions (faster, less portable)
  --dump-tokens    lex only; print the token table
  --dump-effects   print each function's effect summary (Addr/Cont, arena rule)
  --no-line-map    do not emit `#line` directives (default: emit them)

debug switches (they never change the output):
  EXTC_DBG_ARENA=1     check the arena level the checker computed vs codegen
  EXTC_DBG_QN=1        trace how a qualified name (a::b::c) is parsed/resolved
  EXTC_DUMP_EFFECTS=1  print each function's effect summary

  -h, --help       show this help
```

## 环境变量与调试开关

带 `EXTC_` 前缀的开关只用于诊断，不改变输出；它们的含义以编译器自身打印的说明为准（见上）。
