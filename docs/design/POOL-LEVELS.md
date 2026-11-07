# Pool levels, stated as a rule with a boundary

> 状态：**当前口径**

This page states the rule that decides where a pool allocation lives, in the form the checker
implements it, and then states the boundary: which shapes are **proved** sound, which are
**rejected on purpose**, and which are **open gaps** (with a reproducer in `tests/zone/`).
It exists so that one question has one answer: ten hand-written kind lists give ten chances to
drift, and the drift is silent -- `f(x)` accepted where `a + b` is rejected, or the reverse.

## 1. Objects

- A **place** is a scope that owns pool blocks: the function body, a loop body, a nested block, a
  coroutine frame. Places form a tree; `depth(p)` is the number of places from the outermost.
  Smaller depth = longer lived.
- A **pool site** is a call site that allocates pool storage for the value it produces. The
  checker records one level per site: `planZoneLevel(site) = depth(place the storage belongs to)`.
- A **binding** has a level too (`anRefDepth`, and `slotDepth` for the place it names).
- A **value read out of a container** (`k.atDense(i)`, `k.get(h)`, `a[i]`) has **no site of its
  own**: its bytes belong to the container.

## 2. Rule S — store

Storing value `v` into a place at level `p` (an element of `k`, a field, a `mut ref` argument) is
allowed when

    level(v) ≥ p        (order: deeper scope = larger depth = shorter life)

where `level(v)` is the level of the site that produced `v`, or the level of the container it was
read from. Two mechanisms follow:

- **S1 (move).** If `level(v) > p` and `v`'s site is **movable** (it allocates its own storage: a
  literal, `new`, a call or operator whose body builds a pool), the checker lowers the site:
  `planZoneLevel(site) := p`. The generated C then allocates that storage in `p`'s place, so the
  stored pointer stays valid as long as the container.
- **S2 (reject).** If `level(v) > p` and `v` has no movable site (a read out of a container, a view
  of a local), the store is rejected: a value read out of a container lives exactly as long as
  that container does.

Motivation for S1's soundness: the storage is freshly allocated by the very expression being
stored, so no other reference to it exists yet; lowering only changes *which place* owns it, and
the destination place outlives every read reachable through the container (reads happen while the
container is alive). This is what makes `var s = string::from("x") + i` inside a loop legal as
`k.insert(s)` — the buffer is allocated in `k`'s place, not the loop's.

## 3. Rule R — read

A by-value read of one element of a container at level `p` yields a value at level **`p`** — the
container's level, not the level the stored binding had in its own frame. The implementation keeps
that consistent on the store side: the depth recorded on the container after a successful store is
the **post-move** depth (`min(depth(value), level(container))`, see §4 O2).

## 4. The three obligations

| # | Obligation | Where | Discharged by |
|---|---|---|---|
| **O1** | *Store uses one rule*: every "can this call's pool sit at the destination's level" question goes through `promoteInto2`/`lowerPoolSite`, and every walk that asks "is this a call" goes through `exprCarriesCallee`/`exprIsCallKind`. | `src/check/check_escape.c`, `src/check/check_top.c` | `tools/check_call_sites.py` (no local kind lists), `tests/zone/pool_forms.extc` (five call shapes agree) |
| **O2** | *Store records the post-move depth*: after a successful store, the container's recorded depth is the moved level, so reads inherit the right number. | `noteFieldDepthWrite` callers | (open gap G2 — the record is right for movable sites, the *read* side is not) |
| **O3** | *Nothing else may lower a level*: `promoteInto2` is the only writer of level facts; a value with no movable site (a read) never gets lowered. | `check_escape.c` (`lowerPoolSite`), `check_top.c` (store path) | `tests/zone/neg_loop_new_view.extx`, `neg_loop_array_view.extx`, `neg_pool_read_escape.extx` |

## 5. Boundary

**Proved sound and pinned by tests** (each is a case in `tests/zone/`):

- All five call shapes — call, method on a temporary, operator, `??`, struct literal field — build a
  pool value in a loop and store it into a pool that lives outside the loop; storage is allocated in
  the destination's place (`pool_forms.extc`, output `20|true|true|true`).
- Views of loop-local storage (`new` inside a loop, a fixed array inside a loop) are **rejected** at
  check time (`neg_loop_new_view.extx`, `neg_loop_array_view.extx`).
- A loop-built value cannot be returned to a shallower caller (`return a + b` with a loop-local
  operand is rejected) — stated as part of rule S, not a separate rule.

**Open gaps** (documented, pinned as "still behaves this way", not as passing criteria):

- **G1 — unsound acceptance (P1).** A value read out of a *loop-local* pool and stored into an outer
  pool is accepted; reading it back is a **heap-use-after-free** (verified with ASan). Reproducer
  `tests/zone/neg_pool_read_escape.extx`, expected to be rejected once S2 covers reads whose origin
  is a call with a recorded return-borrow argument. Attempts so far: comparing
  `planRetBorrowArg(planCallee(origin))` did not fire because the by-value element read
  (`pool::atDense`) records no return-borrow argument — the fix has to add that fact first, or to
  compare against the *pool's* own level recorded on the receiver.
- **G2 — false rejection (P2).** After a promoted store (`var s = …; k.insert(s)`), the by-value
  read `k.atDense(0)` is rejected with "borrowed from depth 4". The store side is correct (the
  generated C allocates at level 1); the read side computes the element's depth from a container
  depth that was raised to the loop's level, and no rule lowers it back. Reproducer
  `tests/zone/pool_readback.extc`; `tests/zone/run.sh` asserts *today's* rejection so the fix is
  visible in the diff when it lands.

## 6. What "the boundary" means here

The checker's promise is one-directional and stated exactly:

> **No accepted program reads freed pool storage** (soundness — subject to gaps G1), **at the cost
> of rejecting some sound programs** (completeness — G2 and the return of loop-built values).

That trade is deliberate: a silent wrong answer is worse than a compile error, and the rejected
shapes all have an in-language workaround (build the value where it must live, or copy it). G1 is
the one shape where the trade is currently the wrong way round, and it is tracked as P1.
