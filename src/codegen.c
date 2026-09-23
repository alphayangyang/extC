/* extC -> C code generation.
 *
 * This pass does no type inference of its own: the type checker has already
 * written its results back into the AST (Expr.type, Expr.func, Expr.field,
 * Stmt.type), so all that is left here is translation.
 *
 * The generated C is deliberately dull C: no macro tricks, no optimization
 * stunts. `#line` maps gcc diagnostics back to line numbers in the .extc file.
 */

#include "codegen.h"

#include <stdarg.h>
#include <stdlib.h>   /* exit: exceeding the closure limit must fail loudly */
#include <stdio.h>
#include <string.h>

#include "types.h"

/* ---------------------------------------------------------------- type mapping */

typedef struct { const char *extc; const char *c; } TypeMap;

static const TypeMap C_TYPES[] = {
    { "i8",  "int8_t"  }, { "i16", "int16_t" }, { "i32", "int32_t" }, { "i64", "int64_t" },
    { "u8",  "uint8_t" }, { "u16", "uint16_t" }, { "u32", "uint32_t" }, { "u64", "uint64_t" },
    { "f32", "float"   }, { "f64", "double"  },
    { "bool", "bool"   },
    { NULL, NULL }
};

typedef struct { const char *extc; const char *fmt; const char *cast; } PrintFmt;

/* Format-free printing: the compiler picks the format from the static type,
 * so a user never writes "%d". */
static const PrintFmt PRINT_FMT[] = {
    { "i8",  "%d",   "(int)"       }, { "i16", "%d",   "(int)"       },
    { "i32", "%d",   "(int)"       }, { "i64", "%lld", "(long long)" },
    { "u8",  "%u",   "(unsigned)"  }, { "u16", "%u",   "(unsigned)"  },
    { "u32", "%u",   "(unsigned)"  }, { "u64", "%llu", "(unsigned long long)" },
    { "f32", "%g",   "(double)"    }, { "f64", "%g",   "(double)"    },
    { NULL, NULL, NULL }
};

/* ---------------------------------------------------------------- state */

typedef struct {
    Arena      *arena;
    Ctx        *ctx;
    Buf        *out;
    const char *path;
    bool        lineMap;

    TypeTable  *tt;
    Vec         structs;        /* StructDef* - non-generic ones */
    Vec         funcs;          /* FuncDef* - methods of non-generic structs + free functions */
    int         indent;

    /* Monomorphization context: while the code of one generic instance is
     * generated, T stands for the corresponding type argument. */
    Vec        *substParams;    /* const char* */
    Vec        *substArgs;      /* Type* */
    const char *ownerPrefix;    /* instance name decorating method names, e.g. "Pair_i32_u8" */

    /* Slice helpers; see the comment on SliceHelper. Their need is discovered
     * only while generating, so bodies go into `body` first and the output is
     * assembled at the end as prototypes -> helpers -> bodies. */
    Vec         helpers;        /* SliceHelper */
    Buf         body;

    /* Used by `?` expansion: the return type of the current function (a
     * failure value is built from it) and the temporary counter (one per `?`,
     * unique within the function). */
    Type       *retType;
    int         tmpSeq;
    /* Statement prefix: when the subject of `??` must not be evaluated twice
     * it is first computed into a temporary, and those lines must be emitted
     * *before* the statement that uses them (C has no statement expressions).
     * `?` already did this by hand in genTryHead; here it is a general
     * mechanism. `prefixBlk` says whether we are currently inside a statement:
     * outside one (a file-scope initializer, say) nothing may be emitted. */
    Buf         prefix;
    int         prefixBlk;
    /* ---- Arenas are refined per block ----
     * `__extc_a[k]` is the arena of the block at level k; k is the block depth,
     * known at compile time, the same lexical depth the checker uses when it
     * assigns reference depths.
     * `loopLevel[]` is the block level of each enclosing loop body, so that
     * `break` and `continue` know down to which level to release. */
    int         blkLevel;
    bool        noArena;   /* This function puts nothing into its own block arena,
                            * so neither `extc_arena __extc_a[N]` nor the release
                            * calls are emitted. The checker decides this
                            * (`f->mayUseArena`); it saves compile time. */
    int         loopLevel[64];
    int         loopLen;
    /* Is this function self-recursive, that is, does it reach itself, possibly
     * through other calls? If so the prologue increments `__extc_rec_depth`,
     * every exit decrements it, and the recursion guard turns a stack overflow
     * into a clean error. The answer comes from the call graph computed by
     * funcCallsItself: that costs compile time, while the two runtime
     * statements land only in functions that really need a guard. */
    bool        isRecursive;
    /* The `@overwrite` sites of this function (`Stmt*`, in order of
     * appearance). The prologue emits one storage cell per site - a pointer
     * plus a lazily allocated flag - because such a declaration reuses a single
     * block of storage across executions of the site.
     * A site table plus a lookup is used instead of writing an index into the
     * AST: one body is visited once per generic instance, so an index stored in
     * the AST would be overwritten by the next instance. See `owIndex`. */
    Vec         owSites;
    /* Call sites in this function whose callee needs cells (`Expr*`, in order
     * of discovery). Each of them passes one `void *` cell per callee site, so
     * the callee reuses the caller's storage when it is itself @overwrite. */
    Vec         owCalls;
    bool        owLocal;   /* Does the current function keep its @overwrite cells in
                            * its own frame? See FuncDef.owLocal. */
    Vec         insts;          /* Type* - generic instances, deduplicated by C name (see below) */

    /* ---- Descriptor tables are generated on demand ----
     * Printing, and later structural `==`, needs them, but only a type that is
     * really used needs one. The set is found by running to a fixed point:
     * `descRef` both yields `&X_desc` and registers X, and the roots are the
     * argument types of the `extc_print(&x, &x_desc)` calls that genPrint
     * emits. This keeps compile time proportional to what the program uses.
     *
     * Which types are needed is known only after the bodies are generated, so
     * the descriptor region is written after the prototypes and before the
     * bodies - the same layout trick the slice helpers use. */
    Vec         descs;          /* Type* - types needing a table, deduplicated by C name */
    Buf         desc;           /* descriptor region, prepended to the bodies */
    Buf         rt;             /* table types + shared scalars; needed by print or eq */
    Buf         rtPrint;        /* `extc_print` - only if a structured type is printed */
    Buf         rtEq;           /* `extc_eq` - only if an array or slice is compared */
    /* Structural `==` also goes through a descriptor table; this vector lists
     * the types for which `extc_eq` is needed. The closure propagates inwards:
     * a container that needs equality needs it for its elements too, so a
     * struct element gets a generated one-line adapter. */
    Vec         eqNeed;         /* Type* - deduplicated by C name */
    /* Whether the print runtime is emitted must not be decided from
     * `descs.len`: `slice<u8>`, that is a string literal, uses the shared
     * `extc_desc_text` and never enters `descs`, so looking only at `descs`
     * loses `extc_print` and `extc_desc_text`. That bug really happened - the
     * most ordinary line there is, `println("x = ", n)`, failed to compile.
     * genPrint raises this flag directly instead. */
    bool        needRuntime;
} CG;

/* One slice helper: the bounds check and the view construction for `a[lo..hi]`
 * collected into a function. A function rather than an expression so that lo
 * and hi are each evaluated exactly once: written inline, `hi - lo` would
 * mention both bounds twice. */
typedef struct {
    const char *name;
    char       *text;
} SliceHelper;

/* Append one formatted line to the generated output at the current indent.
 *
 * Params:
 *   g   - generator state; supplies the output buffer, the indent and the arena
 *   fmt - printf-style format
 *
 * Notes:
 *   - The formatted line is truncated at 4096 bytes; every caller stays far
 *     below that.
 */
static void cgLine(CG *g, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);

    for (int i = 0; i < g->indent; i++) bufPuts(g->out, "    ");
    bufPuts(g->out, tmp);
    bufPutc(g->out, '\n');
}

/* Append one formatted line to the pending statement prefix.
 *
 * The text is buffered rather than written to the output, because a prefix must
 * appear before the statement that needs it; see flushPrefix.
 */

static void pfLine(CG *g, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    for (int i = 0; i < g->indent; i++) bufPuts(&g->prefix, "    ");
    bufPuts(&g->prefix, tmp);
    bufPutc(&g->prefix, '\n');
}

/* Emit the buffered statement prefix and clear it.
 *
 * Must be called before the first line of the statement the prefix belongs to:
 * those lines compute the temporaries the statement then reads, and that
 * ordering is the whole point of the prefix.
 */

static void flushPrefix(CG *g) {
    if (g->prefix.len == 0) return;
    bufPuts(g->out, bufCstr(&g->prefix));
    bufInit(&g->prefix, g->arena);
}

/* Substitute the TY_PARAM types of the current instance context.
 *
 * Returns:
 *   The substituted type, or t itself when no instance context is active.
 */
static Type *subst(CG *g, Type *t) {
    if (!g->substParams || !g->substArgs) return t;
    return ttSubstitute(g->tt, t, g->substParams, g->substArgs);
}

/* Enter the monomorphization context of one generic instance.
 *
 * Params:
 *   inst - the instance type; a generic enum instance has no `sdef`, its owner
 *          is `edef`, so both cases are handled here
 */
static void substEnter(CG *g, Type *inst) {
    /* A generic enum instance has no `sdef`; its owner is `edef`. */
    g->substParams = inst->sdef ? &inst->sdef->typeParams : &inst->edef->typeParams;
    g->substArgs   = &inst->targs;
    g->ownerPrefix = inst->name;
}

/* Leave the substitution context entered by substEnter. */
static void substLeave(CG *g) {
    g->substParams = NULL;
    g->substArgs   = NULL;
    g->ownerPrefix = NULL;
}


/* Return the C spelling of an extC type.
 *
 * Returns:
 *   A string such as "int32_t" or "pair_i32_u8 *"; a composite spelling is built
 *   in the generator's arena. An error or unresolved type yields "int", which
 *   only matters for a program that has already failed to check.
 */
static const char *cType(CG *g, Type *t) {
    if (!t) return "void";

    /* Substitute once over the whole type: both a ref and a generic instance
     * may have T nested inside. Handling a bare TY_PARAM is not enough, since
     * the outermost part of `ref Pair<A, B>` is the ref. */
    t = subst(g, t);

    if (t->kind == TY_PARAM) return "int";  /* no context to substitute in; cannot happen */

    switch (t->kind) {
        case TY_REF:   return arenaPrintf(g->arena, "%s *", cType(g, t->inner));
        case TY_VOID:  return "void";
        case TY_STRUCT: return t->name;
        case TY_GENERIC: return t->name;    /* already a decorated name */
        case TY_ARRAY:  return t->name;     /* likewise: array_15_i32 */
        case TY_ENUM:  return t->name;      /* a plain enum typedef in C */
        case TY_ERROR: return "int";
        case TY_BUILTIN:
            for (size_t i = 0; C_TYPES[i].extc; i++)
                if (strcmp(C_TYPES[i].extc, t->name) == 0) return C_TYPES[i].c;
            return "int";
        case TY_UNRESOLVED:
            return "int";       /* cannot appear once the checker has run */
        case TY_PARAM:
            return "int";       /* already handled above; this only silences -Wswitch */
    }
    return "int";
}

/* ---------------------------------------------------------------- expressions */

static bool isProtoType(Type *t, const char *name, size_t nargs);
static const char *genExpr(CG *g, Expr *e);
static const char *genSlice(CG *g, Expr *e);
static const char *descRef(CG *g, Type *t);
static bool printArgIsPlace(const Expr *e);
static bool cgIsMain(const FuncDef *f);

static bool isPlaceExpr(const Expr *e);

    
/* Does this enum have a variant that carries a payload?
 *
 * Returns:
 *   true when some variant has fields, in which case the C form is a struct
 *   holding a tag and a union rather than a plain enum.
 */
static bool enumHasPayload(TypeDef *td) {
    if (!td) return false;
    for (size_t i = 0; i < td->variants.len; i++)
        if ((*(Variant **)vecAt(&td->variants, i))->types.len > 0) return true;
    return false;
}

/* Map an extC symbol name to a fragment usable inside a C identifier.
 *
 * An operator is spelled `==` in extC but cannot be spelled that way in C, so
 * the two operators that end up in method names are renamed here.
 *
 * A user name can also collide with a C keyword; writing tests hit this with
 * `fn double(...)`. In extC `double` is not a keyword (the float types are
 * `f32` and `f64`), so it is a perfectly legal user name, yet the generated
 * `int32_t double(int32_t);` does not compile - and the error points into the
 * generated C, where the user cannot see their own source. A `__c` suffix
 * avoids that without changing a single character of the extC source. The
 * keyword set lives in `cIdentIsKeyword` (base.c) and is shared with the
 * checker.
 *
 * Params:
 *   name - the extC symbol or identifier
 *
 * Returns:
 *   The C identifier fragment; freshly built names live in the generator's
 *   arena, so the result outlives the call.
 */
static const char *cSymName(CG *g, const char *name) {
    static const struct { const char *extc, *c; } MAP[] = {
        { "==", "eq" }, { "!=", "ne" },
        { NULL, NULL }
    };
    for (size_t i = 0; MAP[i].extc; i++)
        if (strcmp(MAP[i].extc, name) == 0) return MAP[i].c;
    if (cIdentIsKeyword(name)) return arenaPrintf(g->arena, "%s__c", name);
    return name;
}

/* Name of a function in the generated C.
 *
 * A method name is prefixed with its owner so that `Point_eq` and `Board_eq`
 * cannot collide; in extC they were already two distinct names.
 *
 * Returns:
 *   The C name, allocated in the generator's arena.
 */
static const char *cFuncName(CG *g, FuncDef *f) {
    /* An instance of a generic free function carries its own C name (eq2_i32). */
    if (f->instName) return f->instName;
    if (g->ownerPrefix)
        return arenaPrintf(g->arena, "%s_%s", g->ownerPrefix, cSymName(g, f->name));
    if (f->owner) return arenaPrintf(g->arena, "%s_%s", f->owner->name, cSymName(g, f->name));
    return cSymName(g, f->name);
}

/* Name of a method in the generated C, at a call site.
 *
 * The receiver picks the prefix: a generic instance uses its own name
 * (`Pair_i32_u8_getFirst`), anything else uses the owner of the method.
 *
 * Params:
 *   recvType - static type of the receiver
 *
 * Returns:
 *   The C name, allocated in the generator's arena.
 *
 * Notes:
 *   - cFuncName must not be used here: it prefixes the instance currently being
 *     generated, whereas the callee may belong to a different type - calling
 *     Point.== from inside Wrapper<Point> is exactly that case.
 */
static const char *cMethodName(CG *g, Type *recvType, FuncDef *f) {
    Type *rb = ttBase(subst(g, recvType));
    if (rb && rb->kind == TY_GENERIC)
        return arenaPrintf(g->arena, "%s_%s", rb->name, cSymName(g, f->name));
    /* cFuncName cannot be used here: it prefixes the instance currently being
     * generated, but the callee may belong to another type (calling Point.==
     * from inside Wrapper<Point>). */
    if (f->owner) return arenaPrintf(g->arena, "%s_%s", f->owner->name, cSymName(g, f->name));
    return cSymName(g, f->name);
}

/* Is this type a slice instance?
 *
 * This is the only thing the compiler knows about slices, and it is an output
 * convention rather than knowledge of the container: a view of bytes prints as
 * text, which is basic output, not a property of the container. The definition,
 * fields and methods of the container all live in stdlib/prelude.extc.
 *
 * Returns:
 *   true for `slice<T>` with exactly one type argument.
 */
static bool isView(Type *t) {
    return t && t->kind == TY_GENERIC && t->sdef
        && strcmp(t->sdef->name, "slice") == 0 && t->targs.len == 1;
}

/* Is this a view of bytes, that is `slice<u8>`?
 *
 * Returns:
 *   true when the element type is `u8`; `println` then prints the view as text
 *   instead of as a list of numbers.
 */
static bool isByteView(Type *t) {
    return isView(t) && ttIs(*(Type **)vecAt(&t->targs, 0), "u8");
}

/* Emit the index primitive of one view type: `<C name>_index(v, i, file, line)`.
 *
 * The view is taken by value, so a subscript expression evaluates its operands
 * exactly once and there is no second code path for values and references. The
 * primitive bounds-checks and traps with the extC position when the index is
 * out of range. The `get`, `==` and `find` functions of the prelude all reach
 * it through `self[i]`, which is what keeps pointer arithmetic and hand-written
 * bounds checks out of the library entirely.
 *
 * Notes:
 *   - The emitted function returns a pointer to the element and the call site
 *     dereferences it. Returning the element by value would break two things: a
 *     `ref` parameter needs an lvalue, so `slice<struct>::==` - written as
 *     `self[i] != other[i]` with `self: ref T` - would not compile at all (a
 *     bug that really happened), and an assignment such as `s[i] = x` or
 *     `s[i].field = x` needs an lvalue as well. Returning a pointer covers
 *     both, and a view has reference semantics anyway: that is the `ref` in
 *     `data: ref T`.
 */
static void genViewIndexer(CG *g, Type *inst) {
    Type *elem = *(Type **)vecAt(&inst->targs, 0);
    substEnter(g, inst);
    cgLine(g, "static inline %s *%s_index(%s v, int64_t i, const char *file, int line) {",
           cType(g, elem), inst->name, inst->name);
    g->indent++;
    cgLine(g, "if (i < 0 || i >= v.len) extc_trap(file, line, i, v.len);");
    cgLine(g, "return &v.data[i];");
    g->indent--;
    cgLine(g, "}");
    cgLine(g, "");
    substLeave(g);
}

/* ---------------------------------------------------------------- type descriptors
 *
 * Printing derives no code any more: every type yields one `static const
 * ExtcDesc`, which is pure data, and the whole program shares a single
 * `extc_print`, the runtime part emitted at the top of the generated file.
 *
 * The abandoned scheme, kept here only as a warning, generated a whole printf
 * sequence per struct: a synthetic stress program grew 2028 such functions,
 * 22.9% of the generated C, and printed none of them.
 *
 * `descRef` maps an extC type to the address of the constant describing it:
 *   - scalar, ref, slice<u8>: one of the few descriptors shared by the whole
 *     program, which cost no extra space
 *   - struct, generic instance, array, enum: its own `<C name>_desc`
 *
 * Notes:
 *   - Descriptors reference one another; `struct s { xs: slice<s> }` is a cycle.
 *     That is why the region opens with a `static const ExtcDesc X_desc;`
 *     tentative definition for every descriptor, which makes the definition
 *     order irrelevant.
 */
/* Register that this type needs a descriptor of its own.
 *
 * Deduplication is by C name, because a mutable and a read-only view are the
 * same C struct and share one descriptor. Scalars, `ref` and `slice<u8>` use
 * the shared descriptors and are not registered.
 */
static void needDesc(CG *g, Type *t) {
    if (!t || !t->name) return;
    if (t->kind == TY_BUILTIN || t->kind == TY_REF || isByteView(t)) return;
    for (size_t i = 0; i < g->descs.len; i++)
        if (strcmp((*(Type **)vecAt(&g->descs, i))->name, t->name) == 0) return;
    *(Type **)vecPush(&g->descs) = t;
}

/* Register that this type needs `extc_eq`.
 *
 * Deduplication is by C name. Comparing a container compares its elements, so
 * the elements are registered as well and the closure is closed in
 * emitDescRegion.
 */
static void needEq(CG *g, Type *t) {
    if (!t || !t->name) return;
    for (size_t i = 0; i < g->eqNeed.len; i++)
        if (strcmp((*(Type **)vecAt(&g->eqNeed, i))->name, t->name) == 0) return;
    *(Type **)vecPush(&g->eqNeed) = t;
}

/* Return the C expression denoting the descriptor of a type.
 *
 * Every type this descriptor refers to is registered on the way out, which is
 * what makes the demand set a closure.
 *
 * Returns:
 *   A string such as `&extc_desc_i32` or `&list_i32_desc`, allocated in the
 *   generator's arena.
 */
static const char *descRef(CG *g, Type *t) {
    if (!t) return "&extc_desc_i32";
    needDesc(g, t);                       /* registering the dependency closes the set */
    if (t->kind == TY_BUILTIN) return arenaPrintf(g->arena, "&extc_desc_%s", t->name);
    if (t->kind == TY_REF)     return "&extc_desc_ref";
    if (isByteView(t))         return "&extc_desc_text";
    /* Everything else has one of its own; a generic instance uses its own C
     * name, as in list_i32_desc. */
    return arenaPrintf(g->arena, "&%s_desc", t->name);
}

/* Emit the descriptor of a struct: field names plus `offsetof`.
 *
 * `offsetof` instead of a hand-computed layout means a forgotten field or a
 * wrong layout fails to compile rather than printing garbage.
 *
 * Params:
 *   cname - C name of the struct, which is also the prefix of its descriptor
 *   disp  - name to display; a generic instance displays its template name
 *           (`varArray { ... }`), as it always did
 *   eqFn  - C name of the equality adapter, or NULL when the type has none
 */
static void genStructDesc(CG *g, const char *cname, const char *disp, StructDef *sd,
                          const char *eqFn) {
    size_t n = sd->fields.len;
    if (n) {
        cgLine(g, "static const ExtcField %s_fields[] = {", cname);
        g->indent++;
        for (size_t i = 0; i < n; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            cgLine(g, "{ \"%s\", offsetof(%s, %s), %s },", fd->name, cname, fd->name,
                   descRef(g, subst(g, fd->type)));
        }
        g->indent--;
        cgLine(g, "};");
    }
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_STRUCT, \"%s\", sizeof(%s), %zu, %s, NULL, %s };",
           cname, disp, cname, n, n ? arenaPrintf(g->arena, "%s_fields", cname) : "NULL",
           eqFn ? eqFn : "NULL");
}

/* Emit the descriptor of an array: an element count and an element descriptor.
 *
 * An array has no field names, so it prints as `[1, 2, 3]`.
 */
static void genArrayDesc(CG *g, Type *arr) {
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_ARRAY, \"%s\", sizeof(%s), %lld, NULL, %s };",
           arr->name, arr->name, cType(g, arr->inner), (long long)arr->asize,
           descRef(g, arr->inner));
}

/* Emit the descriptor of a non-byte view, which prints as `[a, b]`.
 *
 * A byte view never reaches here: it shares `extc_desc_text`.
 */
static void genViewDesc(CG *g, Type *v) {
    Type *elem = subst(g, *(Type **)vecAt(&v->targs, 0));
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_SLICE, \"%s\", sizeof(%s), 0, NULL, %s };",
           v->name, v->name, cType(g, elem), descRef(g, elem));
}

/* Emit the descriptor of an enum: its variant names, and `<type name>` for a
 * value outside the table.
 *
 * Only the variant name is printed, never the payload, and the text is
 * byte-for-byte what the previous per-type name function produced.
 */
static void genEnumDesc(CG *g, const char *cname, const char *disp, TypeDef *td) {
    size_t n = td->variants.len;
    if (n) {
        Buf b;
        bufInit(&b, g->arena);
        for (size_t j = 0; j < n; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            if (j) bufPuts(&b, ", ");
            bufPrintf(&b, "\"%s\"", v->name);
        }
        cgLine(g, "static const char *const %s_variants[] = { %s };", cname, bufCstr(&b));
    }
    cgLine(g, "static const ExtcDesc %s_desc = { EXTC_D_ENUM, \"%s\", sizeof(%s), %zu, %s, NULL };",
           cname, disp, cname, n, n ? arenaPrintf(g->arena, "%s_variants", cname) : "NULL");
}

/* ------------------------------------------------------- descriptors on demand
 *
 * Only a type that is really printed gets a descriptor. In the synthetic stress
 * program with N=1000 no struct was printed at all, and the descriptor region
 * came out completely empty.
 *
 * The roots are the argument types of the `extc_print(&x, &x_desc)` calls that
 * genPrint emits, registered through `descRef`; the closure follows the
 * references between descriptors - struct fields, array elements, slice
 * elements - until it reaches a fixed point.
 *
 * Two things this has to get right:
 *   1. Order. Which descriptors are needed is known only after the function
 *      bodies are generated, because `genPrint` runs inside them, while C wants
 *      definitions before uses. The region is therefore written after the
 *      prototypes and before the bodies, and spliced back in at the end, the
 *      same way the slice helpers are.
 *   2. Cycles. `struct s { xs: slice<s> }` gives `s_desc -> slice_s_desc ->
 *      s_desc`. Every descriptor is therefore forward-declared first, with a C
 *      tentative definition, which makes the order of the definitions
 *      irrelevant. Collecting that set needs a dry pass first; it has no side
 *      effects, because the descriptor emitters write only to `g->out` and to
 *      the arena.
 */
static bool eqNeeded(CG *g, Type *t);
static void closeEqNeeds(CG *g);
static FuncDef *findOpMethod(Type *t, const char *sym, const char *fallback);
static void genEqAdapter(CG *g, Type *t, FuncDef *m);

/* Emit the definition of every registered descriptor.
 *
 * Notes:
 *   - The loop condition rereads `len` on purpose: `descRef` appends new
 *     dependencies while the definitions are written.
 */
static void emitDescDefs(CG *g) {
    /* The condition rereads `len` on purpose: `descRef` appends dependencies
     * while the definitions are written. */
    for (size_t i = 0; i < g->descs.len; i++) {
        Type *t = *(Type **)vecAt(&g->descs, i);
        /* Only a struct that `extc_eq` really recurses into fills this slot; others stay NULL. */
        const char *eqFn = eqNeeded(g, t) ? arenaPrintf(g->arena, "%s_eqD", t->name)
                                          : NULL;
        if (t->kind == TY_STRUCT && t->sdef) {
            genStructDesc(g, t->name, t->name, t->sdef, eqFn);
        } else if (t->kind == TY_ENUM && t->edef) {
            genEnumDesc(g, t->name, t->name, t->edef);
        } else if (t->kind == TY_ARRAY) {
            genArrayDesc(g, t);
        } else if (t->kind == TY_GENERIC && t->sdef) {
            substEnter(g, t);              /* T in the fields stands for the type arguments */
            if (isView(t)) genViewDesc(g, t);
            else           genStructDesc(g, t->name, t->sdef->name, t->sdef, eqFn);
            substLeave(g);
        } else {
            /* A missing case must fail loudly: silently emitting no descriptor
             * would leave the generated C referring to a symbol that does not
             * exist. */
            ctxError(g->ctx, 0, 1, NULL,
                     "internal: no descriptor generator for this type");
        }
    }
}

/* Does this type need `eq` generated or filled in? The lookup is by C name. */
static bool eqNeeded(CG *g, Type *t) {
    if (!t || !t->name) return false;
    for (size_t i = 0; i < g->eqNeed.len; i++)
        if (strcmp((*(Type **)vecAt(&g->eqNeed, i))->name, t->name) == 0) return true;
    return false;
}

/* Close the `eq` set inwards: a container that needs equality needs it for its
 * elements too.
 *
 * A struct ends the walk, because its equality is delegated through `d->eq` to
 * the `fn ==` the user wrote.
 */
static void closeEqNeeds(CG *g) {
    for (size_t i = 0; i < g->eqNeed.len; i++) {   /* reread: entries are appended */
        Type *t = *(Type **)vecAt(&g->eqNeed, i);
        if (t->kind == TY_ARRAY) { needEq(g, t->inner); continue; }
        if (t->kind == TY_GENERIC && isView(t) && !isByteView(t))
            needEq(g, *(Type **)vecAt(&t->targs, 0));
    }
}

/* Write the descriptor region into `g->desc`.
 *
 * The region holds the equality adapters and the descriptor definitions and is
 * spliced in between the prototypes and the function bodies.
 */
static void emitDescRegion(CG *g) {
    /* Nothing structured was printed and no array was compared, so the whole
     * region - descriptors, printing and comparison - is unnecessary. */
    if (g->descs.len == 0 && g->eqNeed.len == 0) return;
    closeEqNeeds(g);
    Buf *saved = g->out;
    g->out = &g->desc;
    emitDescDefs(g);                       /* dry pass: collect dependencies, drop text */
    bufInit(&g->desc, g->arena);

    /* ---- Equality adapters for structural `==` ----
     * They must precede the descriptors, which take their address. Only a
     * struct that `extc_eq` really recurses into needs one: that struct's own
     * `fn ==` is arbitrary user code, so the runtime can only delegate to it.
     * Every other kind is recursed into by `extc_eq` itself. */
    for (size_t i = 0; i < g->eqNeed.len; i++) {
        Type *t = *(Type **)vecAt(&g->eqNeed, i);
        if (t->kind != TY_STRUCT && t->kind != TY_GENERIC) continue;
        if (t->kind == TY_GENERIC && isView(t)) continue;   /* extc_eq recurses into views */
        FuncDef *m = findOpMethod(t, "==", NULL);
        if (!m) {
            /* Unreachable: the checker permits a comparison only when the
             * element type is comparable. If it is reached anyway, fail loudly
             * rather than emit C that does not compile. */
            ctxError(g->ctx, 0, 1, NULL,
                     "internal: structural equality needs a `fn ==` on this type");
            continue;
        }
        genEqAdapter(g, t, m);
    }
    if (g->eqNeed.len) cgLine(g, "");

    cgLine(g, "/* ---- 类型描述表（**按需**：只出真会被用到的那些）---- */");
    for (size_t i = 0; i < g->descs.len; i++)   /* all forward-declared: order does not matter */
        cgLine(g, "static const ExtcDesc %s_desc;", (*(Type **)vecAt(&g->descs, i))->name);
    cgLine(g, "");
    emitDescDefs(g);                       /* real pass: emit the definitions */
    cgLine(g, "");
    g->out = saved;
}

/* Per-type print functions are gone: `_debug`, `_writeText` and `_name` used to
 * be derived here, one printf sequence per type, which a synthetic stress
 * program turned into 2028 functions and 22.9% of its generated C without ever
 * printing anything. What remains is descriptor data plus one `extc_print`.
 */

/* Can C compare this type natively with `==`?
 *
 * Returns:
 *   true for builtin numeric and bool types and for enums; every other type
 *   needs `extc_eq`.
 */
static bool nativeCmp(Type *t) {
    if (!t) return false;
    if (t->kind == TY_ENUM) return true;
    return t->kind == TY_BUILTIN;
}

/* Find the user-defined operator method of a type.
 *
 * Params:
 *   sym      - symbol to look for, for example "=="
 *   fallback - symbol to accept when `sym` is absent, for example "==" when
 *              looking for "!=" (whose result is then negated); NULL to accept
 *              `sym` only
 *
 * Returns:
 *   The method, or NULL when neither symbol is defined for this type.
 */
static FuncDef *findOpMethod(Type *t, const char *sym, const char *fallback) {
    Type *b = ttBase(t);
    if (!b || (b->kind != TY_STRUCT && b->kind != TY_GENERIC) || !b->sdef) return NULL;
    StructDef *sd = b->sdef;
    FuncDef *hit = NULL;
    for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
        if (strcmp(m->name, sym) == 0) return m;
        if (fallback && strcmp(m->name, fallback) == 0) hit = m;
    }
    return hit;
}

/* `typeHasEq`, which recursively decided whether an element type could be
 * compared, is gone: code generation derives no `_eq` for arrays any more, so
 * nothing on this side asks the question. Comparability is decided by the
 * checker, which reports ``[2]p` cannot be compared: its element type `p` does
 * not define `==` `` when it is violated. The rule used to exist in two places
 * that had to be kept in sync by hand - a real bug came from exactly that - and
 * with one copy left, that class of bug is gone by construction.
 */

/* ------------------------------------------------------------- structural `==`
 *
 * Structural comparison used to derive one `<T>_eq` function per array type -
 * 200 array types meant 2450 lines, 32% of the generated C in one stress
 * program. Now every type goes through the generic `extc_eq` plus its
 * descriptor.
 *
 * `extc_eq(a, b, desc)` takes addresses, so each operand must be a place; an
 * operand that is not one is first stored in a temporary through the statement
 * prefix, for the same reason `println` does it.
 */
/* Return the address of an operand to pass to `extc_eq`.
 *
 * Params:
 *   x - the operand expression
 *   t - its static type, needed when a temporary must be declared
 *
 * Returns:
 *   A C expression of type pointer to t: `&(code)` for a place, the address of
 *   a fresh temporary `__extc_q<N>` otherwise.
 *
 * Notes:
 *   - The temporary goes through the statement prefix, so it is computed before
 *     the enclosing statement runs.
 */
static const char *eqOperand(CG *g, Expr *x, Type *t) {
    const char *code = genExpr(g, x);
    if (printArgIsPlace(x)) return arenaPrintf(g->arena, "&(%s)", code);
    const char *tmp = arenaPrintf(g->arena, "__extc_q%d", g->tmpSeq++);
    pfLine(g, "%s %s = %s;", cType(g, t), tmp, code);
    return arenaPrintf(g->arena, "&%s", tmp);
}

/* Emit a structural equality test as one `extc_eq` call.
 *
 * Params:
 *   e   - the binary `==` or `!=` expression
 *   arr - static type of the operands, that is the type being compared
 *
 * Returns:
 *   The call expression; the caller negates it for `!=`.
 *
 * Notes:
 *   - Both sides are evaluated exactly once and in source order, including the
 *     temporaries that land in the statement prefix.
 */
static const char *genEqCall(CG *g, Expr *e, Type *arr) {
    /* Both sides are evaluated once each, in source order, temporaries included. */
    const char *l = eqOperand(g, e->u.bin.left, arr);
    const char *r = eqOperand(g, e->u.bin.right, arr);
    return arenaPrintf(g->arena, "extc_eq(%s, %s, %s)", l, r, descRef(g, arr));
}

/* Emit the equality adapter of a struct.
 *
 * It connects the user-written `fn ==` to the `bool (*)(const void *, const
 * void *)` shape that `extc_eq` expects. Only a struct needs an adapter,
 * because its comparison is arbitrary user code that can only be delegated to.
 *
 * Params:
 *   t - the struct type
 *   m - its `==` method; its first two parameters are the operands
 *
 * Notes:
 *   - Taking the address of an operand must follow the method signature exactly:
 *     each of the first two parameters is passed by address when it is declared
 *     `ref`, and by value otherwise.
 */
static void genEqAdapter(CG *g, Type *t, FuncDef *m) {
    const char *tn = cType(g, t);
    cgLine(g, "static bool %s_eqD(const void *a, const void *b) {", t->name);
    g->indent++;
    Param *p0 = *(Param **)vecAt(&m->params, 0);
    Param *p1 = *(Param **)vecAt(&m->params, 1);
    const char *a = p0->type->kind == TY_REF ? arenaPrintf(g->arena, "(%s *)a", tn)
                                             : arenaPrintf(g->arena, "*(const %s *)a", tn);
    const char *b = p1->type->kind == TY_REF ? arenaPrintf(g->arena, "(%s *)b", tn)
                                             : arenaPrintf(g->arena, "*(const %s *)b", tn);
    cgLine(g, "return %s(%s, %s);", cMethodName(g, t, m), a, b);
    g->indent--;
    cgLine(g, "}");
}

/* The recursive element-comparison expression used to be built here; it has no
 * callers any more. Array equality goes through genEqCall and the generic
 * `extc_eq`, and struct or slice equality is emitted where the user's method is
 * called, in genBin below.
 */

/* Emit a binary operation.
 *
 * `==` and `!=` were already resolved by the checker into a call of an equality
 * method, or reported there when the type has none. A comparison inside a
 * generic is deferred to this point, where the instance context makes it
 * resolvable.
 *
 * Returns:
 *   A C expression; the caller is responsible for the surrounding syntax.
 */
static const char *genBin(CG *g, Expr *e) {
    const char *op = e->u.bin.op;

    /* Arrays: the compiler provides `==` and `!=`, because an array has no
     * `sdef` and therefore no method to find.
     *
     * Since the descriptor table exists, no `_eq` function is derived per array
     * type any more; the comparison becomes the generic
     * `extc_eq(&a, &b, &arr_desc)`, one descriptor plus one recursive
     * implementation. The semantics are unchanged: recurse element by element,
     * and call the user's `fn ==` for a struct element.
     *
     * The condition must not also require `!e->needEq`: a comparison inside a
     * generic is resolved at instantiation, and after substitution the element
     * type can well turn out to be an array - comparing two rows of a
     * `slice<[6]i32>` is that case. That guard used to be here, so such a
     * comparison fell through to the method lookup below and reported
     * "`array_6_i32` needs to define `!=`". It was a false error. */
    if (!e->func) {
        Type *lt = ttBase(subst(g, e->u.bin.left->type));
        if (lt && lt->kind == TY_ARRAY &&
            (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0)) {
            needEq(g, lt);                 /* register: needs to be comparable via extc_eq */
            const char *call = genEqCall(g, e, lt);
            return strcmp(op, "!=") == 0 ? arenaPrintf(g->arena, "(!%s)", call) : call;
        }
    }

    if (e->func || e->needEq) {
        Type *lt = ttBase(subst(g, e->u.bin.left->type));
        FuncDef *m = e->func
                     ? e->func
                     : findOpMethod(lt, op, strcmp(op, "!=") == 0 ? "==" : NULL);

        if (!m) {
            /* builtin numeric, bool and enum: C compares them natively */
            if (nativeCmp(lt))
                return arenaPrintf(g->arena, "(%s %s %s)",
                                   genExpr(g, e->u.bin.left), op,
                                   genExpr(g, e->u.bin.right));

            ctxError(g->ctx, e->line, 1,
                     "`==` inside a generic is checked at instantiation, not on the template -- the price of having no traits. "
                     "Add a `fn ==` to that type.",
                     "`%s` needs to define `%s`", cType(g, lt), op);
            return "0";
        }

        Param *p0 = *(Param **)vecAt(&m->params, 0);
        Param *p1 = *(Param **)vecAt(&m->params, 1);
        const char *l = genExpr(g, e->u.bin.left);
        const char *r = genExpr(g, e->u.bin.right);
        if (p0->type->kind == TY_REF) l = arenaPrintf(g->arena, "&(%s)", l);
        if (p1->type->kind == TY_REF) r = arenaPrintf(g->arena, "&(%s)", r);

        const char *call = arenaPrintf(g->arena, "%s(%s, %s)",
                                       cMethodName(g, e->u.bin.left->type, m), l, r);
        return strcmp(op, "!=") == 0 ? arenaPrintf(g->arena, "(!%s)", call) : call;
    }

    /* Division by zero and an over-wide shift are undefined behaviour in C, so
     * they trap with a source position instead of passing silently. Each
     * operand is evaluated exactly once, which is why they go through a helper
     * rather than a comma expression such as `(check(r), l op r)` that would
     * evaluate one operand twice. */
    {
        Type *lt   = ttBase(subst(g, e->u.bin.left->type));
        bool  sint = lt && lt->kind == TY_BUILTIN && lt->name && lt->name[0] == 'i';
        bool  uint = lt && lt->kind == TY_BUILTIN && lt->name && lt->name[0] == 'u';

        if ((sint || uint) && (strcmp(op, "/") == 0 || strcmp(op, "%") == 0)) {
            const char *fn = strcmp(op, "/") == 0 ? (sint ? "extc_divI" : "extc_divU")
                                                  : (sint ? "extc_modI" : "extc_modU");
            const char *ity = sint ? "int64_t" : "uint64_t";
            return arenaPrintf(g->arena, "(%s)%s((%s)(%s), (%s)(%s), \"%s\", %d)",
                               cType(g, lt), fn, ity, genExpr(g, e->u.bin.left),
                               ity, genExpr(g, e->u.bin.right), g->path, e->line);
        }
        if ((sint || uint) && (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0)) {
            int bits = 0;
            for (const char *q = lt->name + 1; *q >= '0' && *q <= '9'; q++) bits = bits * 10 + (*q - '0');
            if (bits > 0)
                return arenaPrintf(g->arena, "(%s %s extc_shiftCount((int64_t)(%s), %d, \"%s\", %d))",
                                   genExpr(g, e->u.bin.left), op,
                                   genExpr(g, e->u.bin.right), bits, g->path, e->line);
        }
    }

    return arenaPrintf(g->arena, "(%s %s %s)",
                       genExpr(g, e->u.bin.left), op, genExpr(g, e->u.bin.right));
}

/* C expression for the zero value of a type.
 *
 * Zero initialization cannot always be `{0}`: `str` is non-nullable, and `{0}`
 * turns a `const char *` into NULL, where `printf("%s", NULL)` is undefined
 * behaviour (glibc happens to print "(null)" and hides the mistake). A struct
 * that contains a `str` therefore spells its zero value out field by field.
 *
 * Returns:
 *   The C initializer expression.
 */
static const char *zeroValue(CG *g, Type *t);

/* Does this type contain a `ref`, directly or nested inside a struct?
 *
 * Such a type must not be zero-initialized with `{0}`, which would build a null
 * reference. This is the defensive branch that lets the C compiler report the
 * mistake instead.
 *
 * Returns:
 *   true when a `ref` appears anywhere inside the type.
 */
static bool needsExplicitZero(Type *t) {
    if (!t) return false;
    if (t->kind == TY_REF) return true;
    if (t->kind != TY_STRUCT || !t->sdef) return false;
    for (size_t i = 0; i < t->sdef->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&t->sdef->fields, i);
        if (needsExplicitZero(fd->type)) return true;
    }
    return false;
}

/* Enter the substitution context of the fields of a generic instance.
 *
 * The fields must be substituted with that instance's own type arguments, never
 * with whatever context the caller happens to leave behind: `subst` would then
 * return the type unchanged, and zero-value generation would recurse into
 * itself until the stack overflows.
 *
 * Params:
 *   sd    - the generic struct definition
 *   targs - its type arguments in this instance
 *   saveP - out: the previous `substParams`
 *   saveA - out: the previous `substArgs`
 *   saveN - out: the previous `ownerPrefix`
 */
static void substEnterInst(CG *g, StructDef *sd, Vec *targs, Vec **saveP, Vec **saveA, const char **saveN) {
    *saveP = g->substParams;
    *saveA = g->substArgs;
    *saveN = g->ownerPrefix;
    g->substParams = &sd->typeParams;
    g->substArgs   = targs;
}

/* Enter the substitution context of a generic free function instance.
 *
 * Without it a `T` in the body of the instance - the `T` of `a == b`, say -
 * stays TY_PARAM forever, which either reports a false error or generates wrong
 * C.
 *
 * Params:
 *   f     - the instance; without a template this is a no-op
 *   saveP - out: the previous `substParams`
 *   saveA - out: the previous `substArgs`
 */
static void substEnterFunc(CG *g, FuncDef *f, Vec **saveP, Vec **saveA) {
    *saveP = g->substParams;
    *saveA = g->substArgs;
    if (f && f->tmpl) {
        g->substParams = &f->tmpl->typeParams;
        g->substArgs   = &f->targs;
    }
}
/* Restore the substitution context saved by substEnterFunc. */
static void substLeaveFunc(CG *g, Vec *saveP, Vec *saveA) {
    g->substParams = saveP;
    g->substArgs   = saveA;
}

/* Restore the substitution context saved by substEnterInst. */
static void substLeaveInst(CG *g, Vec *saveP, Vec *saveA, const char *saveN) {
    g->substParams = saveP;
    g->substArgs   = saveA;
    g->ownerPrefix = saveN;
}

/* Emit the zero value of a type.
 *
 * This is the recursive implementation; the declaration above records why a
 * `{0}` is not always good enough.
 */
static const char *zeroValue(CG *g, Type *t) {
    if (!t) return "0";
    /* The zero value of an enum with payloads is tag 0 with a cleared payload,
     * which is exactly what `(shape){0}` means: C clears the tag and the whole
     * union, and the tag decides which member is the meaningful one. A `ref`
     * inside the payload of tag 0 is reported by the checker as a type that
     * cannot be zero-initialized. */
    if (t->kind == TY_ENUM)
        return enumHasPayload(t->edef) ? arenaPrintf(g->arena, "(%s){0}", t->name) : "0";
    if (t->kind == TY_ARRAY) return arenaPrintf(g->arena, "(%s){0}", t->name);

    if (t->kind == TY_PARAM) {
        Type *a = subst(g, t);
        if (a == t) return "0";     /* no context: cannot happen, but never loop forever */
        return zeroValue(g, a);
    }

    /* A generic instance substitutes its own type arguments into the field
     * types and then spells the zero value out field by field. */
    if (t->kind == TY_GENERIC && t->sdef) {
        StructDef *sd = t->sdef;
        if (sd->fields.len == 0) return arenaPrintf(g->arena, "(%s){0}", t->name);

        Vec *sp, *sa;
        const char *sn;
        substEnterInst(g, sd, &t->targs, &sp, &sa, &sn);

        Buf b;
        bufInit(&b, g->arena);
        bufPrintf(&b, "(%s){ ", t->name);
        for (size_t i = 0; i < sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            if (i) bufPuts(&b, ", ");
            bufPrintf(&b, ".%s = %s", fd->name, zeroValue(g, fd->type));
        }
        bufPuts(&b, " }");

        substLeaveInst(g, sp, sa, sn);
        return bufCstr(&b);
    }

    if (t->kind == TY_STRUCT && t->sdef) {
        StructDef *sd = t->sdef;
        if (sd->fields.len == 0 || !needsExplicitZero(t))
            return arenaPrintf(g->arena, "(%s){0}", sd->name);

        Buf b;
        bufInit(&b, g->arena);
        bufPrintf(&b, "(%s){ ", sd->name);
        for (size_t i = 0; i < sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
            if (i) bufPuts(&b, ", ");
            bufPrintf(&b, ".%s = %s", fd->name, zeroValue(g, fd->type));
        }
        bufPuts(&b, " }");
        return bufCstr(&b);
    }

    if (ttIs(t, "bool")) return "false";

    /* Defensive: a `ref` has no zero value. The checker guarantees that this
     * line is unreachable - a struct containing a `ref` cannot be
     * zero-initialized and such a field cannot be omitted - but should a later
     * path slip through, an identifier that does not exist is emitted and the C
     * compiler reports it, rather than a null reference being inserted quietly.
     */
    /* The zero value of a `?ref T` is `null`, because a nullable reference does
     * have one. Treating every reference as valueless used to make the
     * zero-initialization of a struct with a `?ref` field emit that
     * non-existent identifier; a real bug, found when `var l: list` failed to
     * compile. */
    if (t->kind == TY_REF) return t->nullable ? "((void *)0)"
                                              : "__extc_reference_has_no_zero_value__";

    return "0";
}

/* Return the zero value of a type as an initializer; see zeroValue. */
static const char *zeroInit(CG *g, Type *t) {
    return zeroValue(g, t);
}

/* Is the generated C expression of a `println` argument an lvalue, that is, can
 * its address be taken?
 *
 * This is not the question isPlaceExpr answers, and confusing the two breaks
 * code:
 *   - the slice expression `s[0..5]` is a place in extC, but its C form is
 *     `slice_u8_slice(s, 0, 5, "...", 54)`, a function call, and `&` on a call
 *     is illegal. That really happened: examples/slices.extc and
 *     euler-sieve.extc stopped compiling.
 *   - the enum variant `color.red` looks like a field access in extC, but the
 *     checker has already replaced it with a constructor, which is not a place,
 *     so this correctly answers false.
 *
 * Returns:
 *   false whenever the answer is uncertain. A wrong false merely costs one
 *   extra copy - what passing the argument by value to the old `_debug` did -
 *   while a wrong true produces C that does not compile.
 */
static bool printArgIsPlace(const Expr *e) {
    switch (e->kind) {
    case EX_IDENT: return true;                                  /* root of `x` / `p.f` */
    case EX_FIELD: return printArgIsPlace(e->u.field.obj);
    case EX_INDEX: return printArgIsPlace(e->u.index.obj);
    default:       return false;
    }
}

/* Emit the expression that implements one `print` call.
 *
 * A structured argument - enum, byte view, struct, generic instance or array -
 * is printed through its descriptor as `extc_print(&x, &x_desc)`. That removes
 * per-type print code entirely and leaves one descriptor per type instead. The
 * call needs an address, so an argument that is not a place is first stored in
 * a temporary through the statement prefix: `println("literal")` and
 * `println(makePoint())` both take that path. A C compound literal cannot be
 * used instead, because C refuses to initialize an aggregate from an expression
 * of the same type and gcc answers "incompatible types".
 *
 * Params:
 *   args    - the argument expressions of the call
 *   newline - append a trailing newline as well
 *
 * Returns:
 *   The generated expression, allocated in the generator's arena.
 *
 * Notes:
 *   - Sets `needRuntime` as a side effect; that flag is what pulls the print
 *     runtime into the output.
 */
static const char *genPrint(CG *g, Vec *args, bool newline) {
    Buf b;
    bufInit(&b, g->arena);
    bufPutc(&b, '(');

    for (size_t i = 0; i < args->len; i++) {
        Expr *a = *(Expr **)vecAt(args, i);
        Type *bt = ttBase(subst(g, a->type));
        const char *code = genExpr(g, a);

        if (i) bufPuts(&b, ", ");
        if (!bt) { bufPuts(&b, "0"); continue; }

        /* Structured types - enum, byte view, struct, generic instance, array -
         * all print through their descriptor: `extc_print(&x, &x_desc)`. That
         * is what removes per-type print code and leaves only per-type data.
         *
         * The call takes an address, so the argument must be a place. When it is
         * not - `println("literal")` or `println(makePoint())` - the value is
         * first stored in a temporary and that line is emitted before the
         * enclosing statement through the statement prefix. A compound literal
         * `(T){expr}` was tried and does not work: C will not initialize an
         * aggregate from an expression of the same type, and gcc reports
         * "incompatible types". */
        if (bt->kind == TY_ENUM || isByteView(bt) || bt->kind == TY_STRUCT ||
            bt->kind == TY_GENERIC || bt->kind == TY_ARRAY) {
            g->needRuntime = true;              /* the print runtime must be emitted too */
            if (printArgIsPlace(a)) {
                bufPrintf(&b, "extc_print(&(%s), %s)", code, descRef(g, bt));
            } else {
                const char *tmp = arenaPrintf(g->arena, "__extc_p%d", g->tmpSeq++);
                pfLine(g, "%s %s = %s;", cType(g, bt), tmp, code);
                bufPrintf(&b, "extc_print(&%s, %s)", tmp, descRef(g, bt));
            }
            continue;
        }
        if (bt->kind != TY_BUILTIN) { bufPuts(&b, "0"); continue; }

        if (strcmp(bt->name, "bool") == 0) {
            bufPrintf(&b, "printf(\"%%s\", (%s) ? \"true\" : \"false\")", code);
            continue;
        }
        const PrintFmt *pf = NULL;
        for (size_t k = 0; PRINT_FMT[k].extc; k++)
            if (strcmp(PRINT_FMT[k].extc, bt->name) == 0) { pf = &PRINT_FMT[k]; break; }
        if (!pf) { bufPuts(&b, "0"); continue; }
        bufPrintf(&b, "printf(\"%s\", %s(%s))", pf->fmt, pf->cast, code);
    }

    if (newline) {
        if (args->len) bufPuts(&b, ", ");
        bufPuts(&b, "printf(\"\\n\")");
    } else if (args->len == 0) {
        bufPuts(&b, "printf(\"\")");
    }

    bufPutc(&b, ')');
    return bufCstr(&b);
}

/* Is this expression a place in extC, meaning its address can be taken in C?
 *
 * The same predicate the checker uses, repeated here for a different reason: the
 * generated `&(f())` would not be legal C.
 *
 * Returns:
 *   true for an identifier, for a field of a place, for an index into a place
 *   and for a slice of a place.
 */
static bool isPlaceExpr(const Expr *e) {
    switch (e->kind) {
    case EX_IDENT: return true;
    case EX_FIELD: return isPlaceExpr(e->u.field.obj);
    case EX_INDEX: return isPlaceExpr(e->u.index.obj);
    case EX_SLICE: return isPlaceExpr(e->u.slice.obj);
    default:       return false;
    }
}

static const char *homeArg(CG *g, int marked);   /* defined below */
static void owPassCells(CG *g, Buf *b, Expr *e, size_t nargs, bool hasHome);
static bool f_owLocal(CG *g, Stmt *s);

/* Emit a method call as a plain function call on its receiver.
 *
 * `a.f(x)` means exactly `f(a, x)`; the only work is making the receiver match
 * the first parameter, which may be a `ref` or a value.
 *
 * Returns:
 *   The call expression, or "0" when the checker left no resolved method.
 */
static const char *genMethodCall(CG *g, Expr *e) {
    FuncDef *f = e->func;
    if (!f) return "0";

    Type *recvT = e->u.method.recv->type;
    Param *p0 = *(Param **)vecAt(&f->params, 0);
    const char *recvC = genExpr(g, e->u.method.recv);

    /* Address or dereference the receiver as the first parameter demands: that
     * is the whole of `a.f(x)` == `f(a, x)`. */
    bool wantRef = p0->type && p0->type->kind == TY_REF;
    bool haveRef = recvT && recvT->kind == TY_REF;
    if (wantRef && !haveRef) {
        /* A temporary receiver cannot be addressed directly, since `&(f())`
         * is not legal C. It is materialized first, as a compound literal of a
         * one-element array: `(T[]){ f() }` has type `T *`, because the array
         * decays to a pointer.
         *
         * Do not write `&((T){ f() })` instead: `(T){ x }` is not a copy in C,
         * it initializes the first member from x, which produces nonsense
         * errors such as `_Bool has = <option_i64>`. Array initialization does
         * go element by element, so `{ f() }` really is "initialize element 0
         * with one T". The temporary lives until the end of the statement,
         * which is exactly long enough for this call - the same trick Rust
         * plays for `next().unwrap()`.
         *
         * Only a receiver that is not a place takes this path; doing it for a
         * place would turn a write into the element into a write into a copy. */
        if (isPlaceExpr(e->u.method.recv)) {
            recvC = arenaPrintf(g->arena, "&(%s)", recvC);
        } else {
            recvC = arenaPrintf(g->arena, "(%s[]){ %s }",
                                cType(g, subst(g, recvT)), recvC);
        }
    } else if (!wantRef && haveRef) {
        recvC = arenaPrintf(g->arena, "*(%s)", recvC);
    }

    const char *fname = cMethodName(g, recvT, f);

    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "%s(%s", fname, recvC);
    for (size_t i = 0; i < e->u.method.args.len; i++)
        bufPrintf(&b, ", %s", genExpr(g, *(Expr **)vecAt(&e->u.method.args, i)));
    /* A method passes the home arena too; the receiver counts as the
     * shallowest mutable reference argument. */
    if (f->needsHome) bufPrintf(&b, ", %s", homeArg(g, e->arenaArg));
    /* A method passes the @overwrite cells as well; the receiver counts as the
     * first argument, and the comma handling follows that. */
    owPassCells(g, &b, e, e->u.method.args.len + 1, f->needsHome);
    bufPutc(&b, ')');
    return bufCstr(&b);
}

/* Find the value written for one field of a struct literal.
 *
 * Returns:
 *   The initializer expression, or NULL when the literal omits the field; the
 *   caller then uses the field's zero value.
 */
static Expr *litValueFor(Expr *lit, const char *fname) {
    for (size_t i = 0; i < lit->u.lit.inits.len; i++) {
        FieldInit *fi = *(FieldInit **)vecAt(&lit->u.lit.inits, i);
        if (strcmp(fi->name, fname) == 0) return fi->value;
    }
    return NULL;
}

/* Emit a struct literal as a C compound literal.
 *
 * Every field is spelled out, an omitted one as its zero value: leaving it to
 * C's implicit zero-fill would turn an omitted `str` field into NULL.
 *
 * Returns:
 *   The compound literal, allocated in the generator's arena; `(int){0}` when
 *   the type is not a struct at all.
 */
static const char *genStructLit(CG *g, Expr *e) {
    /* Substitute over the whole type first: inside a generic instance the
     * literal still records the template type (`result<T,E>`), whose type
     * arguments are parameters. Taking that type as it is sends the zero-value
     * path a bare `T`, which degrades to `0` and ends up as `.value = 0` in the
     * generated C. This really happened with `result<unit, E>::failure`. */
    Type *t = subst(g, e->type);
    StructDef *sd = (t && (t->kind == TY_STRUCT || t->kind == TY_GENERIC)) ? t->sdef : NULL;
    if (!sd) return "(int){0}";

    const char *cname = cType(g, t);       /* an instance gets its decorated name */
    if (sd->fields.len == 0) return arenaPrintf(g->arena, "(%s){0}", cname);

    /* A literal of a generic instance substitutes its own type arguments into
     * the field types as well. */
    Vec *sp = g->substParams, *sa = g->substArgs;
    const char *sn = g->ownerPrefix;
    if (t->kind == TY_GENERIC) {
        g->substParams = &sd->typeParams;
        g->substArgs   = &t->targs;
    }

    /* Every field is written out, an omitted one as its zero value: letting C
     * zero-fill it would turn an omitted `str` field into NULL. */
    Buf b;
    bufInit(&b, g->arena);
    bufPrintf(&b, "(%s){", cname);
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        Type *ft = subst(g, fd->type);
        Expr *v = litValueFor(e, fd->name);
        if (i) bufPuts(&b, ", ");
        bufPrintf(&b, ".%s = %s", fd->name,
                  v ? genExpr(g, v) : zeroValue(g, ft));
    }
    bufPuts(&b, "}");

    g->substParams = sp;
    g->substArgs   = sa;
    g->ownerPrefix = sn;
    return bufCstr(&b);
}

static const char *arenaRefAt(CG *g, int level);
static void arenaDriftCheck(CG *g, Expr *e, const char *what);   /* the arena is picked per level */

/* Emit the C expression for one expression node.
 *
 * Every caller goes through genExpr, which wraps this function to add the
 * automatic dereference; this one only dispatches on the node kind.
 *
 * Returns:
 *   A C expression, or "0" for a node that must not reach a value position.
 */
static const char *genExprInner(CG *g, Expr *e) {
    switch (e->kind) {
        case EX_INT:   return arenaPrintf(g->arena, "%lld", e->u.ival);
        case EX_FLOAT: return arenaPrintf(g->arena, "%g", e->u.fval);
        case EX_BOOL:  return e->u.bval ? "true" : "false";
        case EX_STR:
            /* `"abc"` becomes a byte view of the literal in read-only memory.
             * The length comes from `sizeof("...") - 1`, so C handles escapes
             * and nothing has to parse them here. */
            return arenaPrintf(g->arena,
                "(%s){ .data = (uint8_t *)\"%s\", .len = sizeof(\"%s\") - 1 }",
                cType(g, e->type), e->u.str.text, e->u.str.text);
        case EX_IDENT: return e->u.ident.cname ? e->u.ident.cname : e->u.ident.name;

        /* `*p`: an explicit dereference is a C dereference; whether the target
         * may be written is the checker's business. */
        case EX_DEREF:
            return arenaPrintf(g->arena, "(*(%s))", genExpr(g, e->u.deref.operand));

        case EX_BIN: return genBin(g, e);

        case EX_UN:
            return arenaPrintf(g->arena, "(%s%s)", e->u.un.op, genExpr(g, e->u.un.operand));

        case EX_FIELD: {
            const char *base = genExpr(g, e->u.field.obj);
            Type *ot = e->u.field.obj->type;
            const char *arrow = (ot && ot->kind == TY_REF) ? "->" : ".";
            return arenaPrintf(g->arena, "%s%s%s", base, arrow, e->u.field.name);
        }

        case EX_CALL: {
            if (e->u.call.callee->kind != EX_IDENT) return "0";
            const char *name = e->u.call.callee->u.ident.name;
            if (strcmp(name, "print") == 0)   return genPrint(g, &e->u.call.args, false);
            if (strcmp(name, "println") == 0) return genPrint(g, &e->u.call.args, true);
            /* `flush()` becomes `fflush(NULL)`; <stdio.h> is already included
             * by the runtime. */
            if (strcmp(name, "flush") == 0) return "(fflush((void *)0), 0)";
            if (!e->func) return "0";

            /* Only `cSymName` is used, without the owner prefix: a call site
             * names the callee itself, whereas `ownerPrefix` is the instance
             * currently being generated. cFuncName has to tell the two apart,
             * as its comment explains. This also renames a name that collides
             * with a C keyword (`fn double`). */
            name = (e->func && e->func->instName) ? e->func->instName : cSymName(g, name);
            Buf b;
            bufInit(&b, g->arena);
            bufPuts(&b, name);
            bufPutc(&b, '(');
            for (size_t i = 0; i < e->u.call.args.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.call.args, i)));
            }
            /* The callee needs a home arena, so mine is passed down; the
             * current block's arena would be tighter. */
            if (e->func->needsHome) {
                if (e->u.call.args.len) bufPuts(&b, ", ");
                bufPuts(&b, homeArg(g, e->arenaArg));
            }
            /* The callee needs @overwrite cells, so cells of my own frame are
             * passed down. */
            owPassCells(g, &b, e, e->u.call.args.len, e->func->needsHome);
            bufPutc(&b, ')');
            return bufCstr(&b);
        }

        case EX_INDEX: {
            Type *ot = e->u.index.obj->type;
            Type *ob = ttBase(subst(g, ot));
            const char *obj = genExpr(g, e->u.index.obj);
            const char *idx = genExpr(g, e->u.index.index);

            /* Arrays: the length is a compile-time constant, so obj appears
             * exactly once and cannot be evaluated twice. */
            if (ob && ob->kind == TY_ARRAY) {
                /* With a `ref [N]T` base, `obj` is a pointer in C and must be
                 * dereferenced one level first. Otherwise `p[..]` or `p[i]` on
                 * a `ref [8]u8` produced `p.data[..]`, and gcc answered
                 * "'p' is a pointer; did you mean to use '->'?" */
                if (ot && ot->kind == TY_REF)
                    obj = arenaPrintf(g->arena, "(*%s)", obj);
                return arenaPrintf(g->arena,
                    "%s.data[extc_checkedIndex((int64_t)(%s), %lld, \"%s\", %d)]",
                    obj, idx, (long long)ob->asize, g->path, e->line);
            }
            if (!isView(ob)) return "0";
            /* The primitive takes the view by value, so a reference is
             * dereferenced. */
            if (ot && ot->kind == TY_REF) obj = arenaPrintf(g->arena, "*(%s)", obj);

            /* The primitive returns a pointer and the dereference is an
             * lvalue: it can be read, addressed for a `ref` parameter, and
             * assigned to. */
            return arenaPrintf(g->arena, "(*%s_index(%s, (int64_t)(%s), \"%s\", %d))",
                               ob->name, obj, idx, g->path, e->line);
        }

        case EX_ARRAYLIT: {
            Type *t = e->type;
            if (!t || t->kind != TY_ARRAY) return "0";
            Buf b;
            bufInit(&b, g->arena);
            bufPrintf(&b, "(%s){ .data = {", cType(g, t));
            for (size_t i = 0; i < e->u.arraylit.elems.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.arraylit.elems, i)));
            }
            /* A trailing `...` needs nothing extra: a C initializer zero-fills
             * the remaining elements by itself. */
            bufPuts(&b, "} }");
            return bufCstr(&b);
        }

        case EX_SLICE: return genSlice(g, e);

        case EX_TRY:
            /* `?` must not reach here: it expands at statement level, where
             * genStmt handles it directly. Arriving here means the checker's
             * position rule has a hole; report it rather than silently generate
             * wrong C. */
            ctxError(g->ctx, e->line, 1, NULL,
                     "internal: `?` reached expression codegen (position check missed it)");
            return "0";

        case EX_ASSOC: {
            /* An associated function decorates its C name with the instance
             * name (`option_i64_some`), which is the same decoration rule
             * methods follow, so cMethodName is reused directly. */
            Buf b;
            bufInit(&b, g->arena);
            bufPuts(&b, cMethodName(g, e->assocOwner, e->func));
            bufPutc(&b, '(');
            for (size_t i = 0; i < e->u.assoc.args.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPuts(&b, genExpr(g, *(Expr **)vecAt(&e->u.assoc.args, i)));
            }
            /* An associated function such as `Type::make()` allocates, so the
             * home arena argument is appended. */
            if (e->func->needsHome) {
                if (e->u.assoc.args.len) bufPuts(&b, ", ");
                bufPuts(&b, homeArg(g, e->arenaArg));
            }
            owPassCells(g, &b, e, e->u.assoc.args.len, e->func->needsHome);   /* @overwrite cells */
            bufPutc(&b, ')');
            return bufCstr(&b);
        }

        /* Numeric conversion. Without `convCheck` the checker has proven the
         * value fits and a plain cast is enough; with it the value is checked
         * against a range and traps with a source position. */
        case EX_CONV: {
            Type *t = subst(g, e->u.conv.type);          /* target type, from the checker */
            const char *x = genExpr(g, e->u.conv.operand);
            if (!e->convCheck)
                return arenaPrintf(g->arena, "((%s)(%s))", cType(g, t), x);

            /* A checked conversion: look the range up in a table and go
             * through one of the `static inline` helpers. */
            const char *tn = t->name;
            bool isF = ttIsFloat(subst(g, e->u.conv.operand->type));
            if (isF) {
                int64_t lo = 0, hi = 0;
                if (strcmp(tn,"i8")==0)  { lo = -128; hi = 127; }
                else if (strcmp(tn,"i16")==0) { lo = -32768; hi = 32767; }
                else if (strcmp(tn,"i32")==0) { lo = -2147483648LL; hi = 2147483647LL; }
                else if (strcmp(tn,"i64")==0) { lo = 1; hi = 0; }   /* macro form, see below */
                else if (strcmp(tn,"u8")==0)  { lo = 0; hi = 255; }
                else if (strcmp(tn,"u16")==0) { lo = 0; hi = 65535; }
                else if (strcmp(tn,"u32")==0) { lo = 0; hi = 4294967295LL; }
                else { lo = 0; hi = INT64_MAX; }        /* u64 from a float: checked as i64 */
                return arenaPrintf(g->arena,
                    "((%s)extc_convFloat((double)(%s), %lldLL, %lldLL, \"%s\", %d))",
                    cType(g, t), x, (long long)lo, (long long)hi, g->path, e->line);
            }
            bool sign = (tn[0] == 'i');
            if (sign) {
                int64_t lo = 0, hi = 0;
                if (strcmp(tn,"i8")==0)  { lo = -128; hi = 127; }
                else if (strcmp(tn,"i16")==0) { lo = -32768; hi = 32767; }
                else if (strcmp(tn,"i32")==0) { lo = -2147483648LL; hi = 2147483647LL; }
                else
                    return arenaPrintf(g->arena,
                        "((%s)extc_narrowI((int64_t)(%s), INT64_MIN, INT64_MAX, \"%s\", %d))",
                        cType(g, t), x, g->path, e->line);
                return arenaPrintf(g->arena,
                    "((%s)extc_narrowI((int64_t)(%s), %lldLL, %lldLL, \"%s\", %d))",
                    cType(g, t), x, (long long)lo, (long long)hi, g->path, e->line);
            }
            unsigned long long hi = 0;
            if (strcmp(tn,"u8")==0)  hi = 255ULL;
            else if (strcmp(tn,"u16")==0) hi = 65535ULL;
            else if (strcmp(tn,"u32")==0) hi = 4294967295ULL;
            else
                return arenaPrintf(g->arena,
                    "((%s)extc_narrowU((uint64_t)(%s), UINT64_MAX, \"%s\", %d))",
                    cType(g, t), x, g->path, e->line);
            return arenaPrintf(g->arena,
                "((%s)extc_narrowU((uint64_t)(%s), %lluULL, \"%s\", %d))",
                cType(g, t), x, hi, g->path, e->line);
        }

        /* `new T`, `new [N]T` and `new T[n]`: allocate into a block arena and
         * zero the storage. The runtime allocation zeroes on its own, so extC
         * has exactly one rule about fresh memory. */
        case EX_NEW: {
            Type *w = subst(g, e->u.new_.type);
            /* Allocate at the level the checker computed, which may have been
             * promoted because the value is stored into a place that lives
             * further out. This is where memory with automatic cleanup is
             * actually taken. */
            if (getenv("EXTC_DBG_ARENA")) arenaDriftCheck(g, e, "new");
            const char *ar = arenaRefAt(g, e->arenaLevel);
            if (!e->u.new_.count) {
                /* The place of one T (or one [N]T) is simply its address. */
                return arenaPrintf(g->arena,
                    "((%s *)extc_arena_alloc(&%s, (int64_t)sizeof(%s), \"%s\", %d))",
                    cType(g, w), ar, cType(g, w), g->path, e->line);
            }
            /* `T[n]` becomes a view `{ data, len }`, with the count evaluated
             * exactly once: an impure count is marked `needTemp` by the checker
             * and stored in a temporary first. */
            const char *n;
            if (e->needTemp) {
                const char *tmp = arenaPrintf(g->arena, "__extc_n%d", g->tmpSeq++);
                pfLine(g, "int64_t %s = (int64_t)(%s);", tmp, genExpr(g, e->u.new_.count));
                n = tmp;
            } else {
                n = genExpr(g, e->u.new_.count);
            }
            Type *st = subst(g, e->type);      /* the `slice<T>` the checker produced */
            return arenaPrintf(g->arena,
                "(%s){ .data = (%s *)extc_arena_alloc(&%s, (int64_t)(%s) * (int64_t)sizeof(%s),"
                " \"%s\", %d), .len = (int64_t)(%s) }",
                cType(g, st), cType(g, w), ar, n, cType(g, w), g->path, e->line, n);
        }

        case EX_GENCALL: {
            /* A generic call to one of the two builtin allocation primitives:
             *   `alloc<T>(n)`      asks the current block's arena for the place
             *                      of n values of T and returns a pointer
             *   `allocSlice<T>(n)` allocates the same block but returns a
             *                      `{data, len}` view, zeroed
             *
             * `allocSlice` zeroes because the language promises that a byte
             * read after its lifetime ends is always initialized. A bare bump
             * allocation does not zero, so reading memory that was never
             * written would yield an indeterminate value and the promise would
             * not hold. The `new` path zeroes already; this is the other
             * half. */
            const char *tn = cType(g, subst(g, *(Type **)vecAt(&e->u.gencall.targs, 0)));
            const char *n = genExpr(g, *(Expr **)vecAt(&e->u.gencall.args, 0));
            /* The level comes from the checker as well, with `alloc` meaning
             * the current block, so `g->blkLevel` is not counted here. */
            const char *ar = arenaRefAt(g, e->arenaLevel);
            if (strcmp(e->u.gencall.name, "allocSlice") == 0) {
                const char *vt = cType(g, subst(g, e->type));
                const char *tmp = arenaPrintf(g->arena, "__extc_s%d", g->tmpSeq++);
                pfLine(g, "%s %s;", vt, tmp);
                pfLine(g, "%s.data = (%s *)extc_arena_alloc(&%s, (int64_t)(%s) * (int64_t)sizeof(%s), \"%s\", %d);",
                       tmp, tn, ar, n, tn, g->path, e->line);
                pfLine(g, "%s.len = (int64_t)(%s);", tmp, n);
                pfLine(g, "memset(%s.data, 0, (size_t)(%s.len * (int64_t)sizeof(%s)));",
                       tmp, tmp, tn);
                return tmp;
            }
            return arenaPrintf(g->arena,
                "(%s *)extc_arena_alloc(&%s, (int64_t)(%s) * (int64_t)sizeof(%s), \"%s\", %d)",
                tn, ar, n, tn, g->path, e->line);
        }

        case EX_METHOD:    return genMethodCall(g, e);
        case EX_STRUCTLIT: return genStructLit(g, e);

        case EX_REF:
            return arenaPrintf(g->arena, "&(%s)", genExpr(g, e->u.ref.operand));

        /* `a ?? b`: the value if there is one, otherwise the fallback.
         *
         *   - A pure subject - a variable, field or index - becomes a C
         *     conditional directly: `x.tag == t_some ? x.u.some._0 : b`.
         *   - An impure subject, as in `f() ?? -1`, is marked `needTemp` by the
         *     checker and computed into a temporary first; flushPrefix emits
         *     those lines before the enclosing statement, which the
         *     `e->needTemp` branch below relies on.
         *
         * A C conditional evaluates only one side, so the fallback's side
         * effects do not run when the value is present. */
        case EX_COALESCE: {
            Type *mt = e->u.coalesce.main->type;
            const char *m;
            if (e->needTemp) {
                /* An impure subject (`f() ?? -1`) is computed once into a
                 * temporary, and the conditional then reads that variable;
                 * reading one twice has no side effect. flushPrefix emits these
                 * lines before the enclosing statement. */
                const char *tmp = arenaPrintf(g->arena, "__extc_c%d", g->tmpSeq++);
                pfLine(g, "%s %s = %s;", cType(g, mt), tmp, genExpr(g, e->u.coalesce.main));
                m = tmp;
            } else {
                m = genExpr(g, e->u.coalesce.main);
            }
            const char *fb = genExpr(g, e->u.coalesce.fallback);

            /* The fallback side is cast to the result type explicitly, because
             * C's `?:` applies the usual arithmetic conversions to its two
             * arms: an `int64_t` and an `int` together can silently widen the
             * result, an f32 payload next to a double literal being the typical
             * case. The checker already guarantees that the fallback fits the
             * result type, so this is belt and braces that also makes the
             * intended type visible in the generated C.
             *
             * Only a builtin scalar is cast this way: C cannot cast to an array
             * type, so `option<[3]i32> ?? arr` would stop compiling, as was
             * measured.
             *
             * The cast goes to the result type, the payload T, not to
             * `option<T>` itself. Casting by the option's type is wrong and was
             * the first version of this code, which produced no cast at all. */
            Type *rt = mt;
            if (mt && mt->kind != TY_REF && mt->targs.len > 0)
                rt = *(Type **)vecAt(&mt->targs, 0);
            bool scalar = rt && (rt->kind == TY_BUILTIN || rt->kind == TY_REF);
            const char *rhs = scalar
                ? arenaPrintf(g->arena, "((%s)(%s))", cType(g, rt), fb) : fb;

            if (mt && mt->kind == TY_REF) {
                /* A `?ref T` is a plain pointer in C, so a null test is enough. */
                return arenaPrintf(g->arena, "((%s) != ((void *)0) ? (%s) : %s)", m, m, rhs);
            }
            bool isOpt = isProtoType(mt, "option", 1);
            const char *tag = isOpt ? "some" : "success";
            return arenaPrintf(g->arena, "((%s).tag == %s_%s ? (%s).u.%s._0 : %s)",
                               m, cType(g, mt), tag, m, tag, rhs);
        }

        /* `e!` asserts that the value is present and leaves no runtime trace:
         *   - `opt!` and `r!` read the payload, a union member, without testing
         *     the tag
         *   - `p!` on a `?ref T` is that pointer itself; nothing is emitted */
        case EX_SIGN: {
            Type *ot = e->u.sign.operand->type;
            if (ot && ot->kind == TY_REF) return genExpr(g, e->u.sign.operand);
            bool isOpt = isProtoType(ot, "option", 1);
            const char *var = isOpt ? "some" : "success";
            return arenaPrintf(g->arena, "(%s).u.%s._0",
                               genExpr(g, e->u.sign.operand), var);
        }

        /* `null`, the zero value of a nullable reference, is a null pointer in
         * C. The checker has already proven that a null test narrows the value
         * before any use, so no runtime check is generated: what the compiler
         * can prove leaves no trace at runtime. */
        case EX_NULL:
            return "((void *)0)";

        case EX_ENUMVAL: {
            const char *tn = e->u.enumval.typeName;
            const char *vn = e->u.enumval.variant;
            /* The instantiated name of a generic enum (`maybe_i64`) is not in
             * the type table, so the type the checker resolved, recorded in
             * `assocOwner`, is preferred. */
            Type *et = e->assocOwner;
            if (!et && g->tt) et = ttFromName(g->tt, tn);
            /* Inside a generic instance `tn` is the template's name
             * (`option_T`), while the instance's C name is `option_i32`. With
             * an `assocOwner` present, `subst` plus `cType` gives the real
             * name; without that, `varArray<i32>::get()` referred to the
             * non-existent type `option_T`, which was a real bug. */
            if (et) {
                Type *rt = subst(g, et);
                if (rt && rt->name) tn = rt->name;
            }
            bool payload = et && et->kind == TY_ENUM && et->edef && enumHasPayload(et->edef);

            /* An enum without payloads is a plain C enum, so the variant is
             * itself a constant. */
            if (!payload)
                return arenaPrintf(g->arena, "%s_%s", tn, vn);

            /* An enum with payloads is a `struct { tag; union }` in C, so even
             * a payloadless variant has to be constructed:
             * `(shape){ .tag = shape_dot }`. */
            if (e->u.enumval.args.len == 0)
                return arenaPrintf(g->arena, "(%s){ .tag = %s_%s }", tn, tn, vn);

            /* Construction with a payload:
             * `(shape){ .tag = shape_circle, .u.circle = { ._0 = 2.0 } }`. */
            Buf b;
            bufInit(&b, g->arena);
            bufPrintf(&b, "(%s){ .tag = %s_%s, .u.%s = {", tn, tn, vn, vn);
            for (size_t i = 0; i < e->u.enumval.args.len; i++) {
                if (i) bufPuts(&b, ", ");
                bufPrintf(&b, "._%zu = %s", i, genExpr(g, *(Expr **)vecAt(&e->u.enumval.args, i)));
            }
            bufPuts(&b, "} }");
            return bufCstr(&b);
        }
    }
    return "0";
}

/* Add the automatic dereference of a value position.
 *
 * The checker treats a `ref T` in value position as a `T` and marks that node
 * with `deref`; the dereference itself is added here. So `let y = p` copies the
 * value and `p + 1` adds values, while whether the target may be written stays
 * with `ref` versus `mut ref` in the type.
 *
 * Wrapping genExprInner is what makes this complete: every path that generates
 * an expression, recursive calls included, goes through this function, so no
 * path can forget the dereference.
 */
static const char *genExpr(CG *g, Expr *e) {
    const char *s = genExprInner(g, e);
    if (e->deref) return arenaPrintf(g->arena, "*(%s)", s);
    return s;
}

/* ---------------------------------------------------------------- debug printing
 *
 * Recursively printing every field of a struct is something only the compiler
 * can do, the equivalent of Rust's `#[derive(Debug)]`: extC has no reflection,
 * so "walk all fields" cannot be written in the language at all.
 *
 * This is not the same as putting the standard library inside the compiler. What
 * the compiler emits is data plus one generic printer, not one printer per type:
 * see genStructDesc above and the `extc_print` at the top of the generated file.
 * The boundary is the rule that whatever can be expressed in extC belongs in
 * extC, not in the compiler.
 */

/* ---------------------------------------------------------------- statements */

static void genStmt(CG *g, Stmt *s);

/* ------------------------------------------------------------- `?` expansion
 *
 * `?` works at statement level, because C has no statement expressions, so it
 * expands into
 *     <evaluate once>  ->  <return on failure>  ->  <use the payload>
 *
 * What the compiler knows here is the protocol of the two result types, in the
 * same spirit as the `data` and `len` of a view: the struct, its methods and its
 * constructors all live in the prelude, and only a few names are fixed -
 * `some` and `none` for `option`, `success` and `failure` for `result`, with the
 * payload at `u.<variant>._0`. main.c verifies that those names exist where they
 * are needed.
 *
 * `option` and `result` used to be structs with `has` and `ok` fields, which
 * this code read and rebuilt field by field. They are ordinary enums now, so
 * everything below goes through a tag comparison and a payload path.
 */
typedef struct {
    const char *tmp;      /* holds the operand, evaluated exactly once */
    const char *inst;     /* C name of the instance: option_i64, result_unit_gameError */
    const char *okVar;    /* success variant: some / success */
    const char *failVar;  /* failure variant: none / failure */
    Type       *payload;  /* payload type T, carried by the success variant */
    bool        isOpt;    /* true for option, false for result */
} TryInfo;

/* Is this type an instance of a given generic type with a given arity?
 *
 * The definition name is compared, not the instantiated C name, so `option<i64>`
 * is still recognized as `option`.
 *
 * Params:
 *   name  - name of the generic definition, such as "option"
 *   nargs - required number of type arguments
 *
 * Returns:
 *   true for a generic struct (`slice<T>`, `varArray<T>`) or a generic enum
 *   (`option<T>`, `result<T,E>`) with that name and arity.
 */
static bool isProtoType(Type *t, const char *name, size_t nargs) {
    if (!t || t->targs.len != nargs) return false;
    /* generic struct: slice<T>, varArray<T> */
    if (t->kind == TY_GENERIC && t->sdef) return strcmp(t->sdef->name, name) == 0;
    /* generic enum: option<T>, result<T,E> */
    if (t->kind == TY_ENUM && t->edef)    return strcmp(t->edef->name, name) == 0;
    return false;
}

/* Return the C path of the payload on the success side: `x.u.some._0` or
 * `x.u.success._0`.
 */
static const char *tryPayloadPath(CG *g, TryInfo *ti) {
    return arenaPrintf(g->arena, "%s.u.%s._0", ti->tmp, ti->okVar);
}

/* Build the value to return when the operand of `?` failed.
 *
 * The failure value is constructed in the return type of the enclosing function.
 * For an `option` that is its zero value, which is `none`; for a `result` it is
 * a failure variant carrying the inner error.
 *
 * Returns:
 *   A C expression of that return type, or "0" when there is no return type.
 */
static const char *genTryFail(CG *g, TryInfo *ti) {
    Type *rt = subst(g, g->retType);
    if (!rt) return "0";
    if (ti->isOpt) return zeroValue(g, rt);
    /* For a `result` the inner error is copied across as it is: the error type
     * E is the same, so the C member is read directly. */
    return arenaPrintf(g->arena,
                       "(%s){ .tag = %s_%s, .u.%s = { ._0 = %s.u.%s._0 } }",
                       cType(g, rt), rt->name, ti->failVar, ti->failVar,
                       ti->tmp, ti->failVar);
}

/* Decrement the recursion depth counter on one exit path.
 *
 * Params:
 *   g - generator state; emits nothing when the function is not self-recursive
 *
 * Notes:
 *   - The decrement must follow the computation of the return value, never
 *     precede it. Writing it first turned `return spin(n)` into
 *     `--depth; return spin(n);`, which resets the counter to zero in every
 *     frame of a tail call, so the guard never fires. At -O0 gcc really does
 *     fold that into a tail call and the program exits at once with no output,
 *     and at -O2 it loops forever; both are wrong.
 *   - A `return` statement is not the only exit. Hanging the decrement on
 *     cgReturn alone missed the implicit return at the end of a function body:
 *     the function `cdq` of bench/oi/p3810.extc has no explicit return, so its
 *     depth only ever grew and a legitimate program reported "recursion too
 *     deep" after a hundred thousand frames. The end of the body therefore
 *     decrements as well, and an explicit return that already decremented
 *     through cgReturn does not do it a second time.
 */
static void cgRecLeave(CG *g) {
    if (g->isRecursive) cgLine(g, "--__extc_rec_depth;");
}

/* Return a value from the current function.
 *
 * Every exit goes through one shared epilogue rather than repeating the release
 * sequence of the arenas at each return. The value is stored in `__extc_ret_v`,
 * the recursion depth is decremented, and control jumps to `__extc_ret`, where
 * that sequence appears once. Repeating it at every exit used to be one of the
 * largest single costs of gcc time on the stress programs, because each copy had
 * to be analysed on its own.
 *
 * Params:
 *   val - C expression of the value to return, or NULL to return nothing
 *
 * Notes:
 *   - A function with `noArena` returns directly: it allocates nothing and has
 *     no arena array to release.
 *   - The value is computed before the arenas are released, which is slightly
 *     more conservative than releasing first and evaluating afterwards.
 */
static void cgReturn(CG *g, const char *val) {
    if (g->noArena) {
        if (g->isRecursive) {
            if (val) cgLine(g, "{ int64_t __r = (int64_t)(%s); --__extc_rec_depth; return __r; }", val);
            else     cgLine(g, "{ --__extc_rec_depth; return; }");
        } else if (val) cgLine(g, "return %s;", val);
        else            cgLine(g, "return;");
        return;
    }
    if (val) cgLine(g, "__extc_ret_v = %s;", val);
    cgRecLeave(g);
    cgLine(g, "goto __extc_ret;");
}

/* Emit the statements that evaluate the operand of `?` once and return on
 * failure, and describe the result to the caller.
 *
 * Returns:
 *   A TryInfo holding the temporary with the operand, the C name of its type and
 *   the variant names; the caller reads the payload through it.
 */
static TryInfo genTryHead(CG *g, Expr *e) {
    TryInfo ti;
    Type *ot = ttBase(subst(g, e->u.try_.operand->type));
    ti.isOpt = isProtoType(ot, "option", 1);
    ti.inst = cType(g, ot);
    ti.okVar = ti.isOpt ? "some" : "success";
    ti.failVar = ti.isOpt ? "none" : "failure";
    ti.payload = subst(g, *(Type **)vecAt(&ot->targs, 0));
    ti.tmp = arenaPrintf(g->arena, "__extc_try%d", g->tmpSeq++);

    const char *operand = genExpr(g, e->u.try_.operand);
    flushPrefix(g);
    cgLine(g, "%s %s = %s;", ti.inst, ti.tmp, operand);
    /* A failure returns, so every enclosing arena level is released, exactly as
     * for a `return` statement, through the shared epilogue. That decision has
     * to be taken before this line is written, because the line already
     * contains the return. */
    cgLine(g, "if (%s.tag != %s_%s) {", ti.tmp, ti.inst, ti.okVar);
    g->indent++;
    cgReturn(g, genTryFail(g, &ti));
    g->indent--;
    cgLine(g, "}");
    return ti;
}


/* Emit a `#line` directive pointing back at the .extC source of a statement.
 *
 * Emits nothing when line mapping is off or the statement carries no line.
 */
static void lineMark(CG *g, Stmt *s) {
    if (g->lineMap && s->line > 0)
        bufPrintf(g->out, "#line %d \"%s\"\n", s->line, g->path);
}

/* Return the arena argument for a call to a callee that needs a home arena.
 *
 * Which arena to pass has already been decided by the checker and recorded in
 * `Expr.arenaArg`: `ARENA_HOME` means this function's own home arena, and a
 * value of 1 or more means the block arena at that level. Code generation only
 * translates that decision and never makes one; a fallback that second-guessed
 * the checker here used to be a second authority on the same question.
 *
 * Returns:
 *   A C expression denoting the arena, usable as a function argument.
 */
static const char *homeArg(CG *g, int arenaArg) {
    /* Passed as an argument, so this is the pointer itself. Do not confuse it
     * with arenaRefAt, whose result is wrapped in `&` and which therefore
     * returns `(*__extc_home)`. */
    if (arenaArg == ARENA_HOME) return "__extc_home";
    return arenaPrintf(g->arena, "&__extc_a[%d]", arenaArg);
}

/* Return the arena that an allocation site at a given level refers to.
 *
 * The level is computed by the checker and recorded in `Expr.arenaLevel`. It
 * starts as the block the statement lives in and has already been promoted when
 * the value is stored into a place that lives further out; inside a function that
 * has a home arena every site is `ARENA_HOME` already.
 *
 * Params:
 *   level - ARENA_HOME, or a block level of 1 or more
 *
 * Returns:
 *   `(*__extc_home)`, the arena the caller chose and the longest lived one, or
 *   `__extc_a[level]`, which is released when block `level` ends.
 *
 * Notes:
 *   - A fallback that returned `(*__extc_home)` whenever the function had a home
 *     arena was removed here. It was a second authority on the level, with the
 *     checker computing one and code generation deciding another; it existed
 *     only because the checker used to compute levels before the call graph was
 *     known. Now the checker rewrites every site from `FuncDef.arenaSites` once
 *     the closure is built, so the fallback is unnecessary, and removing it left
 *     the generated output byte-for-byte identical, which shows the two
 *     authorities agreed all along.
 */
static const char *arenaRefAt(CG *g, int level) {
    if (level == ARENA_HOME) return "(*__extc_home)";
    return arenaPrintf(g->arena, "__extc_a[%d]", level);
}

/* Report a disagreement between the arena level and the block being generated.
 *
 * This is a drift detector for the level the checker computed: it must agree
 * with the block code generation is currently inside, and a disagreement means
 * the two have started computing the level separately again. It is active only
 * when EXTC_DBG_ARENA is set in the environment, and it changes no output.
 *
 * The two conditions are one-directional and deliberately loose:
 *   - the level must not be 0, which means "not yet decided" and would show
 *     that the checker missed a site
 *   - the level must not be deeper than the current block; promotion only ever
 *     moves an allocation towards a longer lifetime, so a level shallower than
 *     the current block is legal
 */
static void arenaDriftCheck(CG *g, Expr *e, const char *what) {
    if (getenv("EXTC_DBG_ARENA_VERBOSE"))
        fprintf(stderr, "[arena-ok?] %s: 层号=%d 当前块=%d %s:%d\n",
                what, e->arenaLevel, g->blkLevel, g->path, e->line);
    if (e->arenaLevel == 0)
        fprintf(stderr, "[arena!] %s 的层号是 0（没定）✗  %s:%d\n", what, g->path, e->line);
    else if (e->arenaLevel != ARENA_HOME && e->arenaLevel > g->blkLevel)
        fprintf(stderr, "[arena!] %s 的层号 %d 比当前块 %d 还深 ⇒ 两个权威漂了 ✗  %s:%d\n",
                what, e->arenaLevel, g->blkLevel, g->path, e->line);
}

/* Release block arenas down to and including one level.
 *
 * Params:
 *   lvl - innermost level to release; 0 or less releases nothing, since level 0
 *         is not a block arena
 *
 * Notes:
 *   - A function with `noArena` emits nothing: it never allocated.
 */
static void cgReleaseLevel(CG *g, int lvl) {
    if (lvl <= 0) return;
    if (g->noArena) return;   /* never allocates, so nothing to release */
    cgLine(g, "extc_arena_release(&__extc_a[%d]);", lvl);
}

/* Collect every `@overwrite` site of one function body, in source order.
 *
 * The order is what makes the numbering stable: the prologue emits one storage
 * cell per site in this same order, and owIndex looks a site up by position.
 */
static void collectOwSites(Stmt *s, Vec *out) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR:
        if (s->u.var.overwrite) *(Stmt **)vecPush(out) = s;
        return;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            collectOwSites(*(Stmt **)vecAt(&s->u.block.stmts, i), out);
        return;
    case ST_IF:
        collectOwSites(s->u.ifs.thenBody, out);
        collectOwSites(s->u.ifs.elseBody, out);
        return;
    case ST_WHILE: collectOwSites(s->u.whiles.body, out); return;
    case ST_MATCH:
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            collectOwSites((*(MatchArm **)vecAt(&s->u.match.arms, i))->body, out);
        return;
    default: return;
    }
}

/* Return the index of one `@overwrite` site within the current function.
 *
 * Returns:
 *   Its position in `g->owSites`, or -1 when it is not registered.
 *
 * Notes:
 *   - The prologue emits the cells in the same order, so the two sides agree.
 *     The index is looked up rather than stored in the AST, because one body is
 *     visited once per generic instance and a stored index would be overwritten
 *     by the next instance.
 */
static int owIndex(CG *g, Stmt *s) {
    for (size_t i = 0; i < g->owSites.len; i++)
        if (*(Stmt **)vecAt(&g->owSites, i) == s) return (int)i;
    return -1;
}

/* Collect the call sites whose callee needs @overwrite cells.
 *
 * Every expression kind has to be covered. A missed one leaves that call site
 * without cells, so the callee receives NULL; it has a fallback for that, so the
 * program stays defined, but the storage is then allocated afresh on every call
 * instead of being reused.
 */
static void collectOwCallsExpr(Expr *e, Vec *out);
static void collectOwCallsStmt(Stmt *s, Vec *out);

/* Collect the cell-needing call sites of one expression tree; the declaration
 * above records why every expression kind must be covered.
 */
static void collectOwCallsExpr(Expr *e, Vec *out) {
    if (!e) return;
    switch (e->kind) {
    case EX_CALL:
        if (e->func && e->func->owSites > 0 && !e->func->owLocal)
            *(Expr **)vecPush(out) = e;
        for (size_t i = 0; i < e->u.call.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.call.args, i), out);
        return;
    case EX_METHOD:
        if (e->func && e->func->owSites > 0 && !e->func->owLocal)
            *(Expr **)vecPush(out) = e;
        collectOwCallsExpr(e->u.method.recv, out);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.method.args, i), out);
        return;
    case EX_ASSOC:
        if (e->func && e->func->owSites > 0 && !e->func->owLocal)
            *(Expr **)vecPush(out) = e;
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.assoc.args, i), out);
        return;
    case EX_BIN:      collectOwCallsExpr(e->u.bin.left, out); collectOwCallsExpr(e->u.bin.right, out); return;
    case EX_UN:       collectOwCallsExpr(e->u.un.operand, out); return;
    case EX_FIELD:    collectOwCallsExpr(e->u.field.obj, out); return;
    case EX_INDEX:    collectOwCallsExpr(e->u.index.obj, out); collectOwCallsExpr(e->u.index.index, out); return;
    case EX_SLICE:    collectOwCallsExpr(e->u.slice.obj, out);
                      collectOwCallsExpr(e->u.slice.lo, out); collectOwCallsExpr(e->u.slice.hi, out); return;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            collectOwCallsExpr((*(FieldInit **)vecAt(&e->u.lit.inits, i))->value, out);
        return;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.arraylit.elems, i), out);
        return;
    case EX_REF:      collectOwCallsExpr(e->u.ref.operand, out); return;
    case EX_DEREF:    collectOwCallsExpr(e->u.deref.operand, out); return;
    case EX_SIGN:     collectOwCallsExpr(e->u.sign.operand, out); return;
    case EX_CONV:     collectOwCallsExpr(e->u.conv.operand, out); return;
    case EX_TRY:      collectOwCallsExpr(e->u.try_.operand, out); return;
    case EX_NEW:      collectOwCallsExpr(e->u.new_.count, out); return;
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.gencall.args, i), out);
        return;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            collectOwCallsExpr(*(Expr **)vecAt(&e->u.enumval.args, i), out);
        return;
    case EX_COALESCE: collectOwCallsExpr(e->u.coalesce.main, out);
                      collectOwCallsExpr(e->u.coalesce.fallback, out); return;
    default: return;      /* literals, bindings, null */
    }
}

/* Collect the call sites of one statement tree whose callee needs cells.
 *
 * Params:
 *   out - receives the Expr* of every such call site, in discovery order
 */
static void collectOwCallsStmt(Stmt *s, Vec *out) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR:    collectOwCallsExpr(s->u.var.init, out); return;
    case ST_ASSIGN: collectOwCallsExpr(s->u.assign.target, out); collectOwCallsExpr(s->u.assign.value, out); return;
    case ST_EXPR:   collectOwCallsExpr(s->u.expr.expr, out); return;
    case ST_RETURN: collectOwCallsExpr(s->u.ret.value, out); return;
    case ST_IF:     collectOwCallsExpr(s->u.ifs.cond, out);
                    collectOwCallsStmt(s->u.ifs.thenBody, out); collectOwCallsStmt(s->u.ifs.elseBody, out); return;
    case ST_WHILE:  collectOwCallsExpr(s->u.whiles.cond, out); collectOwCallsStmt(s->u.whiles.body, out); return;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            collectOwCallsStmt(*(Stmt **)vecAt(&s->u.block.stmts, i), out);
        return;
    case ST_MATCH:
        collectOwCallsExpr(s->u.match.scrutinee, out);
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            collectOwCallsStmt((*(MatchArm **)vecAt(&s->u.match.arms, i))->body, out);
        return;
    default: return;      /* break, continue */
    }
}

/* Return the index of one call site within the current function.
 *
 * Returns:
 *   Its position in `g->owCalls`, or -1 when it is not registered. The prologue
 *   emits the cells in the same order, so the two sides agree.
 */
static int owCallIndex(CG *g, Expr *e) {
    for (size_t i = 0; i < g->owCalls.len; i++)
        if (*(Expr **)vecAt(&g->owCalls, i) == e) return (int)i;
    return -1;
}

/* Does the current function keep its `@overwrite` cells in its own frame?
 *
 * Yes for `main` and for recursive functions, whose frame is the one that stays
 * alive; FuncDef.owLocal carries the answer. The statement argument is unused,
 * because the answer is a property of the function.
 */
static bool f_owLocal(CG *g, Stmt *s) { (void)s; return g->owLocal; }

/* Append the cell arguments of one call site whose callee needs `@overwrite`
 * storage.
 *
 * Params:
 *   b       - buffer holding the argument list, without the closing parenthesis
 *   e       - the call site
 *   nargs   - number of arguments already in the list, used for the comma
 *   hasHome - whether a home arena argument was already appended
 *
 * Notes:
 *   - A call site that was not registered passes nothing; the callee has a
 *     fallback for that, so the program stays defined.
 */
static void owPassCells(CG *g, Buf *b, Expr *e, size_t nargs, bool hasHome) {
    if (!e->func || e->func->owSites == 0 || e->func->owLocal) return;
    int j = owCallIndex(g, e);
    if (j < 0) return;                       /* not registered; the callee has a fallback */
    for (int k = 0; k < e->func->owSites; k++)
        bufPrintf(b, "%s&__extc_owc%d_%d", (nargs || hasHome || k) ? ", " : "", j, k);
}

/* How many block levels one function can use at the same time.
 *
 * The answer sizes the `__extc_a` array, which is a fixed-length array on the
 * stack, so setting up the block arenas needs no allocation.
 */
static int blkMaxLevel(Stmt *s);

/* Return the deepest block level inside one statement that is a block.
 *
 * Returns:
 *   The deepest level reached inside the block, not counting the level the block
 *   itself adds; 0 when there is no block.
 */
static int blkMaxOfBlock(Stmt *block) {
    int m = 0;
    if (!block || block->kind != ST_BLOCK) return blkMaxLevel(block);
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        if (blkMaxLevel(*(Stmt **)vecAt(&block->u.block.stmts, i)) > m)
            m = blkMaxLevel(*(Stmt **)vecAt(&block->u.block.stmts, i));
    return m;
}
/* Return the deepest block level reached inside one statement.
 *
 * A block counts as one level, and the loops, conditionals and matches inside it
 * add their own, so the result is the number of arena slots the enclosing
 * function needs.
 */
static int blkMaxLevel(Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
    case ST_BLOCK: return 1 + blkMaxOfBlock(s);
    case ST_IF: {
        int a = 1 + blkMaxOfBlock(s->u.ifs.thenBody);
        int b = s->u.ifs.elseBody
                  ? 1 + (s->u.ifs.elseBody->kind == ST_BLOCK
                           ? blkMaxOfBlock(s->u.ifs.elseBody)
                           : blkMaxLevel(s->u.ifs.elseBody))
                  : 0;
        return a > b ? a : b;
    }
    case ST_WHILE: return 1 + blkMaxOfBlock(s->u.whiles.body);
    case ST_MATCH: {
        int m = 0;
        for (size_t i = 0; i < s->u.match.arms.len; i++) {
            MatchArm *a = *(MatchArm **)vecAt(&s->u.match.arms, i);
            int d = 1 + blkMaxOfBlock(a->body);
            if (d > m) m = d;
        }
        return m;
    }
    default: return 0;
    }
}

/* Emit the body of one block, clearing its arena on entry and releasing it on
 * exit.
 *
 * Params:
 *   block - the block statement; its statements are generated in order
 *
 * Notes:
 *   - A loop body is a block too, so every iteration clears the arena on entry.
 *     That is what bounds the memory of a loop by a single iteration instead of
 *     by the number of iterations.
 */
static void genBlockBody(CG *g, Stmt *block) {
    g->blkLevel++;
    if (!g->noArena) cgLine(g, "extc_arena_release(&__extc_a[%d]);", g->blkLevel);   /* clear */
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        genStmt(g, *(Stmt **)vecAt(&block->u.block.stmts, i));
    cgReleaseLevel(g, g->blkLevel);
    g->blkLevel--;
}

static void genStmtInner(CG *g, Stmt *s);

/* Emit one statement, with a statement prefix allowed around it.
 *
 * Everything the statement needs evaluated early is written to the prefix here,
 * before the statement itself, and the prefix block counter tells the prefix
 * mechanism that it is inside a statement and may emit lines at all.
 */
static void genStmt(CG *g, Stmt *s) {
    lineMark(g, s);
    g->prefixBlk++;
    genStmtInner(g, s);
    g->prefixBlk--;
}

/* Emit the statement itself; the prefix bookkeeping lives in genStmt. */
static void genStmtInner(CG *g, Stmt *s) {
    switch (s->kind) {
        case ST_VAR: {
            /* `cname` is the name the checker decided on; a shadowed one
             * carries a `__2` suffix. */
            const char *nm = s->u.var.cname ? s->u.var.cname : s->u.var.name;
            /* `@overwrite` keeps exactly one block of storage. It is allocated
             * the first time this statement runs and only cleared and reused
             * afterwards. What comes out is a few lines of inline C,
             * `extc_arena_alloc` plus `memset`, so no new runtime support is
             * needed. */
            if (s->u.var.overwrite) {
                /* One block of storage is reused; `extc_owcell`, a block plus
                 * its capacity, covers all three shapes - a single value, a
                 * fixed-size buffer and a runtime length. The first execution
                 * allocates and lazily at that, every later one clears and
                 * reuses, and the zero-value contract of `new` is unchanged.
                 *
                 * Where the cell comes from: a site of this function uses a cell
                 * in its own frame, a site of a callee uses the cell passed in
                 * by the call site. */
                int k = owIndex(g, s);
                Expr *nx = s->u.var.init;
                if (getenv("EXTC_DBG_ARENA")) arenaDriftCheck(g, nx, "@overwrite 的 new");
                Type *st_t = subst(g, nx->u.new_.type);
                bool loc = f_owLocal(g, s);
                if (k >= 0) {
                    const char *cell = loc ? arenaPrintf(g->arena, "&__extc_ow%d", k)
                                           : arenaPrintf(g->arena, "__extc_owarg%d", k);
                    /* The storage lives where the cell's own `home` says:
                     * with a home it belongs to `__extc_home`, otherwise to
                     * level 1 of this frame. `arenaRefAt(nx->arenaLevel)` must
                     * not be used here: that number is the callee's own level,
                     * while the cell lives elsewhere, and the two do not share a
                     * lifetime. Using it made a later call memset a block that
                     * had already been freed - an ASan use-after-free, and
                     * without ASan glibc handed the block back and the program
                     * merely looked as if it worked. */
                    const char *owcv = arenaPrintf(g->arena, "__owc%d", k);
                    const char *ar = arenaPrintf(g->arena, "(*%s->home)", owcv);
                    (void)nx->arenaLevel;
                    const char *ct = cType(g, st_t);
                    flushPrefix(g);
                    /* The site index has to be part of the name: two
                     * `@overwrite` sites in one function would otherwise be a
                     * redefinition of the same variable. */
                    cgLine(g, "extc_owcell *%s = %s;", owcv, cell);
                    if (!nx->u.new_.count) {
                        /* A single value, or a fixed-size `new [N]T`, has a
                         * constant size. The binding has to be declared first,
                         * because a declaration inside the if or the else branch
                         * would go out of scope at its closing brace. */
                        cgLine(g, "%s %s;", cType(g, s->type), nm);
                        /* The temporary must not be named `p`: with
                         * `@overwrite var p = ...` a `void *p` would shadow the
                         * outer `array_T *p`, turning `p = (array_T *)p` into a
                         * self-assignment and leaving the binding null, which
                         * showed up as an ASan segmentation fault. */
                        const char *owtp = arenaPrintf(g->arena, "__owp%d", k);
                        cgLine(g, "if (!%s || !%s->p) { void *%s = extc_arena_alloc(&%s,"
                                  " (int64_t)sizeof(%s), \"%s\", %d);  if (%s) %s->p = %s;"
                                  "  %s = (%s)%s; }",     /* the C type is already a pointer */
                                owcv, owcv, owtp, ar, ct, g->path, nx->line,
                                owcv, owcv, owtp, nm, cType(g, s->type), owtp);
                        cgLine(g, "else { memset(%s->p, 0, (size_t)sizeof(%s));"
                                  "  %s = (%s)%s->p; }",
                                owcv, ct, nm, cType(g, s->type), owcv);
                    } else {
                        /* A runtime length uses `{ptr, cap}` and doubles the
                         * capacity when it grows, so the memory stays within
                         * twice the largest length seen and does not depend on
                         * the number of iterations. */
                        const char *cnt = genExpr(g, nx->u.new_.count);
                        if (nx->needTemp) {
                            const char *t = arenaPrintf(g->arena, "__extc_n%d", g->tmpSeq++);
                            pfLine(g, "int64_t %s = (int64_t)(%s);", t, cnt);
                            cnt = t;
                        }
                        cgLine(g, "int64_t __owk = (int64_t)(%s);", cnt);
                        cgLine(g, "if (__owk < 0) { extc_trapMsg(\"%s\", %d,"
                                  " \"negative length\"); }", g->path, nx->line);
                        cgLine(g, "%s %s;", cType(g, s->type), nm);
                        cgLine(g, "if (!%s) { %s = (%s){ .data = (%s *)extc_arena_alloc(&%s,"
                                  " __owk * (int64_t)sizeof(%s), \"%s\", %d), .len = __owk }; }",
                                owcv, nm, cType(g, s->type), ct, ar, ct, g->path, nx->line);
                        cgLine(g, "else { if (__owk > %s->cap) { int64_t c = %s->cap * 2;"
                                  " if (c < __owk) c = __owk;"
                                  "  %s->p = extc_arena_alloc(&%s, c * (int64_t)sizeof(%s),"
                                  " \"%s\", %d);  %s->cap = c; }"
                                  "  %s = (%s){ .data = (%s *)%s->p, .len = __owk }; }",
                                owcv, owcv, owcv, ar, ct, g->path, nx->line, owcv,
                                nm, cType(g, s->type), ct, owcv);
                        cgLine(g, "memset(%s.data, 0, (size_t)(__owk * (int64_t)sizeof(%s)));",
                                nm, ct);
                    }
                    return;
                }
            }
            /* `let q = f()?` and `var q = f()?` are one of the legal positions
             * of `?`. This case was missing once: the checker allowed it while
             * code generation expanded `?` only in an assignment, a return and
             * an expression statement, so it fell through to the EX_TRY branch
             * of genExpr and reported an internal error, which reproduced every
             * time. The shape matches the assignment case, because the generated
             * C is a declaration followed by an initialization. */
            if (s->u.var.init && s->u.var.init->kind == EX_TRY) {
                TryInfo ti = genTryHead(g, s->u.var.init);
                flushPrefix(g);
                cgLine(g, "%s %s = %s;", cType(g, s->type), nm, tryPayloadPath(g, &ti));
                return;
            }
            const char *init = s->u.var.init ? genExpr(g, s->u.var.init)
                                             : zeroInit(g, s->type);
            flushPrefix(g);
            cgLine(g, "%s %s = %s;", cType(g, s->type), nm, init);
            return;
        }

        case ST_ASSIGN:
            /* An assignment whose value is a `?` reads the payload; the failure
             * path has already returned by then. */
            if (s->u.assign.value && s->u.assign.value->kind == EX_TRY) {
                TryInfo ti = genTryHead(g, s->u.assign.value);
                const char *at = genExpr(g, s->u.assign.target);
                flushPrefix(g);
                cgLine(g, "%s = %s;", at, tryPayloadPath(g, &ti));
                return;
            }
            const char *tgt = genExpr(g, s->u.assign.target);
            const char *val = genExpr(g, s->u.assign.value);
            flushPrefix(g);
            cgLine(g, "%s = %s;", tgt, val);
            return;

        case ST_IF: {
            const char *cnd = genExpr(g, s->u.ifs.cond);
            flushPrefix(g);
            cgLine(g, "if (%s) {", cnd);
            g->indent++;
            genBlockBody(g, s->u.ifs.thenBody);
            g->indent--;
            if (!s->u.ifs.elseBody) {
                cgLine(g, "}");
                return;
            }
            cgLine(g, "} else {");
            g->indent++;
            if (s->u.ifs.elseBody->kind == ST_BLOCK) genBlockBody(g, s->u.ifs.elseBody);
            else                                     genStmt(g, s->u.ifs.elseBody);
            g->indent--;
            cgLine(g, "}");
            return;
        }

        case ST_WHILE: {
            const char *cnd = genExpr(g, s->u.whiles.cond);
            flushPrefix(g);
            cgLine(g, "while (%s) {", cnd);
            g->indent++;
            g->loopLevel[g->loopLen++] = g->blkLevel + 1;   /* the body is the next level */
            genBlockBody(g, s->u.whiles.body);
            g->loopLen--;
            g->indent--;
            cgLine(g, "}");
            return;
        }

        case ST_RETURN: {
            /* Every return goes through the shared epilogue: only
             * `__extc_ret_v = ...; goto __extc_ret;` is emitted here, and the
             * release sequence appears once, at the end of the function. */
            if (!s->u.ret.value) {
                cgReturn(g, NULL);           /* shared epilogue */
                return;
            }
            if (s->u.ret.value->kind == EX_TRY) {
                /* `return e?`: on success the payload is wrapped in the
                 * enclosing return type. */
                TryInfo ti = genTryHead(g, s->u.ret.value);
                Type *rt = subst(g, g->retType);
                Buf rb;
                bufInit(&rb, g->arena);
                bufPrintf(&rb, "(%s){ .tag = %s_%s, .u.%s = { ._0 = %s } }",
                          cType(g, rt), rt->name, ti.okVar, ti.okVar,
                          tryPayloadPath(g, &ti));
                cgReturn(g, bufCstr(&rb));
                return;
            }
            const char *v = genExpr(g, s->u.ret.value);
            flushPrefix(g);              /* must precede the return */
            cgReturn(g, v);
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE: {
            /* `break` and `continue` leave these block levels behind, so the
             * arenas are released first, down to the level of the loop body.
             * The body's own arena is cleared when the next iteration enters
             * it. */
            int to = g->loopLen ? g->loopLevel[g->loopLen - 1] : 1;
            for (int lv = g->blkLevel; lv >= to; lv--) cgReleaseLevel(g, lv);
            cgLine(g, "%s", s->kind == ST_BREAK ? "break;" : "continue;");
            return;
        }

        case ST_EXPR:
            /* `f()?` as a statement of its own needs only the two lines that
             * evaluate it once and return on failure; nothing reads the
             * payload. */
            if (s->u.expr.expr->kind == EX_TRY) {
                (void)genTryHead(g, s->u.expr.expr);
                return;
            }
            const char *ex = genExpr(g, s->u.expr.expr);
            flushPrefix(g);
            cgLine(g, "%s;", ex);
            return;

        case ST_BLOCK:
            cgLine(g, "{");
            g->indent++;
            genBlockBody(g, s);
            g->indent--;
            cgLine(g, "}");
            return;

        case ST_MATCH: {
            /* The scrutinee is evaluated exactly once, into a temporary when
             * the enum has a payload, because an arm reads `.u.<variant>` and
             * evaluating the expression a second time - as in
             * `match f() { ... }` - would be wrong. */
            Type *et = ttBase(s->u.match.scrutinee->type);
            bool payload = et && et->kind == TY_ENUM && et->edef && enumHasPayload(et->edef);
            const char *subj = genExpr(g, s->u.match.scrutinee);
            if (payload) {
                const char *tmp = arenaPrintf(g->arena, "__extc_m%d", g->tmpSeq++);
                cgLine(g, "%s %s = %s;", cType(g, et), tmp, subj);
                subj = tmp;
            }

            /* The arms become an if/else chain, deliberately not a `switch`
             * over the variant constants: a `break` or `continue` in an arm
             * body belongs to the enclosing loop, but inside a `switch` it
             * would only leave the switch, so a `while true` with a `break`
             * would never terminate. A read loop hung on exactly that.
             *
             * No final `else` is needed: the checker guarantees that the match
             * is exhaustive, and a missing variant would not compile at all. */
            flushPrefix(g);
            for (size_t i = 0; i < s->u.match.arms.len; i++) {
                MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
                cgLine(g, "%s (%s%s == %s_%s) {", i == 0 ? "if" : "} else if",
                       subj, payload ? ".tag" : "", et ? et->name : "?", arm->variant);
                g->indent++;
                /* Bind the payload: `circle(r) => ...` becomes
                 * `double r = tmp.u.circle._0;`. */
                for (size_t k = 0; k < arm->binds.len; k++) {
                    Variant *v = et && et->edef ? NULL : NULL;
                    (void)v;
                    Type *bt = NULL;
                    if (et && et->edef) {
                        for (size_t j = 0; j < et->edef->variants.len; j++) {
                            Variant *vv = *(Variant **)vecAt(&et->edef->variants, j);
                            if (strcmp(vv->name, arm->variant) != 0) continue;
                            bt = *(Type **)vecAt(&vv->types, k);
                            /* A generic enum instance substitutes its type
                             * arguments into the payload type, so `just(T)`
                             * with `T = slice<u8>` yields the instance type. */
                            if (et->edef->typeParams.len > 0 &&
                                et->targs.len == et->edef->typeParams.len)
                                bt = ttSubstitute(g->tt, bt, &et->edef->typeParams, &et->targs);
                            break;
                        }
                    }
                    cgLine(g, "%s %s = %s.u.%s._%zu;", cType(g, bt), *(const char **)vecAt(&arm->binds, k),
                           subj, arm->variant, k);
                }
                genBlockBody(g, arm->body);
                g->indent--;
            }
            cgLine(g, "}");
            return;
        }
    }
}

/* ---------------------------------------------------------------- top level */

/* Build the C parameter list of a function.
 *
 * A function whose allocations may escape takes one hidden arena pointer at the
 * end: the `new` expressions inside it allocate into the arena chosen by the
 * caller. `@overwrite` cells are passed in the same way, as opaque
 * `extc_owcell *`, so a caller does not need to know their types and a generic
 * instance needs no special case.
 *
 * Returns:
 *   The parameter list text, without the surrounding parentheses.
 */
static const char *cgParamList(CG *g, FuncDef *f) {
    Buf sig;
    bufInit(&sig, g->arena);
    /* C fixes the signature of `main`, so it takes no hidden parameter; its
     * home arena is the local `extc_arena *__extc_home = &__extc_a[1]` in its
     * own body. */
    if (!f->owner && strcmp(f->name, "main") == 0) { bufPuts(&sig, "void"); return bufCstr(&sig); }
    if (f->params.len == 0 && !f->needsHome && f->owLocal) {   /* no hidden parameters follow */
        bufPuts(&sig, "void"); return bufCstr(&sig);
    }
    for (size_t i = 0; i < f->params.len; i++) {
        Param *p = *(Param **)vecAt(&f->params, i);
        if (i) bufPuts(&sig, ", ");
        bufPrintf(&sig, "%s %s", cType(g, p->type), p->cname ? p->cname : p->name);
    }
    if (f->needsHome) {
        if (f->params.len) bufPuts(&sig, ", ");
        bufPuts(&sig, "extc_arena *__extc_home");
    }
    /* @overwrite cells live in the frame of the call site and are passed in as
     * opaque `extc_owcell *`: opaque means a caller does not need to know the
     * types of the callee's sites, and a generic instance needs no special
     * case. */
    if (!f->owLocal) {
        for (int i = 0; i < f->owSites; i++) {
            if (f->params.len || f->needsHome || i) bufPuts(&sig, ", ");
            bufPrintf(&sig, "extc_owcell *__extc_owarg%d", i);
        }
    }
    return bufCstr(&sig);
}

/* Does this statement always return?
 *
 * Only the simplest shapes are recognized: a `return`, or a block whose last
 * statement is one.
 *
 * Returns:
 *   true only when the statement definitely returns; an unsure case answers
 *   false. That is deliberately conservative: a false negative merely emits one
 *   more `return`, which is dead code but compiles, whereas the opposite mistake
 *   leaves a missing return that gcc reports as an error.
 */
static bool stmtIsDefiniteReturn(Stmt *s) {
    if (!s) return false;
    if (s->kind == ST_RETURN) return true;
    if (s->kind == ST_BLOCK && s->u.block.stmts.len > 0)
        return stmtIsDefiniteReturn(*(Stmt **)vecAt(&s->u.block.stmts,
                                                    s->u.block.stmts.len - 1));
    return false;
}

/* Does this function reach itself, directly or through other calls?
 *
 * The answer decides whether a recursion guard is emitted. Runaway recursion
 * used to be silent: gcc either folded it into an endless loop that printed
 * nothing, or the stack overflowed and the OS reported a segmentation fault,
 * which is neither an extC message nor a position in the source. Only a
 * self-recursive function pays for the guard, so an ordinary call costs one
 * increment and nothing else.
 *
 * The reachability uses the call graph the checker already recorded in
 * `e->func`, so no second AST walker is needed, and it is transitive: in the
 * mutual recursion `g -> h -> g`, `g` counts as self-recursive. That shape is
 * the one that used to run for 60 seconds without printing anything.
 *
 * Returns:
 *   true when some path through the call graph leads back to f.
 *
 * Notes:
 *   - The graph is small and the answer is computed once per generated function,
 *     so the plainest fixed-point iteration is good enough.
 */
static bool funcCallsItself(CG *g, FuncDef *f) {
    if (!f || !f->body) return false;
    /* Direct calls, `EX_ASSOC` and `EX_METHOD` included; both carry `e->func`. */
    Vec seen;  vecInit(&seen, g->arena, sizeof(FuncDef *));
    Vec work;  vecInit(&work, g->arena, sizeof(FuncDef *));
    *(FuncDef **)vecPush(&work) = f;
    *(FuncDef **)vecPush(&seen) = f;
    bool hit = false;
    for (size_t i = 0; i < work.len; i++) {
        FuncDef *cur = *(FuncDef **)vecAt(&work, i);
        if (!cur || !cur->body) continue;
        if (cur == f && i > 0) { hit = true; break; }   /* reached again: recursive */
        for (size_t j = 0; j < cur->callees.len; j++) {
            FuncDef *nx = *(FuncDef **)vecAt(&cur->callees, j);
            if (!nx) continue;
            if (nx == f) { hit = true; break; }
            bool dup = false;
            for (size_t k = 0; k < seen.len && !dup; k++)
                if (*(FuncDef **)vecAt(&seen, k) == nx) dup = true;
            if (!dup) { *(FuncDef **)vecPush(&seen) = nx;
                        *(FuncDef **)vecPush(&work) = nx; }
        }
        if (hit) break;
    }
    return hit;
}

/* Emit one complete function definition.
 *
 * The prologue creates the block arena array, the `@overwrite` cells, the slot
 * the value is returned through and the recursion guard; then the body is
 * generated; and finally a single shared epilogue releases every arena level and
 * returns. Every piece of per-function state it changes is saved and restored,
 * because the instances of a generic generate several functions in a row.
 *
 * Params:
 *   f - the function to emit
 */
static void genFunc(CG *g, FuncDef *f) {
    bool isMain = cgIsMain(f);
    if (isMain) {
        /* C fixes the signature of `main`, so it takes no hidden parameter; its
         * home arena is one of its own block arenas. */
        cgLine(g, "int main(void) {");
    } else {
        Buf sig;
        bufInit(&sig, g->arena);
        bufPrintf(&sig, "static %s %s(%s) {", cType(g, f->ret), cFuncName(g, f),
                  cgParamList(g, f));
        cgLine(g, "%s", bufCstr(&sig));
    }

    g->indent++;
    /* `?` needs the return type to build the value to return on failure. The
     * temporary counter restarts in every function, so each function has its own
     * `__extc_try0` and they cannot collide. */
    Type *savedRet = g->retType;
    int   savedSeq = g->tmpSeq;
    g->retType = subst(g, f->ret);
    g->tmpSeq = 0;
    /* ---- One arena per block level ----
     * The number of levels is known at compile time, being the maximum nesting
     * depth of the blocks, so the array has a fixed length and lives on the
     * stack: setting up the arenas needs no allocation. `{0}` is enough, since
     * an `extc_arena` holds only a `top` pointer and NULL means empty. The
     * function body itself is level 1; genBlockBody increments on entry. */
    int maxLv = 1 + blkMaxOfBlock(f->body);
    /* A function that puts nothing into its own block arenas does not even emit
     * the array. The checker decides that (`f->mayUseArena`: the body contains a
     * `new`, or calls a function that has a home arena), and about half of the
     * functions in real programs are pure computation. cgReleaseLevel skips the
     * release for them as well. */
    bool savedNoArena = g->noArena;
    bool savedOwLocal = g->owLocal;
    g->owLocal = f->owLocal;
    /* The @overwrite storage cells, one per site, declared in the prologue.
     * The order matters: a cell is initialized with `&__extc_a[1]` (see `home`
     * below), so the list of sites has to be collected before the decision
     * whether to emit the arena array at all can be taken. */
    Vec savedOw = g->owSites;              /* by value: no arena exists for the first function */
    Vec owNow; vecInit(&owNow, g->arena, sizeof(Stmt *));
    collectOwSites(f->body, &owNow);
    g->owSites = owNow;
    Vec savedOwCalls = g->owCalls;
    Vec owcNow; vecInit(&owcNow, g->arena, sizeof(Expr *));
    collectOwCallsStmt(f->body, &owcNow);
    g->owCalls = owcNow;
    /* A cell is initialized with `&__extc_a[1]`, so the arena array must be
     * emitted whenever there is any cell, even in a function that allocates
     * nothing. An @overwrite variable in `main` is exactly that case, and
     * missing it left `__extc_a` undeclared. */
    g->noArena = !f->mayUseArena && !(owNow.len > 0 || owcNow.len > 0);
    if (!g->noArena)
        cgLine(g, "extc_arena __extc_a[%d] = {0};", maxLv + 1);
    if (f->owLocal)
        for (size_t i = 0; i < owNow.len; i++)
            /* `home` says which arena the storage belongs to: with a home, the
             * home arena, which outlives this call and therefore really is
             * reused; otherwise level 1 of this frame, which certainly outlives
             * the statement. It is never NULL. */
            cgLine(g, "extc_owcell __extc_ow%zu = { 0, 0, %s };", i,
                   f->needsHome ? "__extc_home" : "&__extc_a[1]");
    (void)owcNow;   /* no cells are prepared for a callee: they live in its frame */
    /* `owSites` must not be restored here: the body has not been generated yet,
     * and restoring it early made every lookup answer -1 and silently fall back
     * to allocating on every iteration. The restore happens at the end of
     * genFunc, together with `noArena`. */
    /* The slot the shared epilogue returns through; it is needed only when this
     * function really releases an arena on the way out. */
    bool retVoid = !g->retType || g->retType->kind == TY_VOID;
    if (!g->noArena && !retVoid)
        cgLine(g, "%s __extc_ret_v;", cType(g, g->retType));
    g->blkLevel = 0;
    g->loopLen  = 0;
    if (isMain && f->needsHome)
        cgLine(g, "extc_arena *__extc_home = &__extc_a[1];   /* main 的家 = 自己函数体 */");
    /* Only a self-recursive function needs the depth guard; see
     * funcCallsItself. Entering increments, every exit decrements, and
     * `extc_rec_enter` traps with a position once the limit is passed.
     *
     * The position reported is the line of the function declaration. The line of
     * the call site is what a user would rather see, but recursion comes back
     * around, so pointing at the declaration is the most readable choice. */
    g->isRecursive = funcCallsItself(g, f);
    if (g->isRecursive)
        cgLine(g, "extc_rec_enter(\"%s\", %d);", g->path, f->line);
    genBlockBody(g, f->body);
    /* Falling off the end of the body is an exit too, so the depth is
     * decremented here as well; otherwise the depth would only grow and a
     * legitimate program would be reported as a stack overflow. A function with
     * `noArena` has no epilogue below, so it must return on its own here, or the
     * trailing return would never be reached and the counter would never come
     * down. */
    if (g->isRecursive) {
        cgRecLeave(g);
        /* This return is emitted only when the body can fall off its end. When
         * the last statement already returns, it is unreachable, and in a
         * function with `noArena` it would name `__extc_ret_v`, which is not
         * declared there, so gcc reported an error; in every other case it is
         * dead code and is left out. */
        if (!(f->body && f->body->kind == ST_BLOCK && f->body->u.block.stmts.len > 0
              && stmtIsDefiniteReturn(*(Stmt **)vecAt(&f->body->u.block.stmts,
                                                       f->body->u.block.stmts.len - 1)))) {
            if (g->noArena) {
                if (retVoid) cgLine(g, "return;");
                else         cgLine(g, "return __extc_ret_v;");
            }
        }
    }
    /* The shared epilogue: the release sequence appears once, and falling off
     * the end of the body goes through it as well. The `goto` guarantees that
     * the label has a user, so there is no -Wunused-label warning.
     *
     * Every level is released, because a return may jump out of a deeper block;
     * releasing an already empty arena is a no-op. */
    if (!g->noArena) {
        cgLine(g, "goto __extc_ret;");
        g->indent--;
        cgLine(g, "__extc_ret:");
        g->indent++;
        for (int lv = 1; lv <= maxLv; lv++)
            cgLine(g, "extc_arena_release(&__extc_a[%d]);", lv);
        if (isMain)       cgLine(g, "return 0;");   /* main returns int in the generated C */
        else if (retVoid) cgLine(g, "return;");
        else              cgLine(g, "return __extc_ret_v;");
    }
    g->retType = savedRet;
    g->tmpSeq = savedSeq;
    g->noArena = savedNoArena;
    g->owSites = savedOw;          /* restored last, see the note above */
    g->owCalls = savedOwCalls;
    g->owLocal = savedOwLocal;
    g->indent--;
    cgLine(g, "}");
}

/* ------------------------------------------------------- generic instances
 *
 * The checker verifies a template once; code generation emits one copy of the C
 * per instance. The source of a container is written in extC, in the prelude,
 * and all the compiler does is substitute the type arguments for T.
 */

/* Everything generated is `static`, except `main`, whose linkage C fixes.
 *
 * This is not style but performance. extC emits a single .c file, one
 * translation unit, so external linkage buys nothing while forcing gcc to assume
 * that other code may call the function and that pointers may alias, which makes
 * it reject outer-loop vectorization outright.
 *
 * Measured on a matrix multiply of size n=1000 at OI scale, 10^9 multiply-adds,
 * with the same code differing only in linkage:
 *     static 208 ms   vs   extern 660 ms   => 3.2x
 * The reports from -fopt-info-vec-missed were "unsupported outerloop form" for
 * the extern version and "outer-loop already vectorized" for the static one.
 * Internal linkage also brings cross-function inlining and stronger constant
 * propagation for free, and `-Wl,--gc-sections` stops being the only safety net.
 *
 * Should extC ever support several translation units, which it does not today,
 * this has to become "export only what another unit uses".
 */

/* Is this function the entry point of the program?
 *
 * Returns:
 *   true for a free function named `main`.
 */
static bool cgIsMain(const FuncDef *f) {
    return f && !f->owner && f->name && strcmp(f->name, "main") == 0;
}

/* Emit the forward declaration of one function, so that definitions may appear
 * in any order.
 */
static void genFuncProto(CG *g, FuncDef *f) {
    Buf sig;
    bufInit(&sig, g->arena);
    bufPrintf(&sig, "%s%s %s(%s);", cgIsMain(f) ? "" : "static ", cType(g, f->ret),
              cFuncName(g, f), cgParamList(g, f));
    cgLine(g, "%s", bufCstr(&sig));
}

/* ------------------------------------------------------- struct definition order
 *
 * Ordinary structs and generic instances contain each other - a `player` holds a
 * `slice<u8>`, whose fields are ordinary `i64`s - so emitting all ordinary
 * structs first and the instances afterwards does not work. Definitions are
 * emitted in dependency order instead, which also covers types such as
 * `array<point>` that do not exist yet.
 *
 * A cycle can only close through a `ref`, that is through a pointer, and a
 * pointer needs nothing but the typedef of the type it points at, so all
 * typedefs are emitted first.
 */

typedef struct {
    StructDef *sd;      /* an ordinary struct; NULL for an instance or an array */
    Type      *inst;    /* a generic instance or an array; NULL for a struct */
    TypeDef   *td;      /* an enum with payloads; NULL otherwise */
    Vec        deps;    /* int* - indices of the units this one depends on */
    bool       done;
} SUnit;

/* Return the C name of one definition unit.
 *
 * Returns:
 *   The instance name of a generic instance or array, including a generic enum
 *   instance, the name of a non-generic enum with payloads, or the struct name.
 */
static const char *unitName(const SUnit *u) {
    if (u->inst) return u->inst->name;      /* instance or array, enum instances included */
    if (u->td) return u->td->name;          /* non-generic enum with payloads */
    return u->sd->name;
}

/* Find the unit that defines a type.
 *
 * Returns:
 *   The index of that unit in `units`, or -1 when there is none.
 *
 * Notes:
 *   - A generic instance is matched by C name, not by pointer, for two reasons.
 *     A `mut slice<T>` is a shadow sharing its name and its C struct with the
 *     read-only view, so comparing pointers never finds it, the dependency
 *     ordering misses it, and `struct reader { chunk: mut slice<u8> }` fails
 *     with an incomplete type. And one C name can have two instances, one for
 *     the writable and one for the read-only view.
 */
static int unitFind(Vec *units, Type *t) {
    if (!t) return -1;
    for (size_t i = 0; i < units->len; i++) {
        SUnit *u = *(SUnit **)vecAt(units, i);
        if (t->kind == TY_ENUM && u->td && t->edef == u->td) return (int)i;
        /* A generic instance is matched by C name, not by pointer. A `mut
         * slice<T>` is a shadow sharing its name and its C struct with the
         * read-only view, so comparing pointers never finds it and the
         * dependency ordering misses it, which made
         * `struct reader { chunk: mut slice<u8> }` fail with an incomplete
         * type. And one C name can have two instances, one for the writable and
         * one for the read-only view. */
        if ((t->kind == TY_GENERIC || t->kind == TY_ARRAY) && u->inst &&
            strcmp(u->inst->name, t->name) == 0) return (int)i;
        if (t->kind == TY_STRUCT && !u->inst && u->sd == t->sdef) return (int)i;
    }
    return -1;
}

/* Emit the definition of one struct, array or enum.
 *
 * An enum with payloads becomes `struct { tag; union }` and takes part in the
 * dependency ordering like any other struct, because a payload may contain
 * another struct: a variant holding a `slice<u8>` that was emitted before
 * `slice_u8` made gcc report an unknown type name, a trap that was hit for real.
 *
 * Params:
 *   u - the unit to emit
 */
static void unitBody(CG *g, SUnit *u) {
    if (u->td) {
        /* A generic enum instance such as `option<i64>` enters the
         * substitution context, so that `cType` substitutes the payload
         * types. */
        if (u->inst) substEnter(g, u->inst);
        cgLine(g, "struct %s {", unitName(u));
        g->indent++;
        cgLine(g, "int tag;");
        cgLine(g, "union {");
        g->indent++;
        for (size_t j = 0; j < u->td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&u->td->variants, j);
            if (v->types.len == 0) continue;
            Buf b;
            bufInit(&b, g->arena);
            bufPrintf(&b, "struct { ");
            for (size_t k = 0; k < v->types.len; k++) {
                if (k) bufPuts(&b, " ");
                bufPrintf(&b, "%s _%zu;", cType(g, *(Type **)vecAt(&v->types, k)), k);
            }
            bufPrintf(&b, " } %s;", v->name);
            cgLine(g, "%s", bufCstr(&b));
        }
        g->indent--;
        cgLine(g, "} u;");
        g->indent--;
        cgLine(g, "};");
        cgLine(g, "");
        if (u->inst) substLeave(g);
        return;
    }

    bool generic = u->inst && u->inst->kind == TY_GENERIC;
    if (generic) substEnter(g, u->inst);

    cgLine(g, "struct %s {", unitName(u));
    g->indent++;
    if (u->inst && u->inst->kind == TY_ARRAY) {
        /* An array is a struct holding a bare C array; that is where its value
         * semantics come from. */
        cgLine(g, "%s data[%lld];", cType(g, u->inst->inner),
               (long long)u->inst->asize);
    } else {
        for (size_t i = 0; i < u->sd->fields.len; i++) {
            FieldDef *fd = *(FieldDef **)vecAt(&u->sd->fields, i);
            cgLine(g, "%s %s;", cType(g, fd->type), fd->name);
        }
        /* C does not allow an empty struct: `struct unit { };` is a GNU
         * extension, and `(unit){0}` then warns about excess elements. A struct
         * with no fields therefore gets one placeholder byte. A user never sees
         * it and `(T){0}` stays legal; `unit`, the payload type of
         * `result<unit, E>`, exists thanks to this. */
        if (u->sd->fields.len == 0) cgLine(g, "char __extc_empty;");
    }
    g->indent--;
    cgLine(g, "};");
    cgLine(g, "");
    if (generic) substLeave(g);
}

/* The derived `_eq` for arrays is gone: array equality now goes through
 * `extc_eq(&a, &b, &arr_desc)`, one descriptor plus one recursive
 * implementation. The old shape emitted a loop per array type, which cost 2450
 * lines, 32% of the generated C, for 200 types in a stress program.
 */

/* Register the slice helper of one base type and return its name.
 *
 * Registration deduplicates: the same base and kind registered twice yields the
 * same name and one emitted function.
 *
 * Params:
 *   ob   - type being sliced: an array or a view
 *   st   - resulting slice type
 *   tail - true for "slice to the end" (`s[lo..]`), where the omitted bound is
 *          the length of the base
 *
 * Returns:
 *   The helper name, `extc_slice` or `extc_sliceTo` plus the C name of the base.
 *   One base type is only ever sliced into one kind of slice, so the names
 *   cannot collide. The concatenated name is deliberately not spelled out here:
 *   a repository-wide check rejects the C name pattern of a slice type in the
 *   source, comments included.
 */
static const char *sliceHelper(CG *g, Type *ob, Type *st, bool tail) {
    const char *name = arenaPrintf(g->arena, "extc_slice%s_%s",
                                   tail ? "To" : "", ob->name);
    for (size_t i = 0; i < g->helpers.len; i++)
        if (strcmp(((SliceHelper *)vecAt(&g->helpers, i))->name, name) == 0)
            return name;

    const char *ret = cType(g, st);
    Buf b;
    bufInit(&b, g->arena);
    if (ob->kind == TY_ARRAY) {
        /* A fixed-size array: the length is a compile-time constant. */
        bufPrintf(&b, "static %s %s(%s *a, int64_t lo, int64_t hi,\n",
                  ret, name, ob->name);
        bufPrintf(&b, "                       const char *f, int ln) {\n");
        bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, hi, %lld, f, ln);\n",
                  (long long)ob->asize);
        bufPrintf(&b, "    return (%s){ .data = &a->data[s], .len = hi - lo };\n}\n", ret);
    } else if (tail) {
        /* Slicing to the end: hi is v.len. It is taken as a parameter instead
         * of being expanded in place, so the view is evaluated once. */
        bufPrintf(&b, "static %s %s(%s v, int64_t lo, const char *f, int ln) {\n",
                  ret, name, ob->name);
        bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, v.len, v.len, f, ln);\n");
        bufPrintf(&b, "    return (%s){ .data = v.data + s, .len = v.len - lo };\n}\n", ret);
    } else {
        bufPrintf(&b, "static %s %s(%s v, int64_t lo, int64_t hi,\n",
                  ret, name, ob->name);
        bufPrintf(&b, "                       const char *f, int ln) {\n");
        bufPrintf(&b, "    int64_t s = extc_checkedRange(lo, hi, v.len, f, ln);\n");
        bufPrintf(&b, "    return (%s){ .data = v.data + s, .len = hi - lo };\n}\n", ret);
    }

    SliceHelper *h = (SliceHelper *)vecPush(&g->helpers);
    h->name = name;
    h->text = bufCstr(&b);
    return name;
}

/* Emit the view produced by `a[lo..hi]`.
 *
 * What the compiler can prove leaves no trace at runtime, and that splits this
 * in two:
 *   1. The base is a fixed-size array and both bounds are literals, with the
 *      checker filling in an omitted bound. The range is then known when the
 *      program is checked and an out-of-range one was reported there, so the
 *      generated C is `&a.data[2]` with `.len = 3` and contains no check at all.
 *   2. Anything else gets a helper that checks at runtime and traps with the
 *      extC position.
 *
 * Returns:
 *   The expression for the view, or "0" after reporting an error when the types
 *   make no sense.
 */
static const char *genSlice(CG *g, Expr *e) {
    Type *ob = ttBase(subst(g, e->u.slice.obj->type));
    Type *st = subst(g, e->type);
    if (!ob || !st || !ob->name) {
        ctxError(g->ctx, e->line, 1, NULL, "cannot generate a slice of this type");
        return "0";
    }

    const char *obj = genExpr(g, e->u.slice.obj);
    Expr *lo = e->u.slice.lo, *hi = e->u.slice.hi;
    /* When the base is a `ref [N]T`, which is a pointer in C, reading `.data`
     * needs one dereference, as in `(*p).data[..]`, but the argument passed to
     * the slice helper is that pointer itself, since the helper takes the
     * `array_*` type. Without the dereference the result is `p.data[..]`,
     * and gcc answers "'p' is a pointer". */
    bool objIsRef = e->u.slice.obj->type &&
                    subst(g, e->u.slice.obj->type)->kind == TY_REF;

    if (ob->kind == TY_ARRAY && lo && hi &&
        lo->kind == EX_INT && hi->kind == EX_INT) {
        const char *arr = objIsRef ? arenaPrintf(g->arena, "(*%s)", obj) : obj;
        return arenaPrintf(g->arena, "(%s){ .data = &(%s.data[%lld]), .len = %lld }",
                           cType(g, st), arr, lo->u.ival, hi->u.ival - lo->u.ival);
    }

    const char *loS = lo ? genExpr(g, lo) : "0";
    const char *arg = ob->kind != TY_ARRAY ? obj
                      : (objIsRef ? obj : arenaPrintf(g->arena, "&(%s)", obj));
    if (!hi) {
        /* Only a view base can reach here: for an array base the checker has
         * already filled the omitted bound in as a literal. */
        const char *fn = sliceHelper(g, ob, st, true);
        return arenaPrintf(g->arena, "%s(%s, (int64_t)(%s), \"%s\", %d)",
                           fn, arg, loS, g->path, e->line);
    }
    const char *fn = sliceHelper(g, ob, st, false);
    const char *hiS = genExpr(g, hi);
    return arenaPrintf(g->arena, "%s(%s, (int64_t)(%s), (int64_t)(%s), \"%s\", %d)",
                       fn, arg, loS, hiS, g->path, e->line);
}


/* Close the set of generated instances transitively.
 *
 * An instance mentioned only in a method signature never reached `units` unless
 * the program used it directly, so `varArray<i64>::get()` returning `option<i64>`
 * produced C that named an unknown type. The user had written nothing wrong; the
 * compiler had simply not finished its own accounting.
 *
 * The set is closed by walking the fields, the method signatures and the enum
 * payloads of every unit, substituting the type arguments of a generic instance,
 * and adding every instance met on the way until nothing new appears. The
 * iteration count is capped, and passing the cap is reported loudly rather than
 * quietly generating too little.
 */
/* Add the unit of a generic instance that a type mentions, if it is missing.
 *
 * Params:
 *   units - the unit list, appended to in place
 *   t     - a type; anything that is not an instance is ignored
 *
 * Notes:
 *   - Deduplication is by C name and cannot use unitFind; see the comment at the
 *     lookup below.
 */
static void addInstanceUnit(Arena *arena, Vec *units, Type *t) {
    if (!t) return;
    bool isStructInst = (t->kind == TY_GENERIC && t->sdef);          /* varArray<i32> */
    bool isEnumInst   = (t->kind == TY_ENUM && t->edef && t->edef->typeParams.len > 0);
    if (!isStructInst && !isEnumInst) return;
    /* Deduplication must be by C name and cannot go through unitFind, which
     * compares a generic enum instance by its template: `option_i64` would match
     * an existing `option_i32`, since the two share the template, be considered
     * already present, and never be generated, leaving the generated C with an
     * unknown type name. The original collection path walked the instance list
     * of the type table and so never exposed this. */
    for (size_t i = 0; i < units->len; i++) {
        SUnit *u = *(SUnit **)vecAt(units, i);
        if (u->inst && t->name && strcmp(u->inst->name, t->name) == 0) return;
        if (u->td && t->kind == TY_ENUM && u->inst == NULL && u->td == t->edef) return;
    }
    SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
    if (isStructInst) u->sd = t->sdef; else u->td = t->edef;
    u->inst = t;
    vecInit(&u->deps, arena, sizeof(int));
    *(SUnit **)vecPush(units) = u;
}

/* Scan one type, adding every instance it mentions.
 *
 * References, slice and array elements, and generic arguments are all followed.
 *
 * Params:
 *   depth - recursion guard: nesting deeper than 12 stops the walk, which is
 *           what bounds a self-referential type
 */
static void scanTypeForUnits(Arena *arena, Vec *units, Type *t, int depth) {
    if (!t || depth > 12) return;                                    /* bounds self-reference */
    addInstanceUnit(arena, units, t);
    if (t->inner) scanTypeForUnits(arena, units, t->inner, depth + 1);
    for (size_t i = 0; i < t->targs.len; i++)
        scanTypeForUnits(arena, units, *(Type **)vecAt(&t->targs, i), depth + 1);
}

/* Scan everything one unit mentions and add the instances found.
 *
 * Three places are walked: the fields, the signatures of the methods - both the
 * parameters and the return type, which is where the closure used to miss an
 * instance - and the payloads of an enum's variants. A generic instance
 * substitutes its own type arguments first.
 */
static void scanUnitForUnits(Arena *arena, TypeTable *tt, Vec *units, SUnit *u) {
    Vec *tps = NULL; Vec *tas = NULL;
    StructDef *sd = u->sd ? u->sd : (u->inst ? u->inst->sdef : NULL);
    if (u->inst && sd) { tps = &sd->typeParams; tas = &u->inst->targs; }
    /* 1. fields */
    if (sd) for (size_t i = 0; i < sd->fields.len; i++) {
        Type *ft = (*(FieldDef **)vecAt(&sd->fields, i))->type;
        if (tps && ft) ft = ttSubstitute(tt, ft, tps, tas);
        scanTypeForUnits(arena, units, ft, 0);
    }
    /* 2. method signatures, parameters and return type: the place that was missed */
    if (sd) for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&sd->methods, i);
        for (size_t j = 0; j < f->params.len; j++) {
            Type *pt = (*(Param **)vecAt(&f->params, j))->type;
            if (tps && pt) pt = ttSubstitute(tt, pt, tps, tas);
            scanTypeForUnits(arena, units, pt, 0);
        }
        Type *rt = f->ret;
        if (tps && rt) rt = ttSubstitute(tt, rt, tps, tas);
        scanTypeForUnits(arena, units, rt, 0);
    }
    /* 3. enum payloads */
    if (u->td) for (size_t i = 0; i < u->td->variants.len; i++) {
        Variant *v = *(Variant **)vecAt(&u->td->variants, i);
        for (size_t k = 0; k < v->types.len; k++) {
            Type *pt = *(Type **)vecAt(&v->types, k);
            if (tps && pt) pt = ttSubstitute(tt, pt, tps, tas);
            scanTypeForUnits(arena, units, pt, 0);
        }
    }
}

/* Generate the whole C translation unit for a module.
 *
 * Params:
 *   lineMap - whether to emit `#line` directives
 *   out     - receives the generated text
 *
 * Returns:
 *   false when the checker recorded an error, true otherwise. Reporting a
 *   consistency problem never stops generation, so that the caller sees the
 *   error count and not a damaged output buffer.
 */
bool generateC(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m, bool lineMap, Buf *out) {
    CG g;
    memset(&g, 0, sizeof g);
    g.arena = arena;
    g.ctx = ctx;
    g.out = out;
    g.path = ctx->path;
    g.lineMap = lineMap;
    g.indent = 0;
    g.tt = tt;

    vecInit(&g.structs, arena, sizeof(void *));
    bufInit(&g.prefix, arena);          /* statement prefix; uninitialized, it segfaults */
    /* Deduplicate by C name: a writable and a read-only view are the same C
     * struct - `slice<mut slice<T>>` and `slice<slice<T>>` are both
     * `slice_slice_T` - while `ttEquals` counts `mut` as part of the identity,
     * so the type table can hold two instances under one name. Deduplicating by
     * name keeps every later piece from being generated twice. */
    vecInit(&g.insts, arena, sizeof(void *));
    for (size_t i = 0; i < tt->instances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->instances, i);
        bool dup = false;
        for (size_t k = 0; k < g.insts.len && !dup; k++)
            dup = strcmp((*(Type **)vecAt(&g.insts, k))->name, it->name) == 0;
        if (!dup) *(Type **)vecPush(&g.insts) = it;
    }
    vecInit(&g.funcs, arena, sizeof(void *));
    vecInit(&g.helpers, arena, sizeof(SliceHelper));
    vecInit(&g.descs, arena, sizeof(void *));
    vecInit(&g.eqNeed, arena, sizeof(void *));
    bufInit(&g.desc, arena);
    bufInit(&g.rt, arena);              /* print/compare runtime: emitted only if needed */
    bufInit(&g.rtPrint, arena);
    bufInit(&g.rtEq, arena);
    bufInit(&g.body, arena);
    g.tmpSeq = 0;
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        if (sd->typeParams.len > 0) continue;   /* generic: generated per instance */
        *(StructDef **)vecPush(&g.structs) = sd;
        /* Methods are functions too and share the prototype and definition table. */
        for (size_t j = 0; j < sd->methods.len; j++)
            *(FuncDef **)vecPush(&g.funcs) = *(FuncDef **)vecAt(&sd->methods, j);
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        /* The generic template itself is not generated: `T` is still a
         * parameter, so the result would be wrong C that does not compile. Only
         * the instances are emitted, those with `f->tmpl != NULL`. Without this
         * `continue`, both the template and its instances were emitted and the
         * unsubstituted `T` in the template produced a false error. */
        if (f->typeParams.len > 0) continue;
        *(FuncDef **)vecPush(&g.funcs) = f;
    }

    bufPuts(out,
        "/* Generated by the extC compiler -- do not edit by hand.\n"
        " * The generated C is deliberately boring: no macro tricks, no clever optimizations.\n"
        " * `#line` directives map the C compiler's errors back to the .extc source lines.\n"
        " */\n"
        "#include <stdint.h>\n"
        "#include <stdbool.h>\n"
        "#include <stddef.h>\n"      /* offsetof, needed by the descriptor tables */
        "#include <stdio.h>\n"
        "#include <string.h>\n"
        "#include <stdlib.h>\n\n"
        "/* ⚠️ 下面这些原语全部 `static inline` —— **这不是风格问题**：\n"
        " * 不内联的话，gcc 在 -O1 下**看不见检查体**，于是既不能消掉检查、\n"
        " * 也不能把 `i % 7` 变成乘法+移位。实测（2026-09-20）：取模慢 4.6 倍、\n"
        " * 矩阵乘慢 1.5 倍；加了 inline 之后**全部追平 C** ✓ */\n"
        "/* 越界 trap：带 extC 的位置（由 `#line` 与调用点传进来的 file/line 保证）*/\n"
        "static inline void extc_trap(const char *file, int line, int64_t i, int64_t n) {\n"
        "    fprintf(stderr, \"%s:%d: trap: index %lld out of range (length %lld)\\n\",\n"
        "            file, line, (long long)i, (long long)n);\n"
        "    exit(1);\n"
        "}\n"
        "/* ---- 算术的失败必须**响亮**（LANGUAGE.md 0.5）：\n"
        " * 除零、除法的溢出、移位超宽，在 C 里都是 UB —— 我们让它 trap 带源码位置。\n"
        " * 不静默算错，也不留 UB ✓ */\n"
        "static inline void extc_trapMsg(const char *file, int line, const char *msg) {\n"
        "    fprintf(stderr, \"%s:%d: trap: %s\\n\", file, line, msg);\n"
        "    exit(1);\n"
        "}\n"
        /* The recursion depth guard, used only by self-recursive functions.
         * Runaway recursion used to be hard to diagnose: gcc folded it into a
         * loop that printed nothing, or the stack really overflowed and the OS
         * reported a segmentation fault, which is neither an extC message nor
         * carries a position. A depth counter maintained on entry and exit of a
         * self-recursive function now traps with a position, and it triggers far
         * earlier than a real overflow: with the default 8 MB stack and frames
         * of a few hundred bytes, a hundred thousand levels are comfortable. */
         "#ifndef EXTC_REC_LIMIT\n"
         "#define EXTC_REC_LIMIT 100000\n"
         "#endif\n"
         "static int64_t __extc_rec_depth = 0;\n"
        /* Called by the prologue of a self-recursive function; it traps with
         * the position of the call site, which points at the offending line. */
        "static inline void extc_rec_enter(const char *f, int l) {\n"
        "    if (++__extc_rec_depth > EXTC_REC_LIMIT)\n"
        "        extc_trapMsg(f, l, \"recursion too deep (unbounded recursion?)\");\n"
        "}\n"
        "static inline int64_t extc_divI(int64_t a, int64_t b, const char *f, int l) {\n"
        "    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n"
        "    if (a == INT64_MIN && b == -1) extc_trapMsg(f, l, \"integer overflow in division\");\n"
        "    return a / b;\n"
        "}\n"
        "static inline int64_t extc_modI(int64_t a, int64_t b, const char *f, int l) {\n"
        "    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n"
        "    if (a == INT64_MIN && b == -1) extc_trapMsg(f, l, \"integer overflow in division\");\n"
        "    return a % b;\n"
        "}\n"
        "static inline uint64_t extc_divU(uint64_t a, uint64_t b, const char *f, int l) {\n"
        "    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n"
        "    return a / b;\n"
        "}\n"
        "static inline uint64_t extc_modU(uint64_t a, uint64_t b, const char *f, int l) {\n"
        "    if (b == 0) extc_trapMsg(f, l, \"division by zero\");\n"
        "    return a % b;\n"
        "}\n"
        "/* 移位：C 里移 >= 位宽 或 负数 都是 UB ⇒ 检查移位数，返回它（求值一次）*/\n"
        "static inline int64_t extc_shiftCount(int64_t b, int64_t w, const char *f, int l) {\n"
        "    if (b < 0 || b >= w) extc_trapMsg(f, l, \"shift count out of range\");\n"
        "    return b;\n"
        "}\n"
        "/* 带越界检查的下标：**返回下标**，所以调用点只求值一次。*/\n"
        "static inline int64_t extc_checkedIndex(int64_t i, int64_t n, const char *file, int line) {\n"
        "    if (i < 0 || i >= n) extc_trap(file, line, i, n);\n"
        "    return i;\n"
        "}\n"
        "/* 带范围检查的切片：要求 0 <= lo <= hi <= n，返回 lo。*/\n"
        "static inline int64_t extc_checkedRange(int64_t lo, int64_t hi, int64_t n,\n"
        "                          const char *file, int line) {\n"
        "    if (lo < 0 || hi < lo || hi > n) {\n"
        "        fprintf(stderr, \"%s:%d: trap: slice %lld..%lld is out of range (length %lld)\\n\",\n"
        "                file, line, (long long)lo, (long long)hi, (long long)n);\n"
        "        exit(1);\n"
        "    }\n"
        "    return lo;\n"
        "}\n\n");
    /* The @overwrite cell type is emitted only when it is really used: emitted
     * unconditionally it would add a line to the generated C of every program
     * and fill the golden files with noise. The type contains an
     * `extc_arena *home`, so it must come after `extc_arena`; therefore only
     * `needOw` is computed here and the typedef itself is emitted further down,
     * inside the arena section. */
    bool needOw = false;
    {
        for (size_t i = 0; i < m->funcs.len && !needOw; i++)
            if ((*(FuncDef **)vecAt(&m->funcs, i))->owSites > 0) needOw = true;
        for (size_t i = 0; i < m->structs.len && !needOw; i++) {
            StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
            for (size_t j = 0; j < sd->methods.len; j++)
                if ((*(FuncDef **)vecAt(&sd->methods, j))->owSites > 0) { needOw = true; break; }
        }
        (void)needOw;   /* the typedef needs extc_arena, so it comes later */
    }
    bufPuts(out,
        /* ------------------------------------------------------------------
         * The arena: one per frame, and allocations go through a handle.
         *
         * An arena is one lexical scope, implemented as a linked list of blocks:
         * allocating pushes space in the current block, releasing hands the whole
         * chain back to the system.
         *
         * A handle, an `extc_arena *`, rather than "the current arena" is what
         * lets a container remember which arena it was born in and ask that one
         * for room when it grows. Memory a method allocates therefore outlives
         * the method call, which is the rule that a container allocates into the
         * region where the container itself lives.
         *
         * There is no shared state in the process, since every frame has its own
         * object, so threading will not have to change this structure.
         * ------------------------------------------------------------------ */
        "typedef struct extc_ablock { struct extc_ablock *prev; int64_t cap, used; char data[1]; } extc_ablock;\n"
        "typedef struct extc_arena { extc_ablock *top; } extc_arena;\n"

        "static inline void extc_arena_init(extc_arena *a) { a->top = NULL; }\n"
        "static inline void extc_arena_release(extc_arena *a) {\n"
        "    while (a->top) { extc_ablock *p = a->top->prev; free(a->top); a->top = p; }\n"
        "}\n"
        "/* 显式转换用（PLAN #23）：整数收窄 / 换符号 / 浮点转整数 ⇒ 装不下就 trap（带位置）*/\n"
        "static inline int64_t extc_narrowI(int64_t v, int64_t lo, int64_t hi, const char *f, int l) {\n"
        "    if (v < lo || v > hi) extc_trapMsg(f, l, \"value does not fit in the target type\");\n"
        "    return v;\n"
        "}\n"
        "static inline uint64_t extc_narrowU(uint64_t v, uint64_t hi, const char *f, int l) {\n"
        "    if (v > hi) extc_trapMsg(f, l, \"value does not fit in the target type\");\n"
        "    return v;\n"
        "}\n"
        "static inline int64_t extc_convFloat(double v, int64_t lo, int64_t hi,"
        " const char *f, int l) {\n"
        "    if (!(v >= (double)lo && v <= (double)hi))"
        " extc_trapMsg(f, l, \"float does not fit in the target integer type\");\n"
        "    return (int64_t)v;   /* 向零截断 = C 的规则 ✓ */\n"
        "}\n"
        "void *extc_arena_alloc(extc_arena *a, int64_t n, const char *f, int l) {\n"
        "    if (n <= 0) n = 1;\n"
        "    n = (n + 7) & ~(int64_t)7;\n"
        "    if (!a->top || a->top->cap - a->top->used < n) {\n"
        "        int64_t cap = n > 4096 ? n : 4096;\n"
        "        extc_ablock *b = (extc_ablock *)malloc(sizeof(extc_ablock) + (size_t)cap);\n"
        /* Like every other trap, this one carries a source position. It used to
         * print a bare "out of arena memory" and exit, which breaks the rule
         * that a failure the compiler can locate must say where it happened. */
        "        if (!b) { fprintf(stderr, \"%s:%d: trap: out of arena memory\"\n"
        "                        \" (this allocation wanted %lld bytes)\\n\",\n"
        "                        f, l, (long long)n); exit(1); }\n"
        "        b->prev = a->top; b->cap = cap; b->used = 0;\n"
        "        a->top = b;\n"
        "    }\n"
        "    {\n"
        "        void *p = a->top->data + a->top->used;\n"
        "        a->top->used += n;\n"
        "        memset(p, 0, (size_t)n);   /* ⭐ 分配**永远清零** */\n"
        "        return p;\n"
        "    }\n"
        "}\n\n");

    /* The @overwrite cell type, emitted on demand: emitted unconditionally it
     * would change every golden file. It must come after `extc_arena`, because
     * its `home` field says which arena the storage belongs to. */
    if (needOw)
        bufPuts(out, "typedef struct extc_owcell { void *p; int64_t cap; extc_arena *home; } extc_owcell;\n");

    /* ==================================================================
     * The descriptor table and the single generic printer.
     *
     * Before, one `_debug` print function was derived per type used: a synthetic
     * stress program of N=1000 had 2028 of them, 22.9% of the generated C, and
     * never printed a single struct. Now every type costs one compile-time
     * constant in .rodata, the printing logic exists once for the whole program,
     * and whether to emit it at all is decided on demand, by the reachability
     * closure of the descriptors.
     *
     * This is not runtime reflection: the descriptors are static data, and the
     * types, field offsets and variant names in them are compile-time constants
     * that gcc can see throughout, so nothing is weakened about a failure being
     * loud when it cannot be proven impossible.
     *
     * The same table can later feed structural `==`, serialization or hashing.
     * ================================================================== */
    bufPuts(&g.rt,
        "/* ---- 类型描述表 ----\n"
        " * 每类型一份静态数据；`extc_print` 全程序只有一份。\n"
        " * size 的含义：标量/结构体 = sizeof(T)；数组/切片 = **元素步长**。\n"
        " * 视图的 C 布局固定是 `{ T *data; int64_t len; }`（见 genViewUnit）。\n"
        " */\n"
        "enum {\n"
        "    EXTC_D_I8, EXTC_D_I16, EXTC_D_I32, EXTC_D_I64,\n"
        "    EXTC_D_U8, EXTC_D_U16, EXTC_D_U32, EXTC_D_U64,\n"
        "    EXTC_D_F32, EXTC_D_F64, EXTC_D_BOOL,\n"
        "    EXTC_D_ENUM,      /* table = const char *const[]，tag 在偏移 0 */\n"
        "    EXTC_D_STRUCT,    /* table = ExtcField[] */\n"
        "    EXTC_D_ARRAY,     /* 定长数组：count 个 elem */\n"
        "    EXTC_D_SLICE,     /* 视图：{ T *data; int64_t len; } */\n"
        "    EXTC_D_TEXT,      /* slice<u8>：按**文本**印（跟别的切片不一样）*/\n"
        "    EXTC_D_REF        /* ref / ?ref：一律印 <ref> */\n"
        "};\n"
        "\n"
        "typedef struct ExtcDesc ExtcDesc;\n"
        "typedef struct { const char *name; size_t off; const ExtcDesc *desc; } ExtcField;\n"
        "\n"
        "struct ExtcDesc {\n"
        "    int             kind;\n"
        "    const char     *name;    /* 结构体/枚举的显示名（打印用）*/\n"
        "    size_t          size;    /* 标量/结构体 = sizeof(T)；数组/切片 = 元素步长 */\n"
        "    size_t          count;   /* 字段数 / 变体数 / 数组长度 */\n"
        "    const void     *table;   /* ExtcField[] 或 const char *const[] */\n"
        "    const ExtcDesc *elem;    /* 数组/切片的元素 */\n"
        "    /* ⭐ 结构化 `==` 用：**只有 struct 才填** —— 那是用户（或库）写的 `fn ==`，\n"
        "     * 是**任意代码**，只能委托不能重造 ✓ codegen 为它生成一行适配器。\n"
        "     * 其余 kind 由 `extc_eq` 自己递归 ⇒ 这一格留空（位置在最后 ⇒ 老初始化式\n"
        "     * 少写一个也自动补 0 ✓）*/\n"
        "    bool          (*eq)(const void *a, const void *b);\n"
        "};\n"
        "\n"
        "/* 不依赖具体类型的四只：引用 / 字节视图 / 标量 —— 全程序共享 ✓ */\n"
        "static const ExtcDesc extc_desc_ref  = { EXTC_D_REF,  \"ref\",  sizeof(void *), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_text = { EXTC_D_TEXT, \"slice<u8>\", 1, 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_bool = { EXTC_D_BOOL, \"bool\", sizeof(bool), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_i8  = { EXTC_D_I8,  \"i8\",  sizeof(int8_t),  0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_i16 = { EXTC_D_I16, \"i16\", sizeof(int16_t), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_i32 = { EXTC_D_I32, \"i32\", sizeof(int32_t), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_i64 = { EXTC_D_I64, \"i64\", sizeof(int64_t), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_u8  = { EXTC_D_U8,  \"u8\",  sizeof(uint8_t),  0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_u16 = { EXTC_D_U16, \"u16\", sizeof(uint16_t), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_u32 = { EXTC_D_U32, \"u32\", sizeof(uint32_t), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_u64 = { EXTC_D_U64, \"u64\", sizeof(uint64_t), 0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_f32 = { EXTC_D_F32, \"f32\", sizeof(float),  0, NULL, NULL };\n"
        "static const ExtcDesc extc_desc_f64 = { EXTC_D_F64, \"f64\", sizeof(double), 0, NULL, NULL };\n"
        "\n");
    /* Split into two calls: C99 only guarantees support for string literals of
     * 4095 characters, and one large literal would trigger -Woverlength-strings.
     * That is not an error, but there is no reason to keep the noise. */
    bufPuts(&g.rtPrint,
        "/* 通用递归打印器 —— 输出格式必须跟以前派生的 `_debug` **逐字节一致** ✓\n"
        " * （真值表见 tools/print-formats.txt：浮点 %g、[N]u8 按数字、slice<u8> 按文本 ……）*/\n"
        "static void extc_print(const void *p, const ExtcDesc *d) {\n"
        "    switch (d->kind) {\n"
        "    case EXTC_D_I8:   printf(\"%d\", (int)*(const int8_t *)p); return;\n"
        "    case EXTC_D_I16:  printf(\"%d\", (int)*(const int16_t *)p); return;\n"
        "    case EXTC_D_I32:  printf(\"%d\", (int)*(const int32_t *)p); return;\n"
        "    case EXTC_D_I64:  printf(\"%lld\", (long long)*(const int64_t *)p); return;\n"
        "    case EXTC_D_U8:   printf(\"%u\", (unsigned)*(const uint8_t *)p); return;\n"
        "    case EXTC_D_U16:  printf(\"%u\", (unsigned)*(const uint16_t *)p); return;\n"
        "    case EXTC_D_U32:  printf(\"%u\", (unsigned)*(const uint32_t *)p); return;\n"
        "    case EXTC_D_U64:  printf(\"%llu\", (unsigned long long)*(const uint64_t *)p); return;\n"
        "    case EXTC_D_F32:  printf(\"%g\", (double)*(const float *)p); return;\n"
        "    case EXTC_D_F64:  printf(\"%g\", (double)*(const double *)p); return;\n"
        "    case EXTC_D_BOOL: printf(\"%s\", *(const bool *)p ? \"true\" : \"false\"); return;\n"
        "    case EXTC_D_REF:  printf(\"<ref>\"); return;\n"
        "    case EXTC_D_TEXT: {\n"
        "        int64_t n = *(const int64_t *)((const char *)p + sizeof(void *));\n"
        "        printf(\"%.*s\", (int)n, (const char *)*(const void *const *)p);\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_ENUM: {\n"
        "        const char *const *names = (const char *const *)d->table;\n"
        "        int tag = *(const int *)p;\n"
        "        if (tag < 0 || (size_t)tag >= d->count) printf(\"<%s>\", d->name);\n"
        "        else                                    printf(\"%s\", names[tag]);\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_STRUCT: {\n"
        "        const ExtcField *f = (const ExtcField *)d->table;\n"
        "        printf(\"%s { \", d->name);\n"
        "        for (size_t i = 0; i < d->count; i++) {\n"
        "            if (i) printf(\", \");\n"
        "            printf(\"%s: \", f[i].name);\n"
        "            extc_print((const char *)p + f[i].off, f[i].desc);\n"
        "        }\n"
        "        printf(\" }\");\n"
        "        return;\n"
        "    }\n"
        "    case EXTC_D_ARRAY:\n"
        "    case EXTC_D_SLICE: {\n"
        "        const char *data;\n"
        "        size_t n;\n"
        "        if (d->kind == EXTC_D_SLICE) {\n"
        "            const int64_t len = *(const int64_t *)((const char *)p + sizeof(void *));\n"
        "            data = (const char *)*(const void *const *)p;\n"
        "            n = len > 0 ? (size_t)len : 0;\n"
        "        } else {\n"
        "            data = (const char *)p;\n"
        "            n = d->count;\n"
        "        }\n"
        "        printf(\"[\");\n"
        "        for (size_t i = 0; i < n; i++) {\n"
        "            if (i) printf(\", \");\n"
        "            extc_print(data + i * d->size, d->elem);\n"
        "        }\n"
        "        printf(\"]\");\n"
        "        return;\n"
        "    }\n"
        "    }\n"
        "}\n\n");

    /* Structural `==`, sharing the same descriptor table as `extc_print`.
     *
     * The semantics have to match the old per-array-type `_eq` bit for bit:
     *   - a scalar or an enum compares with `==` directly; an enum compares its
     *     tag only, and an enum with payloads is not comparable at all, which
     *     the checker rejects long before this point
     *   - an array recurses element by element, and a slice compares lengths
     *     first and then recurses element by element, exactly like
     *     `slice<T>::==` in the prelude
     *   - a struct delegates to the `fn ==` written by the user or the library,
     *     which is the `eq` slot of its descriptor, filled in by a one-line
     *     adapter that code generation emits
     * So "one `_eq` function per array type" became "one unit of data per type":
     * 200 distinct array types used to cost 2450 lines of derived code.
     */
    bufPuts(&g.rtEq,
        "static bool extc_eq(const void *a, const void *b, const ExtcDesc *d) {\n"
        "    switch (d->kind) {\n"
        "    case EXTC_D_I8:  return *(const int8_t *)a  == *(const int8_t *)b;\n"
        "    case EXTC_D_I16: return *(const int16_t *)a == *(const int16_t *)b;\n"
        "    case EXTC_D_I32: return *(const int32_t *)a == *(const int32_t *)b;\n"
        "    case EXTC_D_I64: return *(const int64_t *)a == *(const int64_t *)b;\n"
        "    case EXTC_D_U8:  return *(const uint8_t *)a  == *(const uint8_t *)b;\n"
        "    case EXTC_D_U16: return *(const uint16_t *)a == *(const uint16_t *)b;\n"
        "    case EXTC_D_U32: return *(const uint32_t *)a == *(const uint32_t *)b;\n"
        "    case EXTC_D_U64: return *(const uint64_t *)a == *(const uint64_t *)b;\n"
        "    case EXTC_D_F32: return *(const float *)a  == *(const float *)b;\n"
        "    case EXTC_D_F64: return *(const double *)a == *(const double *)b;\n"
        "    case EXTC_D_BOOL: return *(const bool *)a == *(const bool *)b;\n"
        "    case EXTC_D_REF:  return *(const void *const *)a == *(const void *const *)b;\n"
        "    case EXTC_D_ENUM: return *(const int *)a == *(const int *)b;\n"
        "    case EXTC_D_TEXT: {\n"
        "        const int64_t la = *(const int64_t *)((const char *)a + sizeof(void *));\n"
        "        const int64_t lb = *(const int64_t *)((const char *)b + sizeof(void *));\n"
        "        if (la != lb) return false;\n"
        "        if (la <= 0) return true;      /* 空视图：data 可能是 null ⇒ 不比 ✓ */\n"
        "        return memcmp(*(const void *const *)a, *(const void *const *)b, (size_t)la) == 0;\n"
        "    }\n"
        "    case EXTC_D_STRUCT:\n"
        "        /* 用户写的 `fn ==`（适配器）；没有就说明根本不该被比 ✓ */\n"
        "        return d->eq ? d->eq(a, b) : false;\n"
        "    case EXTC_D_ARRAY:\n"
        "    case EXTC_D_SLICE: {\n"
        "        const char *pa, *pb;\n"
        "        size_t n;\n"
        "        if (d->kind == EXTC_D_SLICE) {\n"
        "            const int64_t la = *(const int64_t *)((const char *)a + sizeof(void *));\n"
        "            const int64_t lb = *(const int64_t *)((const char *)b + sizeof(void *));\n"
        "            if (la != lb) return false;\n"
        "            pa = (const char *)*(const void *const *)a;\n"
        "            pb = (const char *)*(const void *const *)b;\n"
        "            n = la > 0 ? (size_t)la : 0;\n"
        "        } else {\n"
        "            pa = (const char *)a;\n"
        "            pb = (const char *)b;\n"
        "            n = d->count;\n"
        "        }\n"
        "        for (size_t i = 0; i < n; i++)\n"
        "            if (!extc_eq(pa + i * d->size, pb + i * d->size, d->elem)) return false;\n"
        "        return true;\n"
        "    }\n"
        "    }\n"
        "    return false;\n"
        "}\n\n");

    /* Enums come first: C11 cannot forward-declare an enum tag, so a struct
     * field of enum type needs the definition to be there already. */
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);

        Buf b;
        bufInit(&b, arena);
        if (!enumHasPayload(td)) {
            /* Without payloads: still a plain C enum, unchanged. */
            bufPuts(&b, "typedef enum { ");
            for (size_t j = 0; j < td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&td->variants, j);
                if (j) bufPuts(&b, ", ");
                bufPrintf(&b, "%s_%s = %zu", td->name, v->name, j);
            }
            bufPrintf(&b, " } %s;", td->name);
            cgLine(&g, "%s", bufCstr(&b));
            cgLine(&g, "");
        } else if (td->typeParams.len > 0) {
            continue;      /* generic enum: the instances own constants and definition */
        } else {
            /* With payloads: the tag constants can be emitted now, since they
             * depend on nothing, while the `struct { tag; union }` definition is
             * left to the dependency-ordered section below, because a payload
             * may contain another struct, as in a variant holding a
             * `slice<u8>`. */
            Buf e2;
            bufInit(&e2, arena);
            bufPrintf(&e2, "enum { ");
            for (size_t j = 0; j < td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&td->variants, j);
                if (j) bufPuts(&e2, ", ");
                bufPrintf(&e2, "%s_%s = %zu", td->name, v->name, j);
            }
            bufPrintf(&e2, " };");
            cgLine(&g, "%s", bufCstr(&e2));
            cgLine(&g, "");
            continue;               /* the name table is deferred too: it reads the tag */
        }

        /* Nothing follows: an enum has textual names automatically, and they
         * come from the array of variant names in its descriptor, the `table`
         * of `<Type>_desc`, rather than from a derived `<Type>_name` function
         * emitted here. */
    }

    /* Collect everything that becomes a struct in C: ordinary structs, generic
     * instances, arrays, and enums with payloads. */
    Vec units;
    vecInit(&units, arena, sizeof(void *));
    /* Deduplicated by C name: a writable and a read-only view are the same C
     * struct - `slice<mut slice<T>>` and `slice<slice<T>>` are both
     * `slice_slice_T` - while `ttEquals` counts `mut` as part of the identity,
     * so the type table can hold two instances under one name. Without
     * deduplication the generated C would hold two identical `struct`
     * definitions and gcc would report a redefinition. */
    for (size_t i = 0; i < g.structs.len; i++) {
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = *(StructDef **)vecAt(&g.structs, i);
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *it = *(Type **)vecAt(&g.insts, i);
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->sd = it->sdef;              /* NULL for an array */
        u->inst = it;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    /* An enum with payloads belongs here too: its union holds the payload
     * types by value. */
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);
        if (!enumHasPayload(td)) continue;
        if (td->typeParams.len > 0) continue;    /* generic enum: instances below */
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->td = td;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }
    /* Each instance of a generic enum (`option<i64>`) is a struct definition of
     * its own. */
    for (size_t i = 0; i < tt->enumInstances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->enumInstances, i);
        SUnit *u = (SUnit *)arenaAllocZero(arena, sizeof(SUnit));
        u->td = it->edef;
        u->inst = it;
        vecInit(&u->deps, arena, sizeof(int));
        *(SUnit **)vecPush(&units) = u;
    }

    /* Run to a fixed point, adding the instances that method signatures
     * mention. The round count is capped, and passing the cap means something
     * unexpected is happening, so it is reported loudly rather than quietly
     * generating too little. */
    {
        bool grew = true;
        int round = 0;
        const char *srcName = NULL, *newName = NULL;   /* names the expansion */
        while (grew && round < 64) {
            grew = false;
            round++;
            size_t cur = units.len;                 /* only this round's entries */
            for (size_t i = 0; i < cur; i++) {
                SUnit *u = *(SUnit **)vecAt(&units, i);
                size_t before = units.len;
                scanUnitForUnits(arena, tt, &units, u);
                if (units.len != before) {
                    grew = true;
                    srcName = unitName(u);
                    for (size_t k = before; k < units.len; k++)
                        newName = unitName(*(SUnit **)vecAt(&units, k));
                }
            }
        }
        if (grew) {
            /* The cap does not guard against a broken loop; it guards against
             * instances that nest without end, which every monomorphizing
             * language has to guard against - C++ has `-ftemplate-depth` and
             * rustc has `recursion_limit`. Those only report that the limit was
             * passed, while this one also names which instance expanded into
             * which, which makes the problem easy to locate. */
            fprintf(stderr,
                    "extc: error: generic instance closure did not settle in 64 rounds.\n"
                    "      last expansion: `%s` mentions `%s`, which needs more instances.\n"
                    "      This usually means instances nest without bound (such as\n"
                    "      `option<option<option<...>>>`). Use a concrete type, or report\n"
                    "      this program if you think it should compile.\n",
                    srcName ? srcName : "?", newName ? newName : "?");
            exit(1);
        }
    }

    /* Every typedef comes first: a pointer field (`ref T`) needs nothing more. */
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        cgLine(&g, "typedef struct %s %s;", unitName(u), unitName(u));
    }
    if (units.len) cgLine(&g, "");

    /* Compute the dependencies: another struct-like type held by value in a
     * field. */
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        if (u->td) {                    /* enum: payloads live in the union by value */
            for (size_t j = 0; j < u->td->variants.len; j++) {
                Variant *v = *(Variant **)vecAt(&u->td->variants, j);
                for (size_t k = 0; k < v->types.len; k++) {
                    Type *pt = *(Type **)vecAt(&v->types, k);
                    if (u->inst)                 /* instance: substitute the arguments */
                        pt = ttSubstitute(tt, pt, &u->td->typeParams, &u->inst->targs);
                    if (pt->kind == TY_REF) continue;
                    int d = unitFind(&units, pt);
                    if (d >= 0 && d != (int)i) *(int *)vecPush(&u->deps) = d;
                }
            }
            continue;
        }
        if (u->inst && u->inst->kind == TY_ARRAY) {
            int j = unitFind(&units, u->inst->inner);
            if (j >= 0 && j != (int)i) *(int *)vecPush(&u->deps) = j;
            continue;
        }
        for (size_t k = 0; k < u->sd->fields.len; k++) {
            FieldDef *fd = *(FieldDef **)vecAt(&u->sd->fields, k);
            Type *ft = fd->type;
            if (u->inst) ft = ttSubstitute(tt, ft, &u->sd->typeParams, &u->inst->targs);
            if (ft->kind == TY_REF) continue;
            int j = unitFind(&units, ft);
            if (j >= 0 && j != (int)i) *(int *)vecPush(&u->deps) = j;
        }
    }

    /* Emit the definitions in dependency order. A cycle, which would have to go
     * through a value and which C forbids anyway, is emitted regardless and left
     * to the C compiler to report. */
    for (;;) {
        bool progressed = false, allDone = true;
        for (size_t i = 0; i < units.len; i++) {
            SUnit *u = *(SUnit **)vecAt(&units, i);
            if (u->done) continue;
            allDone = false;
            bool ready = true;
            for (size_t k = 0; k < u->deps.len && ready; k++)
                ready = (*(SUnit **)vecAt(&units, *(int *)vecAt(&u->deps, k)))->done;
            if (!ready) continue;
            unitBody(&g, u);
            u->done = true;
            progressed = true;
        }
        if (allDone || !progressed) break;
    }
    for (size_t i = 0; i < units.len; i++) {
        SUnit *u = *(SUnit **)vecAt(&units, i);
        if (!u->done) { unitBody(&g, u); u->done = true; }
    }

    /* A `<Type>_name` function used to be emitted here for an enum with
     * payloads, reading `v.tag`, which is why it had to come after the
     * definitions. The variant names now live in the `table` of the descriptor,
     * so that section is gone. The tag constants of an enum are emitted where
     * the enum itself is emitted and are unaffected. */

    for (size_t i = 0; i < tt->enumInstances.len; i++) {
        Type *it = *(Type **)vecAt(&tt->enumInstances, i);
        TypeDef *td = it->edef;
        /* The tag constants of the instance (`maybe_i64_nothing = 0`) carry the
         * same values as the base type, both following the variant order, so the
         * rule that the zero value has tag 0 holds for an instance as well. */
        Buf tb;
        bufInit(&tb, arena);
        bufPuts(&tb, "enum { ");
        for (size_t j = 0; j < td->variants.len; j++) {
            Variant *v = *(Variant **)vecAt(&td->variants, j);
            if (j) bufPuts(&tb, ", ");
            bufPrintf(&tb, "%s_%s = %zu", it->name, v->name, j);
        }
        bufPuts(&tb, " };");
        cgLine(&g, "%s", bufCstr(&tb));
    }

    /* ------------------------------------------------------------------
     * The descriptor table is not emitted here. It goes between the prototypes
     * and the function bodies, because which descriptors are needed is known
     * only from the bodies, through what `genPrint` prints; see emitDescRegion.
     * ------------------------------------------------------------------ */

    /* Globals and constants are plain C static objects; a global of fixed size
     * needs no arena. C zeroes a static object by itself, so a global without an
     * initializer needs no initializer expression at all.
     *
     * The `static` here is a performance decision, for the reason given above
     * cgIsMain: external linkage makes gcc reject outer-loop vectorization,
     * which made a matrix multiply at OI scale 3.2x slower. */
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (ttIsError(gd->ann)) continue;
        const char *ct = cType(&g, gd->ann);
        if (gd->init) {
            cgLine(&g, "static %s %s = %s;", ct, gd->name, genExpr(&g, gd->init));
        } else {
            /* No initializer: C zeroes static storage by itself. */
            cgLine(&g, "static %s %s;", ct, gd->name);
        }
    }
    if (m->globals.len) cgLine(&g, "");


    /* ------------------------------------------------------------------
     * The prototype region: every function is declared before any body.
     *
     * Only prototypes may appear here, never a definition. The reason is a trap
     * that was hit for real: the comparison of an array is generated by the
     * compiler, and its loop calls the `fn ==` of the element type, `point_eq`
     * among others. When the definition of the array comparison came before the
     * prototype of `point_eq`, C took `point_eq` to be implicitly declared as
     * `int()`, and the real prototype that followed produced "conflicting types
     * for 'point_eq'". Example code caught that bug.
     *
     * The lesson is the same as for the ordering of struct definitions: never
     * rely on an ordering that happens to work, rule it out by construction.
     * ---------------------------------------------------------------- */
    /* The prototypes of `_debug`, `_writeText`, `_name` and the array `_eq` are
     * gone: printing and structural `==` go through the descriptor table, so no
     * derived function needs a forward declaration. */
    /* The prototypes of instance methods; an array has no sdef and no methods. */
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g.insts, i);
        if (inst->kind != TY_GENERIC) continue;
        substEnter(&g, inst);
        for (size_t j = 0; j < inst->sdef->methods.len; j++) {
            FuncDef *m = *(FuncDef **)vecAt(&inst->sdef->methods, j);
            /* Only a method that is really called is emitted, which keeps the
             * generated C smaller. The flag is a conservative approximation: it
             * is set whenever the template body mentions a call, so a closure
             * such as `push` calling `grow` is included automatically, and
             * emitting too much is the safe direction. */
            if (!m->used) continue;
            genFuncProto(&g, m);
        }
        substLeave(&g);
    }
    /* Methods of ordinary structs and free functions: their order does not
     * matter, and mutual calls are covered by the prototypes above. */
    for (size_t i = 0; i < g.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g.funcs, i);
        Vec *svP, *svA;
        substEnterFunc(&g, f, &svP, &svA);
        const char *ret = cgIsMain(f) ? "int" : cType(&g, f->ret);
        Buf sig;
        bufInit(&sig, arena);
        /* The parameter list must come from the same cgParamList the definition
         * uses: a function with a home arena has one hidden parameter more, and
         * leaving it out of the prototype produced a C type mismatch, which was
         * hit for real. */
        /* An external declaration is not `static`, so that the linker can see
         * it, and only its prototype is emitted. */
        bufPrintf(&sig, "%s%s %s(%s);",
                  (cgIsMain(f) || f->isExtern) ? "" : "static ",
                  ret, cFuncName(&g, f), cgParamList(&g, f));
        cgLine(&g, "%s", bufCstr(&sig));
        substLeaveFunc(&g, svP, svA);
    }
    if (g.structs.len || g.insts.len || g.funcs.len) cgLine(&g, "");

    /* ================= the body region: definitions from here on ========== */

    /* Bodies are written into a temporary buffer first, because a slice helper is
     * discovered to be needed only while generating, and C wants definitions
     * before uses. Everything is spliced together at the end as prototypes, then
     * helpers, then bodies. */
    g.out = &g.body;

    /* The index primitives of the views; printing and comparison derive no
     * function at all any more, see the descriptor table. */
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g.insts, i);
        if (inst->kind != TY_GENERIC) continue;
        substEnter(&g, inst);
        if (isView(inst)) genViewIndexer(&g, inst);
        substLeave(&g);
    }

    /* method definitions of the instances */
    for (size_t i = 0; i < g.insts.len; i++) {
        Type *inst = *(Type **)vecAt(&g.insts, i);
        if (inst->kind != TY_GENERIC) continue;
        substEnter(&g, inst);
        for (size_t j = 0; j < inst->sdef->methods.len; j++) {
            FuncDef *m = *(FuncDef **)vecAt(&inst->sdef->methods, j);
            if (!m->used) continue;      /* called methods only */
            genFunc(&g, m);
            cgLine(&g, "");
        }
        substLeave(&g);
    }

    for (size_t i = 0; i < g.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&g.funcs, i);
        if (f->isExtern) continue;              /* an external declaration has no body */
        Vec *svP, *svA;
        substEnterFunc(&g, f, &svP, &svA);      /* instances need substitution */
        genFunc(&g, f);
        substLeaveFunc(&g, svP, svA);
        cgLine(&g, "");
    }

    /* Final assembly: the pieces whose need is discovered during generation are
     * spliced in ahead of the bodies - the print runtime and the descriptor
     * table, emitted on demand according to what genPrint printed, and the slice
     * helpers. The order is prototypes, then descriptors, then bodies, which is
     * what the define-before-use rule of C requires. */
    g.out = out;
    emitDescRegion(&g);
    /* Descriptor types and shared scalar descriptors: needed by printing or by
     * comparison, whichever comes first. */
    if (g.needRuntime || g.eqNeed.len) bufPuts(out, bufCstr(&g.rt));
    if (g.needRuntime)                 bufPuts(out, bufCstr(&g.rtPrint));
    if (g.eqNeed.len)                  bufPuts(out, bufCstr(&g.rtEq));
    bufPuts(out, bufCstr(&g.desc));
    for (size_t i = 0; i < g.helpers.len; i++)
        bufPuts(out, ((SliceHelper *)vecAt(&g.helpers, i))->text);
    bufPuts(out, bufCstr(&g.body));

    return !ctx->hasError;
}
