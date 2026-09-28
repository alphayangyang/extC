# extC compiler — engineering review 1

Engineering quality only (ownership, complexity, error handling, testability, dead weight).
Not language design, not feature wishes. Every finding below was verified by reading the code
at the quoted line; every reproduction was actually run. Comments are treated as *claims*.

---

## 1. Scope & method

### What was run

| Command | Result |
| --- | --- |
| `make -B -j24` (forced full rebuild, `-Wall -Wextra -Wpedantic`) | **0 diagnostics** |
| `./check.sh quick` | **44 ok / 0 fail** (includes `tests/run.sh`, arena, warnings, generics, coroutine, STL, attack baseline, `check_walkers.py`, `check_tykind.py`, `check_concurrency_guards.py`, `check_guards.py`) |
| `python3 tools/check_walkers.py --list` | 23 hand-written walkers, "every recursive kind-walker handles every kind ✓" (see F8 — that claim is narrower than it reads) |
| `tools/golden.sha256` differential run (414 programs, with and without `EXTC_NO_LEVELPASS`, `-w --no-line-map -o`, sha256 compared) | 0 exit diffs, **2 output diffs** (F7) |
| gdb probes on an ASan build of the compiler (`gcc -fsanitize=address src/*.c build/prelude_data.c -o /tmp/extc-asan`) | used for F1, F10 |
| ASan/fortify runs of generated programs | used for F4 |
| `EXTC_DBG_M=1 ./build/extc …` | per-module renamed-declaration counts (F1: `io: 41`) |
| corpus scan over `tools/golden.sha256` | longest source line 554 chars (`tests/stl/memFind.extc`); no emitted line near 4095 bytes (F5) |
| hand-written repro programs under `/tmp/rev` | F1, F2, F3, F4, F5 |

No file under `src/`, `stdlib/`, `tests/`, `tools/` was created, modified, or deleted. Probes and
generated C live in `/tmp/rev`; the only repo write is this report. Housekeeping note: an untracked,
empty file `string` appeared in the repo root during the session (timestamp 22:56; `git status`
reports `?? string`). It is not part of this review, I did not deliberately create it, and I left it
in place rather than delete something that might be someone else's.

### What was read (sampling is real and uneven)

Read closely: `src/base.h`, `src/base.c` (arena/Buf/diag), `src/modules.c` (loader/mangler/rewrite),
`src/dataflow.c` (all), `src/codegen.c` (structure plus ~1,800 of 8,252 lines: type map, CG state,
`cgLine`/`pfLine`, place predicates, `genBin`, `cgImplicitArgs`, `genMethodCall`, `genPrint`,
the conversion arm, the `EX_GENCALL` arm, `genFunc`, the text-pruning passes, `funcDefStart`,
`generateC` prologue/epilogue, the emitted `extc_cout_*` runtime), `src/check_top.c` (~900 lines:
the `ReachKind` closures, `calleeMakesPool`/`calleeCreatesPool`, the `levelPass` call site,
`obligExpr`, `checkFunc` head, the DFA write-back, the self-check block), `src/check_stmt.c`
(selected regions), `src/ast.h` (selected regions), `src/check.c` (names), `src/types.c`
(accessors), `stdlib/std/io.extc` (the `cout` overloads), `tools/check_walkers.py`,
`tools/gen_flags.py`, `tools/golden.sh`, `check.sh`.

**Not read:** `parser.c`, `lexer.c`, `check_escape.c` (beyond ~100 lines), `check_expr.c` (beyond
~250 lines), `types.c` (beyond the accessors), `ast.c`, `coroutine.c`, `pools.c`, `memfind.c`,
`domain.c`, `time.c`, most of `check_lookup.c`/`check_inst*`, and most of `stdlib/` and `tools/`.
`tools/attack.py`, `tools/fuzz*.py`, `./tools/golden.sh` were **not** run standalone (`attack.py`
runs inside `check.sh quick`; `golden.sh` is excluded from `quick`). Findings about those areas
would be additions, not corrections, to this list.

---

## 2. Findings

### F1 — blocker — a module with 65 top-level declarations crashes the compiler (OOB read)

`src/modules.c:1319`, `:1323-1337`, `:1341-1354`, crash frame `:1370`

```c
1319:    const char **names = (const char **)arenaAlloc(L->a, sizeof(char *) * 64);
...
1335:    for (size_t i = 0; i < src->funcs.len && n < 64; i++) {       /* compute loops: capped at 64 */
...
1341:    size_t k = 0;
1351:    for (size_t i = 0; i < src->funcs.len; i++) {                 /* store loops: NOT capped */
1352:        FuncDef *d = *(FuncDef **)vecAt(&src->funcs, i);
1353:        Ren *r = (Ren *)vecPush(&u->ren);
1354:        r->from = d->name; r->to = names[k++];
```

The compute loops stop at 64 names; the store loops run over *all* declarations and index
`names[k++]` past the end. `r->to` becomes an arbitrary arena word, which is then assigned to
`d->name` (line 1370-1371, `strcmp((*(FuncDef **)vecAt(&src->funcs, j))->name, r->from)`) and
dereferenced.

Verified: a module `mod/lib/big.extc` with *n* plain functions, `use lib::big` from `main.extc`:

* *n* = 64 → `exit 0`; *n* = 65, 66, 70 → `exit 139` (SIGSEGV).
* gdb: `#0 __strcmp_avx2`, `#1 mangleUnitDecls (L=..., u=...) at src/modules.c:1370`,
  `#2 mergeUnit ... at src/modules.c:1647`, `#3 loadModules ...`.

Why it hurts: the limit is invisible and reachable from ordinary source reorganisation. The
largest bundled module is `stdlib/std/io.extc`, whose own mangler trace
(`EXTC_DBG_M=1 ./build/extc …`) reads `[mangle] io: 41 declarations renamed:` — i.e. **23**
declarations of headroom — and the failure mode is a segfault (or, if the garbage word happens to
be readable, a silently garbage C identifier) rather than a diagnostic.

Direction: size the array from the four list lengths (or allocate one name per declaration while
iterating) and drop the `&& n < 64` guards so compute and store cannot disagree; add
`assert(n == k)` after the store pass. Any module-level declaration budget must be a diagnostic,
never a silent truncation.

### F2 — blocker — a slice expression is treated as a C lvalue: generated C does not compile, exit 0

`src/codegen.c:1688` (predicate) and `:1780` (use); contrast `:1583-1591`

```c
1683: static bool isPlaceExpr(const Expr *e) {
1686:     case EX_FIELD: return isPlaceExpr(e->u.field.obj);
1687:     case EX_INDEX: return isPlaceExpr(e->u.index.obj);
1688:     case EX_SLICE: return isPlaceExpr(e->u.slice.obj);
...
1780:        if (isPlaceExpr(operand)) return arenaPrintf(g->arena, "&(%s)", code);
```

The sibling predicate for the same question documents this exact failure and omits the case:

```c
1564:  * This is not the question isPlaceExpr answers, and confusing the two breaks
1565:  * code:
1566:  *   - the slice expression `s[0..5]` is a place in extC, but its C form is
1567:  *     `slice_u8_slice(s, 0, 5, "...", 54)`, a function call, and `&` on a call
1568:  *     is illegal. That really happened: examples/slices.extc and
1569:  *     euler-sieve.extc stopped compiling.
```

Verified with valid extC (`slice::==` takes `self: ref slice<T>`):

```extc
fn eqTail(s: slice<i32>) -> bool { return s[1..3] == s[1..3] }
```

emits `slice_i32_eq(&(extc_slice_slice_i32(s, (int64_t)(1), (int64_t)(3), "...", 1)), ...)`;
`gcc -std=c11 -fsyntax-only` → `error: lvalue required as unary '&' operand`; plain `-o` mode
**exits 0**; only `--check-c` reports "the generated C does not compile -- this is an extc bug".
The method path is the same (`s[1..3].get(0)`).

Why it hurts: a documented, previously-fixed defect was reintroduced by a second predicate that
answers a nearby question. `printArgIsPlace` states the distinction ("This is not the question
isPlaceExpr answers, and confusing the two breaks code") and names the slice case as the failure;
`isPlaceExpr` states the same hazard in its own doc ("the generated `&(f())` would not be legal C",
`:1673-1675`) and then returns true for exactly that case, and its only non-recursive use in the
whole tree is the address-taking site at `:1780` (grep). Two predicates, one address-taking
question, opposite answers.

Direction: one owner for "is the generated text an lvalue" (or make `isPlaceExpr` defer to
`printArgIsPlace`); the slice case must answer false because its C form is a call.

### F3 — blocker — every `f64 -> i64` conversion traps; `u64` rejects values that fit

`src/codegen.c:2480` and `:2484` (inside the float-source arm of `case EX_CONV`)

```c
2480:                else if (strcmp(tn,"i64")==0) { lo = 1; hi = 0; }   /* macro form, see below */
2484:                else { lo = 0; hi = INT64_MAX; }        /* u64 from a float: checked as i64 */
```

emitted verbatim by `:2486-2488` and tested by `:7025`
(`if (!(v >= (double)lo && v <= (double)hi)) extc_trapMsg(...)`). With `lo = 1, hi = 0` the
condition is false for every value.

Verified:

```extc
fn main() -> i32 { var f: f64 = 2.5   var n: i64 = i64(f)   println("n=", n)   return 0 }
```

→ `int64_t n = ((int64_t)extc_convFloat((double)(f), 1LL, 0LL, "...", 3));`
→ `./extc --run` prints `trap: float does not fit in the target integer type`, exit 1.
`i32(f)` works, so the fault is exactly the two fallback arms. The comment `/* macro form, see
below */` is stale: the macro spelling (`INT64_MIN`/`INT64_MAX`) exists only in the *signed
non-float* branch at `:2498-2501`, and this arm returns before reaching it.

Why it hurts: a language-level conversion is unusable and the failure is a runtime trap in
otherwise valid code; no corpus program performs `f64 -> i64`, which is why the suite is green.

Direction: delete the per-type if-chain and derive `[lo, hi]` from the canonical accessors
(`ttIntBits`/`ttIntSigned`, `src/types.c:870-879`), emitting `INT64_MIN`/`INT64_MAX` and
`0`/`UINT64_MAX` for the 64-bit cases.

### F4 — significant — `extc_cout_put` overruns its 256 KiB buffer for any single write larger than the buffer

`src/codegen.c:6662` (emitted runtime text) and `:6680-6682`

```c
"static uint8_t extc_cout_buf[1 << 18];\n"
...
"void extc_cout_put(uint8_t *p, int64_t n) {\n"
"    if (n <= 0) return;\n"
"    if (extc_cout_len + n > (int64_t)sizeof extc_cout_buf) extc_cout_flush();\n"
"    memcpy(extc_cout_buf + extc_cout_len, p, (size_t)n);\n"
"    extc_cout_len += n;\n"
```

The guard only flushes when the buffer is *nearly* full; it never handles `n > sizeof buf`.
`stdlib/std/io.extc:621-623` (`fn <<(self: ref ostream, v: slice<u8>)`) passes the slice length
straight through, so a single `io::cout << big` overflow is reachable from ordinary source.

Verified with a program that fills a 300,000-element `mut slice<u8>` and prints it:
`ASAN: global-buffer-overflow ... WRITE of size 300000 ... #1 extc_cout_put /tmp/rev/big.c:312`
(also aborts under glibc `_FORTIFY_SOURCE`). No test writes more than 256 KiB at once.

Direction: `if (n >= (int64_t)sizeof extc_cout_buf) { extc_cout_flush(); fwrite(...); return; }`,
or loop in chunks; the sibling `extc_raw_enter` (`:6723-6726`) shows the clamp pattern the file
already uses when a caller-supplied count can exceed a fixed array.

### F5 — significant — `cgLine`/`pfLine` silently truncate emitted lines at 4095 bytes

`src/codegen.c:360-361` (claim), `:364` and `:370` (code), same shape at `:382-388`

```c
360:  *   - The formatted line is truncated at 4096 bytes; every caller stays far
361:  *     below that.
...
364:     char tmp[4096];
...
370:     vsnprintf(tmp, sizeof tmp, fmt, ap);
```

Verified: `fn main() { let s = "<5000 y's>"  println(s) }` compiles with exit **0**, and the
generated line 208 is 4099 bytes (4 indent + 4095) ending mid-literal; `gcc -std=c11 -fsyntax-only`
→ `error: missing terminating " character`. `--check-c` catches it ("this is an extc bug"); the
default `-o` path does not. The longest source line anywhere in the 414-program golden corpus is
554 characters (`tests/stl/memFind.extc`), and none of them emits a line near 4095 bytes, so the
gate cannot see this.

Why it hurts: the comment is the only thing standing between a fixed stack buffer and a silently
corrupt product. Truncation is not a defensive fallback here — it produces C that is not C, or
(worse) C that is valid but differs from the program.

Direction: build the line through `bufPrintf` into the destination `Buf` (unbounded, measured
first), or check the `vsnprintf` return value and raise an internal error on truncation.

### F6 — significant — `levelPass` ignores the data-flow fixed point it is handed, and the ordering the comments describe is the reverse of the code

`src/check_top.c:3361-3362`, doc at `:3337-3338`, call site and comments at `:4135-4144`,
write-back at `:4168-4186`

```c
3337:  *   dfr - the data-flow fixed point for the same body; its depths are final by now, so a
3338:  *         reader of them sees a number that does not depend on traversal order
3361: static void levelPass(Checker *c, FuncDef *f, const DfResult *dfr) {
3362:     (void)dfr;
```

```c
4136:        /* Publications recorded during this body settle the sites they reach. Running
4137:         * here, after the depth fixed point, is the point of the whole split: the
4138:         * decision sees the final depths instead of the numbers that happened to be
4139:         * true while the body was being walked. */
4141:        dfAnalyze(c, f, &dfr);
4144:        if (!getenv("EXTC_NO_LEVELPASS")) levelPass(c, f, &dfr);
...
4168:        if (!dfr.overflow) {
4169:            symIdxSync(c);
...
4174:                    int d = dfLookup(&dfr, sy->cname);
4175:                    if (d > sy->refDepth) sy->refDepth = d;
```

`dfr` is never read inside `levelPass` (verified by reading the whole 121-line function), and the
fixed-point depths are copied into `Sym.refDepth` only *after* `levelPass` returns. The level
solver reads its own `LvlState` table (`symLevel`, `:3231-3240`), so the "whole point of the
split" (the decision seeing final depths) is not realized. Additionally, when `dfr.overflow` is
set the entire write-back is skipped with no diagnostic; the only trace is a debug line
(`:4159`, `" (OVERFLOW: result unused)"`). `dataflow.c` raises `overflow` for a *fifth*
ref-carrying field of one binding (`dataflow.c:34` `#define DFA_MAX_FIELDS 4`, `:100-105`) or a
64-round cap (`:400`) — both ordinary.

Why it hurts: the dead parameter and the inverted ordering are a standing invitation to "fix"
the wrong end; a maintainer who believes the comment will assume the level decision is
order-independent when it in fact uses walk-time values that the same file calls unsound
(`:4123-4126`: "The checking walk above maintains a binding's depth by destructive assignment and
has no join at control-flow merges … That cannot produce the upper bound the escape check needs").
The neighbouring claim at `:4129-4131` ("It reads the tree and writes only the depths, so every
later reader sees a value that no longer depends on the order branches were visited") is false for
`levelPass`, which is a later reader that runs before the write. The silent overflow drop then
makes a whole analysis' result invisible.

Direction: either pass the facts (`factsFromResult`) into `levelPass` and move the write-back
before it, or delete the parameter and both comments; diagnose the overflow drop instead of
printing it only under a debug switch.

### F7 — significant — two environment variables change the product, while the generated manual says `EXTC_` switches never do

`src/check_top.c:4141`, `:6269`; `src/main.c:165`; `docs/manual/21-flags.md:35`

```c
4141:        if (!getenv("EXTC_NO_LEVELPASS")) levelPass(c, f, &dfr);
6269:        if (bad) ctx->hasError = true;      /* so scripts see the failure */
```

```c
main.c:165:        "debug switches (they never change the output):\n"
docs/manual/21-flags.md:35: 带 `EXTC_` 前缀的开关只用于诊断，不改变输出；…
```

Verified:

* **38** distinct `EXTC_*` switches are read by the code (23 via `dbgOn`, 16 via `getenv`,
  overlapping union); `--help` (and therefore the generated page) names **3**.
* With `EXTC_NO_LEVELPASS=1`, 2 of the 414 golden programs emit **byte-different C**
  (`tests/arena-promoted/C3_if_join_wholevalue.extc`,
  `tests/arena-promoted/arena_if_join_whole_value.extc`), i.e. the switch changes arena placement,
  not just diagnostics. No exit-code change on this corpus.
* `EXTC_SELFCHECK` sets `ctx->hasError`, which skips code generation and makes the compiler exit 1.
* The gate that is supposed to protect the claim (`python3 tools/gen_flags.py --check`, run by
  `check.sh`) only diffs the page text against `--help`; it cannot notice that the sentence about
  `EXTC_` switches is false.

Why it hurts: `EXTC_NO_LEVELPASS` is an unattached switch that silently changes what is
generated; `EXTC_SELFCHECK` silently turns a previously-good build red. Both are invisible to
`--help`, to docs, and to the corpus (grep: no hits in `tests/`, `tools/`, `check.sh`).

Direction: product-affecting switches belong on the command line (`--no-level-pass`,
`--selfcheck`) or must be documented as such; fix the help/manual sentence, and add one test per
switch that can change acceptance or output.

### F8 — significant — `ST_DOMAIN` is missing from the data-flow walker, and the walker gate cannot see the omission

`src/dataflow.c:322-431` (no `case ST_DOMAIN`), `src/ast.h:424`, `tools/check_walkers.py:171`

`dfStmt` handles `ST_VAR/ST_ASSIGN/ST_IF/ST_WHILE/ST_BLOCK/ST_MATCH`; the tail is

```c
425:    case ST_RETURN:
426:    case ST_BREAK:
427:    case ST_CONTINUE:
428:    case ST_EXPR:
429:    default:
430:        break;
```

`grep -n ST_DOMAIN src/dataflow.c` → no hits, while `ast.h:424` says the domain block is
"[a] statement on purpose: the block … the statement walkers handle its body the same way they
handle any other block", and sibling walkers do handle it (`check_top.c:720`,
`check_stmt.c:1163`). `dfStmt` is a genuine walker (`dfBlock` at `dataflow.c:316-320` calls
`dfStmt`, `dfStmt` calls `dfBlock`), but `tools/check_walkers.py` recognises a walker only when
the switch block names its own enclosing function:

```python
171:            recursive = bool(fn) and bool(re.search(r"\b" + re.escape(fn) + r"\s*\(", block))
```

so `dfStmt` is classified as "a classifier, not a walker", is absent from `--list`, and the gate
still prints `every recursive kind-walker handles every kind ✓`. Mutual recursion is exactly the
shape the gate is blind to.

Why it hurts: a store inside `d.run { … }` never contributes to the depth fixed point, so such a
function falls back to the pre-fixed-point numbers; and the tool that exists to prevent this class
of bug (its docstring: "`EX_DYN` was skipped by ten of them") reports success. Whether a dangling
program results is unverified (I built no repro).

Direction: add `case ST_DOMAIN:` walking `u.domain_.body` (or amend `ast.h` if the omission is
deliberate), and extend the gate to follow mutual recursion within a file — or tag `dfStmt` with
the existing `/*@@all-kinds*/` marker, which is what that mechanism is for.

### F9 — significant — the hidden *zone* argument has two owners; the hidden *home* argument has one

`src/codegen.c:1717-1734` vs `src/check_top.c:1503-1517`, `src/check_escape.c:1173`,
single owner for the home half at `src/ast.h:943`

```c
codegen.c:1717: static bool cgHasImplicitArgs(const FuncDef *callee) {
codegen.c:1718:     return callee && (funcTakesHomeArena(callee) || callee->makesPool);
...
codegen.c:1730:     if (callee->makesPool) {
codegen.c:1732:         bufPuts(b, forSignature ? "int64_t __extc_home_zone" : zoneArgRef(g, (Expr *)site));
```

```c
check_top.c:1507:     if (f->owner && f->owner->poolObject) return true;
check_top.c:1508:     if (f->makesPool) return true;
...
check_top.c:1516:     if (f->body == NULL && isPoolCtorName(f->name)) return true;
check_top.c:1517:     return f->isAssoc && f->owner && f->owner->makesPoolAny;
```

```c
check_escape.c:1173:        if (calleeMakesPool(val->func) && val->zoneLevel != 0) {
...
ast.h:943: static inline bool funcTakesHomeArena(const FuncDef *f) { return f && f->usesHome; }
```

The home half of this convention was unified (both the signature emitter and the checker ask
`funcTakesHomeArena`, one accessor, commit 510bd03). The zone half was not: the *promotion*
decision uses the richer `calleeMakesPool` (also true for `@poolObject` methods, extern
constructor names, and `isAssoc && owner->makesPoolAny`), while both the emitted signature and
the emitted argument use the raw `callee->makesPool`. For those three cases a lowered
`Expr.zoneLevel` can be computed and never emitted. The two predicates also duplicate their last
three lines with opposite NULL answers (see F15).

Why it hurts: this is exactly the drift shape that produced the bug the home-half fix closed —
"the definition side and the call side ask different questions". Whether a concrete program
dangles is **unverified** (it needs a pool-creating callee whose raw flag is false, e.g. a
`@poolObject` method that creates the pool only through a path the closure cannot see).

Direction: give the zone half the same treatment as the home half — one shared predicate in
`check_internal.h` used by the checker's promotion rule, the signature emitter and the call
emitter — and keep `cgImplicitArgs` the only place that spells the parameter.

### F10 — significant — the "the result is never longer" invariant behind an unguarded `memcpy` is false

`src/codegen.c:5947-5948` (claim), `:5962-5964` (code)

```c
5947:            /* One assembly, in order: the spans are disjoint and ascending, and the result is
5948:             * never longer than what it replaces, so it fits in the same buffer. */
...
5962:            memcpy(out->data, bufCstr(&nb), nb.len);
5963:            out->data[nb.len] = 0;
5964:            out->len = nb.len;
```

The replacement built at `:5909-5912` is `indent + "(void)(" + rhs + ");\n"` while the span it
replaces is `indent + <type> <name> = rhs ;\n`. The replacement is longer whenever
`len("<type> <name> = ") < 8`, i.e. for a short user type name.

Verified with gdb at `codegen.c:5962` on `struct a { v: i32 }` + `let q: a = mk()` (unused local):

```
NAME=[q] len=12145 cap=16384 nb.len=12147 slack=4237 delta=2
TEXT=[    a q = mk();]
```

so `nb.len > len` (delta +2) while the comment claims otherwise, and the following `memcpy` has no
capacity check. Two caveats, both checked: the copy stays in bounds while `len <= cap - 1`, and in
every shape I tried at least 15 bytes had already been removed earlier (the emitted prelude's
unread `skip` local; even `fn main() {}` gets that `-15` cut, gdb at `dropUnusedLocals` entry:
`len=12024` then `CUT name=[skip] delta=-15`). Overflow therefore needs "at most 1 byte removed
before this cut" and a total length within 2 bytes of the buffer capacity; I could not construct
it. **OOB reachability: unverified**; the false invariant and the missing check are verified.

Direction: rebuild the text through the `Buf` API (or at least `if (nb.len > out->cap) ...
grow`), and restate the invariant as what it is: "no longer than the span it replaces, *given*
this generator's declaration shapes".

### F11 — significant — the six pool-primitive names are listed twice; the codegen copy gates unchecked indexing

`src/codegen.c:2587-2590` and `src/check_expr.c:2087-2092`

```c
codegen.c:2587:            if (strcmp(gcName, "poolSlice") == 0 || strcmp(gcName, "poolSliceRaw") == 0
codegen.c:2588:                || strcmp(gcName, "poolResize") == 0 || strcmp(gcName, "poolResizeRaw") == 0
codegen.c:2589:                || strcmp(gcName, "poolGive") == 0 || strcmp(gcName, "copyInto") == 0) {
```

```c
check_expr.c:2087:            bool isPoolPrim = strcmp(e->u.gencall.name, "poolSlice") == 0
...
check_expr.c:2208:            if (isPoolPrim) return checkPoolPrim(c, e, elem);
```

The checker's copy is what decides whether `checkPoolPrim` (`check_expr.c:502`) validates arity;
codegen's copy is what decides whether the arm reads `args[0]/args[1]/args[2]` and `targs[0]` with
`vecAt` (no bounds check, `base.c:385-387`) and re-derives the arity itself
(`codegen.c:2600-2601`). Nothing enforces that the two string lists agree; if a name is added on
one side only, codegen reads past the end of a `Vec` and reinterprets arena bytes as `Expr *`.

Direction: move the name list and the arity into `check_internal.h` beside the existing single
owner for the constructor family (`isPoolCtorName`/`poolCtorNeedsZone`), and assert the arity in
codegen instead of re-deriving it.

### F12 — significant — integer width/signedness re-derived by parsing the type *name*, while the canonical accessors exist

`src/codegen.c:1300-1316`; canonical source `src/types.c:870-879`, already used at `src/check.c:344`

```c
1300:        bool  sint = lt && lt->kind == TY_BUILTIN && lt->name && lt->name[0] == 'i';
1301:        bool  uint = lt && lt->kind == TY_BUILTIN && lt->name && lt->name[0] == 'u';
...
1314:            int bits = 0;
1315:            for (const char *q = lt->name + 1; *q >= '0' && *q <= '9'; q++) bits = bits * 10 + (*q - '0');
1316:            if (bits > 0)
```

`ttIntBits`/`ttIntSigned` (`types.c:870-879`) answer both questions from the same `INTS[]` table the
checker uses. When the digit parse yields 0, the over-wide-shift trap is silently skipped and a
bare C `<<`/`>>` is emitted — undefined behaviour with no diagnostic. Unreachable only because
every builtin name currently has the form `i|u` + digits.

Direction: call the accessors; treat `bits == 0` for an integer as an internal error.

### F13 — minor — disabled checks and dead decisions left in the tree, with `-Wunused` silenced by hand

| Location | Quote | Effect |
| --- | --- | --- |
| `src/check_stmt.c:236` | `if (0 && s->u.var.ann && typeContainsProto(c->tt, s->u.var.ann, "coroutine")) {` | a 12-line diagnostic is unreachable |
| `src/check_stmt.c:833` | `if (0) checkEscape(c, s->u.assign.value, placeDepth(c, s->u.assign.target),` | dead; the live `checkStoreEscape` above already calls `checkEscape` with a different layer |
| `src/check_top.c:4053` | `if (1 || !pp || !typeContainsProto(c->tt, pp->type, "coroutine")) continue;` | makes the loop body (7 lines) unreachable |
| `src/check_top.c:6618` | `if (0 && typeContainsRef(c->tt, d->type)) {` | the "local across `yield` carries a reference" error is disabled |
| `src/check_top.c:6081-6082` | `f->owLocal = true;` / `(void)funcReachesItself;` | the call-graph decision the comment above promises is discarded; `owLocal` is set true for every function this pass visits, so codegen's `!f->owLocal` branches (`codegen.c:3361/3367/3374/3487/4150`) never fire for them |
| `src/codegen.c:4290-4295` | `if (g_cycReady || 1) { … (void)g_cycFellBack; return cycleSaysRecursive(g, f); }` | the 24-line fallback walk below is unreachable; `g_cycFellBack` is declared and never written |
| `src/codegen.c:2641-2644` | second `return arenaPrintf(… "extc_pool_take" …)` | unreachable behind the `take_raw/take` return at `:2635` |

Why it hurts: the code reads as if the check exists. `(void)funcReachesItself;` and `|| 1` are
explicit smoke screens for `-Wunused`, and the comments around them still describe the deleted
behaviour (e.g. `check_top.c:6070-6071`: "Must run after every function body has been checked:
whether a function can reach itself is decided from the call graph" — it is not decided).

Unverified extension: `FuncDef.owLocal`/`owSites` are assigned only in this pass
(`check_top.c:6076/6081`), so a generic instance created *later*, during code generation, keeps
`owLocal == false`; `codegen.c:4131` then falls through to `if (!f->owLocal)` and emits `name()`
rather than `name(void)` for a zero-parameter instance — the very failure the comment at
`:6078-6080` says a golden test caught. I did not verify whether such an instance reaches this
emitter.

Direction: delete the disabled blocks (git has the history), or keep them behind a named
`#if 0`-equivalent with a tracked reason; remove `FuncDef.owLocal` and the codegen branches it
gates if "always local" is now the rule.

### F14 — minor — meaning-changing booleans, passed as bare literals at every call site

| Definition | Meaning of the flag | Call sites (bare) |
| --- | --- | --- |
| `src/codegen.c:1720` `cgImplicitArgs(..., bool forSignature)` | declaration text vs argument expression | `:1287`, `:1948`, `:2354`, `:2456` pass `false`; `:4139` passes `true` |
| `src/codegen.c:1994` `genStructLitAs(CG *g, Expr *e, bool staticInit)` | compound literal vs brace initializer | `:2039` `false`, `:2046` `true` |
| `src/codegen.c:5162` `sliceHelper(CG *g, Type *ob, Type *st, bool tail)` | different mangled name **and** a different ABI (4 vs 5 parameters) | `:5247` `true`, `:5251` `false` |
| `src/check_top.c:710` `stmtMakesPool(Stmt *s, bool descendBlocks)` | transitive "can this function create a pool" vs block-local "can this block" | `check_top.c:611`/`:6446` `true`, `codegen.c:3232` `false` |
| `src/check_top.c:3546` `obligExpr(Checker *c, Expr *e, Vec *obs, bool escape)` | whether the value leaves the function's hands; a wrong `true` **suppresses** the leak warning (`:3711`) | 38 call sites (`grep -c`), all bare (e.g. `:3563` `false`, `:3565` `true`) |

`stmtMakesPool` is at least documented at `:677-682` with the two questions spelled out; the other
four are not discoverable at the call site. The file already contains the counter-pattern it
should follow: `check_top.c:551-553` replaced the old `bool precise` with two named predicates.

Direction: split into named entry points (`cgImplicitArgsForSignature`,
`blockCreatesPool`, `obligArg`), as `callsNeedsHome`/`callsUsesHome` already do.

### F15 — minor — `calleeMakesPool` and `calleeCreatesPool` duplicate the same three lines with opposite NULL answers

`src/check_top.c:1503-1517` and `:1536-1541`

Both end with

```c
if (f->makesPool) return true;
if (f->body == NULL && isPoolCtorName(f->name)) return true;
return f->isAssoc && f->owner && f->owner->makesPoolAny;
```

the first after an extra `@poolObject` relaxation and `false` for NULL, the second with `true` for
NULL ("unresolved ⇒ conservative"). `check_internal.h:838-844` pins only the *name list* to one
owner; nothing keeps the two bodies in step. (See F9 for the consequence at the codegen boundary.)

Direction: implement `calleeCreatesPool` as the shared body plus the documented NULL relaxation.

### F16 — minor — `genPrint` silently drops an argument (`0`) in three places

`src/codegen.c:1625`, `:1650`, `:1659`

```c
1625:        if (!bt) { bufPuts(&b, "0"); continue; }
1650:        if (bt->kind != TY_BUILTIN) { bufPuts(&b, "0"); continue; }
1659:        if (!pf) { bufPuts(&b, "0"); continue; }
```

The `0` becomes one operand of the generated comma expression, so the value is simply not printed
and nothing is reported. The routes I could test are closed by the checker, so this is defensive
today, but it is the one pattern that turns "unknown type" into silently missing output. The same
file shows the right habit in `emitDescDefs`: `"internal: no descriptor generator for this type"`
(`:974-983`).

Direction: `ctxError(ctx, e->line, 1, …, "internal: cannot print a value of type %s")`.

### F17 — minor — `funcDefStart` reads `p[-1]` unguarded; its two sibling scans guard it

`src/codegen.c:6294-6296` vs `:6289`, `:6841`

```c
6294:    for (char *p = at; *p; p++)
6295:        if (p[0] == '}' && p[-1] == '\n') { e = p + 1; break; }
```

If `at == text` the first iteration reads before the buffer. The structurally identical scans do
guard (`6289: if (p != text && p[-1] != '\n') continue;`). Reachability looks impossible today
(the unit always starts with the "Generated by" preamble) — **unverified-reachability**, but the
inconsistency is one token of work.

Direction: add `p != at` (or `p != text`) to the condition.

### F18 — minor — stale comment and a no-op "discovery" loop in `dfAnalyze`

`src/dataflow.c:443-449` and `:80-81`

```c
443:    /* Bindings are discovered by walking statements before the analysis proper, so that
444:     * the fact table has an entry for every one of them; a variable that is only read
445:     * still needs a row. */
446:    for (size_t i = 0; i < f->params.len; i++) {
447:        Param *p = *(Param **)vecAt(&f->params, i);
448:        if (p->cname) raiseTo(&facts, p->cname, 0);
449:    }
```

```c
80: static void raiseTo(Facts *f, const char *cname, int depth) {
81:     if (depth <= 0) return;
```

There is no discovery walk in `dfAnalyze`, and `raiseTo(..., 0)` returns immediately, so the loop
does nothing and no row is created for parameters. Downstream code does depend on row existence
(`dataflow.c:353`: `VarFact *v = factOf(f, root->cname); if (v) …`), so the documented invariant
is false. Together with F6 this makes the DFA/variable-fact contract read very differently from
what it does.

Direction: add the discovery loop with a row-creating primitive, or delete both the comment and
the loop.

### F19 — minor — the "no mutable static globals" rule is stated in the header and violated in at least a dozen places, including a checker hook installed by one loop

`src/base.h:12-15`

```c
 *   2. No mutable static globals: all state hangs off a Ctx passed explicitly,
 *      mirroring extC's rule that a program carries no hidden state.
```

Counter-examples (verified by grep): `base.c:19-24` (`dbgOn`'s 16-entry cache),
`check_top.c:697` (`poolCalleeResolve`, installed at `:4966` and torn down at `:4968` only inside
one generic-instance loop), `check_top.c:701-703` (`instResTT/instResParams/instResTargs`),
`check_top.c:3736-3740` (`g_sb`/`g_sbArena`/`g_sbBad`), `codegen.c:4247-4250`
(`g_cycSeen/g_cycOn/g_cycGrey/g_cycBlack/g_cycStack/…`), `codegen.c:4503`, `modules.c:1482`
(`OwnMemo g_ownMemo[1 << 16]`), `main.c:74` (`g_explainRoot`). `main.c:244`/`:494` go further and
use the process environment as an inter-pass channel (`setenv("EXTC_EXPLAIN_ROOT", …)`, read at
`check_top.c:6277`), which also overrides a user's exported value.

The comment at `check_top.c:695-696` claims the pool resolver is installed by codegen
("codegen 生成某个实例时知道该解析成谁 … 由 codegen / 闭包的实例那一轮装上"); codegen never calls
`setPoolCalleeResolver` (grep: `check_top.c:4966`/`:4968` only). The consequence is conservative
(`calleeCreatesPool(NULL) == true`, `:1537`), so it costs an extra zone hook, not correctness.

Why it hurts: the header rule is presented as an architectural invariant, and the one hook that is
semantically load-bearing is installed by a single loop with no assertion that it is installed.
Pass-scoped globals also make the checker non-reentrant and untestable in isolation.

Direction: either weaken the header rule to what is true (counters, caches, and named pass
contexts) or move the pass context (including the pool resolver) into `Checker`, install it once
per pass and assert it.

### F20 — minor — the pass pipeline lives in functions of 1,749 / 1,324 / 2,552 lines

Measured with comments and string literals stripped (raw brace counting is wrong in `codegen.c`
because emitted C inside string literals contains braces):

| Function | Span | Lines |
| --- | --- | --- |
| `checkExprInner` | `src/check_expr.c:1007-3558` | 2552 |
| `generateC` | `src/codegen.c:6504-8252` | 1749 |
| `checkModule` | `src/check_top.c:5004-6327` | 1324 |
| `checkStmt` | `src/check_stmt.c:226-1206` | 981 |
| `genExprInner` | `src/codegen.c:2060-2786` | 727 |
| `genStmtInner` | `src/codegen.c:3613-4104` | 492 |
| `genFunc` | `src/codegen.c:4529-4901` | 373 |
| `checkFunc` | `src/check_top.c:3913-4197` | 285 |

Concrete invariants that only a reader can hold: `generateC` records byte offsets into `out`
(`deadDefAdd`/`deadFuncBody`) and therefore requires the coroutine/vt blocks to stay last
(`:8242-8248`); `checkModule`'s sub-passes are order-coupled and the file itself documents a case
where the first level solve ran before the `makesPool` closure and "not a single pool-site
promotion landed" (`:6183-6187`, repaired by a replay at `:6191`); `genFunc` must save/restore
every per-function `CG` field because generic instances are emitted back to back; `checkStmt`
carries per-case state (`c->narrow.len` watermarks, `c->noHoist`, `c->curStoreVal`) across ~40
`return`s in one switch.

Direction: split at the phase boundaries the comments already name (`generateC`: preamble →
prototypes → bodies → runtime → prune passes; `checkModule`: the closure/settle/placement stages),
and give each split a one-line contract instead of leaving the ordering implicit.

### F21 — minor — stale PLAN-#83 comments: the "known exception" no longer exists

`src/codegen.c:1713-1715`, `src/check_expr.c:179-184`

```c
1713:  * One site is **knowingly** not routed through here: the operator path (`genOpCall`) emits only the
1714:  * zone half, because the arena half is PLAN #83 and still open -- it should keep failing loudly in
1715:  * the C compiler rather than silently leaking.
```

`genOpCall` exists nowhere else (single grep hit: this comment). The operator path *does* route
through the owner: `codegen.c:1287` `cgImplicitArgs(g, &cb, m, e, 2, false);` with the comment at
`:1279-1282` saying "Emitting both is what closed #83". The checker-side twin at
`check_expr.c:179-184` still says "The arena half is still open (PLAN #83 …)" while the function
below it records exactly that escape site (`callHomeDepth` at `:215`).

Direction: delete both paragraphs; a comment describing an exception that no longer exists is
worse than no comment, because the exception is the thing a maintainer will re-add.

### F22 — minor — the mangler's two-phase complication is justified by a hazard the allocator cannot produce

`src/modules.c:277-279`

```c
 *   - The result lives in the arena, and another `arenaPrintf` may reuse that buffer.
 *     Compute every name first and only then store the pointers (see
 *     `mangleUnitDecls`).
```

`arenaAlloc` (`src/base.c:118-127`) is a pure bump allocator — `b->used += n`, new blocks are
appended, nothing is ever recycled — and `arenaPrintf` (`base.c:175-186`) allocates `n + 1` fresh
bytes per call. A later `arenaPrintf` therefore cannot overwrite an earlier result. (The
reuse hazard is real for `bufCstr`, which returns the `Buf`'s own storage, but no name here comes
from a `Buf`.)

Why it hurts: this premise is what produced the `names[]` array, the 64-entry cap and the
compute/store split — i.e. blocker F1. A false reason kept a fragile design in place.

Direction: verify or delete the premise; with a bump allocator the whole pass can be one loop that
computes and stores each name, and the cap disappears.

---

## 3. Top maintenance impediments (ranked)

1. **Cross-pass conventions still have more than one owner.** F2 (place predicates: one says
   slices are lvalues, the other's comment says the opposite), F9 (zone argument: checker promotes
   on `calleeMakesPool`, codegen emits on `makesPool`), F11 (pool-primitive names and arity spelled
   twice), F15 (two near-identical pool predicates), F12 (integer width from the type name while
   accessors exist). The home-arena fix (ast.h:943) is the template; the zone half and the place
   predicate were not migrated.
2. **Gates that report success while blind to the thing they exist for.** F8: `check_walkers.py`
   prints "every recursive kind-walker handles every kind ✓" while `dfStmt` — mutually recursive
   with `dfBlock` — is not even classified as a walker, and `ST_DOMAIN` is missing from it. F7: the
   generated flags page asserts `EXTC_` switches never change output; two of them do, and the gate
   only diffs the page against `--help`. F5: the cgLine comment is the only bound on a truncating
   `vsnprintf`.
3. **Silent-failure paths in emission.** F4 (unchecked `memcpy` into a fixed 256 KiB buffer), F5
   (line truncation producing non-C), F16 (`"0"` placeholders instead of a diagnostic), F10 (a
   `memcpy` whose stated size invariant is false). All of these fail with exit 0 and no message;
   `--check-c` catches two of them but is not the default.
4. **Analysis plumbing that does not do what its comments say.** F6 (`levelPass` ignores the DFA
   result; the ordering claim is reversed; overflow drops the result silently), F13 (six disabled
   checks/decisions, one silenced by `(void)funcReachesItself;`), F18 (a "discovery walk" that is
   an empty loop because `raiseTo(…, 0)` returns early), F21, F22.
5. **Fixed-size tables against unbounded input, with no bounds reasoning.** F1 (64 names →
   segfault at 65 declarations; largest bundled module is 41), F10 (buffer capacity vs +2 write),
   F11 (`vecAt` on unvalidated arity).
6. **Size and pass-order coupling.** F20 (`checkExprInner` 2,552 lines, `generateC` 1,749,
   `checkModule` 1,324, `checkStmt` 981) plus the documented incident where a pass ran before the
   closure it depends on and "not a single promotion landed" (check_top.c:6183-6187). Nothing
   asserts the order.
7. **Two blockers are in the "compile-time visible" class that the corpus does not reach:**
   `f64 -> i64` (F3, no corpus program converts a float to `i64`; the trap is a runtime failure so
   only `--run` tests could see it) and the module declaration budget (F1, whose largest bundled
   user is `std/io.extc` at 41 of 64). The golden corpus pins 414 programs byte-for-byte and
   syntax-checks the products; it does not exercise these shapes.
8. **Ratchet-style coupling is acknowledged rather than removed.** 23 hand-written AST-kind
   walkers must each list every kind; the repo's own tool documents two historical incidents
   (`EX_SLICE`, `EX_DYN`). The ratchet is honest, but F8 shows the enforcement has a hole exactly
   where the walkers are mutual.

---

## 4. Open questions

1. **F9 exploitability.** Is there a program where a `@poolObject` method (or an assoc function on a
   type with `makesPoolAny`) creates a pool that must land in the caller's zone, while the raw
   `makesPool` flag stays false? I verified the divergence, not a dangling result.
2. **F8 consequences.** Does a store inside `d.run { … }` actually produce an accepted-but-dangling
   program, or is the DFA depth only ever an upper bound that the walk already provides? I did not
   build a repro.
3. **F7 intent.** Is `EXTC_NO_LEVELPASS` a debugging aid for a pass that is known to be
   order-sensitive? The two differing golden programs suggest the answer matters: one of the two
   outputs must be the intended one, and nobody can tell which from the code.
4. **F6 intent.** Was `dfr` meant to be passed *into* `levelPass` (and the write-back moved up), or
   was the parameter left behind by a refactor that moved the write-back? The doc comment and the
   caller's comment still describe the first design.
5. **F1 headroom.** Was the 64 cap ever intended as a limit with a diagnostic? No test or doc
   mentions it; `stdlib/std/io.extc` renames 41 declarations, so it is 23 away.
6. **`allocState` tri-state.** `FuncDef.allocState` is documented as 0/1/2/3 (`ast.h:810`) but read
   as bare literals in two files (`check_top.c:592`, `:602`, `:6354-6363`), and the closure writes
   `v ? 1 : 2` over a possibly in-flight lazy query. I did not test whether the closure can clobber
   state `3`.
7. **Corpus blind spots beyond the ones above.** `tools/attack.py` runs inside `check.sh quick`, but
   it does not contain a long-line case, a `>256 KiB` print, a 65-declaration module, or a
   float→`i64` conversion. Which of those are considered in-scope for the hostile-input corpus is a
   policy question this review cannot answer.
8. **Golden-gate semantics.** `tools/golden.sh` freezes generated text and checks it is legal C; it
   cannot tell whether a frozen program still *runs* correctly. `examples/prelude.extc` having been
   frozen as illegal C (the comment at the top of `golden.sh`) shows the failure mode; a
   semantically wrong but legal product would be frozen just as quietly.
