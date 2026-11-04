# Zones, and why allocators should be a library concern

Status: **plan** (2026-11-03). Nothing here is implemented yet.

## 0. The one-sentence version

A **zone** is the compiler's only allocator-facing concept: a scope that owns a set of storages and
releases them all when it ends (or bumps an epoch). Everything a user could want — arenas, pools,
slab allocators, growable containers — is written on top of it, in extC, by the library.

The compiler's whole job is three things:

1. know where the scope ends (it already does — blocks are places);
2. create **typed** views over zone storage (`zone::alloc<T>` returns `slice<T>`, so no cast is ever
   exposed to users);
3. keep a per-zone **epoch** so a handle into zone storage can be checked for staleness
   (`zone::epoch`), which is the same idea already used by `dyn` and by pool generations.

If those three exist, `@poolObject`, `poolSlice<T>`, `poolResize<T>`, `std::sys::pool` and the plate
code in `src/back/pools.c` are all redundant.

## 1. What is privileged today, and why

| Knob | Where | Why it exists |
|---|---|---|
| `@poolObject` | type attribute | tells the checker/codegen that this type's storage lives on a plate rather than in the frame, and that its blocks must be revoked on epoch change |
| `poolSlice<T>` / `poolSliceRaw<T>` | generator intrinsic | turns plate memory into a `slice<T>` — the language cannot express this |
| `poolResize<T>` / `poolResizeRaw<T>` | generator intrinsic | realloc-style growth of a plate block |
| `extc_pool_*` | runtime C | new/drop/reset/generation/zone chaining |
| `std::sys::pool` | privileged stdlib layer | the wrapper containers call |

The zone machinery underneath is good and should be kept: `extc_pool_zoneEnter` /
`extc_pool_zoneLeaveTo` / `extc_pool_zoneReset` already give "scope exit releases everything, or bumps
an epoch". The problem is purely that **only pools the compiler created can register there**, so:

- a library author can allocate bytes (`malloc` via `extern!`) but cannot get a typed view of them;
- a value allocated with `new` cannot be put under zone management (that is why
  `new` inside a loop accumulates: 100 × 1 MiB churn must touch every page and peaks at 103.8 MB,
  while the same shape declared in the loop body peaks at 2.4 MB);
- a struct cannot declare "my storage is zone-managed" without `@poolObject`, i.e. without the
  compiler agreeing.

In other words: **the zone is RAII, but only the compiler may write into it.**

## 2. The generalized zone

<!-- fragment -->
```extc
/* 当前 zone；也可以显式开一个子 zone */
var z: zone = zone::current()

/* 分配：返回类型化视图，存储挂在这个 zone 上，zone 结束时整批归还 */
let buf: mut slice<i64> = zone::alloc<i64>(z, n)?

/* 读取代数：库可以用它做“陈旧句柄”检查 */
let e: i64 = zone::epoch(z)

/* 显式重置（一般由作用域退出自动做） */
zone::reset(z)                       // 归还这个 zone 的所有存储，epoch += 1
```

Semantics:

- `zone::alloc<T>(z, n)` returns a fresh `slice<T>`; the storage is owned by `z`, **not** by the
  current frame. It is released when `z` ends (block exit, `return`, trap) or on `zone::reset`.
- `zone::reset` releases everything and increments the epoch; a handle that stored the old epoch is
  then stale and libraries check it with `zone::epoch` (exactly the `dyn` stale check, and the pool
  generation check that already exists).
- Allocation cost is a bump pointer inside zone chunks (`new u8[CHUNK]` under the hood), i.e. the same
  order as today's `new`; the per-alloc registry entry is only needed for `reset`, and a chunk list
  gives that for free.
- **Zone exit is statically safe**: by the existing depth rules a view cannot outlive the scope it was
  created in, so collective release needs no runtime check and no pin.

With this, a library pool is 150 lines of extC — the shape that was already written by hand and
validated (`insert` 12.3 ms vs the built-in 13.5 ms for 1M operations):

<!-- fragment -->
```extc
struct pool<T> {
    blocks: vector<slice<T>>   /* zone-backed chunks */
    free:   slice<i64>         /* free-list of slot indices */
    gen:    slice<i32>         /* per-slot generation */
    epoch:  i64
    fn alloc(self: mut ref pool<T>) -> handle        /* pop free list, else carve a chunk */
    fn release(self: mut ref pool<T>, h: handle)     /* push back, gen += 1 */
    fn at(self: ref pool<T>, h: handle) -> ?T        /* gen check -> none when stale */
}
```

## 3. The two things a library cannot do, and what each needs

1. **Return an old block early** (why `varArray` peaks at 2×). The honest options are:
   - *segmented chunks*: never grow a block, append a chunk; peak = live + one chunk; no release
     needed at all. Pure library, works today except for one thing: chunks come from `new`, so they
     die with the **frame**, not with the container. With `zone::alloc` they die with the zone, which
     is what a container wants.
   - *release one block*: `zone::release<T>(z, s) -> bool`, needed only if a container insists on one
     contiguous buffer. Then a live view of that block would dangle, so this is the **only** place a
     freeze bit is needed: `zone::release` traps while a view of that block is alive.
2. **Grow in place** (the reason `vector` is 62 MB vs `varArray` 126 MB for 8M i64). With zones this
   is a library question: a container chooses (a) one contiguous buffer allocated with
   `zone::alloc` and re-allocated on growth (peak live × 1.5, one copy), (b) segmented chunks (peak
   live + chunk, no copy), or (c) a zone-backed pool of blocks (peak live × growth factor, no copy of
   live data). All three are library policies; all three are deterministic.

## 4. Freezing: only for early release

`pin` (docs/DESTRUCTORS.md §8) shrinks to its real job: **guard `zone::release`**. Zone *exit* needs no
pin because the depth rules already forbid a view outliving the scope; an *early* release is the only
way to invalidate a view inside a live scope, so that is the only operation that checks the count.

<!-- fragment -->
```extc
let old = self.vals
if !zone::release(z, old) { trap "this buffer is still borrowed" }
```

Cost: one counter per block (`i32`), incremented by `view()`, decremented when the view's place dies,
one branch on the release path. Element access stays free.

## 5. What this deletes

- `@poolObject`, `poolSlice<T>`, `poolSliceRaw<T>`, `poolResize<T>`, `poolResizeRaw<T>`,
  `std::sys::pool`, and the plate code paths in `src/back/pools.c` that only exist to serve those.
- The `fromRaw<T>` idea from revision 1 of this document: with typed `zone::alloc` there is no cast to
  expose. (Raw C pointers stay available through `extern!` for platform shims, as today.)
- `stl::pool<T>` as a language concept. It becomes `std::mem::pool<T>`, an ordinary library type with
  the same generation design that `dyn` and pool handles already use.

## 6. Migration

| Step | Work | Judge |
|---|---|---|
| 1. zone handle | expose `zone` type + `zone::current/alloc/epoch/reset`; runtime keeps the existing registry (chunk list + epoch), ~150 lines C + builtin plumbing | `tests/mem/zone.extc`: nested zones, `reset` bumps epoch, `alloc` storage survives a `return` out of the block that allocated it |
| 2. pin (early release only) | block counter + `view()` inc + place-death dec + `zone::release` trap | stale-view-after-release program traps with a named message instead of ASan UAF |
| 3. `std::mem::arena` | pure extC over `zone::alloc` | `tests/stl/arena_*`; peak = live + one chunk |
| 4. `std::mem::pool<T>` | free list + generations over zone chunks; handle API (`slotOf/at/setAt/entry`) already exists in `stdMap` land | `tests/pool/*` ported to the library type; `bench/app`/`realsvc` within noise |
| 5. containers | `varArray` → segmented chunks on `zone::alloc` (peak from 129 MB to ≈70 MB for 8M i64, per prototype); `vector` → zone-backed pool with growth policy; `string` keeps SSO | `tests/run.sh` 377/0; `bench/crazy`, `bench/app`, `bench/realsvc` within noise; 8M-i64 peak recorded in the paper's table |
| 6. delete privilege | drop the five knobs above | `grep -rn "@poolObject\|poolSlice\|poolResize" src/ stdlib/` = 0 |
| 7. numbers | re-run `bench/memstudy`; update the memory table in the paper honestly | numbers match the code |

Step 5 is where the 129 MB → ~70 MB improvement shows up, and it is a *library* change: the language
only gained a zone handle.

## 7. Acceptance

- A user-written `std::mem::pool<T>` and a user-written segmented container pass the same judgements as
  the built-in ones, with no attribute, no intrinsic, no `std::sys`.
- The stale-view program traps (named message) rather than reading freed memory.
- `zone::alloc` costs within ~10% of `new` for a bump-shaped workload.
- The memory model document gains one paragraph: *storage has an owner zone; a zone releases at scope
  exit and bumps an epoch; only early release checks for live views.*
