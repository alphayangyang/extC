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
| concurrency | `serve` | a coroutine (extC), goroutine (Go), thread (Rust), or epoll state machine (C++) per connection, 64 connections x 8 in flight, 200,000 requests, every reply checked by the client. |

## How the reclamation axis is meant to be read

`sessions` is deliberately **not** the same allocation strategy ported four times. It
is each language's ordinary discipline on the same workload:

- C++: `new`/`delete` for the message object, `unordered_map` of session pointers.
- Rust: `Box` dropped immediately, `HashMap` of owned sessions.
- Go: `&event{}` and a `map` of session pointers, so the GC decides.
- extC: the message object is an arena allocation reclaimed as a block when the loop
  frame exits, and the session table is a flat arena array indexed by user id where
  eviction only marks an entry dead.

So the table answers "what does the natural thing cost", not "which allocator is
best". No language was given a strategy it would not write by itself.

## What the first full run says

Medians of five repetitions on one 24-core WSL2 host, all arms printing the same
checksums (`RESULTS.md` has the tables; ratios are to C++):

- **conventional**: `etl` extC 65.0 ms vs C++ 68.0 ms (0.96x), Go 90.7 (1.33x),
  Rust 73.5 (1.08x).
- **reclamation**: `sessions` extC 62.2 ms vs C++ 107.3 ms (0.58x), Go 138.4
  (1.29x), Rust 168.8 (1.57x) -- and peak RSS is 76 MB against 85-86 MB. The
  gap is the axis, not a trick: the other three create and destroy an object per
  record, extC moves an arena pointer and reclaims the frame as a block.
- **parallel**: extC 337.9 / 91.5 / 70.9 ms at 1 / 8 / 24 threads (3.7x and
  4.8x self-speedup), within 10-13% of C++ at each width.
- **concurrency**: 200,000 requests over 64 connections, every reply verified,
  `bad=0` for all four arms: extC 430k req/s, C++ 595k, Go 578k, Rust 565k.

## Caveats

- WSL2 does not expose CPU frequency or power state, so absolute times drift between
  sessions. Only ratios measured in the same session should be compared; `run.sh`
  takes the median of repeated runs for that reason.
- `serve` compares the client's completion time and throughput, not server CPU time:
  the client is the same C program for all four arms, so that is the like-for-like
  number, but a fancier client could shift all four.
- The dataset is synthetic. It has the shape of an access log (a heavy-tailed user
  population, sixteen sites, a long latency tail), not the content of a real one.
