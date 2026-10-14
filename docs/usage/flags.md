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

debug switches -- diagnostics only, **except the marked below**:
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
  EXTC_RECLAIM=0       turn reclamation off; it is **on by default** -- **CHANGES THE OUTPUT**.
                       A program with a governed allocation point (`new T` / `alloc<T>(n)` /
                       `@overwrite`) gets the slab allocator / holder-count / region-sweep
                       block; `=1` **allows** it (side-by-side runs) but does not force it: a
                       unit proved to have no reclaim work emits no reclamation runtime at all
  EXTC_RC_SHARDS=N     reclamation's shard count, the pause knob -- **CHANGES THE OUTPUT**.
                       Default 1 = one runtime region per logical one; N > 1 splits each
                       logical region into N runtime regions that sweep one at a time
  EXTC_ARENA_FILL=NN   diagnostic only: fill every **non-zeroed** arena allocation with the
                       byte NN (0-255), so "read of never-written arena memory" becomes
                       deterministic instead of load-dependent -- the arena's own
                       `MALLOC_PERTURB_`. Unset (default) writes nothing: byte-identical output
  EXTC_RUN_DIR=DIR     `--run`/`--build` write `<DIR>/<name>.c` and `<DIR>/<name>` instead of
                       `build/<name>.c` and `build/<name>`. **Does not change the program's
                       output** -- it only picks where the artifacts land, so two runs that
                       share a source basename (`a/generic.extc` and `b/generic.extc`) do not
                       overwrite each other. Default `build`, the path every caller gets

  -h, --help       show this help
  EXTC_RC_NOCOUNT=0    re-enable holder counting (it is **off by default**): the default
                       rests on region sweeps alone -- **CHANGES THE OUTPUT**.
  EXTC_RC_DBG_LEVEL=1  diagnostic only: print, for every reference store site, whether the
                       value and its host object sit in the same arena level.
  EXTC_RC_TIME=1       diagnostic only: the generated program counts how often each
                       reclamation component is called and prints the tally to stderr.
  EXTC_RC_SWEEPFREE=1  diagnostic only: event regime -- turn holder counting on and stop
                       sweeping entirely (reclaim only when a holder count hits zero).
  EXTC_DBG_ACYCLIC=1   diagnostic only: at compile time print one line per used function,
                       `[acyclic] <name> fn=<0|1> reason=<...>`, plus a summary.
  EXTC_DBG_RCIDLE=1    diagnostic only: at compile time print one line per used function,
                       `[rcidle] <name> idle=<0|1> reason=<...>` -- the `(a)(b)(c)` proof
                       that its region has no reclaim work to do.
  EXTC_RC_PAUSE=1      diagnostic only: the generated program times every real sweep (one
                       clock read before and after) and prints max/total/collects at exit.
                       Uses a monotonic clock when the platform exposes one, else ISO C
                       clock() (CPU time). See prototype-reclaim/sim.c for the method.
```

## 环境变量与调试开关

带 `EXTC_` 前缀的开关只用于诊断，不改变输出；它们的含义以编译器自身打印的说明为准（见上）。
