# realsvc — a log-analytics service, measured on four axes

This benchmark is one realistic system seen from four angles. It is not a kernel
microbenchmark: every phase reads the same production-shaped log file, and the
phases are the ones a log-analytics service actually runs — ingest, session
store, query serving, and a parallel scoring pass.

Four implementations: **extC**, **C++**, **Go**, **Rust**. Each one implements
the same four workloads over the same dataset and must print the same
`cs=` checksum, byte for byte.

The rules that make the comparison honest:

* one dataset, generated once by `gen/gen.c`, read by every implementation;
* every implementation uses its language's normal container and I/O facilities
  (no hand-rolled allocators, no `unsafe`, no compiler-specific intrinsics);
* work is measured *after* the file has been read into memory, so the number
  reported is CPU work, not disk;
* absolute numbers are machine state, the **ratios** are the result.

## 1. Dataset

`data/log.tsv`, 2,000,000 lines (71,582,794 bytes). Fields are separated
by a single TAB, the line ends with a single `\n`, there is no trailing
whitespace and no header.

| # | field  | type | range / value |
|---|--------|------|---------------|
| 0 | ts     | i64  | `1712000000 + i/1000` (unix seconds) |
| 1 | uid    | i64  | heavy-tailed: 90% of records draw `[0, 19999]`, the other 10% draw `[20000, 199999]` |
| 2 | site   | i64  | `[0, 15]` |
| 3 | path   | i64  | `[0, 23]` |
| 4 | status | i64  | one of `200 201 204 301 400 403 404 500 502` |
| 5 | lat    | i64  | microseconds, `[0, 1999]`, except one record in 1000 drawn from `[100000, 299999]` |
| 6 | bytes  | i64  | `[0, 65535]` |

`i` is the zero-based record index. The generator uses xorshift64* in a fixed
order, so the file is reproducible from `(seed, lines)`.

## 2. Common conventions

* **Reading**: each implementation reads the whole file into one buffer with
  ordinary buffered I/O, then parses from that buffer. `read_ms` is printed for
  reference and excluded from the comparison.
* **Output**: exactly one line on stdout, exit code 0, nothing on stderr. Every
  line starts with the workload name; `run.sh` compares `cs=` across arms.
* **Timing**: the harness measures wall time around the process (plus the
  workload's own `work_ms`); it pins the process and repeats, taking the
  median and the minimum.
* **Checksum**: 64-bit FNV-1a over the accumulated state, wrap-around:
  `h = 14695981039346656037; h = (h ^ v) * 1099511628211` where `v` is the
  value folded as a 64-bit unsigned integer. Printed as an unsigned decimal.

## 3. Workload A — `etl` (regular throughput)

Single-threaded batch ingest: parse every record, filter nothing, aggregate.

* per site `s` in `[0,15]`: `count`, `sumLat`, `maxLat`, `sumBytes`;
* per status (index in the table `T = [200,201,204,301,400,403,404,500,502]`):
  `count`.

Then fold, in this exact order: for `s = 0..15` fold `count`, `sumLat`,
`maxLat`, `sumBytes`; then for `k = 0..8` fold `statusCount[k]`; then fold
`lines`.

```
etl n=<lines> read_ms=<m> work_ms=<m> cs=<cs> count=<c> sumlat=<sl> maxlat=<ml> sumbytes=<sb>
```

## 4. Workload B — `sessions` (reclaim / GC pressure)

A service that materialises every incoming message and keeps a per-user session
registry with a time-to-live. Two lifetimes, on purpose:

* **per-record object**: every record is parsed into a message object — the thing
  a real ingest path allocates before it does anything with it — used to update
  the session, and then dropped. 2,000,000 short-lived objects.
* **session object**: `{lastTs, count, sumIdx, sumLat}`, created the first time a
  uid appears and destroyed when its session expires. About 200,000 objects of
  which about three quarters are dropped by the end.

Pass 1 — ingest: materialise the record, then
`count++; sumIdx += i; sumLat += lat; lastTs = ts` on `sessions[uid]`.

Pass 2 — evict: `cutoff = max(lastTs) - 300`; a session is *retained* when
`lastTs[uid] >= cutoff`, and every other session object is destroyed.

```
sessions n=<lines> read_ms=<m> work_ms=<m> cs=<cs> retained=<r> liveoff=<sum of retained counts>
```

`cs` folds, in ascending uid order over **retained** sessions only: `uid`,
`count`, `sumIdx`, `sumLat`.

Each arm uses its language's natural representation — that *is* the axis:

| arm | per-record object | session registry |
|---|---|---|
| C++ | `new Event{…}` / `delete` | `unordered_map<i64, Session*>` + `delete` on evict |
| Rust | `Box<Event>` / drop | `HashMap<i64, Box<Session>>` + `remove` on evict |
| Go | `&Event{…}` (heap) | `map[i64]*Session` + `delete` (GC reclaims) |
| extC | arena bump (`new Event`) | arena array + liveness flags; the region reclaims at exit |

extC has no per-object free, so its session objects are reclaimed when the
enclosing region dies rather than at eviction; the peak-RSS column is where that
trade shows up. The checksum is identical because the retained set and its
folded values are the same.

## 5. Workload C — `serve` (coroutines / event loop)

A query server on loopback. At startup the server computes the same per-site
aggregates as workload A (not measured), then answers a line protocol:

```
request:  G <site>\n
response: <count> <sumLat> <maxLat> <sumBytes>\n
```

* The server is pinned to **one core** and must be single-threaded; how it gets
  concurrency is the point of the axis — extC uses one coroutine per connection,
  Go uses one goroutine per connection with `GOMAXPROCS=1`, C++ and Rust use a
  single-threaded `epoll` state machine (no dependencies, no threads).
* The load generator is `client/client.c`, shared by all four arms: it opens
  64 connections, keeps 8 requests in flight per connection, sends the request
  count given on its command line (200,000 in the default harness run), verifies
  every reply against the expected site statistics, and prints
  `client req=<n> bad=<n> ms=<ms> reqps=<r> closed=<c> cs=<cs>`.
* The server prints one line when the client is done:

```
serve n=<bytes> read_ms=<m> req=<n> secs=<ms> cs=<cs>
```

`cs` folds every reply's four fields in arrival order (equivalently: fold the
16 site rows once, `site, count, sumLat, maxLat, sumBytes`).

The server controls its own write policy, because that policy is part of what
the axis measures: an arm may buffer replies for a connection and flush them
with one `send` per read burst, as long as replies keep their order and a
request that arrives alone is answered without waiting for the next one. The
flush must therefore happen before the arm waits for more input, not on a timer.

## 6. Workload D — `parmix` (parallel compute)

A parallel scoring pass over the same records: parse in place, then apply a
fixed 64-round mix to each record's fields.

```
x = ts ^ (uid << 17) ^ (lat << 33) ^ bytes            (all 64-bit wrap-around)
repeat 64 times:
    x ^= x >> 33
    x *= 0xff51afd7ed558ccd
    x ^= x >> 29
    x *= 0xc4ceb9fe1a85ec53
out[i] = x
```

Then fold in ascending `i`: `sum += out[i]`, `h = (h ^ out[i]) * 1099511628211`,
and fold `n`. Threads come from argv (`t=1`, `8`, `24`; default 8). The parse
itself is part of the measured work and happens inside the parallel loop; the
record offset table is built in one sequential pass so each worker parses only its
own records and the bytes are touched once before the parallel pass.

```
parmix n=<lines> t=<t> read_ms=<m> work_ms=<m> cs=<cs> sum=<sum>
```

## 7. What is compared

| axis | workload | reported |
|---|---|---|
| regular throughput | A `etl` | `work_ms` |
| reclaim / GC | B `sessions` | `work_ms` + peak RSS |
| coroutines | C `serve` | server `secs` → requests/second + peak RSS |
| parallel compute | D `parmix` | `work_ms` at `t=1` and `t=8`, and the speedup |

Peak RSS is read from `/proc/self/status:VmHWM` by the harness (`/usr/bin/time
-v` or `/proc/<pid>/status` of the child). Every arm prints the same checksum
for the same workload; the runner refuses to report a row whose checksum
disagrees with the reference.
