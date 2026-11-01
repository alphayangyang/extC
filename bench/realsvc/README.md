# realsvc -- one synthetic service, four axes, four languages

A cross-language benchmark that is not a microbenchmark. One synthetic request log
is parsed, aggregated, boxed, scanned in parallel, and served over TCP by the same
four programs -- extC, C++, Go, and Rust -- and each program prints a checksum per
workload. The checksums are how the numbers stay honest: a row that disagrees on
`cs=` is measuring a different amount of work and would be caught before it reached
a table.

The axis this repository cares about is the first one: **what does reclamation cost
when the program is otherwise ordinary**? `sessions` is the axis that asks it, and
the other three are there so that the answer cannot be mistaken for a property of an
artificial loop.

## Layout

```
SPEC.md          the dataset, the four workloads, the exact output line of each
gen/gen.c        dataset generator (xorshift64*, fixed draw order)
data/log.tsv     the dataset, 2,000,000 rows, 71 MB -- regenerated, not checked in
client/client.c  the shared HTTP/TCP client: computes expected replies from the file
cpp/main.cpp     the reference arm
extc/main.extc   the extC arm
go/main.go       the Go arm
rust/main.rs     the Rust arm
run.sh           build all arms, run the four axes pinned and repeated
report.py        RESULTS.tsv -> RESULTS.md
RESULTS.tsv      raw per-run numbers (workload, language, run, work_ms, peak_rss_kb)
out/             per-arm logs: the printed line, the timer detail, the rss
```

## Running it

```sh
./run.sh                      # build what is missing, 5 repetitions per case
./run.sh -n 3 -l "cpp extc"   # fewer repetitions, two arms
./run.sh -m "etl parmix"      # a subset of the axes
./run.sh -r && python3 report.py   # report from the numbers already on disk
```

`run.sh` generates `data/log.tsv` if it is missing (it is 71 MB, so it is not in
git), builds every arm, then runs each case pinned with `taskset`: one core for the
single-threaded axes, `0..t-1` for `parmix` at `t` threads, and the server on one core
with the client on three others for `serve`. The ingest time (`read_ms`) and the
workload time (`work_ms`) are reported separately and the tables compare `work_ms`,
because the ingest is syscalls, not language.

## The four axes

| axis | workload | what it stresses |
| --- | --- | --- |
| conventional | `etl` | one pass, a state machine, an aggregate table. No allocation past the input buffer. |
| reclamation | `sessions` | a message object per record plus a session table with a 300 s TTL; about three quarters of the users seen expire inside the window. |
| parallel | `parmix` | 64 rounds of a mixing function per record, folded into one checksum; 1, 8, and 24 threads. |
| concurrency | `serve` | a coroutine per connection (extC) or a goroutine per connection (Go), or a single-threaded epoll state machine (C++, Rust), 64 connections x 8 in flight, 200,000 requests, every reply checked by the client. |

## How the reclamation axis is meant to be read

`sessions` is deliberately **not** the same allocation strategy ported four times. It
is each language's ordinary discipline on the same workload:

- C++: `new`/`delete` for the message object, `unordered_map` of session pointers.
- Rust: `Box` dropped immediately, `HashMap` of owned sessions.
- Go: `&event{}` and a `map` of session pointers, so the GC decides.
- extC: the message object is a frame value (a struct value in the loop body, the same
  lifetime Go gets when `&Event{}` does not escape) and the session table is
  `hashMapI64<session>` updated through the value-pool handle; eviction compacts the
  survivors into a fresh table (cheaper than erasing the expires one probe at a time).

So the table answers "what does the natural thing cost", not "which allocator is
best". No language was given a strategy it would not write by itself.

## What the full run says

Medians of five repetitions on one 24-core WSL2 host, all arms printing the same
checksums (`RESULTS.md` has the tables; ratios are to C++):

- **conventional**: `etl` extC 69.0 ms vs C++ 70.2 ms (0.98x), Go 95.4 (1.36x),
  Rust 74.9 (1.07x).
- **reclamation**: `sessions` extC 142.1 ms vs C++ 131.5 ms (1.08x), Go 178.8 (1.36x),
  Rust 223.5 (1.70x) -- peak RSS 91 MB against 86 MB. All four arms build one object per
  record and keep a hash table of sessions; extC's record is a frame value (the same
  lifetime Go gets when `&Event{}` does not escape). This axis took two steps: first the
  arm was rewritten to the other arms' shape (an earlier run measured 0.52x by not doing
  that work at all -- a flat array indexed by user id, no per-record object, no table --
  and that number is withdrawn), then the table's own value access moved off the value-pool
  handle onto its dense column. `hashMap` was routing every `get`/`put` through
  `handleAtDense` + `vals.get`, i.e. a slot-array load plus a generation and epoch check,
  to re-derive a dense index the probe had already produced. Reading and writing the dense
  column directly measured 141.9 ms against 176.3 ms for the handle spelling and 185 ms
  before either change. Handles remain for identities that outlive the loop; a loop should
  use the index its own probe handed it. The residual 1.08x is C++'s `operator[]` returning
  a reference: no 32-byte value in and out of the slot. Closing that needs an in-place
  update entry point (a block that lends `mut ref V`) or a struct-of-arrays layout.
- **parallel**: extC 313.4 / 80.8 / 63.5 ms at 1 / 8 / 24 threads (self-speedup
  3.9x / 4.9x) against C++ 329.7 / 83.2 / 69.5. Single-threaded the byte
  scan is dominated by the sequential record table, which is why the curve
  flattens; the arm builds that table in one pass, like the C++ arm.
- **concurrency**: 200,000 requests over 64 connections, every reply verified,
  `bad=0` for all four arms: extC 603k req/s, C++ 582k, Go 583k, Rust 551k (the
  four are within run-to-run noise of each other on this host). The first run of
  this arm was 430k: it called `send` once per reply, and the arm now flushes one
  write per read burst like the other three (the profiled difference, and the
  syscall counts behind it, are in the changelog entry).

Two compiler bugs surfaced while measuring, and fixing them moved the parallel
axis: the bounds test of a subscript was never hoisted out of a loop when the
subscript was converted (`i64(v[i])` read as a side effect), and an `if`
condition's subscripts were not covered at all. `parmix` went from 1,705 to
1,351 instructions per record; see the changelog entry. A third one surfaced while
porting `sessions` to the shape the other three arms use: a deferred protocol call on
a type parameter (`k.hash()` inside `hashMap::find`) left the method's effect summary
incomplete for good, so any function that took a view *and* called
`hashMapI64::remove` was rejected at its call site. The arm still evicts by compacting
the survivors into a fresh table, but now for speed, not because `remove` is
unavailable: 152k swap-removes measured 207-238 ms against ~180 ms for the compaction.

## Caveats

- WSL2 does not expose CPU frequency or power state, so absolute times drift between
  sessions. Only ratios measured in the same session should be compared; `run.sh`
  takes the median of repeated runs for that reason.
- `serve` compares the client's completion time and throughput, not server CPU time:
  the client is the same C program for all four arms, so that is the like-for-like
  number, but a fancier client could shift all four.
- The dataset is synthetic. It has the shape of an access log (a heavy-tailed user
  population, sixteen sites, a long latency tail), not the content of a real one.
