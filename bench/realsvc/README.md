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
- extC: the message object is an arena allocation (it stays in the function's region, so
  peak RSS grows with the record count rather than being reused) and the session table is
  `hashMapI64<session>`; eviction compacts the survivors into a fresh table, because
  `hashMapI64::remove` cannot be called from a function that also takes a view today.

So the table answers "what does the natural thing cost", not "which allocator is
best". No language was given a strategy it would not write by itself.

## What the full run says

Medians of five repetitions on one 24-core WSL2 host, all arms printing the same
checksums (`RESULTS.md` has the tables; ratios are to C++):

- **conventional**: `etl` extC 67.9 ms vs C++ 67.7 ms (1.00x), Go 92.7 (1.37x),
  Rust 73.8 (1.09x).
- **reclamation**: `sessions` extC 178.9 ms vs C++ 121.3 ms (1.47x), Go 170.7 (1.41x),
  Rust 215.2 (1.77x) -- peak RSS 91 MB against 86 MB. All four arms now build one object
  per record and keep a hash table of sessions; extC's record is a frame value (the same
  lifetime Go gets when `&Event{}` does not escape) and its map API returns *values*, so
  a read-modify-write costs two probes (`get` + `put`) where C++'s `operator[]` costs one.
  That gap is the whole difference, and it is an API-shaped gap: exposing the pool handle
  (`slotOf` / `at` / `setAt` / `entry`) would make it one probe per record. The earlier
  0.52x came from not doing this work at all -- a flat array indexed by user id, one
  reused object, no reclamation -- so it measured a different program and is withdrawn.
- **parallel**: extC 306.7 / 76.8 / 62.1 ms at 1 / 8 / 24 threads (self-speedup
  4.0x / 4.94x) against C++ 327.0 / 106.5 / 68.9. Single-threaded the byte
  scan is dominated by the sequential record table, which is why the curve
  flattens; the arm builds that table in one pass, like the C++ arm.
- **concurrency**: 200,000 requests over 64 connections, every reply verified,
  `bad=0` for all four arms: extC 541k req/s, C++ 567k, Go 575k, Rust 479k (the
  four are within run-to-run noise of each other on this host). The first run of
  this arm was 430k: it called `send` once per reply, and the arm now flushes one
  write per read burst like the other three (the profiled difference, and the
  syscall counts behind it, are in the changelog entry).

Two compiler bugs surfaced while measuring, and fixing them moved the parallel
axis: the bounds test of a subscript was never hoisted out of a loop when the
subscript was converted (`i64(v[i])` read as a side effect), and an `if`
condition's subscripts were not covered at all. `parmix` went from 1,705 to
1,351 instructions per record; see the changelog entry.

## Caveats

- WSL2 does not expose CPU frequency or power state, so absolute times drift between
  sessions. Only ratios measured in the same session should be compared; `run.sh`
  takes the median of repeated runs for that reason.
- `serve` compares the client's completion time and throughput, not server CPU time:
  the client is the same C program for all four arms, so that is the like-for-like
  number, but a fancier client could shift all four.
- The dataset is synthetic. It has the shape of an access log (a heavy-tailed user
  population, sixteen sites, a long latency tail), not the content of a real one.
