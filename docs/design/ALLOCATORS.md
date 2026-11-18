# Zones, and why allocators should be a library concern

> 状态：**当前口径** —— 这篇写的是现在的事实，改语义先改它。

Status: **plan** (2026-11-03)，其中「容器离开池板块」这一半已经落地（2026-12-01）：`vector` / `string` / `varArray` / 全部关联容器现在持有地址预留或 `block<T>`，stdlib 的池与 `place` 模块已删除，运行期池只剩 `dyn` 与 zone 链在用。下面 §2 起的 `zone::alloc` 提案**仍未实现**。

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

If those three exist, `@place` (formerly `@poolObject`), `poolSlice<T>`, `poolResize<T>`, `std::sys::pool` and the block-chain
code in `src/back/pools.c` are all redundant.

### Three things that were called "plate"

The word was used for three different things, and the confusion cost real debugging time, so the
vocabulary is fixed here:

| Term | What it is | Where |
|---|---|---|
| **`mem::plate`** | a `mmap`-reserved, lazily committed region with a *stable* address (`DEFAULT_RESERVE` = 4 GiB, committed in 1 MiB chunks). It exists so C interop and big scratch buffers can take a pointer that never moves. **A pool never allocates one.** | `stdlib/std/mem.extc` |
| **block chain** | a pool's own memory: a singly linked list of `malloc(sizeof(ExtcBlock) + bytes)` blocks that the pool grows by adding to, and hands back on release. No reserved region, no 4 GiB. | `src/back/pools.c` |
| **buffer reservation** | `buf<T>` 的存储：一片 2 MiB 对齐的地址预留，从进程级 buddy VA arena 里切（2 MiB 量子、8 个 64 GiB 竞技场懒创建）；没写过的页不驻留，`shrink` 用 `madvise(DONTNEED)` 把页还掉而地址不变。不进回收器、不占 `rc` 槽。 | `stdlib/std/data/buf.extc` + `src/back/bufrt.c`；语义见 [`BUF-INVARIANT.md`](BUF-INVARIANT.md) |
| **privileged heap emission** | `extc_viewOf` / `extc_memCopy` and friends: helpers the *generator* emits for code the language cannot spell. They hang off the heap runtime block, not off either of the above. | `src/back/plate.c` |

So: 4 GiB of `PROT_NONE` + `mprotect` means `mem::plate`; a growing pool means a block chain; a
`buf<T>` means a reservation carved out of the process-wide VA arena (and it is the one storage that
keeps its size, so it is also the only one that can give capacity back); the
`plate.c` helpers are a third thing with an overlapping name.

## 1. What is privileged today, and why

| Knob | Where | Why it exists |
|---|---|---|
| `@place` (formerly `@poolObject`) | type attribute | tells the checker/codegen that this type's storage lives in a block chain rather than in the frame, and that its blocks must be revoked on epoch change |
| `poolSlice<T>` / `poolSliceRaw<T>` | generator intrinsic | turns block-chain memory into a `slice<T>` — the language cannot express this |
| `poolResize<T>` / `poolResizeRaw<T>` | generator intrinsic | realloc-style growth of a block-chain block |
| `extc_pool_*` | runtime C | new/drop/reset/generation/zone chaining |
| `std::sys::pool` | privileged stdlib layer | the wrapper containers call |

The zone machinery underneath is good and should be kept: `extc_pool_zoneEnter` /
`extc_pool_zoneLeaveTo` / `extc_pool_zoneReset` already give "scope exit releases everything, or bumps
an epoch". The problem is purely that **only pools the compiler created can register there**, so:

- a library author can allocate bytes (`malloc` via `extern!`) but cannot get a typed view of them;
- a value allocated with `new` cannot be put under zone management (that is why
  `new` inside a loop accumulates: 100 × 1 MiB churn must touch every page and peaks at 103.8 MB,
  while the same shape declared in the loop body peaks at 2.4 MB);
- a struct cannot declare "my storage is zone-managed" without `@place`, i.e. without the
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

1. **Return an old block early** (why a container that reallocates its buffer peaks at 2×). The honest options are:
   - *segmented chunks*: never grow a block, append a chunk; peak = live + one chunk; no release
     needed at all. Pure library, works today except for one thing: chunks come from `new`, so they
     die with the **frame**, not with the container. With `zone::alloc` they die with the zone, which
     is what a container wants.
   - *release one block*: `zone::release<T>(z, s) -> bool`, needed only if a container insists on one
     contiguous buffer. Then a live view of that block would dangle, so this is the **only** place a
     freeze bit is needed: `zone::release` traps while a view of that block is alive.
2. **Grow in place** (the reason one contiguous buffer gives way to segmented chunks once the array grows large). With zones this
   is a library question: a container chooses (a) one contiguous buffer allocated with
   `zone::alloc` and re-allocated on growth (peak live × 1.5, one copy), (b) segmented chunks (peak
   live + chunk, no copy), or (c) a zone-backed pool of blocks (peak live × growth factor, no copy of
   live data). All three are library policies; all three are deterministic.

## 4. Freezing: only for early release

`pin` (docs/design/DESTRUCTORS.md §8) shrinks to its real job: **guard `zone::release`**. Zone *exit* needs no
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

- `@place`/`@poolObject`, `poolSlice<T>`, `poolSliceRaw<T>`, `poolResize<T>`, `poolResizeRaw<T>`,
  `std::sys::pool`, and the plate code paths in `src/back/pools.c` that only exist to serve those.
- The `fromRaw<T>` idea from revision 1 of this document: with typed `zone::alloc` there is no cast to
  expose. (Raw C pointers stay available through `extern!` for platform shims, as today.)
- `stl::pool<T>` as a language concept：容器自己持有存储（地址预留或 `block<T>`），不再需要槽回收层；`pool` 模块已删除。

## 6. Migration

| Step | Work | Judge |
|---|---|---|
| 1. zone handle | expose `zone` type + `zone::current/alloc/epoch/reset`; runtime keeps the existing registry (chunk list + epoch), ~150 lines C + builtin plumbing | `tests/mem/zone.extc`: nested zones, `reset` bumps epoch, `alloc` storage survives a `return` out of the block that allocated it |
| 2. pin (early release only) | block counter + `view()` inc + place-death dec + `zone::release` trap | stale-view-after-release program traps with a named message instead of ASan UAF |

**已经能跑的那一半（2026-11-04）。** 编译器侧的静态检查只覆盖同一个函数体，跨函数/跨块的借出
没有声音。库里的 `buf::borrow()` 用 `@noCopy` 守卫 + `@drop` 把计数做成语言能表达的东西：
守卫活着时 `pins > 0`，`push` 触到 `realloc` 就 `trap: this buffer is frozen by a live borrow`，
守卫离开作用域计数减回去。判据在 `tests/pin`（`borrow_freezes` 跨函数 trap、`borrow_release`
放行）。等编译器有 `buf<T>` 内建之后再把这个机制收进存储头，`borrow()` 就是它的糖。

同一条守卫在 `vector<T>` 上也有一份（`vector::borrow()` / `vectorGuard<T>`），因为它是最早被抓到
悬垂的那一个：`var s = v.view()` 之后 `v.push(…)` 触发换板，旧视图指向已归还的板块，ASan 报
heap-use-after-free。同一块里的这种写法现在是编译错误；跨函数的增长由守卫在运行期 trap
（`tests/pin/vector_frozen.extc`）。代价是每个 `vector` 多带一个 `i64` 计数。

`string` 也一样（`string::borrow()` / `stringGuard`）：SSO 态取 `view()` 之后 `append` 让第 17 个
字节升级到板块，旧视图悬垂在"内联缓冲"上 —— 同一条守卫把这条路也变成确定性 trap
（`tests/pin/string_frozen.extc`）。`bench/app` 的日志场景（20M 次追加）测不出差异。
| 3. `std::mem::arena` | pure extC over `zone::alloc` | `tests/stl/arena_*`; peak = live + one chunk |
| 4. `std::mem::pool<T>` | free list + generations over zone chunks; handle API (`slotOf/at/setAt/entry`) already exists in `stdMap` land | `tests/pool/*` ported to the library type; `bench/app`/`realsvc` within noise |
| 5. containers | `varArray` → segmented chunks on `zone::alloc` (peak from 129 MB to ≈70 MB for 8M i64, per prototype); `vector` → zone-backed pool with growth policy; `string` keeps SSO | `tests/run.sh` 377/0; `bench/crazy`, `bench/app`, `bench/realsvc` within noise; 8M-i64 peak recorded in the paper's table |
| 6. delete privilege | drop the five knobs above | `grep -rn "@place\|@poolObject\|poolSlice\|poolResize" src/ stdlib/` = 0 |
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

### Labels answer "where does the storage live", not "what does this function do"

Two shortcuts in the checker answer "may this call create a pool", and **both are load-bearing today**:

1. `calleeMakesPool` says *yes* for any method of a `@place` type. That makes the label double as a
   behaviour claim — every method looks like a pool site, and adding a constructor reclassifies its
   siblings — but removing it changes escape depth for calls on containers: with no home zone
   threaded through, `let a = outer.get(2)` on a locally built container is rejected with
   "borrowed from depth 2, but this can only hold up to depth 1" (`tests/pool/rt_nest_promote.extc`).
2. `calleeCreatesPool` says *yes* for an associated function of a type that makes pools anywhere.
   Removing it produces the same rejection for the same reason.

The proper fix is not to delete them but to make **the depth of a returned view come from the
receiver** rather than from the callee's home zone. That is a change in the escape rules, not in the
pool machinery; until then these two lines stay, and the label keeps answering two questions at once.