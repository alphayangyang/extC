# Rust arm — realsvc benchmark

Build (plain `rustc`, no cargo, no crates):

```
./rust/build.sh        # -> build/realsvc_rust
# equivalent: rustc -O --edition 2021 -o build/realsvc_rust rust/main.rs
```

`rustc -O` turns off overflow checks; the checksum/mix arithmetic spells out
`wrapping_mul` / `wrapping_add` so the wrap-around is explicit rather than an
optimization-level accident.

Checksums match the C++ arm byte for byte:

| workload | cs |
|---|---|
| etl | 11191182498282499399 |
| sessions | 16199615779603620825 |
| parmix (t=1 and t=8) | 10280782628320053244, sum=2919411389961607233 |
| serve | 620352923810747373 |

## How each mode is built

* **etl** — `read_file` into one `Vec<u8>`, then a byte-level `parse_record`
  (7 fields, optional `-`, single TAB/`\n` skip) into a stack `Record`. The
  aggregator is `[SiteAgg; 16]` plus `[i64; 9]` status counts, folded in the
  spec's order (sites, then statuses, then `lines`).
* **sessions** — same read/parse, then `let ev = Box::new(rec)` per record and
  `drop(ev)` immediately after the session update, exactly the C++ shape.
  Registry is `HashMap<i64, Box<Session>>`: `entry(uid).or_insert_with(...)`
  creates on first sight, `retain(|_, s| s.last_ts >= cutoff)` removes on TTL
  eviction (the removed `Box<Session>` is dropped there). Retained uids are
  collected, `sort_unstable`ed, and folded in ascending uid order.
* **serve** — ingest + `computeEtl` first (unmeasured), then a single-threaded
  `epoll` state machine. Sockets come from `std::net` (`TcpListener` /
  `TcpStream`, `set_nonblocking`, `set_nodelay`); readiness comes from epoll.
  Per event: drain the socket into `inbuf`, consume whole `G <site>` lines and
  append one reply each to `outbuf` (manual decimal formatting into a
  `[u8; 96]`, no allocation per reply), flush what the socket accepts, then
  `EPOLL_CTL_MOD` to `EPOLLIN`/`EPOLLOUT` as needed. A connection that has hit
  EOF and has an empty `outbuf` is dropped, matching the C++ order of
  operations and the same termination condition (`conns.is_empty() && served > 0`).
* **parmix** — offsets are computed sequentially (`Vec<u32>`), `out: Vec<u64>`
  is split with `chunks_mut`, and one `std::thread::scope` thread per chunk
  parses only its own records and writes its own `out[i]`
  (`offs`/`buf` shared by reference, disjoint `&mut [u64]` slices). At `t=1`
  the worker runs inline with no spawn. The fold after the joins is serial and
  in ascending `i`, so `sum` and `cs` are independent of `t`.

## Deviations from the C++ arm

1. **epoll needs 4 C declarations.** `std` has no readiness API, so the file
   declares `epoll_create1`, `epoll_ctl`, `epoll_wait` and `close` from libc
   (`extern "C"`). That is the only `unsafe` in the file besides the
   `read_unaligned` needed to read the kernel's packed `struct epoll_event`
   `data` word (taking a reference to a packed field would be UB). No memory is
   hand-managed and no `unsafe` block contains logic beyond the call itself.
   The alternative — threads or a busy poll loop — would change the axis (the
   spec asks for the single-threaded epoll state machine).
2. **Buffers are `Vec<u8>`, not `String`.** Same bytes on the wire and the same
   append/consume/erase shape; `Vec<u8>` just skips UTF-8 validation on data
   that is never re-encoded.
3. **Replies are formatted by hand** (`push_i64` into a stack buffer) instead of
   `snprintf`, to keep serve allocation-free per request. Output bytes are
   identical.
4. **sessions keeps `HashMap`'s default hasher.** `RandomState`/SipHash13 is the
   natural std container, so it stays even though it costs real time here. A
   probe (same parse, only the hasher varied, one core) attributes the
   sessions gap: parse 62.6 ms; + `Box<Record>` per record 68.7 ms; + full
   `HashMap` with the default hasher 146.8 ms; the same map with an identity
   hasher 97.4 ms. So roughly 50 of the ~60 ms delta against C++ is SipHash13
   versus the identity hash an integer-keyed `unordered_map`/Go map uses, and
   the remainder is the `entry` API plus `Box<Session>` indirection. That is
   the reclaim/GC axis doing what it is supposed to show.
5. **parmix is unpinned at t>1.** Threads need more than one core; `t=1`,
   etl, sessions and the serve server are pinned to core 0, the serve client to
   cores 1–23.
6. **serve request count.** The server takes the same `serve <port> [requests]`
   arguments as C++ but ignores `<requests>` (the client owns the load). The
   reported run uses `200000` requests, 64 connections, depth 8, as in the
   acceptance command; the serve `cs` folds the 16 site rows and does not depend
   on the request count.

## Numbers

`out.tsv` has the raw per-run values: median of 5 runs, `taskset -c 0` for the
single-threaded modes, `/usr/bin/time -v` VmHWM for peak RSS. `serve`'s
`work_ms` column is the server's `secs=` window (first request → last
connection closed), which is what requests/second is computed from; peak RSS is
the server process's VmHWM.

| workload | Rust median | Rust min | peak RSS | C++ median | Rust/C++ |
|---|---|---|---|---|---|
| etl | 73.7 | 72.6 | 72,204 kB | 68.1 | 1.08× |
| sessions | 168.5 | 168.1 | 87,956 kB | 108.6 | 1.55× |
| parmix t=1 | 316.9 | 316.4 | 95,632 kB | 318.4 | 1.00× |
| parmix t=8 | 70.6 | 68.6 | 95,512 kB | 78.5 | 0.90× |
| serve | 325.5 ms | 305.3 | 72,452 kB | 322.5 ms | 1.01× |

serve: 200,000 requests, `bad=0`, reqps median 613,185 (min 542,561, max
653,973) over the same 5 runs.
