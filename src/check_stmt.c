/* Statement checking: declarations, assignments, control flow and `match`.
 *
 * Each statement is checked here for what it does to the depth bookkeeping and to the
 * set of narrowing facts; the expressions inside it are handed to the expression
 * checker. This file sits between `check_top.c`, which walks the declarations, and
 * `check_expr.c`, which types the expressions.
 */

#include "check_internal.h"
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------- statements */

/* Rewrite a bare constructor into the qualified variant construction.
 *
 * The short forms are `success(v)`, `failure(e)`, `some(v)` and `none()`. The type
 * comes from the return type already written in the function signature, so nothing is
 * guessed: the compiler reads the `-> result<i64, gameError>` the user wrote. That
 * saves a repetition, and the repetition is long enough to bury the point, which is
 * the failure itself. `?` collects a failure and `failure` raises one, and neither end
 * should be verbose.
 *
 * Params:
 *   c    - checker
 *   e    - the expression to rewrite; a name that is not one of the four above is left
 *          exactly as it is
 *   want - the expected type, which decides whether the name is a known constructor
 *
 * Notes:
 *   - The node is rewritten in place into a variant construction, so every later check
 *     and the code generator take the ordinary path.
 *   - A user-defined function with the same name wins; the short form only applies
 *     where it cannot be ambiguous.
 */
void desugarBareCtor(Checker *c, Expr *e, Type *want) {
    if (!e) return;

    const char *nm = NULL;
    const char *proto = NULL;
    size_t nargs = 0;
    bool noParens = false;

    if (e->kind == EX_CALL && e->u.call.callee->kind == EX_IDENT) {
        nm = e->u.call.callee->u.ident.name;
    } else if (e->kind == EX_IDENT) {
        /* A constructor with no payload may drop its parentheses: `return none`,
         * the same treatment an enum variant gets. */
        nm = e->u.ident.name;
        noParens = true;
    } else return;

    if      (strcmp(nm, "success") == 0 || strcmp(nm, "failure") == 0) { proto = "result"; nargs = 2; }
    else if (strcmp(nm, "some")    == 0 || strcmp(nm, "none")    == 0) { proto = "option"; nargs = 1; }
    else return;
    if (noParens && strcmp(nm, "none") != 0) return;   /* `none` is the only nullary one */

    /* A user-defined function of the same name wins: the short form is only a
     * convenience for the unambiguous case. */
    if (findFunc(c, nm)) return;
    /* The return type is not the matching container, so the name is left alone and
     * reported as undefined, which is the truth. */
    if (!isProtoType(want, proto, nargs)) return;

    /* A union: copy `args` out before changing the kind, or the new field overwrites
     * it. */
    Vec args;
    if (noParens) vecInit(&args, c->arena, sizeof(void *));
    else          args = e->u.call.args;
    /* `option` and `result` are ordinary enums, so a bare constructor is a variant
     * construction. `typeName` takes the instance name, which the code generator uses
     * to build the C tag constant `option_i64_some`. */
    e->kind = EX_ENUMVAL;
    e->u.enumval.typeName = want->name;
    e->u.enumval.variant  = nm;
    e->u.enumval.args     = args;
    e->assocOwner = want;
}


/* Check a block body in a fresh scope, so bindings declared in it go out of scope
 * when the block ends. */
static void checkBlockBody(Checker *c, Stmt *block) {
    pushScope(c);
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        checkStmt(c, *(Stmt **)vecAt(&block->u.block.stmts, i));
    popScope(c);
}

/* Check one statement and apply the depth and narrowing consequences it has.
 *
 * Params:
 *   c - checker
 *   s - the statement to check
 *
 * Notes:
 *   - The side-effect counter is reset per statement. A `??` may hoist its subject
 *     into a temporary only while nothing before it in the same statement has a side
 *     effect, because that temporary is evaluated at the top of the statement.
 */
void checkStmt(Checker *c, Stmt *s) {
    c->stmtFx = 0;      /* count side effects from the start of each statement */
    switch (s->kind) {
        case ST_VAR: {
            /* The annotation of a local declaration is resolved here; the parser
             * only builds an unresolved type name. */
            if (s->u.var.ann)
                s->u.var.ann = ttResolve(c->tt, c->ctx, s->u.var.ann, s->line, c->curParams);
            if (ttIsError(s->u.var.ann)) s->u.var.ann = NULL;

            /* No initializer means zero initialization, and the parser guarantees an
             * annotation is present in that case. */
            if (!s->u.var.init) {
                /* The annotation mentions `T`, so whether a zero value exists cannot
                 * be decided yet; record the check for the instantiation. */
                if (s->u.var.ann && mentionsParam(s->u.var.ann))
                    recordZeroCheck(c, s->u.var.ann, s->line, s->u.var.name);
                else if (s->u.var.ann && typeLacksZeroValue(c->tt, s->u.var.ann)) {
                    ckError(c, s->line,
                            "`ref` is a non-nullable reference, so it has no zero value -- "
                            "and neither does any struct that contains one",
                            "cannot zero-initialize `%s`: it contains a reference",
                            s->u.var.name);
                }
                s->type = s->u.var.ann ? s->u.var.ann : ttError(c->tt);
                Sym *sym = declare(c, s->u.var.name, s->type, s->u.var.mut,
                                   !s->u.var.mut, s->line, c->scopes.len);
                /* A zero-initialized value that can hold references can only hold
                 * nulls: a non-nullable reference has no zero value and was rejected
                 * just above. Its references therefore point at depth 0, which is not
                 * the depth of the slot -- using the slot depth would falsely reject
                 *   var a: [2]?ref node  a[0] = ref *p  return a */
                if (s->type && typeContainsRef(c->tt, s->type)) sym->refDepth = 0;
                s->u.var.cname = sym->cname;
                return;
            }

            /* A `let` is a read-only binding, so its type may not carry `mut`:
             * asking for both read-only and writable in one declaration contradicts
             * itself. */
            if (!s->u.var.mut && s->u.var.ann) {
                Type *at = s->u.var.ann;
                if (at && ((at->kind == TY_REF && at->mut) ||
                           (at->kind == TY_GENERIC && at->mut))) {
                    ckError(c, s->line,
                            "a read-only binding cannot carry a writable reference or view; "
                            "write `var` if you need to modify through it",
                            "`let %s` cannot be declared with a `mut` type", s->u.var.name);
                }
            }

            /* `@overwrite` is about an allocation: two checks and one mark.
             *   - the binding must be `var`, because every execution re-points the name
             *     at the same piece of storage and usually writes through it as well
             *   - the initializer must be `new`: a value, a local or a call result has
             *     no piece of storage to reuse
             *   - the mark is set before the initializer is checked, so the level is
             *     computed for the function body rather than for the enclosing block */
            if (s->u.var.overwrite) {
                if (!s->u.var.mut) {
                    ckError(c, s->line,
                            "Reusing one piece of storage means the binding is re-pointed at it"
                            " on every execution, so it must be writable. Write `var`.",
                            "`@overwrite` needs `var`: it re-binds the name to the same storage"
                            " on every execution, and you will usually write into it too");
                    return;
                }
                if (!s->u.var.init || s->u.var.init->kind != EX_NEW) {
                    ckError(c, s->line,
                            "`@overwrite` is about an **allocation**: it says \"this piece of"
                            " storage is reused; the previous round's contents are gone\". A value,"
                            " a local, or a call result has no such piece of storage to reuse."
                            " Write `@overwrite var n = new T` (or drop the annotation).",
                            "`@overwrite` only applies to `new`: the initializer must be an"
                            " allocation");
                    return;
                }
                s->u.var.init->reuse = true;
            }

            /* `let x = new T` leaves that storage zero forever. The claim is
             * decidable and has no false positives:
             *   - the allocation has exactly one reference, this name, and `let` hands
             *     out a read-only one
             *   - a read-only reference can neither be written nor turned back into a
             *     writable one: `ref *n`, a `mut ref` parameter and `x.f = v` are all
             *     rejected, so read-only-ness propagates along the chain
             *   - no path can write the storage, so every byte stays zero
             *
             * This is a warning rather than an error because one legitimate corner
             * exists: feeding an all-zero buffer to a read-only consumer, as in
             * `let buf = new [64]u8  hash(buf[..])`.
             *
             * `var w = new T  let r = w` is deliberately not warned about: the object
             * has a writable source, and turning an existing writable alias into a
             * read-only view is what `let` is for. */
            if (!s->u.var.mut && s->u.var.init &&
                (s->u.var.init->kind == EX_NEW || s->u.var.init->kind == EX_GENCALL)) {
                ckWarn(c, s->line,
                       "`let` is about the **name**, not the object: it promises you will not"
                       " write through this name. A fresh allocation has no other name, so"
                       " nothing can ever write into it and the storage stays zero. Use `var`"
                       " to write into it -- or, for a read-only view, bind one of something"
                       " writable: `var w = new T  let r = w`.",
                       "this allocation can never be written: `let` bound a fresh allocation,"
                       " and a read-only reference cannot be turned back into `mut ref`");
            }

            if (s->u.var.ann) adoptContextType(s->u.var.init, s->u.var.ann);

            /* `let x = e?` is one of the places where `?` is allowed. With an
             * annotation present the annotation decides whether a reference is
             * dereferenced; without one the initializer is a value position. */
            Type *it;
            if (s->u.var.init->kind == EX_TRY) it = checkTryInner(c, s->u.var.init);
            else if (s->u.var.ann)             it = checkInto(c, s->u.var.ann, s->u.var.init);
            /* Without an annotation the natural type is kept, so a reference stays a
             * reference; a copy of the value is written `let v = *p`. Dereferencing
             * here instead would make it impossible to bind a reference returned by a
             * call, as in `let r = pickFirst(ref a, ref b)`. */
            else                               it = checkExpr(c, s->u.var.init);

            /* Decide after the initializer has been checked: `checkExpr` sets the
             * home depth from the arguments, and this call overrides it with the depth
             * the value is really being stored at. */
            markCallHomeIfEscaping(c, s->u.var.init, (int)c->scopes.len);

            /* A type inferred for a `let` is downgraded to read-only: `let p = ref x`
             * gives `p: ref T` rather than `mut ref T`, and `let s = a[..]` gives
             * `s: slice<T>`. Seeing `let` therefore means the chain stays read-only,
             * because the permission lives in the type and travels with every copy:
             * `var q = p` cannot launder it back into a writable reference. */
            if (!s->u.var.mut && !s->u.var.ann) {
                if (it && it->kind == TY_REF && it->mut) {
                    Type *ro = ttRef(c->tt, it->inner);
                    it = ro;
                } else if (it && it->kind == TY_GENERIC && it->mut && ttIsViewType(it)) {
                    it = ttViewReadonly(c->tt, it);
                }
            }
            Type *declT = s->u.var.ann ? s->u.var.ann : it;

            if (s->u.var.ann)
                checkAssignable(c, s->u.var.ann, it, s->u.var.init, "initializer");

            /* The initializer may not point at a local that dies before this binding. */
            checkEscape(c, s->u.var.init, c->scopes.len, s->line, "this initializer");
            s->type = ttIsError(declT) ? ttError(c->tt) : declT;
            Sym *sym = declare(c, s->u.var.name, s->type, s->u.var.mut,
                               !s->u.var.mut, s->line, c->scopes.len);
            s->u.var.cname = sym->cname;
            /* Record where this binding's value came from, which is its initializer.
             * Promotion walks that chain backwards: after `var n = new node`, a later
             * `head = n` follows `n` to the `new` site and raises it to the level of
             * `head`. A declaration without an initializer records NULL. */
            noteOrigin(c, sym, s->u.var.init);
            /* For a binding that holds a reference, the depth of what it points at is
             * computed from the initializer: in `var cur: ?ref node = head` the head is
             * a parameter, so the cursor has depth 0 and may be returned. */
            /* An initializer that is a call needs its own bookkeeping, as in
             * `let t = make()`.
             *
             * Only the call site knows where the references in the result live, since
             * the call site picks `arenaArg`, and the field table filled in below only
             * covers a struct literal. Without this, the field `t.p` has no entry,
             * `exprRefDepth(t)` falls back to `sym->refDepth` and gets 0, and a later
             * `b = t` with a shallower `b` looks safe while the block exit frees the
             * node. The sanitizer reports a heap use after free, and running it once
             * does not even crash: only building inside a block and storing outside it
             * exposes the bug.
             *
             * The rule: when the callee has a home arena it allocates into the arena
             * this call passes, which is the level `markCallHomeIfEscaping` decided from
             * `at` just above, so the result has depth `at`. Here `at` is
             * `c->scopes.len`, the level the binding lives at. */
            if (s->type && typeContainsRef(c->tt, s->type)) {
                int d = exprRefDepth(c, s->u.var.init);
                Expr *ini = s->u.var.init;
                if ((ini->kind == EX_CALL || ini->kind == EX_METHOD)
                    && ini->func && ini->func->needsHome && (int)c->scopes.len > d)
                    d = (int)c->scopes.len;      /* a callee with a home arena uses mine */
                sym->refDepth = d;
            }
            /* A struct literal records a depth for each field it writes. A field that
             * is omitted is zero-initialized, so it can only hold nulls and has depth
             * 0; no entry is created for it and reads fall back to the conservative
             * answer. That costs a possible false rejection, never a hole. */
            if (s->u.var.init && s->u.var.init->kind == EX_STRUCTLIT && sym) {
                for (size_t fi = 0; fi < s->u.var.init->u.lit.inits.len; fi++) {
                    FieldInit *fip = *(FieldInit **)vecAt(&s->u.var.init->u.lit.inits, fi);
                    c->curStoreVal = fip->value;          /* which value wrote this field */
                    noteFieldDepthWrite(c, sym, fip->name, exprRefDepth(c, fip->value));
                }
                c->curStoreVal = NULL;
                /* The field table of a struct literal is complete. Every written field
                 * has an entry, and an omitted one is either a nullable reference whose
                 * zero value is null, hence depth 0, or a reference with no zero value,
                 * which the checker rejects on the spot when it fills the omissions
                 * with zero values. No field can therefore hold an unrecorded value. */
                sym->fieldsComplete = true;
            }
            return;
        }

        case ST_ASSIGN: {
            Type *tt_ = checkExpr(c, s->u.assign.target);

            /* The target is a binding that was narrowed, so the comparison uses the
             * declared type of the slot: narrowing changes what a read produces, not
             * what the slot can hold. That is what makes
             * `while cur != null { cur = cur.next }` legal -- the assignment retires the
             * proof inside the loop body, and the loop condition proves it again for the
             * next round. */
            if (s->u.assign.target->kind == EX_IDENT) {
                Sym *slot = lookup(c, s->u.assign.target->u.ident.name);
                if (slot && slot->type && slot->type->kind == TY_REF && slot->type->nullable &&
                    tt_->kind == TY_REF && !tt_->nullable) tt_ = slot->type;
            }

            /* The target has reference type, so this assignment either writes the
             * object it points at or retargets the reference; the type of the
             * right-hand side decides which, below. Assigning a value to a reference
             * has to match the pointee type. */
            if (tt_->kind == TY_REF) {
                /* `requireMutable` does not apply here: it asks whether the reference
                 * type carries `mut`, while retargeting writes the slot itself, that is
                 * the variable or the field. Only the binding has to be writable, which
                 * is why `var p: ref T = ref x  p = ref y` is legal. */
                /* Retargeting writes the slot itself, so only the slot has to be
                 * writable:
                 *   - a variable, field or element needs a writable root, that is `var`
                 *   - for `*p` the writability comes from the type of `p`, namely
                 *     `mut ref T`
                 * Routing this through `placeRoot` alone returned NULL for `*p` and made
                 * `fn push(head: mut ref ?ref node) { *head = cell }` report "the binding
                 * is read-only", which was a real bug. */
                /* Walk the path first, then look at the slot it lands on.
                 *   - every reference crossed on the way has to be a `mut ref`
                 *     (`*p`, `cur.f`, `(*p).f`)
                 *   - crossing a reference means the storage lives inside the pointee,
                 *     so there is no root in this frame and the walk stops there
                 *   - without a crossing, the storage is the slot of a binding in this
                 *     frame, and that binding has to be writable
                 * Recognising only a target that is itself `*p`, plus `placeRoot`, both
                 * falsely rejected `(*cell).next = v` and missed a write through a
                 * read-only reference. */
                bool crossed  = false;
                bool refsOk   = pathRefsAllMut(s->u.assign.target, &crossed);
                bool slotOk   = refsOk;
                if (slotOk && !crossed) {
                    Sym *slotRoot = placeRoot(c, s->u.assign.target);
                    slotOk = slotRoot && slotRoot->mut;
                }
                if (!slotOk) {
                    /* The two causes get two different messages, or the user looks for
                     * a mistake in a binding that is fine. */
                    if (!refsOk)
                        ckError(c, s->line,
                                "`ref T` is a **read-only** borrow; writing the object it "
                                "points to needs `mut ref T` in the declaration. Read-only "
                                "is the default so that a signature says what it does.",
                                "cannot write through a read-only reference");
                    else
                        ckError(c, s->line,
                                "rebinding a reference writes the binding itself, so the binding"
                                " must be writable (`var`, or a `mut ref`)",
                                "cannot rebind `%s`: the binding is read-only",
                                s->u.assign.target->kind == EX_DEREF
                                  ? typeStr(c, s->u.assign.target->u.deref.operand->type)
                                  : "this");
                    return;
                }

                Expr *v = s->u.assign.value;

                /* What the right-hand side is decides which operation this is; the
                 * ambiguity is resolved by the types rather than by forbidding one of
                 * the two:
                 *   right side is a value `T`         => write into the pointee
                 *   right side is a reference `ref T` => retarget the reference
                 * The two have different types and the right-hand side is visible in the
                 * source, so retargeting does not have to be forbidden. */
                /* `null` needs the reference itself (`?ref T`) as its context, not the
                 * pointee: `p.next = null` arrives on this retargeting path, because the
                 * right-hand side is a reference. */
                adoptContextType(v, v->kind == EX_NULL ? tt_ : tt_->inner);
                Type *vt0 = checkExpr(c, v);          /* natural type: references survive */

                /* Dropping the non-null proof has to wait until the right-hand side has
                 * been checked: in `cur = cur.next` the `cur` on the right still relies
                 * on the proof from the loop condition. Doing this earlier produced a
                 * false rejection. */
                if (s->u.assign.target->kind == EX_IDENT)
                    unNarrow(c, s->u.assign.target->u.ident.cname);

                if (vt0->kind == TY_REF) {
                    /* Retargeting: the types have to line up, downgrading `mut ref` to
                     * `ref` stays allowed, and the new target may not die before the
                     * reference does.
                     *
                     * The depth comparison uses the old `refDepth`, the depth of what
                     * the slot pointed at before. Updating it has to wait until these
                     * checks are done, or the new value is compared against itself and
                     * `p = ref <deeper local>` slips through entirely, which an
                     * adversarial test did produce. */
                    /* `storeLayer` answers "which level is this stored at"; the trap
                     * with a reference-typed binding is described in its own comment. */
                    int at0 = storeLayer(c, s->u.assign.target);
                    /* The lifetime this reference has to reach is capped by the life
                     * of the container.
                     *
                     * `storeLayer(h.q)` goes through `placeDepth` and answers with the
                     * depth of the projection, namely 2, while `h` itself lives at
                     * level 1, since a local binding cannot outlive its scope. Resolving
                     * the site at 2 frees it when the block ends, even though the pointer
                     * inside `h.q` escaped the frame with `out = h`.
                     *
                     * The cap is `c->scopes.len`: nothing in this frame outlives the
                     * current scope, so that is the shallowest level this store can
                     * require. Only the number used for bookkeeping is capped; the `at0`
                     * handed to `checkEscape` below is left alone. */
                    /* Try to promote first: can every `new` inside this value live at
                     * level `at0`? If not, the depth check below reports as before.
                     * This has to run before `checkEscape`, which reads the depth of the
                     * value. */
                    promoteInto(c, v, at0);
                    checkAssignable(c, tt_, vt0, v, "assignment");
                    checkEscape(c, v, at0, s->line, "this reference");
                    /* Retargeting moves the pointee depth along with it
                     * (`cur = cur.next`). */
                    if (s->u.assign.target->kind == EX_IDENT) {
                        Sym *slot0 = lookup(c, s->u.assign.target->u.ident.name);
                        if (slot0 && slot0->type && slot0->type->kind == TY_REF)
                            slot0->refDepth = exprRefDepth(c, v);
                        /* The origin moves with it: after `mid = n` the origin of `mid`
                         * is `n`, so the intermediate bindings on a promotion walk stay
                         * reachable.
                         *
                         * This is recorded only in this branch, where a reference-typed
                         * target receives a reference. A slot whose address was taken is
                         * skipped, because an alias could change it, and the conservative
                         * fallback then reports the depth error as before. Cycles such as
                         * `a = b  b = a` are caught by the hop limit in `promoteInto`. */
                        if (slot0) noteOrigin(c, slot0, v);
                    }
                    /* Retargeting an element or a field that holds a reference
                     * (`a[0] = ref local`) has to be recorded too. Otherwise
                     * `var a: [2]?ref node`, zero-initialized so that every slot holds
                     * null and the depth is 0, keeps its upper bound of 0 after
                     * `a[0] = ref local` at depth 1, and `return a` is accepted while the
                     * array points at a dead local. The maximum is taken because
                     * `refDepth` is an upper bound on where the references inside point. */
                    {
                        Sym *rootA = placeRoot(c, s->u.assign.target);
                        if (rootA && rootA->type && typeContainsRef(c->tt, rootA->type)) {
                            int dA = exprRefDepth(c, v);
                            /* Record per field. Without a taken address the entry is
                             * overwritten rather than merged, so after `h.p = null` the
                             * effective depth of the root really does go down; always
                             * taking the maximum caused false rejections. */
                            const char *fn_ = (s->u.assign.target->kind == EX_FIELD)
                                              ? s->u.assign.target->u.field.name : NULL;
                            c->curStoreVal = v;
                            noteFieldDepthWrite(c, rootA, fn_, dA);
                            c->curStoreVal = NULL;
                        }
                    }
                    /* Retargeting runs the borrowed-value check as well. In
                     *     struct slot { r: mut ref i32 }
                     *     fn stash(b: mut ref slot, p: mut ref i32) { b.r = ref *p }
                     * the permission is sufficient and the depth is sufficient (`p` is
                     * a parameter, so depth 0), which leaves "this value is really the
                     * caller's" as the only rule that can stop it. Without this call the
                     * program printed stale data, a real dangling reference. */
                    checkStoreEscape(c, v, s->u.assign.target, s->line);
                    return;
                }

                /* There is no implicit write-through on a reference: `p = v` only
                 * retargets, and writing through is spelled `*p = v`. Each form then has
                 * exactly one meaning, and the reader does not have to look at the
                 * right-hand side to tell them apart. */
                ckError(c, s->line,
                        "write through the reference instead: `*p = v` -- `=` on a reference"
                        " only ever retargets it",
                        "cannot assign a value to `%s`: it is a reference, not a place",
                        typeStr(c, tt_));
                return;
            }

            desugarBareCtor(c, s->u.assign.value, tt_);
            adoptContextType(s->u.assign.value, tt_);
            Type *vt = checkMaybeTry(c, s->u.assign.value);

            /* Feed the destination depth back into the `new` sites inside the value
             * first. The order matters: the two blocks just below record
             * `exprRefDepth(value)` into the depth table of the target, and promoting
             * one step later would record the old number and cause false rejections
             * afterwards. */
            int atDst = storeLayer(c, s->u.assign.target);
            /* Record the new value only when the target *is* the binding.
             *
             * `*q = n * 2` writes through the reference into the box `q` points at; `q`
             * still holds the same box, so "what does `q` hold now" is unchanged. Taking
             * `placeRoot` here would answer with `q` anyway and overwrite the binding's
             * value with `n * 2`, and the allocation published at line 17 would then be
             * read back through `*q` as if it had been stored at the level of `*q`, which
             * is this frame: measured on `examples/alloc-in-block`, the `alloc` inside the
             * nested block came out with the level "must outlive this frame" while the
             * function has no home arena, so the generated C said `__extc_home` and did not
             * compile. Assignments through a projection (`s.f`, `v[i]`) are the same
             * question and take the same answer: they do not rebind the root binding. */
            if (s->u.assign.target->kind == EX_IDENT) {
                Sym *dst = identBindOf(s->u.assign.target);
                if (dst) dst->lastStore = s->u.assign.value;
            }
            recordStore(c, s->u.assign.value, atDst, s->line);
            promoteInto(c, s->u.assign.value, atDst);
            markCallHomeIfEscaping(c, s->u.assign.value, atDst);
            /* A value binding that can hold references is written, either wholesale or
             * through one of its fields or elements, so the recorded "where do the
             * references inside point" is widened with the maximum, since `refDepth` is
             * an upper bound. */
            {
                /* The plain assignment path used to write only the `refDepth` of the
                 * root and never the field table, so after
                 * `b.c = inner{v: ref local}` the entry for `b.c` kept its old value and
                 * `return b.c` was accepted. Both kinds of assignment now go through
                 * `noteFieldDepthWrite`, which updates the field entry and the root as
                 * the maximum over the fields. */
                Sym *vs = placeRoot(c, s->u.assign.target);
                if (vs) {
                    int d2 = valDepthForStore(c, s->u.assign.value);
                    const char *fn2 = (s->u.assign.target->kind == EX_FIELD)
                                        ? s->u.assign.target->u.field.name : NULL;
                    if (d2 > 0 || (vs->type && typeContainsRef(c->tt, vs->type))) {
                        c->curStoreVal = s->u.assign.value;
                        noteFieldDepthWrite(c, vs, fn2, d2);
                        c->curStoreVal = NULL;
                    }
                }
            }
            if (requireMutable(c, s->u.assign.target, s->line, "write")) return;
            /* A value stored into a field or an element may not be deeper than the
             * target. */
            checkStoreEscape(c, s->u.assign.value, s->u.assign.target, s->line);
            if (0) checkEscape(c, s->u.assign.value, placeDepth(c, s->u.assign.target),
                        s->line, "this assignment");
            if (ttIsError(tt_)) return;
            checkAssignable(c, tt_, vt, s->u.assign.value, "assignment");
            return;
        }

        case ST_IF: {
            expectBool(c, checkValue(c, s->u.ifs.cond), s->u.ifs.cond);

            /* Narrowing a `?ref T`: `if p != null`, `if p == null ... else` and the
             * guard `if p == null { return }` all tell the compiler that `p` is not null
             * on the branch, so no runtime check has to be generated inside it. */
            bool whenTrue = false;
            const char *tg = narrowTarget(c, s->u.ifs.cond, &whenTrue);
            const size_t mark = c->narrow.len;

            if (tg && whenTrue) narrowFactsOf(c, s->u.ifs.cond);
            checkBlockBody(c, s->u.ifs.thenBody);
            c->narrow.len = mark;              /* the proof ends with the then branch */

            if (s->u.ifs.elseBody) {
                if (tg && !whenTrue) pushNarrow(c, tg);
                if (s->u.ifs.elseBody->kind == ST_BLOCK) checkBlockBody(c, s->u.ifs.elseBody);
                else                                      checkStmt(c, s->u.ifs.elseBody);
                c->narrow.len = mark;
            } else if (tg && !whenTrue && blockExits(s->u.ifs.thenBody)) {
                /* The guard form: the condition leaves the function or the loop, so
                 * reaching the statements after it means `p` is not null. The proof stays
                 * in the current scope, until the end of this block. */
                pushNarrow(c, tg);
            }
            return;
        }

        case ST_WHILE: {
            /* The condition of a `while` may not be hoisted: a hoisted prefix is
             * evaluated once before the loop, not once per round. */
            c->noHoist++;
            expectBool(c, checkValue(c, s->u.whiles.cond), s->u.whiles.cond);
            c->noHoist--;

            /* `while cur != null { cur = cur.next }` is the shape the whole language
             * exists for, so the loop condition narrows as well: inside the body the
             * binding is known not to be null. Assigning to it drops the proof, so the
             * next round compares again, exactly as in the C idiom
             * `while (p) { p = p->next; }`. */
            bool whenTrue = false;
            const char *tg = narrowTarget(c, s->u.whiles.cond, &whenTrue);
            const size_t mark = c->narrow.len;
            if (tg && whenTrue) narrowFactsOf(c, s->u.whiles.cond);
            checkBlockBody(c, s->u.whiles.body);
            c->narrow.len = mark;
            return;
        }

        case ST_EXPR:
            /* `f()?` as a statement on its own: the most useful of the four places
             * where `?` is allowed, meaning "this step has to succeed, or the whole
             * chain fails". */
            checkMaybeTry(c, s->u.expr.expr);
            return;

        case ST_RETURN: {
            Type *want = c->curFunc ? c->curFunc->ret : NULL;
            if (!s->u.ret.value) {
                if (want && !ttIs(want, "void")) {
                    ckError(c, s->line, NULL, "`%s` must return a value of type `%s`",
                            c->curFunc->name, typeStr(c, want));
                }
                return;
            }
            if (!want) {
                checkExpr(c, s->u.ret.value);
                ckError(c, s->line, NULL, "`%s` does not return a value",
                        c->curFunc ? c->curFunc->name : "this function");
                return;
            }
            /* A bare constructor such as `return failure(e)`; the type comes from the
             * `want` above. */
            desugarBareCtor(c, s->u.ret.value, want);

            /* `return e?`: the value of the expression is the payload, while the
             * function hands back the outer type, so what is compared is whether the
             * payload fits the payload of the outer type; the code generator wraps it
             * back up. The `adoptContextType` plus `checkAssignable` path below cannot
             * be used here, because it would ask an `i64` to be an `option<i64>`. */
            if (s->u.ret.value->kind == EX_TRY) {
                Type *vt = checkTryInner(c, s->u.ret.value);
                Type *wb = ttBase(want);
                /* This branch skips the `checkEscape` below, so `promoteInto` would
                 * never run and an allocation handed out by `e?` would never be marked
                 * as escaping the frame; the pass that runs after the bodies would then
                 * put the site back at block level. `return v` in
                 * `varArray<T>::withCap` takes exactly this branch, and re-checking the
                 * instantiation reported "depth 1, but this can only hold up to 0". The
                 * payload is a value whose references end up with the caller, so it is
                 * promoted to level 0. */
                recordStore(c, s->u.ret.value, 0, s->line);
                promoteInto(c, s->u.ret.value, 0);
                if (wb && wb->kind == TY_GENERIC && wb->targs.len >= 1)
                    checkAssignable(c, *(Type **)vecAt(&wb->targs, 0), vt,
                                    s->u.ret.value, "return value");
                return;
            }

            adoptContextType(s->u.ret.value, want);
            Type *vt = checkInto(c, want, s->u.ret.value);   /* no deref when a ref is wanted */
            markCallHomeIfEscaping(c, s->u.ret.value, 0);    /* handed out, so use the home arena */
            checkAssignable(c, want, vt, s->u.ret.value, "return value");
            /* A returned reference must point at a parameter or at static data, which
             * is depth 0. */
            if (getenv("EXTC_DBG_RET3"))
                fprintf(stderr, "[ret3] %-8s line=%d kind=%d d=%d mentionsParam=%d\n",
                        c->curFunc?c->curFunc->name:"?", s->line,
                        (int)s->u.ret.value->kind, exprRefDepth(c, s->u.ret.value),
                        mentionsParam(s->u.ret.value->type)?1:0);
            /* A returned value is handed to the caller, so it is published at level 0:
             * the whole point of returning it is that it outlives this frame. */
            recordStore(c, s->u.ret.value, 0, s->line);
            checkEscape(c, s->u.ret.value, 0, s->line, "this return value");
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE:
            return;

        case ST_MATCH: {
            /* `match e { variant => ... }`
             *
             * The whole value of this feature is the exhaustiveness check. Written as
             * `if e == gameError.outOfRange { } else { }` the else branch has to be
             * filled in by hand, and a variant added later would silently fall into it;
             * `match` reports the missing arm. */
            Type *st = checkValue(c, s->u.match.scrutinee);
            Type *sb = ttBase(st);
            if (ttIsError(st)) return;

            if (!sb || sb->kind != TY_ENUM || !sb->edef) {
                ckError(c, s->line,
                        "`match` works on enums (`type color = | red | green`); "
                        "everything else is compared with `==`",
                        "cannot `match` on a value of type `%s`", typeStr(c, st));
                return;
            }
            TypeDef *td = sb->edef;

            /* Each arm: the name has to be a variant of this enum, and no variant may
             * be matched twice. */
            for (size_t i = 0; i < s->u.match.arms.len; i++) {
                MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
                if (!findVariant(td, arm->variant)) {
                    Buf note;
                    bufInit(&note, c->arena);
                    bufPuts(&note, "variants of ");
                    bufPuts(&note, sb->name);
                    bufPuts(&note, ":");
                    for (size_t j = 0; j < td->variants.len; j++)
                        bufPrintf(&note, " %s", (*(Variant **)vecAt(&td->variants, j))->name);
                    ckError(c, arm->line, bufCstr(&note),
                            "`%s` is not a variant of `%s`", arm->variant, sb->name);
                    return;
                }
                for (size_t j = 0; j < i; j++) {
                    MatchArm *prev = *(MatchArm **)vecAt(&s->u.match.arms, j);
                    if (strcmp(prev->variant, arm->variant) == 0) {
                        ckError(c, arm->line, NULL,
                                "`%s` is matched twice", arm->variant);
                        return;
                    }
                }
            }

            /* Exhaustiveness: no variant may be left out. */
            for (size_t j = 0; j < td->variants.len; j++) {
                const char *vn = (*(Variant **)vecAt(&td->variants, j))->name;
                bool covered = false;
                for (size_t i = 0; i < s->u.match.arms.len && !covered; i++)
                    covered = strcmp((*(MatchArm **)vecAt(&s->u.match.arms, i))->variant, vn) == 0;
                if (covered) continue;

                Buf note;
                bufInit(&note, c->arena);
                bufPuts(&note, "add a `");
                bufPuts(&note, vn);
                bufPuts(&note, " => { ... }` arm");
                ckError(c, s->line, bufCstr(&note),
                        "`match` does not handle `%s` -- every variant must be listed",
                        vn);
                return;
            }

            /* Each arm body gets its own scope, and the payload bindings of an arm, as
             * in `circle(r) => ...`, live there: the i-th name takes the i-th payload
             * field. */
            for (size_t i = 0; i < s->u.match.arms.len; i++) {
                MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
                Variant  *v   = findVariant(td, arm->variant);

                if (arm->binds.len != v->types.len) {
                    ckError(c, arm->line, NULL,
                            "`%s` carries %zu value(s), so this arm binds %zu name(s), not %zu",
                            v->name, v->types.len, v->types.len, arm->binds.len);
                    return;
                }
                pushScope(c);
                for (size_t k = 0; k < arm->binds.len; k++) {
                    const char *bn = *(const char **)vecAt(&arm->binds, k);
                    Type *bt = payloadType(c->tt, sb, v, k);
                    /* A payload binding is a copy of the value, following value
                     * semantics, and the name is read-only; copy it into a `var` to
                     * modify it. */
                    Sym *bs = declare(c, bn, bt, false, false, arm->line, c->scopes.len);
                    /* Write the resolved C name back into the arm, the same treatment
                     * `Param.cname` and a declaration's `cname` get. Without it, two
                     * `match` statements in one scope binding the same name would declare
                     * the original name while the body looks up `s__2`, and the generated
                     * C would not compile: `'s__2' undeclared`. */
                    *(const char **)vecAt(&arm->binds, k) = bs->cname;
                }
                for (size_t k = 0; k < arm->body->u.block.stmts.len; k++)
                    checkStmt(c, *(Stmt **)vecAt(&arm->body->u.block.stmts, k));
                popScope(c);
            }
            return;
        }

        case ST_BLOCK:            checkBlockBody(c, s);
            return;
    }
}

