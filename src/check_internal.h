#ifndef EXTC_CHECK_INTERNAL_H
#define EXTC_CHECK_INTERNAL_H

/* Internal header of the checker: the state shared between the check_*.c files and the
 * prototypes that cross between them.
 *
 * The only public entry point is `checkModule` from `check.h`. This header holds types
 * and prototypes only, never logic.
 */

#include "check.h"

/* Display name of a declaration for diagnostics: `DN(x)` takes a `StructDef *` or a
 * `TypeDef *`.
 *
 * After module mangling `name` is the internal `io$reader`, while `srcName` is the
 * `io::reader` the user wrote. Printing `->name` leaks the internal encoding into the
 * message, as in "struct `alpha$pair` has no field `zzz`".
 */
#define DN(d) ttDispName((d) ? (d)->srcName : NULL, (d) ? (d)->name : NULL)

/* Display name of a function, for diagnostics and debug output: `FN(f)`.
 *
 * After mangling `f->name` is `pair$make` while the user wrote `pair::make`, which the
 * effect dump used to print verbatim. In the root module, or in a single-file program,
 * `modName` is empty and `name` already is the source name. The implementation replaces
 * the `mod$` prefix with `mod::`, and that prefix is always `modName`.
 */
const char *checkFnDisplay(const char *name, const char *modName);
#define FN(f) checkFnDisplay((f) ? (f)->name : NULL, (f) ? (f)->modName : NULL)

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------ shared state and shared types
 * Whatever more than one file needs, so that whoever uses it can see it. */

typedef struct {
    const char *name;    /* name as written in the source */
    const char *cname;   /* name used in generated C; a shadowed `a` becomes `a__2` */
    Type       *type;    /* declared type, or the inferred one for an unannotated `let` */
    bool        mut;     /* true for `var`; false for `let` and for payload bindings */
    /* True once the address of this binding has been handed out: `ref x`, `x[..]`, or
     * passing it as a `mut ref` argument.
     *
     * With a taken address an alias can change the contents behind our back, so the
     * depth bookkeeping may only be updated weakly. Without one an entry is overwritten
     * outright, which removes the false rejections where writing null or a shallower
     * value could not lower the recorded depth. */
    bool        addressed;
    /* Per-field depths, at most four entries. To be read as one field block, not as
     * four independent ones.
     *
     * A field such as `h.p` gets a depth of its own instead of sharing one number with
     * `h`, so `h.p = null` can really overwrite that entry. Writes that cannot be
     * attributed to a single field, such as an element write, go to `otherDepth`. The
     * effective depth of the root is the maximum of `otherDepth` and the field entries. */
    struct { const char *name; int depth;  /* field name; recorded depth of the value in it */
             /* Which value was last written into this field, or NULL when that is
              * unknown. It is used only to find the allocation site inside a container,
              * never to change any bookkeeping.
              *
              * It is needed because `var h: box = { p: null, q: null }` has an origin
              * made of nulls which never mentions the `x` that a later `h.q = x` stored,
              * so walking origins would never reach that site. */
             Expr *src;
             /* Depth of the write that produced `src`. Compare against this number, not
              * against `depth`: `depth` is merged with the maximum on a weak update, so
              * after `h.q = x  h.q = null` it is still deep, and comparing with it would
              * call the null write the deeper one, clear the source and break the chain. */
             int   srcDepth;
             /* The shallowest level this field has been required to reach; 0 means it
              * must outlive the frame. `out = h` stores at level 1, but `return out`
              * needs the frame-outliving level, so the site has to end up in the home
              * arena. Looking only at the current destination computes a level that is
              * too shallow and leaves the site there, which dangles. */
             int   minReq; } fields[4];
    /* Whether the field table is complete.
     *
     * Complete means every field of the binding has an entry, or is known to be 0; only
     * then may an entry be cleared and the root recomputed as the maximum of the
     * remaining ones. An incomplete table, such as one copied from elsewhere, can have
     * fields with values that are missing here, and lowering a depth would make those
     * values vanish; that is how a stack-use-after-scope was introduced. The default is
     * false, the conservative answer. */
    bool        fieldsComplete;
    int         nfields;    /* number of valid entries in `fields` */
    int         otherDepth; /* depth for writes that belong to no single field */
    int         depth;      /* lexical depth: a parameter is 0, a local in the function
                             * body is 1, and every nested block adds 1. The escape
                             * checks compare this number. */
    /* For a binding that holds a reference: how deep is the thing it points at?
     *
     * This differs from `depth`. In `var cur: ?ref node = head` the slot holding the
     * reference is in this frame, at depth 1, while the node it points at is outside,
     * at depth 0. The rule "a reference may not outlive what it points at" asks about
     * the pointee, so it needs this number. Using the slot depth everywhere falsely
     * rejected the most ordinary list search, `while cur != null { ... return cur }`,
     * as pointing at a local that dies.
     *
     * It is meaningful for a reference-typed binding, and it doubles as an upper bound
     * on where the references inside a reference-carrying value point. The default is
     * the slot depth, the conservative answer; a declaration or a retarget narrows it
     * to the real depth of the value. */
    int         refDepth;
    /* The value this binding was initialized with, or NULL when it had no initializer.
     *
     * Promotion walks origins backwards:
     *     var n = new node        // the origin of `n` is this `new`
     *     head = n                // stored into an outer place, so follow `n` back and
     *                             // raise that `new` to the level of `head`
     * Only the initializer is followed, and only through explicit assignments; aliases
     * are not chased, so a site that cannot be reached keeps its depth error. */
    Expr       *origin;

    int         line;      /* source line of the declaration, for diagnostics */
    /* The module this binding belongs to; set for globals, NULL for locals. Name
     * resolution uses it to reject an unqualified reference to another module's name. */
    const char *modName;
} Sym;

/* Pins the binding a name resolved to onto the AST node that used it.
 *
 * The pass that runs after all bodies have been checked cannot look a local up by name,
 * because its scope is gone by then; the same trap is described in `noteOrigin`. What
 * the pinning avoids is a re-check that silently uses the depth frozen before the
 * solver ran and rejects a correct program.
 *
 * A binding pointer is stored rather than a name, because one name in different scopes
 * is a different binding: the answer at the moment of resolution is the authoritative
 * one, while looking the name up again later can find another binding that happens to
 * have the same name. The lifetime is not a problem, because bindings are
 * arena-allocated, so leaving a scope only removes them from visibility.
 *
 * The wrapper is there for type safety: `ast.h` cannot name `Sym` and therefore holds
 * an opaque pointer, the same arrangement a `cname` uses, so each side casts once
 * instead of passing a bare `void *` around. */
typedef struct { Sym *sym; } IdentBinding;

/* The binding an `EX_IDENT` node resolved to, or NULL when it has not been resolved. */
static inline Sym *identBindOf(Expr *e) {
    return (e && e->kind == EX_IDENT && e->u.ident.sym) ? ((IdentBinding *)e->u.ident.sym)->sym : NULL;
}

/* The one place that converts between "which level an allocation site lives at" and
 * "how deep the references inside it are".
 *
 * The same conversion used to be written five times across four files and the variants
 * did not agree: two of them computed `(arenaLevel == HOME) ? 0 : arenaLevel`, while
 * another preferred a non-zero `refDepth` and could return a depth that contradicted
 * the level of the site. The bug surfaced on a site whose `refDepth` was 0 while its
 * level said the depth should be 1.
 *
 * `refDepth` and `arenaLevel` have to be two spellings of the same number, and that
 * holds only while the conversion lives in one place. Deriving the answer from the
 * level is also strictly more accurate, since `refDepth` is not authoritative while a
 * level is still undecided.
 *
 * Params:
 *   arenaLevel - arena level of a site: `ARENA_HOME` for storage that outlives this
 *                frame, k >= 1 for the k-th nested block, 0 or negative while the
 *                level is not decided yet
 *
 * Returns:
 *   The equivalent depth: 0 for the home arena and for an undecided level, k for block
 *   level k. A caller that has to tell "undecided" from "depth 0" must look at
 *   `arenaLevel` itself.
 */
static inline int arenaDepthOf(int arenaLevel) {
    if (arenaLevel == ARENA_HOME) return 0;
    return arenaLevel > 0 ? arenaLevel : 0;
}

/* How often a name is used in the current function; the C name generator uses the count
 * to decide whether a rename is needed. */
typedef struct { const char *name; int count; } NameUse;

/* One lexical scope: the bindings declared in it. The innermost scope is last. */
typedef struct { Vec syms; } Scope;

/* A call site whose choice of home arena depends on which locals escape.
 *
 * Everything needed to recompute that choice is recorded here, so the pass that runs
 * after all bodies have been checked does not have to look anything up in a scope that
 * is gone by then. */
typedef struct {
    Expr  *call;         /* the EX_CALL / EX_METHOD / EX_ASSOC node; its arenaArg is
                          * rewritten by the later pass */
    char  *argRoot[8];   /* root name of each `mut ref` argument, NULL when that position
                          * is not a `mut ref` argument */
    int    argDepth[8];  /* depth of that argument at the call site; 0 = a parameter or a
                          * global, which already outlives this frame */
    int    n;            /* number of slots in use, at most 8 */
    bool   overflow;     /* more arguments than slots, so the picture is incomplete and
                          * the call is treated as receiving the home arena: conservative
                          * but sound */
} EArenaSite;

typedef struct Checker {
    Ctx       *ctx;         /* parser and module context, for diagnostics and lookup */
    Arena     *arena;       /* arena the checker allocates its own bookkeeping from */
    TypeTable *tt;          /* type table of the module being checked */
    Module    *m;           /* the module being checked */
    Vec        scopes;      /* Scope*, innermost last */
    FuncDef   *curFunc;     /* body being checked, or NULL outside a function body */
    Vec       *curParams;   /* names of the type parameters in scope, NULL outside a
                             * generic context */
    Vec        eqChecks;    /* EqCheck*: `==` checks deferred to instantiation */
    /* Instances of generic free functions: `fn f<T>` gets one instance per set of type
     * arguments. They are created here, at the call site that infers the arguments, and
     * the code generator emits them as ordinary functions. */
    Vec        funcInsts;   /* FuncDef*, with tmpl/targs/instName filled in */
    Vec        globals;     /* Sym* for module-level bindings (depth 0). They are not in
                             * `scopes`; see the comment in `lookup` */
    /* Every binding the checker has declared, appended by `declare`.
     *
     * The self-check mode walks this list to verify that the `refDepth` of a binding
     * agrees with the allocation site it came from. */
    Vec        allSyms;
    /* The value currently being stored somewhere.
     *
     * `noteFieldDepthWrite` records which value wrote a field, and its callers already
     * hold that value; passing it through a field keeps the signature of that function
     * unchanged, which would otherwise ripple through every call site. */
    Expr      *curStoreVal;
    Vec        nameUses;    /* NameUse*: how often each name is used in the current
                             * function, for the C name generator */
    Vec        moduleNames;     /* const char*, sorted; see check.c for the compile-time
                                 * optimization it enables */
    size_t     moduleNamesSize;
    /* Non-null narrowing for `?ref T`: which bindings are known not to be null at the
     * current position.
     *
     * A nullable reference may not be dereferenced before it has been shown to be
     * non-null, and the only way to show that is a comparison with null inside an `if`.
     * Narrowing is lexical, so it unwinds together with pushScope and popScope, and it
     * generates no runtime check at all: what the compiler can prove leaves no trace in
     * the generated code. */
    /* Non-zero while the current position cannot emit a statement prefix, which makes
     * every `??` that needs one an error. Two positions behave that way: the condition
     * of a `while`, where a hoisted prefix would be evaluated once before the loop
     * instead of once per round, and a global initializer, which has no statement to
     * hang a prefix on. */
    /* Checks deferred out of a generic body.
     *
     * A generic body is checked once, as a template, with `T` opaque, so every rule of
     * the form "only check this when the type carries a reference" returns early and an
     * instantiation with `T = slice<u8>` is never checked at all. The fix is to compute
     * the depth when the rule is recorded, assuming that `T` may carry a reference,
     * while the scope is still open and `lookup` still works; instantiation then only
     * has to answer whether `T` really carries one.
     *
     * Deferring the depth computation itself does not work: the function scope is gone
     * by then, `lookup` cannot find the parameters, the depth comes out as 0 and the
     * check silently stops working. */
    Vec        refChecks;   /* RefCheck*: reference rules re-run at instantiation */
    Vec        callChecks;  /* CallCheck*: call sites resolved at instantiation */
    Vec       *substParams; /* type arguments substituted while an instance is being
                             * re-checked, NULL when not re-checking */
    Vec       *substArgs;
    int        noHoist;     /* non-zero while a statement prefix cannot be emitted */
    /* How many subexpressions with a side effect have already been checked in the
     * current statement.
     *
     * A `??` whose subject is not pure has to be evaluated into a temporary, and that
     * prefix is emitted before the whole statement, which would move it ahead of earlier
     * side effects in the same statement. Once a side effect has been seen and this `??`
     * still needs a temporary, the program is rejected and the user is asked to split
     * the line. Only calls count, because the relative order of allocations, literals
     * and conversions is not observable. */
    int        stmtFx;
    /* Nodes in the current function body whose arena assignment has to be decided again
     * once every effect summary is closed: `new` sites, and the call sites whose
     * allocation arena is still open. After the body has been checked they move to
     * `FuncDef.arenaSites`, and their levels are fixed once the pass that computes
     * `needsHome` has run. */
    Vec        curArenaSites;
    /* Call sites whose choice of home arena depends on which locals escape.
     *
     * That choice has to wait until the effect summaries of all functions are closed,
     * because the rule "pass my home arena when the callee may move the argument out of
     * this function" depends on the set of escaping locals, and computing that set needs
     * the summary of the callee. The summary is not closed while bodies are still being
     * checked, so with an empty summary the call would look like one that never
     * publishes its argument, the home arena would be taken to be the caller's own
     * frame, and everything the callee stored in that container would be released as
     * soon as the call returned. The call sites are recorded here and recomputed by the
     * final pass. */
    Vec        eSites;      /* EArenaSite* */
    /* The table of "this value was stored at level k" requirements.
     *
     * Every entry is handed over by an escape check that was going to run anyway, so no
     * extra traversal is written to find the store sites. The final pass replays them to
     * a fixed point, and the level of an allocation site ends up as the smallest of all
     * the requirements that reached it, since a smaller level lives longer.
     *
     * The fixed point is needed because one requirement only moves the sites it reaches,
     * and once those live longer, further requirements start to reach further sites.
     * This has the same shape as the call-site arena decision and as the transitive
     * closure of `needsHome`. */
    Vec        lvlFacts;    /* LvlFact* */
    Vec        stores;      /* StoreSite* -- publications, folded over by the level pass */
    /* True while requirements are being replayed. Recording a new requirement during a
     * replay would make the table grow without bound, which once ended with the compiler
     * killed by the memory limit. */
    bool       lvlSolving;
    Vec        narrow;      /* const char*: cnames proved to be non-null */
    /* Names of the locals of the current function that can be moved out of it.
     *
     * This only affects which arena is chosen as the home arena, and the conservative
     * direction is to include too many names: that costs memory, while missing one would
     * let a live pointer dangle. */
    Vec        escapees;    /* const char* */
    int        escapeesFor; /* function the set was computed for, -1 when not yet */

    Vec        narrowMarks; /* size_t: length of `narrow` when each scope was entered,
                             * used to unwind it on scope exit */

    Type *tI32, *tF64, *tBool;  /* cached primitive types */
    StructDef *sliceDef;/* declaration of `slice<T>` from the prelude, which is the view
                         * protocol */
    Type *tSliceU8;     /* type of a string literal: `slice<u8>` */
} Checker;

/* One reference rule deferred to instantiation. */
typedef struct {
    Expr       *val;     /* the value being checked */
    Expr       *target;  /* non-NULL when the value is being stored into this target */
    int         at;      /* destination depth, computed at record time while the scope
                          * still existed */
    int         depth;   /* depth of the references in the value, computed the same way
                          * and assuming `T` may carry a reference */
    bool        borrowed;/* whether the value is borrowed, computed the same way */
    /* A second kind of deferred rule: the zero value.
     *
     * `var local: T` with no initializer looks harmless while `T` is opaque, but the
     * instantiation `T = slice<u8>` produces a null reference, and the language promises
     * that a `ref` is never null. */
    bool        isZero;
    /* A third kind: the size in `new T[n]`.
     *
     * While a body is a template, `T` has no size, so the check can neither pass nor
     * fail; it is recorded and then run for every concrete instantiation. */
    bool        isNewSize;
    Type       *declType;/* declared type the deferred rule applies to */
    int         line;    /* source line to report */
    const char *what;    /* short noun phrase naming the operation, for the diagnostic */
    FuncDef    *func;    /* the generic function this rule belongs to */
} RefCheck;

/* The requirement "this value has to fit into level `at`".
 *
 * It is kept as a fact rather than as a decision because a smaller level means a longer
 * life, so the final level of an allocation site is the smallest of all the
 * requirements that reached it, while which requirements reach it depends on the level
 * the site currently has. Replaying to a fixed point is therefore necessary. Deciding a
 * level as soon as a `new` is seen allows movement in one direction only and makes it
 * impossible to tell afterwards whether a level was decided or computed. */
typedef struct { Expr *val; int at; } LvlFact;

/* One publication: a value observed being handed to a place that may outlive it.
 *
 * Every store, return, and "may keep it" argument call is one of these. Recording is
 * the checking pass's entire involvement with arena levels; deciding which arena each
 * allocation site ends up in is a separate pass that folds over this table. Keeping the
 * two apart is what makes the decision independent of the order the checker walks the
 * tree in, which is the property the previous destructive update could not have.
 *
 * Params are captured rather than derived later, because `at` needs the scope stack and
 * because the binding's origin may legitimately be retargeted later in the body. */
typedef struct {
    Expr       *value;   /* the value being published */
    int         at;      /* arena level of the destination; 0 = beyond this frame */
    int         line;    /* for diagnostics */
} StoreSite;

/* An `==` deferred to instantiation: recorded while an expression is checked and
 * consumed by the pass that runs afterwards.
 *
 * `func` is the function the comparison belongs to, which is how the consumer asks
 * whether that function was ever called. */
typedef struct { Expr *node; StructDef *owner; const char *op; FuncDef *func; } EqCheck;

/* A call site whose resolution is deferred to instantiation.
 *
 * In `fn twice<T>(x: T) -> T { return idOf(x) }` the template is checked with `T`
 * opaque, so the type argument of `idOf` is `T` itself and no correct instance can be
 * built at that moment: the only one available would be `idOf_T`, an instance that
 * still takes a type parameter. This is the same family of problem as `RefCheck` and
 * `EqCheck` and is solved the same way: record it while the template is checked and
 * resolve it again for each instantiation.
 *
 * Skipping the checks that follow is not an option, because the rest of the template
 * body uses the type of `e->func` to check arguments and to compute the return type, so
 * some signature has to be available. A provisional instance with `T` as its argument
 * is therefore created, which is self-consistent because `T` is used as an opaque type,
 * and instantiation later points `e->func` at the concrete instance.
 *
 * The difference from `RefCheck` is that this rewrites `e->func` in the AST, so it has
 * to run in the context of the instance being re-checked. That context already exists,
 * because the re-check pass sets `c.substParams` and `c.substArgs`. */
typedef struct {
    Expr    *node;      /* the EX_CALL / EX_METHOD / EX_ASSOC node */
    FuncDef *tmpl;      /* the template being called: a free function or a method of a
                         * generic type */
    Vec      targs;     /* arguments inferred while the template was checked; they may
                         * contain `T` and have to be passed through `tsub` */
    FuncDef *func;      /* the template function this entry belongs to, which decides
                         * the instances it applies to */
} CallCheck;

/* -------------------------------------------------- cross-file prototypes
 *
 * Generated by running `gcc -aux-info` over the single check.c that these files were
 * split from and taking each signature verbatim, with `static` removed: writing sixty
 * signatures by hand would only introduce typos. */

/* Build an integer literal node; the compiler uses it to fill in a slice bound the user omitted. */
 Expr *intLit (Checker *c, long long int v, int line);
/* Find a field by name in a struct definition, or NULL. */
 FieldDef *findField (StructDef *sd, const char *name);
/* Find a module-level function by name; template instances do not match, templates do. */
 FuncDef *findFunc (Checker *c, const char *name);
/* Find a method declared in the body of a struct or generic type. */
 FuncDef *findMethod (Type *st, const char *name);
/* Find the operator method for `sym`, falling back to `fallback` (`!=` falls back to `==`). */
 FuncDef *findOp (Type *b, const char *sym, const char *fallback);
/* The StructDef behind a TY_STRUCT or TY_GENERIC type, or NULL. */
 StructDef *structOf (Type *t);
/* Declare a binding in the innermost scope and return it; the caller writes the generated C name
 * back into the AST. Unless `shadow` is set, a name already declared in the same scope is an
 * error, because two `var`s of the same name in one scope are almost always a typo. */
 Sym *declare (Checker *c, const char *name, Type *t, _Bool mut, _Bool shadow, int line, int depth);
/* Resolve a name to a binding, innermost scope first and module-level bindings last. */
 Sym *lookup (Checker *c, const char *name);
/* The binding at the root of a place, found by walking through fields, indexes and slices; NULL
 * when the place is not rooted in a binding. */
 Sym *placeRoot (Checker *c, Expr *e);
/* Check an expression in a place position and return its type; a reference is not dereferenced. */
 Type *checkExpr (Checker *c, Expr *e);
/* Check an expression against an expected type; a reference is dereferenced unless the expected
 * type is itself a reference. */
 Type *checkInto (Checker *c, Type *want, Expr *e);
/* Check an expression that may be `e?`, which is legal in statement positions. */
 Type *checkMaybeTry (Checker *c, Expr *e);
/* Check an argument of `print` or `println`, which dereferences a reference because printing an
 * address is never what the user means. */
 Type *checkPrintArg (Checker *c, Expr *e);
/* Check `e?`: the operand must be an `option` or a `result`, and the enclosing function must
 * return the same kind with the same error type. */
 Type *checkTryInner (Checker *c, Expr *e);
/* Check an expression in a value position; a reference here is an error whose message asks for
 * an explicit `*p`. */
 Type *checkValue (Checker *c, Expr *e);
/* The type of the k-th payload of a variant, substituted for the type arguments when the enum is
 * an instance of a generic type. */
 Type *payloadType (TypeTable *tt, Type *et, Variant *v, size_t k);
/* Build the type `slice<elem>` from the declaration in the prelude. */
 Type *sliceOf (Checker *c, Type *elem);
/* Substitute the type arguments of the instance being re-checked into `t`; a type that mentions
 * no parameter comes back unchanged. */
 Type *tsub (Checker *c, Type *t);
/* The element type of a view, or NULL when the type is not a view. */
 Type *viewElemOf (Type *t);
/* Find a variant of an enum by name, or NULL. */
 Variant *findVariant (TypeDef *td, const char *name);
/* Read an expression as a literal integer, accepting a negated literal, so that a negative slice
 * bound is rejected at compile time. */
 _Bool asIntLit (Expr *e, long long int *out);
/* True when a statement always leaves its enclosing block (return, break or continue); it is how
 * the `if p == null { return }` guard is recognised. */
 _Bool blockExits (Stmt *s);
/* Report an error when a value of type `got` cannot be assigned where `want` is expected;
 * returns true when the assignment is rejected. */
 _Bool checkAssignable (Checker *c, Type *want, Type *got, Expr *node, const char *what);
/* Report an error when a value would outlive the place it is being stored into. */
 _Bool checkEscape (Checker *c, Expr *val, int at, int line, const char *what);
/* Run every check a store needs: the depth rule plus "a borrowed value may not be stored where
 * it outlives the call". */
 _Bool checkStoreEscape (Checker *c, Expr *val, Expr *target, int line);

/* Record that `val` is being published at level `at`; decides nothing. The checking
 * pass only records; one pass folds over the records and settles the levels. */
 void recordStore (Checker *c, Expr *val, int at, int line);
/* True when C can compare values of this type directly; `str` is excluded because its `==` would
 * compare pointers. */
 _Bool cmpIsNative (Type *t);
/* True when at least one variant carries a payload; such an enum is a tagged struct in C and
 * cannot be compared with `==`. */
 _Bool enumHasPayload (TypeDef *td);
/* True for `==`, `!=`, `<`, `<=`, `>` and `>=`. */
 _Bool isCmpOp (const char *op);
/* True for `&&` and `||`. */
 _Bool isLogicOp (const char *op);
/* True when an address can be taken of this expression: a binding, a field or a dereference. */
 _Bool isLvalue (Expr *e);
/* True when this generated name has been proved non-null at the current position. */
 _Bool isNarrowed (Checker *c, const char *cname);
/* True for an integer or a float literal. */
 _Bool isNumericLit (Expr *e);
/* True when the expression is a place, that is a chain of bindings, fields, indexes and slices.
 * Slicing a fixed array requires one, because the address of an element is taken. */
 _Bool isPlace (Expr *e);
/* True when a value of this type can be printed; structs, enums, arrays and views are printed by
 * a generated helper that recurses into them. */
 _Bool isPrintable (Type *t);
/* True when the type is the named container from the prelude with exactly `nargs` arguments,
 * such as `option<T>` or `result<T, E>`. */
 _Bool isProtoType (Type *t, const char *name, size_t nargs);
/* True when this place can be written: its root is `var` and no read-only reference or read-only
 * view is crossed on the way. */
 _Bool isWritablePlace (Checker *c, Expr *e);
/* True when a literal's type can adapt to `want`, because a literal takes its type from the
 * target rather than the other way round. */
 _Bool literalFits (Expr *e, Type *want);
/* True when the type mentions a type parameter, directly or inside an aggregate; such a type
 * cannot be decided before instantiation. */
 _Bool mentionsParam (Type *t);
/* Report whether every reference crossed on the way to this place is a `mut ref`, and set
 * `*crossed` when the storage lives behind such a reference. */
 _Bool pathRefsAllMut (Expr *e, _Bool *crossed);
/* True when the path to this place crosses a read-only reference, including the type of the
 * place itself. */
 _Bool pathHasReadonlyRef (Expr *e);
/* Report an error and return true when a nullable reference is dereferenced without a preceding
 * proof that it is not null. */
 _Bool rejectNullableDeref (Checker *c, Type *t, Expr *node, const char *what);
/* True when evaluating this expression twice is observably the same; `??` needs that, because
 * its subject appears twice in the generated C. */
 _Bool repeatablePure (Expr *e);
/* Report an error when writing to this place is not allowed; returns true when the write is
 * rejected. */
 _Bool requireMutable (Checker *c, Expr *e, int line, const char *what);
/* True when the type can carry a reference, directly or inside an array, a struct, or a payload
 * of any variant. */
 _Bool typeContainsRef (TypeTable *tt, Type *t);
/* True when the type has no zero value, which holds for a non-nullable reference and for any
 * aggregate that contains one. A payload-carrying enum looks only at its first variant, because
 * its zero value is tag 0 with the payload zeroed. */
 _Bool typeLacksZeroValue (TypeTable *tt, Type *t);
/* True when values of this type can be compared with `op`: the comparison is native, or the type
 * is an array of a comparable element, or it defines the operator method. */
 _Bool typeSupportsEq (Type *t, const char *op);
/* The name used in generated C for a source name, renamed when it would collide with a binding
 * of the same name in the same scope. */
 const char *cNameFor (Checker *c, const char *name);
/* The name a condition proves non-null, or NULL; `*whenTrue` says whether the proof holds on the
 * taken branch. */
 const char *narrowTarget (Checker *c, Expr *cond, _Bool *whenTrue);
/* Render a type as source text, for a diagnostic. */
 const char *typeStr (Checker *c, Type *t);
/* Check a whole module: the declarations, the function bodies, the deferred rules, and the arena
 * decisions that wait for closed effect summaries. */
/* True when this local of the current function can be moved out of it. */
 bool isEscapeeName (Checker *, const char *);
/* Compute the effect summary of a function, following the callees, and report whether the
 * summary is complete; only a complete summary may be used to skip a check. */
 bool computeEffectsTransitive (Checker *, FuncDef *);
/* The field-table entry for a field, created on demand when `create` is set, or NULL when there
 * is none. */
 int *fieldDepthEntry (Checker *, Sym *, const char *, bool);
/* Record that a value of depth `d2` was stored into a field, updating the field entry and the
 * effective depth of the root. */
 void noteFieldDepthWrite (Checker *, Sym *, const char *, int);
/* The name of the binding at the root of a place, or NULL when the place is not rooted in one. */
 const char *placeRootName (Expr *);
/* The depth of the home arena to pass at this call site, derived from the arguments and the
 * parameters of the callee. */
  int callHomeDepth (Checker *c, Vec *args, Vec *params, Expr *callNode);
/* Depth to record for a value that is being stored, taking the references and the structure of
 * the value into account. */
int valDepthForStore (Checker *, Expr *);
/* Depth of the references a value can carry, read off its type and its shape. */
 int exprRefDepth (Checker *c, Expr *e);
/* Depth of the storage a place expression denotes, as opposed to the depth of what a reference
 * stored there points at. */
 int placeDepth (Checker *c, Expr *e);
/* Arena level at which the storage of a place lives, which is a different question from
 * `placeDepth`. */
 int storeLayer (Checker *c, Expr *e);
/* True when the root of the value is one of the function's parameters, so that the call site can
 * decide its lifetime. */
 _Bool valTracesToParam (Checker *c, FuncDef *f, Expr *val);
/* Raise every allocation inside a value to the level it is being stored at, instead of rejecting
 * the store. */
 _Bool promoteInto (Checker *c, Expr *val, int at);
/* Remember where a binding's value came from, flattened to the root of the chain. */
 void noteOrigin (Checker *c, Sym *sy, Expr *val);
/* Give a bare `{}` or a bare constructor the type the surrounding context expects. */
 void adoptContextType (Expr *e, Type *want);
/* Check the `ref` and `mut ref` arguments of a call: each has to outlive the call, and the arena
 * argument is decided here. */
 void checkCallRefArgs (Checker *, FuncDef *, Vec *, Vec *, int, int, const char *);
/* Check the signature of an operator method at the place where it is defined. */
 void checkOperatorSig (Checker *c, FuncDef *f);
/* Check one statement and apply the depth and narrowing consequences it has. */
 void checkStmt (Checker *c, Stmt *s);
/* Report a compile error at a line, with an optional note suggesting a fix. */
 void ckError (Checker *c, int line, const char *note, const char *fmt, ...);
/* Report a warning at a line. A warning is recorded but does not stop the compilation. */
 void ckWarn  (Checker *c, int line, const char *note, const char *fmt, ...);
/* Rewrite a bare constructor into the qualified variant construction. */
 void desugarBareCtor (Checker *c, Expr *e, Type *want);
/* Report an error unless the type is `bool`; the language has no implicit truthiness. */
 void expectBool (Checker *c, Type *t, Expr *node);

/* Mark the call as allocating into the caller's home arena when the value it produces escapes
 * the current frame. */
 void markCallHomeIfEscaping (Checker *c, Expr *v, int at);
/* Resolve the home depth of a call site into the arena argument the code generator emits. */
 void setCallArenaArg (Checker *c, Expr *e);
/* Report an error when a name of another module is used without its module qualifier. */
 void requireQualified (Checker *c, const char *what, const char *whatMod, bool qualified, int line);
/* The type parameters visible in this function: those of the function itself, or those of the
 * generic type that owns it. */
 Vec *funcTParams (FuncDef *f);
/* The instance of a generic function for one set of type arguments, created on first use. */
 FuncDef *funcInstance (Checker *c, FuncDef *tmpl, Vec *targs, int line);
/* Fill in the type arguments of a generic call by unifying the parameter types with the argument
 * types; false means they cannot be inferred and have to be written out. */
 _Bool unifyTParams (TypeTable *tt, Vec *tp, Vec *targs, Type *want, Type *got);
/* Record every non-null fact a condition proves, including both sides of an `&&`. */
 void narrowFactsOf (Checker *c, Expr *cond);
/* Leave the innermost scope: drop its bindings and unwind the narrowing facts it established. */
 void popScope (Checker *c);
/* Record that a binding has been proved non-null at the current position. */
 void pushNarrow (Checker *c, const char *cname);
/* Enter a new lexical scope; the bindings declared in it disappear at the matching `popScope`. */
 void pushScope (Checker *c);
/* Defer the size check of `new T[n]` to instantiation, for a body whose type parameter has no
 * size yet. */
 void recordNewSizeCheck (Checker *c, Type *t, int line);
/* Defer the zero-value check of a declaration to instantiation. */
 void recordZeroCheck (Checker *c, Type *t, int line, const char *name);
/* Drop the non-null proof of a binding, and of the paths that begin with it, which an assignment
 * invalidates. */
 void unNarrow (Checker *c, const char *cname);

#endif /* EXTC_CHECK_INTERNAL_H */
