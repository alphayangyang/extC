# Destructors

> 状态：**当前口径** —— 这篇写的是现在的事实，改语义先改它。

Status: **implemented for `@noCopy` and `@value` types**. A `@drop` method runs when the binding
leaves its block (fall-through, `return`, `break`/`continue`), when it is overwritten by `=`, on a
by-value parameter when the callee returns, on fields in reverse declaration order, and on globals at
program end. `drop x` is an explicit statement that runs the hook **there**, and the name is dead
afterwards — reading it, dropping it twice, or dropping a value someone still holds a view of is a
compile error; assigning a new value revives it. Not in yet: coroutine frames (a `@drop` value across
`yield`), and the hooks of a runtime-raised `trap` (bounds / stale dyn).

`extC` already has linear types: `@noCopy` means "exactly one owner, may be borrowed but never copied"
(`fs::file`, `ifstream`, `ofstream`, `loop`, `mem::pool`, `json::doc`, …). Linear types own things the
arena does not: file descriptors, sockets, locks. Without a destructor every early return leaks one,
and the user has to write `close()` on every path. That is the awkwardness this note fixes.

The design is deliberately narrower than C++: **a destructor releases what the value owns, and nothing
else.** Storage that belongs to a *place* (a frame block, a plate, a coroutine frame) is released by
the place, not by user code; storage that belongs to a *value* — a reservation the value asked for and
nobody else can reach — is returned by that value's `@drop` (`string`, `buf<T>`). The distinction is
what keeps destructors cheap: no `Rc`, no rule-of-5, no move constructors. A `@value` type can have a
`@drop` because every copy is built from scratch by its `clone`.

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
- `@drop` is only legal on a method of a `@noCopy` or `@value` type that takes `self: mut ref T` and
  returns `void`. A `@drop` type must declare exactly one such method (`check_` gate).
- A type with `@drop` must be `@noCopy` or `@value`: dropping is unambiguous only when each copy owns
  its own storage. An `@value` type gets that from its `clone` (every copy is built from scratch); a
  `@noCopy` type from the fact that it never gets a second owner.

## 2. When it runs

Automatic, emitted by the compiler as part of leaving a place:

| Event | Order |
|---|---|
| block exit (fall-through, `return` through it) | reverse declaration order, innermost block first |
| `break` / `continue` out of a block | reverse declaration order, down to the blocks actually left |
| overwrite of the place (`f = ofstream::open(..)?`) | old value's drop, then store |
| end of program | globals with `@drop`, declaration order, via one `atexit` that `main` registers **after** the console flush (LIFO ⇒ hooks first, flush last, so whatever they print still gets out) |
| trap | **not covered**: a bounds/stale-dyn trap is raised inside an emitted helper where the enclosing block chain is not visible, so no hook runs before the message. Hooks do run for a `trap` that the *program* raises (`trap(...)` statement) |
| a by-value parameter of the function exiting | before the block arenas are torn down |
| a global of a `@drop` type | at process exit; `traps`/`exit()` also go through it |

Fields of a `@drop` type die with it: the compiler runs the type's `@drop` body first, then drops the
fields in reverse declaration order (same as C++ members / Rust `Drop`+fields). A field of a `@drop`
type thus never needs manual cleanup.

**A binding with no initializer is still a value.** `var s: string` runs its `@drop` at block exit like
any other binding; the hook must be safe on the zero value (the library asks `if reserved > 0` instead
of assuming there is storage to return). Without this rule a loop that declares an empty value and
writes it long pays one reservation per iteration: the set of places that need a drop is about the
place, not about whether the author wrote an initializer.

A container that owns its elements (`buf<T>`, `vector<T>`, `hashMap<K,V>`) runs the element's hook when
an element leaves the container (`clear`, `shrink`, `release`, `pop`, overwrite) and takes an element by
*move* when one is stored into it. The rules and their checks are in `docs/design/CONTAINER-OWNERSHIP.md`.

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
| `@drop` on a type that is neither `@noCopy` nor `@value` | dropping needs an owner per copy | `@drop` needs a `@noCopy` or `@value` struct |
| `drop x` while `x` is borrowed | dangling | at the statement |
| `drop` of a field outside a destructor | partial move out of a value | at the statement |
| a `@drop` value inside `varArray`/`dyn`/`any` | those copy/clone values | at the container's element check |
| a `@drop` value captured by a closure | closures copy the environment | capture check |
| a `@drop` value held across `yield` | frame drop list not implemented yet (stage 3) | at the coroutine's frame layout |
| a `@drop` value passed as `par` capture | parallel copies | the `par` capture check |
| trap inside a `@drop` body | cleanup must not fail halfway | callee effects: only non-trapping calls |

## 5. Implementation

Checker:

- `Type.dropMethod` (or a flag on the `StructDef`) filled while checking the type body; `@drop`
  validated (arity, `self: mut ref Self`, `void`, `@noCopy` or `@value`).
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

## 6. Tests (`tests/drop/`, 19 cases)

Each positive case prints a trace of hook calls, so order and multiplicity are the assertion:

| Case | What it pins |
|---|---|
| `order.extc` | block exit, innermost first, reverse declaration order |
| `overwrite.extc` | `=` drops the old value before the store |
| `no_hook.extc` | a type without a hook is untouched |
| `early_return.extc` | `return` through nested blocks runs every hook once |
| `break_continue.extc` | `break` / `continue` run the hooks of the blocks they leave |
| `fields.extc` | a `@drop` field runs after the outer body, reverse order |
| `return_move.extc` | returning a local moves it out: no hook in the callee, one in the caller |
| `explicit_drop.extc` | `drop x` runs the hook there and the block exit does not repeat it |
| `revive_after_drop.extc` | assigning to a dropped name makes it live again |
| `param_owner.extc` | a by-value parameter is the callee's copy and is dropped there (`n=0 leak=0`) |
| `generic.extc` | the hook is reachable through a generic instance |
| `global_exit.extc` | a global's hook runs at program end, after the body |
| `resource_warning.extc` | a hook counts as the release point, so the leak warning goes quiet |
| `errors/drop_needs_nocopy.extc` | `@drop` on a type that is neither `@noCopy` nor `@value` |
| `errors/two_hooks.extc` | one hook per type |
| `errors/drop_then_use.extc` | reading a dropped name |
| `errors/drop_twice.extc` | dropping twice |
| `errors/drop_no_hook.extc` | `drop` of a type with no hook |
| `errors/drop_borrowed.extc` | `drop` while a live view borrows the value |

`script/tests` runs the same programs under ASan; `tests/copy` covers the `@value` side (every copy is a
`clone`, so each copy owns its storage and each runs its own hook).

## 7. Stages

- **Stage 1 (in)**: attribute + automatic drops on block exit, `return`, `break`/`continue`, overwrite,
  fields (reverse order) and globals at program end; hook reachable through reachability pruning for
  plain and generic types; a by-value parameter is dropped by the callee.
- **Stage 2 (in)**: explicit `drop x` with the borrow census, revive-by-assignment, and the leak
  warning counting a hook as the release point.
- **Stage 3 (open)**: coroutine frames get a drop list, so a `@drop` value may live across `yield`
  (`conn` coroutines holding an `ifstream`); the trap path of runtime-raised traps (bounds / stale dyn)
  still skips the hooks because the enclosing block chain is not visible inside the emitted helper.

## 8. Relation to `pin` (docs/design/ALLOCATORS.md)

`pin` counts live borrows so that storage growth can trap instead of leaving a dangling view. It needs
the same two things destructors need: a per-place death point (to decrement) and a statement that
marks a place dead (for explicit `drop`). Implementing destructors first therefore removes the two
hard parts of `pin`; the pin counter is then a field plus an increment and a decrement at a place
death the compiler already emits.
