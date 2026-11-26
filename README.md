# extC

extC is a language that compiles to C11: **memory safety, bounds and error checks at compile time; C's speed at run time.** No virtual machine, no shared runtime library, no tracing collector — and you never write a lifetime annotation or a `free` call. Whatever the compiler can prove leaves no trace at run time.

*[中文版](README.zh.md)* · [![ci](https://github.com/alphayangyang/extC/actions/workflows/ci.yml/badge.svg)](https://github.com/alphayangyang/extC/actions)

## Quick start

```sh
make -j"$(nproc)"                             # needs only cc; produces build/extc
./build/extc --version                        # 0.0.1-alpha, target platform, stdlib location
./build/extc --run examples/hello-world.extc   # generate C, compile, run
make install PREFIX="$HOME/.local"            # optional: install as the `extc` command
```

Only **Linux / WSL2** is supported (the coroutine runtime is bound to epoll); supported platforms, the alpha promise and the version policy are in [`RELEASE.md`](RELEASE.md). The guided tour is [`../extc-docs/guide/getting-started.md`](../extc-docs/guide/getting-started.md).

## Shape of the thing

| | extC |
|---|---|
| Target | C11. The generated C is compiled by `cc`; `#line` directives map errors back to the `.extc` source (`--no-line-map` turns that off) |
| Runtime | No shared runtime library; `hello` depends on libc only and is about 16 KB |
| Memory | A scoped arena is the base: leaving a block returns the whole slab; values that outlive their block are covered by a per-region sweep (the compiler decides per unit whether to emit counting code at all; it never walks the whole heap); the language has no `free` |
| Checks | What the compiler can prove becomes a compile error; what it cannot prove becomes a trap carrying a source position. There is no silent-degradation path |
| Escape hatches | Exactly two: `!` (check first, then trap) and `@unchecked` (the only UB channel). Both must be written out in the source |

How reclamation works, what it guarantees and what is still missing: [`../extc-docs/design/RECLAMATION.md`](../extc-docs/design/RECLAMATION.md); the plain-language version is [`../extc-docs/guide/memory-model.md`](../extc-docs/guide/memory-model.md).

## A program and the C it generates

```extc
use std::io

struct point { x: i32  y: i32 }

fn main() -> i32 {
    var p = point { x: 1, y: 2 }
    io::cout << "p = " << p.x << ", " << p.y << "\n"   // p = 1, 2
    var a: [4]i32 = [1, 2, 3, ...]                      // fixed-size array, ... pads with zeros
    let s = a[1..3]                                     // a slice view; provable bounds get no check
    io::cout << "s[0] = " << s[0] << "\n"              // s[0] = 2
    return 0
}
```

The generated C is deliberately boring: no macro tricks, no clever optimizations. To see a whole translation unit run `./build/extc examples/hello.extc`; `--check-c` additionally runs `cc -fsyntax-only` over it. A fuller tour lives in [`examples/tour.extc`](examples/tour.extc); the language reference is [`../extc-docs/reference/language.md`](../extc-docs/reference/language.md).

## Caught at compile time

A provable mistake is not a warning, it is a compile error. Change `a[1..3]` above to an out-of-range index:

<!-- expect-error -->
```extc
fn main() -> i32 {
    var a: [4]i32 = [1, 2, 3, ...]
    return a[7]
}
```

and the compiler answers `bad.extc:3:1: error: index 7 is not inside `[4]i32` (length 4)`, with a note that it can prove the index out of range. A lossy narrowing does not compile either — assigning an `i32` to an `i8` gives `initializer expects i8, found i32`; a narrowing has to be written out. What cannot be proved is not waved through silently: it becomes a trap with a source position, never a wrong answer.

## What you can write

| Area | What is there |
|---|---|
| Language | Generics · `trait` and `dyn` · enums with payloads · `option`/`result`/`?`/`??` · `match` (with a `_` arm) · `if let`/`while let` · multi-name declarations and parallel assignment · slices and references · operator overloading · `f"…"` (with format specifiers) · `any` dispatch by method name · attributes ([language reference](../extc-docs/reference/language.md)) |
| Memory | Lexical-block arenas · `array<T,N>` · `vector<T>` · `buf<T>` · domains; the compiler emits the reclamation runtime only for units that actually need it ([language reference](../extc-docs/reference/language.md)) |
| Concurrency and I/O | Coroutines and a scheduler · threads · an epoll event loop · terminal raw mode ([standard library](../extc-docs/reference/stdlib.md)) |
| Systems and C | `extern!` for C primitives · `@export` to be called from C · dynamic loading · link switches and `--pkg-config` ([modules and C interop](../extc-docs/reference/modules-cabi.md)) |
| Standard library | Console and files · time · terminal · JSON · Base64 · sha1/sha256/hmac · sockets · HTTP · WebSocket · processes · threads · heap slabs · dynamic loading · event loop · containers ([standard library](../extc-docs/reference/stdlib.md)) |

## Four things that rarely live in one language

### Data parallelism the compiler can check

```extc
use std::io

fn main() -> i32 {
    var n: i64 = 1000000
    var out: mut slice<i64> = new i64[n]
    par i in 0..n {                       // the body may not call functions or allocate
        var x: i64 = i * 3 + 7
        var acc: i64 = 0
        while acc < 4 { x = (x * 1103515245 + 12345) % 2147483647  acc = acc + 1 }
        out[i] = x & 1023
    }
    var s: i64 = 0
    var k: i64 = 0
    while k < n { s = s + out[k]  k = k + 1 }
    io::println(f"sum = {s}")
    return 0
}
```

No lock, no atomic, no data race to reason about: the compiler proves each worker writes only its own slot and reads only shared data, and rejects the loop otherwise. The speedup and its criterion live in [`bench/par/`](bench/par/RESULTS.md).

### Coroutines without threads

```extc
use std::io

fn counter(n: i64) -> coroutine<i64> {
    var i: i64 = 0
    while i < n { yield i  i = i + 1 }
}

fn main() -> i32 {
    var total: i64 = 0
    for x in counter(4) { total = total + x }     // pauses a real stack frame, builds no thread
    io::println(f"total = {total}")
    return 0
}
```

`yield` suspends a frame the compiler laid out in advance; resuming it costs about a nanosecond and no allocation. The same runtime drives epoll, which is what a single core serves HTTP with in [`bench/realsvc/`](bench/realsvc/) and [`bench/httpd/`](bench/httpd/README.md).

### Ownership written as labels, not as magic type names

```extc
use std::io
use std::data::buf

fn main() -> i32 {
    var b: buf<i64> = buf<i64>::withCap(4)     // owns its bytes; `@noCopy` makes assignment a move
    var i: i64 = 0
    while i < 10 {
        b.push(i)                               // growing while borrowed traps, never a silent UAF
        i = i + 1
    }
    io::println(f"len={b.len()} last={b.view()[9]}")
    return 0
}
```

Three labels carry the semantics — `@noCopy` (move on value position), `@value` (copyable, with a `clone` hook), `@drop` (releases, hook runs on every exit path) — and nothing in the compiler knows the name `buf`, `string` or `vector`. Write your own container, put the same labels on it, and you get the same lifetime guarantees and the same diagnostics.

### Reclamation you can reason about

Every allocation belongs to a lexical block, so leaving the block returns the whole slab at once; a value that escapes is *promoted* to the caller's region by copying one pointer-sized slot, which is why no reference is ever rewritten and no stack is ever scanned. Cycles, when they can happen, are collected by a mark-and-sweep that is incremental, budgeted and adaptive. And a per-function `idle` proof (no reference field written twice, no reference call across, no reference dropped) lets the compiler emit **no counting and no sweeping code at all** for that function — nearly every function in the workloads needs none. Memory stays bounded instead of growing; the long-run peak comparison is in [`bench/memstudy/`](bench/memstudy/) and [`tests/reclaim-perf/`](tests/reclaim-perf/README.md).

## Where it is fast, and where it is not

Six languages (extC, C, C++, Rust, Go, Java) over five heavy shapes: all data arrives on stdin (one generator produces one input, all six read that same input), each language gets its own full optimisation, and all six outputs must be byte-identical before a run counts. The measuring script is [`bench/bigmatrix/`](bench/bigmatrix/); full results are in [`RESULTS.md`](bench/bigmatrix/RESULTS.md), the table below is a snapshot.

| | extC | C | C++ | Rust | Go | Java |
|---|---|---|---|---|---|---|
| Geometric mean over five shapes (1.00 = fastest on that shape) | **1.17** | 1.52 | 1.62 | 1.80 | 1.94 | 2.23 |
| Peak memory | no higher than C on all five shapes | — | — | — | — | 1.6× to 96× higher |
| Delivered binary | 21–25 KB | 16–20 KB | 16–37 KB | 11.5 MB | 2.4 MB | 3–5 KB |

- On `rebuild` (allocate + reclaim) extC wins by a wide margin: 5.8× faster than C; on `bt` (allocate + recurse), `cdq` (random access) and `mandel` (floating point) it is essentially level with C.
- The bad cell is in the table too: `radix` (sorting 10⁷ u32) is 1.66× slower than C — the bill is the bounds check on slice indexing plus the zero-initialisation guarantee of `new`.
- [`RESULTS.md`](bench/bigmatrix/RESULTS.md) keeps one record worth more than the numbers: for the same out-of-range binary-indexed tree, the C version silently produced a number while the extC version trapped on the spot and printed the source position.

Comparisons closer to applications, each report carrying its own criterion and caveats: [`bench/app/`](bench/app/RESULTS.md) (STL containers against C++ and Go) · [`bench/httpd/`](bench/httpd/README.md) (a ~300-line HTTP/1.1 server) · [`bench/fortran/`](bench/fortran/RESULTS.md) (one numerical kernel against C and gfortran) · [`bench/par/`](bench/par/RESULTS.md) (the language-level `par` form) · [`bench/stream/`](bench/stream/RESULTS.md) (the DRAM cost of a streaming pipeline).

## Why believe it

Run these and the numbers are fresh (`make status` prints pass/fail counts and unpushed commits):

```sh
make -j"$(nproc)"     # needs only cc; produces build/extc
./tests/run.sh        # regressions: positive cases must run right, compile-error cases must be caught, run-time error cases must really trap with a source position
./check.sh precommit  # the above + gates + per-subsystem suites, minus the minute-scale deep dives (≈3 min)
./check.sh ci         # CI tier: the slim correctness subset (no performance or instruction-count criteria)
./check.sh nobench    # before pushing: everything precommit has, plus the deep dives (≈6 min)
./check.sh ratchet    # only after touching codegen: frozen artifact / emission-identity / idle fingerprints
./check.sh            # full acceptance: also the whole corpus under ASan/UBSan plus benchmarks (needs g++, rustc)
```

The gates in `precommit` / `nobench` are invariants checked one by one, not samples: size (any input must produce a verdict — no crash, no hang) · escape soundness · differential (extC and equivalent C must reach the same verdict) · instance wiring · attack library (the accept set compared line by line against a baseline). Above those sit meta-gates: manual coverage · the driver-flags page matching `extc --help` · type-branch exhaustiveness · include guards · concurrency guards, plus the gates that read the separate documentation project when it is checked out. Ratchet material (public-surface coverage, Doxygen debt) may only shrink.

`ratchet` is a separate tier because its criteria are **frozen snapshots**, not pass/fail: it
answers "is the blast radius of this codegen change the batch I expected?". Run it when you touch
`src/back/` or the plan flags in `src/check/` `src/plan/`, then re-freeze deliberately — never
blindly.

## Documentation

Today's usage, the mechanisms and the history live in a **separate documentation project**, [`../extc-docs/`](../extc-docs/) (a sibling clone, not a subdirectory):

- [`../extc-docs/guide/`](../extc-docs/guide/) — getting started · memory model · internals · editor support
- [`../extc-docs/reference/`](../extc-docs/reference/) — language · standard library · modules and C interop
- [`../extc-docs/design/`](../extc-docs/design/) — the mechanisms: reclamation, type and memory model, parallelism, containers…
- [`../extc-docs/plans/`](../extc-docs/plans/) — what is left to do (the only list)
- [`../extc-docs/devlog/changelog/`](../extc-docs/devlog/changelog/) — development history (the only one)
- [`../extc-docs/archive/`](../extc-docs/archive/) — historical snapshots (archaeology only, **do not read as current**)

This repository keeps only the pages that change with the code: [`docs/README.md`](docs/README.md) (build, capabilities, navigation) · [`docs/flags.md`](docs/flags.md) (driver switches, generated) · [`docs/surface.md`](docs/surface.md) (public surface, generated) · [`docs/SUGAR.md`](docs/SUGAR.md) (sugar manual) · [`docs/COMMENT-STYLE.md`](docs/COMMENT-STYLE.md) (comment rules). Maintenance rules: [`docs/README.md`](docs/README.md) and [`../extc-docs/STYLE.md`](../extc-docs/STYLE.md).

## Repository layout

```
src/        the compiler (C11, split by pass)          stdlib/  prelude and standard library (written in extC)
tests/      regressions (one entry: ./check.sh)         bench/  performance and comparison harnesses
docs/       pages that change with the code            examples/  examples
tools/      helpers and gates                          third_party/  third-party bindings (pulled in with -I)
prototype/  early prototypes (archaeology only)         proofs/  machine-checked invariant proofs
```

- To change code, read [`CONTRIBUTING.md`](CONTRIBUTING.md) first.
- What each benchmark measures is in [`bench/README.md`](bench/README.md); how criteria are organised is in [`tests/README.md`](tests/README.md). The documentation project is checked out next to this repository (`../extc-docs`); without it the cross-repository doc gates print `跳过` and exit 0 instead of failing.

## License
Apache License 2.0, copyright Yang Yang. Full text in [`LICENSE`](LICENSE), attribution in [`NOTICE`](NOTICE).
