# Destructors

Status: **partly implemented** (2026-11-03, updated 2026-11-04). Step S1 is in: a `@drop` method on a
`@noCopy` struct runs when the binding leaves its block, when it is overwritten by `=`, before a
`return`, and on the way out of a loop through `break`/`continue`. Step S2 is in as well: `drop x`
is an explicit statement that runs the hook **there**, and the name is dead afterwards — reading it,
dropping it twice, or dropping a value someone still holds a view of is a compile error. Not in yet:
generators' frames, drops on the `trap` path, and program exit for globals.

`extC` already has linear types: `@noCopy` means "exactly one owner, may be borrowed but never copied"
(`fs::file`, `ifstream`, `ofstream`, `loop`, `mem::pool`, `json::doc`, …). Linear types own things the
arena does not: file descriptors, sockets, locks. Without a destructor every early return leaks one,
and the user has to write `close()` on every path. That is the awkwardness this note fixes.

The design is deliberately narrower than C++: **a destructor releases external resources, it never
frees storage.** Storage belongs to places (frame block, plate, frame of a coroutine) and is released
by the place, not by user code. That one restriction is what makes destructors cheap here: no `Rc`,
no rule-of-5, no move constructors.

## 1. Marking a destructor

<!-- fragment -->
```extc
@noCopy
struct ofstream {
    fd:   i32
    open: bool

    @drop
    fn dispose(self: mut ref ofstream) -> void {
        if self.open { sysio::close(self.fd) }
    }
}
```

- The marker is the **attribute `@drop`**, not the method name. A method called `drop` without the
  attribute is an ordinary method and is never called implicitly.
- Reason: a magic name makes an accidental `fn drop` silently become a destructor. An attribute is
  opt-in and shows up in review; the call site stays readable whichever name the author picks
  (`dispose`, `close`, `release`).
- `@drop` is only legal on a method of a `@noCopy` type that takes `self: mut ref T` and returns
  `void`. A `@drop` type must declare exactly one such method (`check_` gate).
- A type with `@drop` must be `@noCopy`: dropping is only unambiguous when there is exactly one owner.

## 2. When it runs

Automatic, emitted by the compiler as part of leaving a place:

| Event | Order |
|---|---|
| block exit (fall-through, `return` through it) | reverse declaration order, innermost block first |
| `break` / `continue` out of a block | reverse declaration order, down to the blocks actually left |
| overwrite of the place (`f = ofstream::open(..)?`) | old value's drop, then store |
| end of program | globals with `@drop`, reverse declaration order, before arena teardown |
| trap | drops of the current block chain run first, then the trap message (keeps ASan clean) |
| a by-value parameter of the function exiting | before the block arenas are torn down |

Fields of a `@drop` type die with it: the compiler runs the type's `@drop` body first, then drops the
fields in reverse declaration order (same as C++ members / Rust `Drop`+fields). A field of a `@drop`
type thus never needs manual cleanup.

A parameter passed **by value** is the callee's copy, so the callee owns it and drops it on the way
out; a `ref` or a view parameter names the caller's storage and drops nothing. That is also why only a
temporary can be passed by value to a `@noCopy` type: a named variable would be a second name for one
state, so the checker asks for `ref` or for a move instead.

What is **not** automatic: nothing inside the `@drop` body is generated for you; if the type holds a
raw fd and does not close it, that is a leak (same as C++).

## 3. Explicit drop

<!-- fragment -->
```extc
var f = ifstream::open(path)?
let head = f.readSome(buf)?
drop f                       // release now; `f` is dead from here on
```

`drop x` is a **statement**: it runs the hook at that point and marks the place dead, so the end of
the block does not run it again. The compiler rejects four shapes, each with its own sentence:

- a name that is not a binding (`` `drop` takes a variable name ``),
- a type with no `@drop` method and no field that has one (``` `p` has no `@drop` method ```),
- a place that already gave its value away by a move, or that was already dropped,
- a container that a live view still borrows: `` `b` is dropped while `v` still views it `` — the
  same rule that rejects growing it, reached through the destructor.

Assigning a new value to a dropped name makes it usable again; there is no "drop twice".

Not in yet: `drop` on a **field** (`drop self.inner`, so that a wrapper can delegate), and the
broader question of a borrow that escaped the block. Both are rejected today as "not a variable
name" / "not tracked here" — the recoverable side (see the status line).

## 4. What is rejected

| Shape | Why | Checked |
|---|---|---|
| `@drop` on a copyable type | dropping needs one owner | `@drop requires @noCopy` |
| `drop x` while `x` is borrowed | dangling | at the statement |
| `drop` of a field outside a destructor | partial move out of a value | at the statement |
| a `@drop` value inside `varArray`/`vector`/`hashMap`/`dyn`/`any` | those copy/clone values | at the container's element check |
| a `@drop` value captured by a closure | closures copy the environment | capture check |
| a `@drop` value held across `yield` | frame drop list not implemented yet (stage 2) | at the coroutine's frame layout |
| a `@drop` value passed as `par` capture | parallel copies | the `par` capture check |
| trap inside a `@drop` body | cleanup must not fail halfway | callee effects: only non-trapping calls |

## 5. Implementation

Checker:

- `Type.dropMethod` (or a flag on the `StructDef`) filled while checking the type body; `@drop`
  validated (arity, `self: mut ref Self`, `void`, `@noCopy`).
- Place bookkeeping: the checker already knows, per block, which locals are declared and their
  types; add "needs drop" to the block's scope frame so codegen can walk it.
- `drop x` statement: `ST_DROP`, new statement kind (walkers and the AST-freeze baseline need the
  usual bookkeeping), with the liveness check described above.
- Trap-path drops: a `trap` records the enclosing blocks' drop lists.

Codegen:

- `genBlockBody` keeps a `Vec dropList` of `(name, type)` in declaration order; every exit path
  (`ST_BLOCK` end, `break`, `continue`, `return`) emits the drops of the blocks it leaves, innermost
  first, reverse order. Reuse the existing block-exit machinery that already emits
  `extc_arena_release` per level.
- Overwrite: `stAssign` to a `@drop` place emits `drop; store`.
- `drop x` → the call, plus the symbol marked dead so later uses were already rejected by the checker.

Runtime: none. A destructor is a plain C function call on a `mut ref`.

## 6. Tests (`tests/destructor/`)

1. `auto_block.extc` — three resources, block exit, reverse order, counters print `3 2 1`.
2. `auto_early_return.extc` — resources taken, `return` in the middle of nested blocks; everything closed once.
3. `auto_break_continue.extc` — loop body resources dropped per iteration.
4. `overwrite.extc` — assigning a new handle drops the old one first.
5. `explicit.extc` — `drop f`, then a later `f.close()` **must fail to compile** (negative case).
6. `borrowed.extc` — `let v = f.view()` then `drop f` **must fail** with the borrow message (negative).
7. `field_auto.extc` — a `@drop` field inside a `@drop` struct, both bodies run, reverse order.
8. `trap_path.extc` — a trap after two resources were taken: both closed (check the fd count via `/proc/self/fd`).
9. `container_reject.extc` — `varArray<@drop type>` **must fail** (negative).
10. `coro_reject.extc` — a `@drop` value across `yield` **must fail** (negative, until stage 2).
11. `no_double_close.extx` — ASan build, same handle dropped on a trap path and a return path.
12. `@drop` on a copyable type **must fail** (negative).

## 7. Stages

- **Stage 1** (landed, `tests/drop/` 7 cases): attribute + automatic drops on block exit, `return`,
  and overwrite; hook reachable through reachability pruning for plain and generic types; the two
  rejection cases. Still open in S1: `break`/`continue`, program end, the trap path, field order
  (a field of a `@drop` type is not dropped yet), and the borrow census.
- **Stage 2**: explicit `drop x` with the borrow census (tests 5, 6, 9).
- **Stage 3**: coroutine frames get a drop list, so a `@drop` value may live across `yield`
  (`conn` coroutines holding an `ifstream`); then test 10 becomes a positive case.

## 8. Relation to `pin` (docs/ALLOCATORS.md)

`pin` counts live borrows so that storage growth can trap instead of leaving a dangling view. It needs
the same two things destructors need: a per-place death point (to decrement) and a statement that
marks a place dead (for explicit `drop`). Implementing destructors first therefore removes the two
hard parts of `pin`; the pin counter is then a field plus an increment and a decrement at a place
death the compiler already emits.
