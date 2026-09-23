/* Module loading: semantic imports into a single translation unit.
 *
 * One file is one module. `use std::io` is a semantic import, not a textual include:
 * the file is parsed on its own, its declarations are merged into the program, and
 * the compiler still emits a single C file, so everything internal can stay `static`.
 * Paths use `::`, declarations are public unless marked `@private`, and an import
 * cycle is refused.
 *
 * All of the module machinery lives in this loader, so the checker and codegen stay
 * almost untouched:
 *
 *   1. A `use` in the root file resolves to a path and then to a file: next to the
 *      importing file, in a `-I` directory, or in the standard library directory.
 *   2. Modules load recursively. A state flag on each unit detects cycles: a module
 *      that is asked for while it is still loading is a cycle, and that is an error.
 *   3. Each module's declarations are merged into the main Module in post-order, so
 *      a dependency is merged before the declarations that need it - the very path
 *      the prelude takes.
 *   4. Qualified names are resolved before the checker runs:
 *        `io::readLine(...)`    -> flat name, rewritten into EX_CALL
 *        `io::STDIN`            -> flat name, rewritten into EX_IDENT
 *        `io::File` (in a type) -> flat name
 *      The checker therefore still sees the flat table it was written against and
 *      needs no change at all.
 *
 * Limits of the current implementation, stated plainly:
 *   - Top-level names must be globally unique. Two modules that each declare a
 *     private `helper` are rejected as a duplicate name today, and the message says
 *     that the clash is between modules. A real per-module namespace is future work.
 *   - Referring to another module's function or global *without* a qualifier still
 *     works, because the checker sees one flat table. So `@private` currently blocks
 *     only the qualified form, and blocking the unqualified form belongs in name
 *     resolution.
 *   - Two imports whose last path segment is the same (`a::util`, `b::util`) cannot
 *     be told apart in the source, so that is an error asking the user to rename one.
 */

#define _POSIX_C_SOURCE 200809L

#include "modules.h"
#include "lexer.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* readlink: locate `<extc>/../stdlib` */

/* -------------------------------------------------------------- small helpers */

/* Read a whole file into the arena, NUL-terminated.
 *
 * Params:
 *   a      - arena that owns the buffer
 *   path   - file to read
 *   outLen - receives the number of bytes actually read
 *
 * Returns:
 *   The file contents, or NULL when it cannot be opened, seeked, or sized.
 *
 * Notes:
 *   - The length must be reported back. An early version dropped it, and callers
 *     that lex from a length then treated the module as an empty file - a failure
 *     that shows up far away from its cause.
 *   - The length is the byte count `fread` returned, not the size reported by the
 *     file system, so a file that shrinks mid-read is still terminated correctly.
 */

static char *readWhole(Arena *a, const char *path, size_t *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)arenaAlloc(a, (size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (outLen) *outLen = got;
    return buf;
}

/* Report whether `path` can be opened for reading. */
static bool fileExists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* Take the module short name out of a path: `dir/io.extc` -> `io`. */
static const char *baseNameNoExt(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t n = strlen(base);
    if (n > 5 && strcmp(base + n - 5, ".extc") == 0) n -= 5;
    return arenaStrndup(a, base, n);
}

/* Turn a module path into a relative file path: `std::io` -> `std/io`. */
static char *pathToRel(Arena *a, const char *modPath) {
    Buf b;
    bufInit(&b, a);
    for (const char *p = modPath; *p; p++) {
        if (p[0] == ':' && p[1] == ':') { bufPutc(&b, '/'); p++; }
        else bufPutc(&b, *p);
    }
    return bufCstr(&b);
}

/* Directory part of a path: `dir/io.extc` -> `dir`; no slash means `.`. */
static char *dirOf(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return arenaStrndup(a, ".", 1);
    return arenaStrndup(a, path, (size_t)(slash - path));
}

/* ------------------------------------------------------------- loader state */

/* One rename entry: a declaration's source name and the name it was given. */
typedef struct { const char *from; const char *to; } Ren;

/* One loaded module together with the state of its loading. */
typedef struct {
    const char *file;       /* resolved path; the unique key for this module */
    const char *modName;    /* short name (the file name without `.extc`); NULL for the root */
    Module      mod;        /* its own parsed declarations, before merging */
    Ctx        *ctx;        /* its own source context, so a diagnostic names the right
                             * file and line. The root unit points at the context the
                             * caller passed in, by pointer: a copy would swallow the
                             * diagnostics reported through it. */
    int         state;      /* 0 = not started, 1 = loading (this detects a cycle),
                             * 2 = done */
    /* Source name -> generated name for every declaration of this module, so that a
     * reference written in this module can be mapped to the name that was emitted
     * (`pair` -> `liba$pair`). */
    Vec         ren;        /* Ren* */
} ModUnit;

/* Everything one call to `loadModules` needs while it runs. */
typedef struct {
    Arena     *a;           /* arena that owns every unit and every generated name */
    Module    *out;         /* the merged module; the prelude is already in it */
    Vec        units;       /* ModUnit* - every unit loaded so far, for the duplicate check */
    Vec        order;       /* ModUnit* - post-order, which is a topological order */
    Vec       *ctxs;        /* Ctx* - the source context of each loaded unit, so the
                             * caller can render their diagnostics. Kept as a pointer:
                             * the length has to reach the caller, so it must not be
                             * copied by value. */
    Vec        searchDirs;  /* const char* - directories from `-I` */
    const char *rootDir;    /* project root, the directory of the entry file; every
                             * `use a::b` is resolved relative to it */
    const char *stdDir;     /* standard library directory holding `std/io.extc` */
    int         errors;     /* number of errors reported; non-zero means the load failed */
} Loader;

/* Prefix a declaration name with its module name: `readLine` -> `io$readLine`.
 *
 * Params:
 *   L    - loader whose arena owns the built string
 *   u    - the declaration's unit; a NULL or empty module name leaves `name` alone,
 *          which is how the root file keeps its names and stays the entry point
 *   name - the source name
 *
 * Returns:
 *   The prefixed name, or `name` unchanged for the root module.
 *
 * Notes:
 *   - `$` is legal in a C identifier and illegal in an extC identifier, so a
 *     generated name can never collide with a name the user wrote.
 *   - The result lives in the arena, and another `arenaPrintf` may reuse that buffer.
 *     Compute every name first and only then store the pointers (see
 *     `mangleUnitDecls`).
 */

static const char *mangleName(Loader *L, ModUnit *u, const char *name) {
    if (!u->modName || !*u->modName) return name;
    return arenaPrintf(L->a, "%s$%s", u->modName, name);
}

/* Report whether `name` is declared by an `extern!` in this module.
 *
 * An `extern!` name is the symbol the linker has to find, so it must never be
 * prefixed: `extern!("libc") fn read(...)` declared as `sys$read` only produces
 * `undefined reference to sys$read`, reported by the linker and hard to trace back to
 * module renaming. Every libc primitive in the standard library's system module hit
 * this, which made the whole io test suite fail to link.
 *
 * Params:
 *   src  - the module to search
 *   name - the function name to look for
 *
 * Returns:
 *   True when the module declares `name` as an external function, in which case the
 *   name is kept and its return ticket maps it to itself, so references inside the
 *   module need no rewrite either.
 */

static bool externKeepsName(Module *src, const char *name) {
    for (size_t i = 0; i < src->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&src->funcs, i);
        if (f->isExtern && strcmp(f->name, name) == 0) return true;
    }
    return false;
}
/* Look up the name a declaration received, in the unit that declares it.
 *
 * Params:
 *   target - the unit whose rename table is consulted
 *   name   - the source name
 *
 * Returns:
 *   The generated name, or `name` when the unit has no rename for it.
 *
 * Notes:
 *   - The lookup must use the *target* unit and not the caller. The root file has a
 *     NULL module name, so a lookup keyed on the caller would return early and leave
 *     every reference from the root file unrenamed.
 */

static const char *renOfTarget(ModUnit *target, const char *name) {
    if (!target || !target->modName || !*target->modName) return name;
    for (size_t i = 0; i < target->ren.len; i++) {
        Ren *r = (Ren *)vecAt(&target->ren, i);
        if (strcmp(r->from, name) == 0) return r->to;
    }
    return name;
}
/* Look up `name` in a unit's own rename table.
 *
 * Params:
 *   u    - the unit whose own declarations are searched
 *   name - the source name written in that unit
 *
 * Returns:
 *   The generated name, or NULL when this unit declares nothing of that name.
 *
 * Notes:
 *   - Only names this unit itself declares are mapped. Mapping "any name any module
 *     exports" would silently rewrite a local reference to another module's
 *     declaration, which compiles and quietly means something else.
 */

static const char *renLookup(ModUnit *u, const char *name) {
    if (!u || !u->modName || !*u->modName) return name;
    for (size_t i = 0; i < u->ren.len; i++) {
        Ren *r = (Ren *)vecAt(&u->ren, i);
        if (strcmp(r->from, name) == 0) return r->to;
    }
    return NULL;
}

/* Find the unit already loaded from `file`.
 *
 * Params:
 *   L    - loader
 *   file - the resolved path, which is the unique key of a unit
 *
 * Returns:
 *   The unit, or NULL when this file has not been loaded.
 */

static ModUnit *findUnit(Loader *L, const char *file) {
    for (size_t i = 0; i < L->units.len; i++) {
        ModUnit *u = *(ModUnit **)vecAt(&L->units, i);
        if (u && strcmp(u->file, file) == 0) return u;
    }
    return NULL;
}

/* Find the file a `use` names, searching the closest directory first.
 *
 * The search order is the project root, then the `-I` directories in the order they
 * were given, then the standard library directory. The first hit wins, so a project
 * library shadows a standard one of the same name.
 *
 * Params:
 *   L            - loader (supplies the root and search directories)
 *   modPath      - the module path as written, e.g. `std::io`
 *   importerFile - the file containing the `use`, named in the diagnostic
 *
 * Returns:
 *   The resolved path, or NULL after reporting an error. The diagnostic lists every
 *   path that was tried, because otherwise the user can only guess where the loader
 *   looked.
 */

static char *resolveModFile(Loader *L, const char *modPath, const char *importerFile) {
    char *rel  = pathToRel(L->a, modPath);
    char *cand = arenaPrintf(L->a, "%s.extc", rel);
    Buf   tried;
    bufInit(&tried, L->a);

    char *p = arenaPrintf(L->a, "%s/%s", L->rootDir, cand);
    if (fileExists(p)) return p;
    bufPrintf(&tried, "\n        %s", p);

    for (size_t i = 0; i < L->searchDirs.len; i++) {
        const char *sd = *(const char **)vecAt(&L->searchDirs, i);
        char *q = arenaPrintf(L->a, "%s/%s", sd, cand);
        if (fileExists(q)) return q;
        bufPrintf(&tried, "\n        %s", q);
    }
    const char *stdDir = L->stdDir;
    if (stdDir && *stdDir) {
        char *r = arenaPrintf(L->a, "%s/%s", stdDir, cand);
        if (fileExists(r)) return r;
        bufPrintf(&tried, "\n        %s", r);
    }
    fprintf(stderr,
            "error: cannot find module `%s` (imported by %s)\n"
            "note:  `use a::b` looks for a file `a/b.extc`. Searched:%s\n"
            "note:  add a search directory with `-I <dir>`, or point $EXTC_STD at the\n"
            "       standard library's directory.\n",
            modPath, importerFile, bufCstr(&tried));
    L->errors++;
    return NULL;
}

/* Report whether `name` is a type this module declares itself.
 *
 * Params:
 *   self - the unit being rewritten
 *   name - the left-hand side of a `a::b` form
 *
 * Returns:
 *   True when the module declares a struct or enum of that name, in which case `a::b`
 *   is an associated item of that type and not a module reference at all.
 */

static bool isOwnType(ModUnit *self, const char *name) {
    for (size_t i = 0; i < self->mod.structs.len; i++)
        if (strcmp((*(StructDef **)vecAt(&self->mod.structs, i))->name, name) == 0) return true;
    for (size_t i = 0; i < self->mod.types.len; i++)
        if (strcmp((*(TypeDef **)vecAt(&self->mod.types, i))->name, name) == 0) return true;
    return false;
}

/* Report whether a file for `modPath` exists anywhere on the search path.
 *
 * Params:
 *   L       - loader (supplies the search directories)
 *   modPath - the module path as written
 *
 * Returns:
 *   True when some search directory holds the file.
 *
 * Notes:
 *   - Silent on purpose, and the two cases it separates need different messages: a
 *     module that exists but was not imported is a missing `use`, while a name that
 *     matches no file at all is not a module. Reporting "not imported" for the second
 *     case sends the user looking in the wrong place.
 */

static bool moduleExists(Loader *L, const char *modPath) {
    char *cand = arenaPrintf(L->a, "%s.extc", pathToRel(L->a, modPath));
    char *p = arenaPrintf(L->a, "%s/%s", L->rootDir, cand);
    if (fileExists(p)) return true;
    for (size_t i = 0; i < L->searchDirs.len; i++) {
        const char *sd = *(const char **)vecAt(&L->searchDirs, i);
        if (fileExists(arenaPrintf(L->a, "%s/%s", sd, cand))) return true;
    }
    const char *stdDir = L->stdDir;
    if (stdDir && *stdDir && fileExists(arenaPrintf(L->a, "%s/%s", stdDir, cand))) return true;
    return false;
}

/* ------------------------------------------------------- declaration lookup */

/* Find a function declared by this unit.
 *
 * Params:
 *   u    - the unit to search
 *   name - the source name the caller wrote
 *
 * Returns:
 *   The declaration, or NULL when the unit declares nothing of that name.
 *
 * Notes:
 *   - The caller always passes a source name, while a declaration may already carry
 *     its generated name, so the rename table is consulted first.
 */

static FuncDef *unitFunc(ModUnit *u, const char *name) {
    /* The caller passes a source name, the declaration may be renamed: try the table. */
    const char *want = renLookup(u, name);
    if (!want) want = name;
    for (size_t i = 0; i < u->mod.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&u->mod.funcs, i);
        if (strcmp(f->name, want) == 0) return f;
    }
    return NULL;
}

/* Find a global declared by this unit.
 *
 * Params:
 *   u    - the unit to search
 *   name - the source name the caller wrote
 *
 * Returns:
 *   The declaration, or NULL when the unit declares nothing of that name.
 */

static GlobalDef *unitGlobal(ModUnit *u, const char *name) {
    /* Same as `unitFunc`: a source name in, a possibly renamed declaration out. */
    const char *want = renLookup(u, name);
    if (!want) want = name;
    for (size_t i = 0; i < u->mod.globals.len; i++) {
        GlobalDef *g = *(GlobalDef **)vecAt(&u->mod.globals, i);
        if (strcmp(g->name, want) == 0) return g;
    }
    return NULL;
}

/* Find a struct declared by this unit, accepting either of its names.
 *
 * `mergeUnit` renames declarations in place, and the root file's qualified names are
 * resolved after that merge, so by the time this runs the table already holds
 * `e1$color` and a search keyed on the source name `color` alone finds nothing:
 * `e1::color` is then rejected as "the module has nothing named color", which blames
 * a name that is nowhere near the real cause.
 *
 * Params:
 *   u    - the unit to search
 *   name - the source name the caller wrote
 *
 * Returns:
 *   The declaration, or NULL when the unit has no struct of either name.
 */

static StructDef *unitStruct(ModUnit *u, const char *name) {
    /* The caller passes a source name, the declaration may be renamed: try both. */
    const char *want = renLookup(u, name);
    if (!want) want = name;
    for (size_t i = 0; i < u->mod.structs.len; i++) {
        StructDef *s = *(StructDef **)vecAt(&u->mod.structs, i);
        if (strcmp(s->name, want) == 0 || strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

/* Find a type declaration of this unit, accepting either of its names.
 *
 * Params:
 *   u    - the unit to search
 *   name - the source name the caller wrote
 *
 * Returns:
 *   The declaration, or NULL when the unit has no enum of either name.
 *
 * Notes:
 *   - Renaming behaves exactly as in `unitStruct`.
 */

static TypeDef *unitType(ModUnit *u, const char *name) {
    /* Same as `unitStruct`. */
    const char *want = renLookup(u, name);
    if (!want) want = name;
    for (size_t i = 0; i < u->mod.types.len; i++) {
        TypeDef *t = *(TypeDef **)vecAt(&u->mod.types, i);
        if (strcmp(t->name, want) == 0 || strcmp(t->name, name) == 0) return t;
    }
    return NULL;
}

/* Find the module this unit imports under the full path `qname`.
 *
 * Params:
 *   L     - loader
 *   self  - the unit whose `use` list is searched
 *   qname - a full module path as written, e.g. `std::sys::io`
 *
 * Returns:
 *   The imported unit, or NULL when no import names that path.
 *
 * Notes:
 *   - This has to exist next to the short-name lookup because a `UseDecl` carries two
 *     names: `path`, the full path the user wrote (`std::sys::io`), and `shortName`,
 *     its last segment (`io`). Matching on the short name alone means a reference
 *     written as `std::sys::io::STDOUT` asks with a full path that never matches, and
 *     the user is told `std` is not imported even though `use std::sys::io` is right
 *     there in the file.
 *   - So one rule: a short name matches a short name, a full path matches a full
 *     path, and both forms must resolve to the same module.
 */

static ModUnit *importedAsPath(Loader *L, ModUnit *self, const char *qname) {
    for (size_t i = 0; i < self->mod.uses.len; i++) {
        UseDecl *u = *(UseDecl **)vecAt(&self->mod.uses, i);
        if (strcmp(u->path, qname) == 0)
            return u->file ? findUnit(L, u->file) : NULL;
    }
    return NULL;
}

/* Find the module this unit imports under the short name `shortName`.
 *
 * Params:
 *   L         - loader
 *   self      - the unit whose `use` list is searched
 *   shortName - the last segment of an imported path, e.g. `io`
 *
 * Returns:
 *   The imported unit, or NULL when nothing is imported under that name, which is
 *   what makes "modules must be imported explicitly" enforceable.
 */

static ModUnit *importedAs(Loader *L, ModUnit *self, const char *shortName) {
    for (size_t i = 0; i < self->mod.uses.len; i++) {
        UseDecl *u = *(UseDecl **)vecAt(&self->mod.uses, i);
        if (strcmp(u->shortName, shortName) == 0)
            return u->file ? findUnit(L, u->file) : NULL;
    }
    return NULL;
}

/* --------------------------------------------------- qualified name rewrite */

/* Forward declarations: these three rewrite passes are mutually recursive. */
static void rwStmt(Loader *L, ModUnit *self, Stmt *s);
static void rwExpr(Loader *L, ModUnit *self, Expr *e);
static void rwExprName(ModUnit *self, Expr *e);

/* Rewrite a type written in a declaration: `io::File` becomes the name of the
 * declaration in the merged module, and visibility is checked on the way.
 *
 * Params:
 *   L    - loader
 *   self - the unit the type was written in
 *   t    - the type to rewrite in place
 *
 * Notes:
 *   - A bare name that this module itself declares is renamed too. Left alone, it
 *     would reach the checker as the source name `pair` while the flat table holds
 *     two structs of that name, and the checker would have to pick one through the
 *     alias table - silently binding whichever module registered first, which
 *     compiles and has the wrong type.
 *   - A `use` suggestion in a diagnostic may only name a module: the remainder of a
 *     path can contain `::` itself (`alpha::pair` inside `lib::box<alpha::pair>`), so
 *     the first segment after the module name is what gets printed. Suggesting
 *     `use alpha::pair` tells the user to import a module that does not exist.
 */

static void rwType(Loader *L, ModUnit *self, Type *t) {
    if (!t) return;
    if (t->kind == TY_REF) { rwType(L, self, t->inner); return; }
    for (size_t i = 0; i < t->targs.len; i++) rwType(L, self, *(Type **)vecAt(&t->targs, i));
    if (t->kind != TY_UNRESOLVED || !t->name) return;
    const char *sep = strstr(t->name, "::");
    if (!sep) {
        /* A bare name has to be renamed as well when this module declares it.
         * Left as the source name `pair` on a return type, it reaches the checker
         * while the flat table holds two structs named `pair`, and the alias table
         * picks one: the reference is silently bound to whichever module registered
         * first. Then `var q: beta::pair = beta::make(7)` reports
         * `struct \`alpha$pair\` has no field \`s\``, naming a type that appears
         * nowhere in the source. It compiles and has the wrong type, which is the
         * worst kind of bug. The rule matches `rwExprName`: only a name this module
         * declares itself is rewritten. */
        const char *m = renLookup(self, t->name);
        if (m) t->name = m;
        return;
    }
    const char *shortName = arenaStrndup(L->a, t->name, (size_t)(sep - t->name));
    const char *rest = sep + 2;
    /* A `use` suggestion may only name a module, and `rest` can still contain `::`:
     * `alpha::pair` inside `lib::box<alpha::pair>` leaves `rest` as `pair`, but a
     * three-segment path would fold `b::c` into the suggestion. A message reading
     * "add `use alpha::pair`" tells the user to import a module that does not exist. */
    const char *restSep = strstr(rest, "::");
    const char *restTop = restSep ? arenaStrndup(L->a, rest, (size_t)(restSep - rest)) : rest;
    ModUnit *target = importedAs(L, self, shortName);
    if (!target) {
        ctxError(self->ctx, 0, 1,
                 "Modules are imported explicitly (semantic import, not a textual include)."
                 " Write `use a::b` at the top of the file, then use `b::Name`.",
                 "`%s` is not imported here -- add `use %s`", shortName, shortName);
        L->errors++;
        t->name = rest;
        return;
    }
    StructDef *sd = unitStruct(target, rest);
    TypeDef   *td = unitType(target, rest);
    if ((sd && sd->isPrivate) || (td && td->isPrivate)) {
        ctxError(self->ctx, 0, 1,
                 "`@private` means other modules must not name it. Drop the annotation if it is"
                 " meant to be used from here.",
                 "`%s::%s` is private to module `%s`", shortName, restTop, shortName);
        L->errors++;
    } else if (!sd && !td) {
        ctxError(self->ctx, 0, 1,
                 "A module exports its top-level declarations; `@private` ones are hidden.",
                 "module `%s` has no type `%s`", shortName, restTop);
        L->errors++;
    }
    t->name = renOfTarget(target, rest);
}

/* Resolve a type name written in expression position: `mod::Type { ... }` and
 * `mod::Type.variant`.
 *
 * The single test is whether `rest` names a *type* of the module `short`: those two
 * forms are the only way to write a type that another module declares under a name
 * that is also declared here, because the bare name is ambiguous and the loader
 * therefore records no return ticket for it. Without this pass such a type has no
 * writable spelling, and a struct literal or a variant of it cannot be named at all.
 * Both symptoms have been seen: `expected \`{\`` because the parser does not accept a
 * qualified name in a literal, and "module `e1` has nothing named `color`" because
 * the type name was being looked up as a function or a constant.
 *
 * Params:
 *   L     - loader
 *   self  - the unit the expression was written in
 *   qname - the qualified name to resolve
 *   out   - receives the resolved declaration name
 *
 * Returns:
 *   True when `qname` names a type of an imported module and `out` was filled. False
 *   means "not a qualified type name", so the caller leaves the expression to the
 *   ordinary path.
 *
 * Notes:
 *   - A qualified name that points at the current module is left alone. The prelude
 *     itself contains `pcg32::withStream(...)` where `pcg32` is a struct of that same
 *     file, and the prelude is one module: rewriting it to `prelude$pcg32` produces a
 *     name the type table never saw, because the type table snapshots the prelude
 *     before module loading starts, and the call is then reported as undefined.
 */

static bool rwQualifiedTypeName(Loader *L, ModUnit *self, const char *qname, const char **out) {
    if (!qname || !out) return false;
    const char *sep = strstr(qname, "::");
    if (!sep) return false;
    const char *shortName = arenaStrndup(L->a, qname, (size_t)(sep - qname));
    const char *rest = sep + 2;
    if (strstr(rest, "::")) return false;          /* `a::b::c` is not supported here */
    ModUnit *target = importedAs(L, self, shortName);
    if (!target) return false;
    /* A qualified name that points at this very unit is never rewritten (see the
     * block above): the prelude writes `pcg32::withStream(...)` for a struct of its
     * own file, and `pcg32` is registered before module loading, so a rewritten
     * `prelude$pcg32` names nothing. `rwQualified` guards the same case by letting a
     * name that equals this file's own name through. */
    if (target == self) return false;              /* pointing at itself: leave it alone */
    StructDef *sd = unitStruct(target, rest);
    TypeDef   *td = sd ? NULL : unitType(target, rest);
    if (!sd && !td) return false;                  /* not a type: leave the path alone */
    if ((sd && sd->isPrivate) || (td && td->isPrivate)) {
        ctxError(self->ctx, 0, 1,
                 "`@private` means other modules must not name it. Drop the annotation if it is"
                 " meant to be used from here.",
                 "`%s::%s` is private to module `%s`", shortName, rest, shortName);
        L->errors++;
        return true;                               /* already reported: no second error */
    }
    *out = renOfTarget(target, rest);              /* the checker resolves by source name */
    return true;
}

/* Resolve the module part of a deep qualified name such as `std::sys::io::STDOUT`.
 *
 * The parser splits that into the prefix `std::sys::io` and the symbol `STDOUT` and
 * passes the prefix here, so the only question left is whether that prefix is
 * imported.
 *
 * The parser can do the split on its own, and it does not need a symbol table to do
 * it, because the split is positional: every segment but the last is a module and the
 * last one is the symbol. So this side has exactly one job - match a full path against
 * the full path recorded by a `use`.
 *
 * Params:
 *   L     - loader
 *   self  - the unit the name was written in
 *   qname - the module prefix, e.g. `std::sys::io`
 *
 * Returns:
 *   The imported unit, or NULL when the prefix is not imported.
 */

static ModUnit *rwDeepQName(Loader *L, ModUnit *self, const char *qname) {
    /* The longest candidate comes from cutting at the separators, and the prefix the
     * parser handed over is already a whole number of segments, so complete names
     * need no repair. A candidate is always cut at a `::`, which keeps its tail a
     * whole segment instead of half of one. */
    /* Full path first: `std::sys::io` is matched against `use std::sys::io` written
     * the same way. The short-name form (`io::read`) is covered by `importedAs`. The
     * order matters, because a short name can collide with another module's while a
     * full path cannot. */
    ModUnit *u = importedAsPath(L, self, qname);
    if (!u) u = importedAs(L, self, qname);
    /* Debug switch `EXTC_DBG_QN=1` reports a deep qualified name that matched no
     * imported module. Like the other debug switches, it changes no output on the
     * normal path, and a name that resolved stays silent. */
    if (!u && getenv("EXTC_DBG_QN"))
        fprintf(stderr, "[qn] deep `%s` matched no imported module\n", qname);
    return u;
}

/* Resolve `a::b` in expression position, where the parser has built an EX_ASSOC node.
 *
 * Params:
 *   L    - loader
 *   self - the unit the expression was written in
 *   e    - the expression, rewritten in place, possibly into another kind
 *
 * Notes:
 *   - The node is read in full before anything is written back: rewriting changes the
 *     kind and therefore the union, so `args` and the flags are saved first.
 */

static void rwQualified(Loader *L, ModUnit *self, Expr *e) {
    const char *modPrefix = e->u.assoc.modPrefix;
    const char *symName   = e->u.assoc.name;
    const int   saveTargs = (int)e->u.assoc.targs.len;
    const bool  saveCall  = e->u.assoc.isCall;

    /* A deep path (`std::sys::io::write`) only has to find its module here. Which
     * symbol is meant has already been decided by the parser, and the visibility,
     * lookup, and `@private` rules below are then reused unchanged. */
    ModUnit *target = NULL;
    if (modPrefix) {
        /* `modPrefix` is already the module name: the parser consumed every middle
         * segment and kept only the last one as the symbol. Cutting a symbol out of it
         * again would eat the real module name (`std::sys::io` would become
         * `std::sys`), so the job here is matching a full path, not splitting one. */
        target = rwDeepQName(L, self, modPrefix);
        if (target) {
            /* The resolved unit must be bound to `target` directly. The
             * `importedAs(tn)` below matches short names only, and `tn` here is the
             * full path `std::sys::io`, so it would find nothing and report the module
             * as not imported one line after it was found. */
            e->u.assoc.typeName = modPrefix;
        } else {
            /* Report it with a full path and an instruction the user can follow. A
             * first version printed the segment before the first `::`, producing
             * "`std` is not imported -- add `use std`", which tells the user to import
             * a module that does not exist (`std` is only a path prefix). Calling
             * `resolveModFile` to separate "does not exist" from "not imported" is
             * wrong here as well: it prints its own message when it fails, so the user
             * would get two errors. Just say "not imported" and name the `use` to add;
             * if the file really is missing, the loader says so once the `use` exists
             * and lists every path it searched. */
            const char *note = arenaPrintf(L->a,
                    "Modules are imported explicitly (semantic import, not a textual include)."
                    " Add `use %s` at the top of the file.", modPrefix);
            ctxError(self->ctx, e->line, 1, note,
                     "`%s` is not imported here -- add `use %s`", modPrefix, modPrefix);
            L->errors++;
            return;
        }
    }

    /* Type arguments mean this is a generic instance and not an associated item. */
    const char *tn = e->u.assoc.typeName;
    if (!tn || e->u.assoc.targs.len != 0) return;
    if (!target) target = importedAs(L, self, tn);
    if (!target) {
        /* `a` may be a type of module `b` instead of a module itself, which
         * `rwQualifiedTypeName` recognizes.
         * Only a two-segment shape is worth trying: three segments or more cannot be
         * `mod::Type`, and trying anyway produces a misleading "unknown type `a`". */
        if (saveTargs != 0 || !saveCall || strstr(tn, "::")) return;
        const char *mangled = NULL;
        if (rwQualifiedTypeName(L, self, tn, &mangled) && mangled) {
            e->u.ident.name = mangled;             /* the union changes: take the name out first */
            e->kind = EX_IDENT;
            e->qualified = true;                   /* the source wrote a qualified name */
            return;
        }
        (void)0;
        /* Two cases remain: `tn` is a type name, in which case this is an associated
         * item and the checker handles it; or `tn` is a module that exists on disk but
         * was never imported, which deserves a message of its own. Left alone, the
         * second case surfaces as "unknown type `greet`", which is nowhere near the
         * cause. */
        /* The test has to stay narrow. In `fenwick::new(N)`, `fenwick` is a struct
         * declared by this very file and the file is named `fenwick.extc`, so "a file
         * with that name exists" alone would report a missing import for a type the
         * file declares itself. So a name this unit declares as a type passes, and so
         * does the module name that points at this file. */
        bool isSelfFile = strcmp(baseNameNoExt(L->a, self->file), tn) == 0;
        if (!isOwnType(self, tn) && !isSelfFile && moduleExists(L, tn)) {
            /* `note` is passed through verbatim and does not consume varargs the way
             * the format string does, so every value has to be formatted with
             * `arenaPrintf` first. Passing a `%s` here prints the literal
             * "Add `use %s` at the top of the file.", which the user cannot act on. */
            const char *note = arenaPrintf(L->a,
                    "Modules are imported explicitly (semantic import, not a textual include)."
                    " Add `use %s` at the top of the file.", tn);
            ctxError(self->ctx, e->line, 1, note,
                     "module `%s` is not imported here -- add `use %s`", tn, tn);
            L->errors++;
        }
        return;
    }

    const bool isCall = saveCall;
    const char *nm = symName;
    FuncDef   *f = unitFunc(target, nm);
    GlobalDef *g = f ? NULL : unitGlobal(target, nm);

    if ((f && f->isPrivate) || (g && g->isPrivate)) {
        ctxError(self->ctx, e->line, 1,
                 "`@private` hides it from other modules. Drop the annotation if it is meant to be"
                 " used from here.",
                 "`%s::%s` is private to module `%s`", tn, nm, tn);
        L->errors++;
        return;
    }
    /* The shape has to match the declaration: one name has one meaning, and a
     * mismatch is reported. This test is necessary because `isCall` only records
     * whether a `(` followed, so both `io::STDOUT()` (calling a constant) and
     * `io::read` (using a function as a value, which the language has no notion of)
     * reach this point. Without the test, the code below would silently pick one. */
    if (g && isCall) {
        ctxError(self->ctx, e->line, 1,
                 "Only functions take an argument list.",
                 "`%s` is a `let`/`var` constant, not a function -- drop the `()`", nm);
        L->errors++;
        return;
    }
    if (f && !isCall) {
        ctxError(self->ctx, e->line, 1,
                 "extC has no function values -- a function name is only usable as a call.",
                 "`%s` is a function -- write `%s(...)`", nm, nm);
        L->errors++;
        return;
    }
    if (f) {
        /* An EX_CALL holds the callee *expression*, not a name, so an EX_IDENT is
         * built for it. The union is about to be overwritten, so the arguments are
         * moved out first. */
        Vec  args = e->u.assoc.args;
        Expr *id  = exprNew(L->a, EX_IDENT, e->line);
        id->u.ident.name = renOfTarget(target, nm);
        e->kind = EX_CALL;
        e->u.call.callee = id;
        e->u.call.args   = args;
        e->qualified     = true;               /* already qualified: the checker stops asking */
        return;
    }
    if (g) {
        e->kind = EX_IDENT;
        e->u.ident.name = g->name;
        e->qualified    = true;                /* the source wrote a qualified name */
        return;
    }
    ctxError(self->ctx, e->line, 1,
             "A module exports its top-level `fn` / `struct` / `type` / `let`/`var`;"
             " `@private` ones are hidden.",
             "module `%s` has nothing named `%s`", tn, nm);
    L->errors++;
}

/* Rewrite the qualified names inside an expression.
 *
 * Params:
 *   L    - loader
 *   self - the unit the expression was written in
 *   e    - the expression, rewritten in place
 *
 * Notes:
 *   - An expression carries names of its own, not only children, and missing them was
 *     the worst hole in this pass: the initializers of a struct literal were visited
 *     while the literal's own type name was not. With two modules declaring `pair`,
 *     the name `liba::pair { ... }` reached the checker unchanged, the checker looked
 *     a bare name up and found whichever struct was registered first, and the value
 *     was silently bound to the wrong type - it compiled without a word when the
 *     field names happened to line up. The symptom was
 *     `struct \`alpha$pair\` has no field \`s\``, naming a type that is not in the
 *     source. Every type name is therefore rewritten here, including the one in
 *     `EX_CONV` (`mod::T(x)`).
 */

static void rwExpr(Loader *L, ModUnit *self, Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EX_STRUCTLIT: {
        const char *m = NULL;
        if (e->u.lit.name && rwQualifiedTypeName(L, self, e->u.lit.name, &m) && m)
            e->u.lit.name = m;
        break;
    }
    case EX_ENUMVAL: {
        const char *m = NULL;
        if (e->u.enumval.typeName && rwQualifiedTypeName(L, self, e->u.enumval.typeName, &m) && m)
            e->u.enumval.typeName = m;
        break;
    }
    case EX_CONV: {
        const char *m = NULL;
        if (e->u.conv.typeName && rwQualifiedTypeName(L, self, e->u.conv.typeName, &m) && m)
            e->u.conv.typeName = m;
        break;
    }
    /* `mod::Type.variant`: the parser built `mod::Type` as a plain identifier, since
     * it joins names without consulting a symbol table. Recognize that it is a type,
     * replace it with the bare type name, and leave the rest to the checker's existing
     * enum-variant path. */
    case EX_IDENT: {
        const char *m = NULL;
        if (e->u.ident.name && rwQualifiedTypeName(L, self, e->u.ident.name, &m) && m) {
            e->u.ident.name = m;
            e->qualified = true;     /* written qualified: stop asking for a qualifier */
        } else {
            rwExprName(self, e);     /* the `color` of `color.green` becomes `e1$color` */
        }
        break;
    }
    default: break;
    }
    /* An EX_ASSOC may be rewritten into another kind, so the children are visited
     * afterwards, against the node that actually resulted. */
    if (e->kind == EX_ASSOC) rwQualified(L, self, e);

    switch (e->kind) {
    case EX_BIN:    rwExpr(L, self, e->u.bin.left);  rwExpr(L, self, e->u.bin.right); break;
    case EX_UN:     rwExpr(L, self, e->u.un.operand); break;
    case EX_REF:    rwExpr(L, self, e->u.ref.operand); break;
    case EX_DEREF:  rwExpr(L, self, e->u.deref.operand); break;
    case EX_SIGN:   rwExpr(L, self, e->u.sign.operand); break;
    case EX_CONV:   rwExpr(L, self, e->u.conv.operand); break;
    case EX_TRY:    rwExpr(L, self, e->u.try_.operand); break;
    case EX_INDEX:  rwExpr(L, self, e->u.index.obj); rwExpr(L, self, e->u.index.index); break;
    case EX_SLICE:  rwExpr(L, self, e->u.slice.obj); break;
    case EX_FIELD:  rwExpr(L, self, e->u.field.obj); break;
    case EX_NEW:    rwExpr(L, self, e->u.new_.count); rwType(L, self, e->u.new_.type); break;
    case EX_COALESCE:
        rwExpr(L, self, e->u.coalesce.main); rwExpr(L, self, e->u.coalesce.fallback); break;
    case EX_CALL:
        /* The callee is visited as well. Inside a module, `fn a() { b() }` calls `b`
         * of that same module, and the declaration is renamed, so skipping this step
         * leaves `b` unfindable: the error is
         * `call to undefined function \`twice\`` while `greet$twice` sits in the table. */
        rwExpr(L, self, e->u.call.callee);
        for (size_t i = 0; i < e->u.call.args.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.call.args, i));
        break;
    case EX_METHOD:
        rwExpr(L, self, e->u.method.recv);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.method.args, i));
        break;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.assoc.args, i));
        break;
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.gencall.args, i));
        break;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.enumval.args, i));
        break;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            rwExpr(L, self, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value);
        break;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.arraylit.elems, i));
        break;
    default: break;               /* literals, bindings, and null have no children */
    }
}

/* Rewrite the qualified names inside a statement and everything nested in it.
 *
 * Params:
 *   L    - loader
 *   self - the unit the statement was written in
 *   s    - the statement, rewritten in place
 */

static void rwStmt(Loader *L, ModUnit *self, Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR:
        if (s->u.var.ann) rwType(L, self, s->u.var.ann);
        rwExpr(L, self, s->u.var.init);
        break;
    case ST_ASSIGN: rwExpr(L, self, s->u.assign.target); rwExpr(L, self, s->u.assign.value); break;
    case ST_IF:  rwExpr(L, self, s->u.ifs.cond);
                 rwStmt(L, self, s->u.ifs.thenBody); rwStmt(L, self, s->u.ifs.elseBody); break;
    case ST_WHILE: rwExpr(L, self, s->u.whiles.cond); rwStmt(L, self, s->u.whiles.body); break;
    case ST_RETURN: rwExpr(L, self, s->u.ret.value); break;
    case ST_EXPR:  rwExpr(L, self, s->u.expr.expr); break;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            rwStmt(L, self, *(Stmt **)vecAt(&s->u.block.stmts, i));
        break;
    case ST_MATCH:
        rwExpr(L, self, s->u.match.scrutinee);
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            rwStmt(L, self, (*(MatchArm **)vecAt(&s->u.match.arms, i))->body);
        break;
    case ST_BREAK: case ST_CONTINUE: break;
    }
}

/* ------------------------------------------------------------------ merging */

/* Rename the declarations of a unit and record a return ticket for each of them.
 *
 * The root module is left completely alone. For every other unit, each declaration
 * name is prefixed with the module name and entered in `u->ren`, so that a reference
 * written inside the module can be mapped to the name that was actually emitted.
 * `extern!` declarations keep their names, because their names are linker symbols.
 *
 * Params:
 *   L - loader whose arena owns the generated names
 *   u - the unit to rename in place
 *
 * Notes:
 *   - Every name is computed before any of them is stored. `mangleName` returns a
 *     pointer into an arena buffer that a later allocation may reuse, so computing
 *     and storing one name at a time overwrites the previous one. The symptom was
 *     ugly and silent: the alias table printed `pair=>make`, meaning the target of
 *     `pair` had been overwritten by the name computed for `make`, so the bare name
 *     `pair` stopped resolving and the error was `unknown type pair` with the real
 *     cause in memory reuse, not in any lookup.
 *   - `srcName` is filled here, while `d->name` is still the source name. Filling it
 *     in `mergeUnit` would build `alpha::alpha$pair`, because by then the name has
 *     already been renamed.
 */

static void mangleUnitDecls(Loader *L, ModUnit *u) {
    if (!u->modName || !*u->modName) return;              /* the root module: never renamed */
    Module *src = &u->mod;
    /* Allocate every name first and only then fill the table. */
    const char **names = (const char **)arenaAlloc(L->a, sizeof(char *) * 64);
    size_t n = 0;
    /* The user-facing name is built on this pass, while the source name is still
     * available: `alpha::pair`. */
    for (size_t i = 0; i < src->structs.len && n < 64; i++) {
        StructDef *d = *(StructDef **)vecAt(&src->structs, i);
        d->srcName = arenaPrintf(L->a, "%s::%s", u->modName, d->name);
        names[n++] = mangleName(L, u, d->name);
    }
    for (size_t i = 0; i < src->types.len && n < 64; i++) {
        TypeDef *d = *(TypeDef **)vecAt(&src->types, i);
        d->srcName = arenaPrintf(L->a, "%s::%s", u->modName, d->name);
        names[n++] = mangleName(L, u, d->name);
    }
    for (size_t i = 0; i < src->globals.len && n < 64; i++)
        names[n++] = mangleName(L, u, (*(GlobalDef **)vecAt(&src->globals, i))->name);
    for (size_t i = 0; i < src->funcs.len && n < 64; i++) {
        const char *fn = (*(FuncDef **)vecAt(&src->funcs, i))->name;
        /* An `extern!` keeps its name: see the note on `externKeepsName`. */
        names[n++] = externKeepsName(src, fn) ? fn : mangleName(L, u, fn);
    }
    /* Every name is computed, so this pass only stores pointers and allocates nothing. */
    size_t k = 0;
    for (size_t i = 0; i < src->structs.len; i++) {
        StructDef *d = *(StructDef **)vecAt(&src->structs, i);
        Ren *r = (Ren *)vecPush(&u->ren); r->from = d->name; r->to = names[k++]; }
    for (size_t i = 0; i < src->types.len; i++) {
        TypeDef *d = *(TypeDef **)vecAt(&src->types, i);
        Ren *r = (Ren *)vecPush(&u->ren); r->from = d->name; r->to = names[k++]; }
    for (size_t i = 0; i < src->globals.len; i++) {
        GlobalDef *d = *(GlobalDef **)vecAt(&src->globals, i);
        Ren *r = (Ren *)vecPush(&u->ren); r->from = d->name; r->to = names[k++]; }
    for (size_t i = 0; i < src->funcs.len; i++) {
        FuncDef *d = *(FuncDef **)vecAt(&src->funcs, i);
        Ren *r = (Ren *)vecPush(&u->ren);
        r->from = d->name; r->to = names[k++];
        /* When `to` equals `from` nothing is renamed, but the return ticket is still
         * recorded - it maps the name to itself. That keeps `renLookup` returning a
         * value, so a bare reference inside the module is not skipped. */}
    for (size_t i = 0; i < u->ren.len; i++) {
        Ren *r = (Ren *)vecAt(&u->ren, i);
        for (size_t j = 0; j < src->structs.len; j++)
            if (strcmp((*(StructDef **)vecAt(&src->structs, j))->name, r->from) == 0)
                (*(StructDef **)vecAt(&src->structs, j))->name = r->to;
        for (size_t j = 0; j < src->types.len; j++)
            if (strcmp((*(TypeDef **)vecAt(&src->types, j))->name, r->from) == 0)
                (*(TypeDef **)vecAt(&src->types, j))->name = r->to;
        for (size_t j = 0; j < src->globals.len; j++)
            if (strcmp((*(GlobalDef **)vecAt(&src->globals, j))->name, r->from) == 0)
                (*(GlobalDef **)vecAt(&src->globals, j))->name = r->to;
        for (size_t j = 0; j < src->funcs.len; j++)
            if (strcmp((*(FuncDef **)vecAt(&src->funcs, j))->name, r->from) == 0)
                (*(FuncDef **)vecAt(&src->funcs, j))->name = r->to;
    }
    if (getenv("EXTC_DBG_M")) {
        fprintf(stderr, "[mangle] %s: %zu declarations renamed:", u->modName, u->ren.len);
        for (size_t i = 0; i < u->ren.len; i++) { Ren *r = (Ren *)vecAt(&u->ren, i); fprintf(stderr, " %s->%s", r->from, r->to); }
        fprintf(stderr, "\n");
    }
}

/* Rewrite a bare name written inside a module to the name that module declares.
 *
 * A bare name is what a module writes for its own declarations: `color` in
 * `color.green`, `twice` in a call from `shout`. Once the declarations are renamed,
 * those bare names no longer name anything: the error is
 * `call to undefined function \`twice\`` while `greet$twice` is right there in the
 * table.
 *
 * Only names this unit declares itself are rewritten, never "any name some module
 * exports". With the wider rule, a bare name that another module also declares would
 * be quietly rewritten to that other declaration, so the program compiles and means
 * something else - far worse than a rejection.
 *
 * A bare name that this unit declares means this unit's declaration, including a
 * `@private` one: visibility is checked separately, at the point where a qualified
 * reference is required.
 *
 * Params:
 *   self - the unit whose rename table is consulted
 *   e    - an identifier expression, updated in place
 */

static void rwExprName(ModUnit *self, Expr *e) {
    if (!e || e->kind != EX_IDENT) return;
    const char *nm = e->u.ident.name;
    /* In callee position the name may already have become `lib$open` (see
     * `rwQualified`), so this has to be able to fall back to the source name `open`.
     * Without the fallback, the diagnostic that asks for a qualified name turns into
     * "`lib$open` belongs to module lib - write `lib::lib$open`", which is nonsense. */
    if (!nm) nm = e->u.ident.srcName;
    const char *m = nm ? renLookup(self, nm) : NULL;
    if (m) { e->u.ident.srcName = nm; e->u.ident.name = m; }
}

/* Merge one unit's declarations into the program module.
 *
 * Declarations are renamed first, then every type annotation, expression, and
 * statement in them has its qualified names resolved, and finally the declarations are
 * appended to the merged module. Units are merged in topological order, so a
 * dependency is already present when the declarations that use it arrive.
 *
 * Params:
 *   L - loader owning the merged module
 *   u - the unit to merge
 *
 * Notes:
 *   - The declarations are rewritten while they still live in `u->mod`, before they
 *     are appended, so the rewrite passes see the unit's own name table.
 *   - Merging mutates the declarations in place; they are the same objects afterwards.
 */

static void mergeUnit(Loader *L, ModUnit *u) {
    Module *src = &u->mod;
    mangleUnitDecls(L, u);
    for (size_t i = 0; i < src->structs.len; i++) {
        StructDef *s = *(StructDef **)vecAt(&src->structs, i);
        s->modName = u->modName;
        for (size_t j = 0; j < s->fields.len; j++)
            rwType(L, u, (*(FieldDef **)vecAt(&s->fields, j))->type);
        for (size_t j = 0; j < s->methods.len; j++) {
            FuncDef *m = *(FuncDef **)vecAt(&s->methods, j);
            m->modName = u->modName;
            if (m->ret) rwType(L, u, m->ret);
            for (size_t k = 0; k < m->params.len; k++)
                rwType(L, u, (*(Param **)vecAt(&m->params, k))->type);
            rwStmt(L, u, m->body);
        }
        *(StructDef **)vecPush(&L->out->structs) = s;
    }
    for (size_t i = 0; i < src->types.len; i++) {
        TypeDef *t = *(TypeDef **)vecAt(&src->types, i);
        t->modName = u->modName;
        for (size_t k = 0; k < t->variants.len; k++) {
            Variant *v = *(Variant **)vecAt(&t->variants, k);
            for (size_t j = 0; j < v->types.len; j++)
                rwType(L, u, *(Type **)vecAt(&v->types, j));
        }
        *(TypeDef **)vecPush(&L->out->types) = t;
    }
    for (size_t i = 0; i < src->globals.len; i++) {
        GlobalDef *g = *(GlobalDef **)vecAt(&src->globals, i);
        g->modName = u->modName;
        if (g->ann) rwType(L, u, g->ann);
        rwExpr(L, u, g->init);
        *(GlobalDef **)vecPush(&L->out->globals) = g;
    }
    for (size_t i = 0; i < src->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&src->funcs, i);
        f->modName = u->modName;
        if (f->ret) rwType(L, u, f->ret);
        for (size_t j = 0; j < f->params.len; j++)
            rwType(L, u, (*(Param **)vecAt(&f->params, j))->type);
        rwStmt(L, u, f->body);
        *(FuncDef **)vecPush(&L->out->funcs) = f;
    }
}

/* --------------------------------------------------------- recursive loading */

/* Load a module and everything it imports, or return the unit already loaded.
 *
 * Params:
 *   L            - loader
 *   modPath      - the module path as written in the `use`
 *   importerFile - file containing that `use`, used to resolve and to blame
 *   line         - line of the `use`, blamed by the cycle diagnostic
 *
 * Returns:
 *   The unit for this module, or NULL when its file could not be resolved or read; all
 *   diagnostics are already reported in that case.
 *
 * Notes:
 *   - The unit is registered before its source is parsed, and its state says
 *     "loading". A `use` that reaches a module in that state is a cycle, which is
 *     refused because each module's types are needed to check the other. The message
 *     suggests moving the shared part into a third module.
 *   - A module that is already loaded is returned as it is, so a diamond of imports
 *     loads each file once and the unit list stays a duplicate check.
 *   - Dependencies are loaded after the module itself is parsed, and each is pushed
 *     onto `order` on the way out, which makes `order` a post-order and therefore a
 *     topological order.
 */

static ModUnit *loadUnit(Loader *L, const char *modPath, const char *importerFile, int line) {
    char *file = resolveModFile(L, modPath, importerFile);
    if (!file) return NULL;

    ModUnit *u = findUnit(L, file);
    if (u) {
        if (u->state == 1) {                       /* still loading: a cycle */
            fprintf(stderr,
                    "%s:%d: error: import cycle: `%s` is still being loaded\n"
                    "note:  two modules must not depend on each other (each one's type is\n"
                    "       needed to check the other). Put the shared part in a third module.\n",
                    importerFile, line, modPath);
            L->errors++;
        }
        return u;                                  /* already loaded: reuse it */
    }

    size_t len = 0;
    char *src = readWhole(L->a, file, &len);
    if (!src) {
        fprintf(stderr, "error: cannot read `%s`\n", file);
        L->errors++;
        return NULL;
    }

    u = (ModUnit *)arenaAllocZero(L->a, sizeof(ModUnit));
    u->file    = file;
    u->modName = baseNameNoExt(L->a, file);
    vecInit(&u->ren, L->a, sizeof(Ren));
    u->state   = 1;                                /* loading */
    moduleInit(&u->mod, L->a);
    u->ctx = (Ctx *)arenaAllocZero(L->a, sizeof(Ctx));
    ctxInit(u->ctx, L->a, file, src, len);
    *(ModUnit **)vecPush(&L->units) = u;
    *(Ctx **)vecPush(L->ctxs) = u->ctx;

    Vec toks;
    vecInit(&toks, L->a, sizeof(Token));
    lexAll(u->ctx, &toks);
    if (!u->ctx->hasError) parseModule(u->ctx, L->a, &toks, &u->mod);
    if (getenv("EXTC_DBG_MOD"))
        fprintf(stderr, "[mod] loaded %s: %zu tokens, %zu funcs, %zu globals\n",
                file, toks.len, u->mod.funcs.len, u->mod.globals.len);
    if (u->ctx->hasError) { L->errors++; u->state = 2; return u; }

    /* Only the root file may define `main`: a module is a library. */
    for (size_t i = 0; i < u->mod.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&u->mod.funcs, i);
        if (strcmp(f->name, "main") == 0) {
            ctxError(u->ctx, f->line, 1,
                     "A module is a library -- the program starts at the file you pass on the"
                     " command line.",
                     "`main` must live in the entry file, not in a module");
            L->errors++;
        }
    }
    /* A short-name clash: `use a::util` next to `use b::util` cannot be told apart
     * when the name is used. */
    for (size_t i = 0; i < u->mod.uses.len; i++) {
        UseDecl *a = *(UseDecl **)vecAt(&u->mod.uses, i);
        for (size_t j = i + 1; j < u->mod.uses.len; j++) {
            UseDecl *b = *(UseDecl **)vecAt(&u->mod.uses, j);
            if (strcmp(a->shortName, b->shortName) == 0 && strcmp(a->path, b->path) != 0) {
                ctxError(u->ctx, b->line, 1,
                         "Two modules with the same last name cannot be told apart in the source."
                         " Rename one of the files.",
                         "both `%s` and `%s` are used as `%s`", a->path, b->path, b->shortName);
                L->errors++;
            }
        }
    }

    /* Load the dependencies; each enters `order` after its own dependencies. */
    for (size_t i = 0; i < u->mod.uses.len; i++) {
        UseDecl *ud = *(UseDecl **)vecAt(&u->mod.uses, i);
        ModUnit *dep = loadUnit(L, ud->path, file, ud->line);
        ud->file = dep ? dep->file : NULL;
    }
    u->state = 2;
    *(ModUnit **)vecPush(&L->order) = u;           /* post-order: dependencies are in first */
    return u;
}

/* ------------------------------------------------------------------- entry */

/* Load every module the root file imports and merge them into `out`.
 *
 * Params:
 *   a          - arena that owns every loaded module
 *   out        - the merged module; the prelude must already be in it
 *   rootm      - the root file's own parsed module
 *   rootCtx    - the root file's source context, which blames errors in that file
 *   rootPath   - path of the root file; its directory becomes the project root
 *   searchDirs - directories from `-I`, or NULL
 *   outCtxs    - receives the Ctx of every loaded module so the caller can render
 *                their diagnostics afterwards, or NULL when the caller does not need
 *                them
 *
 * Returns:
 *   False when any error was reported; the diagnostics are already printed and
 *   compilation must stop.
 *
 * Notes:
 *   - `rootCtx` is stored by pointer, never copied. A copy would swallow the
 *     diagnostics reported through it.
 *   - `outCtxs` is used by pointer as well, because its length has to reach the
 *     caller.
 */

bool loadModules(Arena *a, Module *out, Module *rootm, Ctx *rootCtx,
                 const char *rootPath, Vec *searchDirs, Vec *outCtxs) {
    Loader L;
    memset(&L, 0, sizeof L);
    L.a = a;
    L.out = out;
    L.rootDir = dirOf(a, rootPath);          /* project root */
    /* The standard library directory: `$EXTC_STD` wins, otherwise it is
     * `<directory of the extc binary>/../stdlib`. The prelude is embedded and is not
     * here; a real module such as `std::io` needs a real file. */
    {
        const char *env = getenv("EXTC_STD");
        if (env && *env) L.stdDir = env;
        else {
            char buf[4096];
            ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
            if (n > 0) {
                buf[n] = '\0';
                char *slash = strrchr(buf, '/');
                if (slash) { *slash = '\0'; L.stdDir = arenaPrintf(a, "%s/../stdlib", buf); }
            }
        }
    }
    vecInit(&L.units, a, sizeof(void *));
    vecInit(&L.order, a, sizeof(void *));
    if (outCtxs) { L.ctxs = outCtxs; if (!outCtxs->arena) vecInit(outCtxs, a, sizeof(void *)); }
    else { L.ctxs = (Vec *)arenaAllocZero(a, sizeof(Vec)); vecInit(L.ctxs, a, sizeof(void *)); }
    if (searchDirs) L.searchDirs = *searchDirs;
    else vecInit(&L.searchDirs, a, sizeof(void *));

    /* The root file's own imports. */
    for (size_t i = 0; i < rootm->uses.len; i++) {
        UseDecl *u = *(UseDecl **)vecAt(&rootm->uses, i);
        ModUnit *dep = loadUnit(&L, u->path, rootPath, u->line);
        u->file = dep ? dep->file : NULL;
    }
    /* The root file's short-name clashes are checked the same way. */
    for (size_t i = 0; i < rootm->uses.len; i++) {
        UseDecl *x = *(UseDecl **)vecAt(&rootm->uses, i);
        for (size_t j = i + 1; j < rootm->uses.len; j++) {
            UseDecl *y = *(UseDecl **)vecAt(&rootm->uses, j);
            if (strcmp(x->shortName, y->shortName) == 0 && strcmp(x->path, y->path) != 0) {
                fprintf(stderr, "error: both `%s` and `%s` are used as `%s`\n"
                                "note:  two modules with the same last name cannot be told apart\n"
                                "       in the source -- rename one of the files.\n",
                        x->path, y->path, x->shortName);
                L.errors++;
            }
        }
    }
    if (getenv("EXTC_DBG_MOD")) fprintf(stderr, "[mod] load done: errors=%d units=%zu order=%zu\n",
                                         L.errors, L.units.len, L.order.len);
    if (L.errors) return false;

    /* Merge the modules in topological order, dependencies first. */
    for (size_t i = 0; i < L.order.len; i++)
        mergeUnit(&L, *(ModUnit **)vecAt(&L.order, i));

    /* Collect the return tickets for bare names. This has to run after `mergeUnit`,
     * because that is where the rename tables are built. */
    if (!out->aliases.arena) vecInit(&out->aliases, a, sizeof(Alias));
    for (size_t i = 0; i < L.order.len; i++) {
        ModUnit *dep = *(ModUnit **)vecAt(&L.order, i);
        for (size_t j = 0; j < dep->ren.len; j++) {
            Ren *r = (Ren *)vecAt(&dep->ren, j);
            /* Both entries are kept when two modules export the same name: the type
             * table has to be able to count two matches and report "ambiguous, write a
             * qualified name". Collapsing them into one would make a bare name resolve
             * silently to whichever module registered first - it compiles and has the
             * wrong type. */
            bool same = false;
            for (size_t k = 0; k < out->aliases.len && !same; k++) {
                Alias *seen = (Alias *)vecAt(&out->aliases, k);
                if (strcmp(seen->from, r->from) == 0 && strcmp(seen->to, r->to) == 0) same = true;
            }
            if (same) continue;                /* the same entry, from importing one module twice */
            Alias *al = (Alias *)vecPush(&out->aliases);
            al->from = r->from;  al->to = r->to;
        }
    }

    /* The root file's declarations are merged last, since the modules it uses are
     * already in place. Its function bodies need their qualified names resolved too. */
    {
        ModUnit root;
        memset(&root, 0, sizeof root);
        root.file    = rootPath;
        root.mod     = *rootm;
        root.ctx     = rootCtx;   /* the real context, never a copy */
        root.state   = 2;
        root.modName = NULL;
        for (size_t i = 0; i < rootm->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&rootm->structs, i);
            for (size_t j = 0; j < sd->fields.len; j++)
                rwType(&L, &root, (*(FieldDef **)vecAt(&sd->fields, j))->type);
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *m = *(FuncDef **)vecAt(&sd->methods, j);
                if (m->ret) rwType(&L, &root, m->ret);
                for (size_t k = 0; k < m->params.len; k++)
                    rwType(&L, &root, (*(Param **)vecAt(&m->params, k))->type);
                rwStmt(&L, &root, m->body);
            }
            *(StructDef **)vecPush(&out->structs) = sd;
        }
        for (size_t i = 0; i < rootm->types.len; i++)
            *(TypeDef **)vecPush(&out->types) = *(TypeDef **)vecAt(&rootm->types, i);
        for (size_t i = 0; i < rootm->globals.len; i++) {
            GlobalDef *g = *(GlobalDef **)vecAt(&rootm->globals, i);
            if (g->ann) rwType(&L, &root, g->ann);
            rwExpr(&L, &root, g->init);
            *(GlobalDef **)vecPush(&out->globals) = g;
        }
        for (size_t i = 0; i < rootm->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&rootm->funcs, i);
            if (f->ret) rwType(&L, &root, f->ret);
            for (size_t j = 0; j < f->params.len; j++)
                rwType(&L, &root, (*(Param **)vecAt(&f->params, j))->type);
            rwStmt(&L, &root, f->body);
            *(FuncDef **)vecPush(&out->funcs) = f;
        }
    }
    if (getenv("EXTC_DBG_MOD")) fprintf(stderr, "[mod] merge done: errors=%d declarations funcs=%zu globals=%zu\n",
                                         L.errors, out->funcs.len, out->globals.len);
    if (L.errors) return false;

    return true;
}
