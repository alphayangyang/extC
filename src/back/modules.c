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
#include "check.h"     /* hintIndexSetStdDir: the checker's INDEX hint */
#include "parser.h"
#include "plan.h"      /* the qualified-name flag the loader records on a node */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* readlink: locate `<extc>/../stdlib` */
#include <time.h>

/* Time-based split of the loader: it is the steepest phase (x5.8 per doubling) and callgrind's
 * instruction ranking does not explain it (strstr is 71% of instructions but SIMD-cheap, getenv
 * 1.6% and free in time). So measure time: mergeUnit per unit against everything else. */
/**
 * @brief Report whether `EXTC_DBG_TIME` is set, reading the environment once and caching it.
 *
 * `getenv` would otherwise sit in loops that run per local, per candidate and per level; the
 * environment cannot change while the compiler runs, so the cached answer is stable.
 *
 * @return  true when the variable is set.
 */
static int ldTimeOn(void) { static int v = -1; if (v < 0) v = getenv("EXTC_DBG_TIME") != NULL; return v; }

/**
 * @brief Return the process CPU time in seconds, for the phase timings.
 *
 * @return  `clock()` converted to seconds.
 */
static double ldNow(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }

/**
 * @brief Print one phase's elapsed seconds when `EXTC_DBG_TIME` is set.
 *
 * @param n   phase name.
 * @param t0  start time, from `ldNow`.
 */
static void ldPhase(const char *n, double t0) { if (ldTimeOn()) fprintf(stderr, "[time] %-9s %.3f s\n", n, ldNow() - t0); }
static long g_renCalls, g_renEarly, g_renCmps, g_usCalls, g_usCmps, g_utCalls, g_utCmps;
static long g_stN, g_exN;

/**
 * @brief Print the walk counters to stderr when `EXTC_DBG_TIME` is set.
 *
 * Read-only: it reports the counters and changes nothing.
 */
static void ldCount(void) {
    if (ldTimeOn()) {
        fprintf(stderr, "[walk] rwStmt=%ld rwExpr=%ld | renLookup=%ld early=%ld cmps=%ld",
                g_stN, g_exN, g_renCalls, g_renEarly, g_renCmps);
        fprintf(stderr, " | unitStruct=%ld cmps=%ld | unitType=%ld cmps=%ld\n",
                g_usCalls, g_usCmps, g_utCalls, g_utCmps);
    }
}
typedef struct { const char *name; double sec; } LdAcc;
static LdAcc ldAccs[8];

/**
 * @brief Add the elapsed seconds of one phase to the debug breakdown table.
 *
 * Entries with the same name are summed; once the eight-entry table is full the call is
 * dropped, which only affects the debug output.
 *
 * @param n    breakdown name, compared by content rather than by pointer.
 * @param sec  elapsed seconds of this call.
 */
static void ldAcc(const char *n, double sec) {
    if (!ldTimeOn()) return;
    for (size_t i = 0; i < 8; i++) {
        if (!ldAccs[i].name) { ldAccs[i].name = n; ldAccs[i].sec = sec; return; }
        if (strcmp(ldAccs[i].name, n) == 0) { ldAccs[i].sec += sec; return; }
    }
}
/**
 * @brief Print the summed breakdown when `EXTC_DBG_TIME` is set.
 */
static void ldReport(void) {
    if (!ldTimeOn()) return;
    for (size_t i = 0; i < 8 && ldAccs[i].name; i++)
        fprintf(stderr, "[time] %-9s %.3f s\n", ldAccs[i].name, ldAccs[i].sec);
}

/* -------------------------------------------------------------- small helpers */

/**
 * @brief Report whether `path` can be opened for reading.
 *
 * @param path  file to try.
 * @return      true when it opens; false otherwise.
 */
static bool fileExists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/**
 * @brief Take the module short name out of a path: `dir/io.extc` -> `io`.
 *
 * Only a trailing `.extc` is stripped; any other suffix is kept.
 *
 * @param a     arena that owns the result.
 * @param path  file path.
 * @return      the short name, owned by the arena.
 */
static const char *baseNameNoExt(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t n = strlen(base);
    if (n > 5 && strcmp(base + n - 5, ".extc") == 0) n -= 5;
    return arenaStrndup(a, base, n);
}

/**
 * @brief Turn a module path into a relative file path: `std::io` -> `std/io`.
 *
 * @param a        arena that owns the result.
 * @param modPath  module path with `::` separators.
 * @return         the path with every `::` replaced by `/`.
 */
static char *pathToRel(Arena *a, const char *modPath) {
    Buf b;
    bufInit(&b, a);
    for (const char *p = modPath; *p; p++) {
        if (p[0] == ':' && p[1] == ':') { bufPutc(&b, '/'); p++; }
        else bufPutc(&b, *p);
    }
    return bufCstr(&b);
}

/**
 * @brief Take the directory part of a path: `dir/io.extc` -> `dir`; no slash means `.`.
 *
 * @param a     arena that owns the result.
 * @param path  file path.
 * @return      the directory part, owned by the arena.
 */
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

/**
 * @brief Find an import path that would really work, or NULL when none does.
 *
 * The name a module is **called by** is not the path it is imported with: `use std::sys::domain`
 * brings that module in as `domain`, so suggesting "add `use sys::domain`" tells the reader to
 * import something that does not exist -- the standard library lives under `std/`.
 * Look the file up quietly and suggest the path that would really work; if nothing matches, say
 * nothing about a path and state the rule instead of inventing one.
 *
 * @param L        loader, supplying the search directories and the standard library directory.
 * @param spelled  one segment of a qualified name; NULL or empty returns NULL.
 * @return         the `use` path that works (with a `std::` prefix when needed), or NULL.
 */
static const char *suggestImportPath(Loader *L, const char *spelled) {
    if (!spelled || !*spelled) return NULL;
    Buf rel;
    bufInit(&rel, L->a);
    for (const char *s = spelled; *s; s++) {
        if (s[0] == ':' && s[1] == ':') { bufPutc(&rel, '/'); s++; }
        else bufPutc(&rel, *s);
    }
    const char *relC = bufCstr(&rel);
    /* `stdDir` is the directory that *holds* `std/…`, so the candidates under it are
     * `<stdDir>/<rel>` and `<stdDir>/std/<rel>`; `sys::domain` really lives in the second
     * (`…/std/sys/domain.extc` ⇒ `use std::sys::domain`). */
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && !L->stdDir) break;
        size_t n = (pass == 0) ? L->searchDirs.len : 1;
        for (size_t i = 0; i < n; i++) {
            const char *dir = (pass == 0) ? *(const char **)vecAt(&L->searchDirs, i) : L->stdDir;
            const char *subs[2] = { "", "std/" };
            for (int k = 0; k < 2; k++) {
                Buf b;
                bufInit(&b, L->a);
                bufPrintf(&b, "%s/%s%s.extc", dir, subs[k], relC);
                FILE *fp = fopen(bufCstr(&b), "r");
                if (!fp) continue;
                fclose(fp);
                /* The suggestion is spelled with `::`, which is exactly the caller's own
                 * spelling (`relC` is only that with `/` for the file system). */
                if (subs[k][0] == '\0') return spelled;
                Buf u;
                bufInit(&u, L->a);
                bufPrintf(&u, "std::%s", spelled);
                return bufCstr(&u);
            }
        }
    }
    return NULL;
}


/**
 * @brief Prefix a declaration name with its module name: `readLine` -> `io$readLine`.
 *
 * `$` is legal in a C identifier and illegal in an extC identifier, so a generated name can
 * never collide with a name the user wrote. The result lives in the arena, and another
 * `arenaPrintf` may reuse that buffer: compute every name first and only then store the
 * pointers (see `mangleUnitDecls`).
 *
 * @param L     loader whose arena owns the built string.
 * @param u     the declaration's unit; a NULL or empty module name leaves `name` alone,
 *              which is how the root file keeps its names.
 * @param name  the source name.
 * @return      the prefixed name, or `name` unchanged for the root module.
 */
static const char *mangleName(Loader *L, ModUnit *u, const char *name) {
    if (!u->modName || !*u->modName) return name;
    return arenaPrintf(L->a, "%s$%s", u->modName, name);
}

/**
 * @brief Report whether `name` is declared by an `extern!` in this module.
 *
 * An `extern!` name is the symbol the linker has to find, so it must never be prefixed:
 * `extern!("libc") fn read(...)` declared as `sys$read` only produces
 * `undefined reference to sys$read`, reported by the linker and hard to trace back to
 * module renaming. This is why every libc primitive in the standard library's system
 * module keeps its own name.
 *
 * @param src   the module to search.
 * @param name  the function name to look for.
 * @return      true when the module declares `name` as an external function; the name is
 *              then kept and its return ticket maps it to itself, so references inside the
 *              module need no rewrite either.
 */
static bool externKeepsName(Module *src, const char *name) {
    for (size_t i = 0; i < src->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&src->funcs, i);
        if (f->isExtern && strcmp(f->name, name) == 0) return true;
    }
    return false;
}
/**
 * @brief Look up the name a declaration received, in the unit that declares it.
 *
 * The lookup must use the *target* unit and not the caller: the root file has a NULL module
 * name, so a lookup keyed on the caller would return early and leave every reference from
 * the root file unrenamed.
 *
 * @param target  the unit whose rename table is consulted.
 * @param name    the source name.
 * @return        the generated name, or `name` when the unit has no rename for it.
 */
static const char *renOfTarget(ModUnit *target, const char *name) {
    if (!target || !target->modName || !*target->modName) return name;
    for (size_t i = 0; i < target->ren.len; i++) {
        Ren *r = (Ren *)vecAt(&target->ren, i);
        if (strcmp(r->from, name) == 0) return r->to;
    }
    return name;
}
/**
 * @brief Look up `name` in a unit's own rename table.
 *
 * Only names this unit itself declares are mapped. Mapping "any name any module exports"
 * would silently rewrite a local reference to another module's declaration, which compiles
 * and quietly means something else.
 *
 * @param u     the unit whose own declarations are searched.
 * @param name  the source name written in that unit.
 * @return      the generated name, or NULL when this unit declares nothing of that name.
 */
static const char *renLookup(ModUnit *u, const char *name) {
    g_renCalls++;
    if (!u || !u->modName || !*u->modName) { g_renEarly++; return name; }
    for (size_t i = 0; i < u->ren.len; i++) {
        Ren *r = (Ren *)vecAt(&u->ren, i);
        g_renCmps++;
        if (strcmp(r->from, name) == 0) return r->to;
    }
    return NULL;
}

/**
 * @brief Find the unit already loaded from `file`.
 *
 * @param L     loader.
 * @param file  the resolved path, which is the unique key of a unit.
 * @return      the unit, or NULL when this file has not been loaded.
 */
static ModUnit *findUnit(Loader *L, const char *file) {
    for (size_t i = 0; i < L->units.len; i++) {
        ModUnit *u = *(ModUnit **)vecAt(&L->units, i);
        if (u && strcmp(u->file, file) == 0) return u;
    }
    return NULL;
}

/**
 * @brief Find the file a `use` names, searching the closest directory first.
 *
 * The search order is the project root, then the `-I` directories in the order they were
 * given, then the standard library directory. The first hit wins, so a project library
 * shadows a standard one of the same name.
 *
 * @param L        loader, supplying the root and the search directories.
 * @param modPath  the module path as written, e.g. `std::io`.
 * @return         the resolved path, or NULL when nothing matched.
 *
 * Diagnostics belong to the caller `resolveModFile`, which lists every path that was tried.
 */
static char *findModFile(Loader *L, const char *modPath) {
    char *rel  = pathToRel(L->a, modPath);
    char *cand = arenaPrintf(L->a, "%s.extc", rel);

    char *p = arenaPrintf(L->a, "%s/%s", L->rootDir, cand);
    if (fileExists(p)) return p;
    for (size_t i = 0; i < L->searchDirs.len; i++) {
        const char *sd = *(const char **)vecAt(&L->searchDirs, i);
        char *q = arenaPrintf(L->a, "%s/%s", sd, cand);
        if (fileExists(q)) return q;
    }
    const char *stdDir = L->stdDir;
    if (stdDir && *stdDir) {
        char *r = arenaPrintf(L->a, "%s/%s", stdDir, cand);
        if (fileExists(r)) return r;
    }
    return NULL;
}

/**
 * @brief Resolve a module path to a file, reporting a diagnostic that lists every path tried.
 *
 * Before giving up it says what the reader most likely meant: the last segment may be a
 * **name** of the module named by the path before it, and `use` resolves module paths only.
 *
 * @param L             loader.
 * @param modPath       the module path as written.
 * @param importerFile  the file containing the `use`, named in the diagnostic.
 * @return              the resolved path; NULL after reporting the error (`L->errors` counts it).
 */
static char *resolveModFile(Loader *L, const char *modPath, const char *importerFile) {
    char *found = findModFile(L, modPath);
    if (found) return found;

    /* Nothing there. The last segment may be a **name** rather than a module:
     * `use std::io::cin` reads like "bring `cin` in", which is what a brace list does,
     * while `use` resolves module paths only -- the reader is owed that spelling, not a
     * list of directories that were tried. */
    const char *lastSep = strrchr(modPath, ':');
    if (lastSep && lastSep > modPath) {
        /* `lastSep` is the **second** colon of the final `::`, so the name starts right
         * after it and the parent path ends right before it (one character too many
         * reports ``in`` for `std::io::cin`, which reads like a different name). */
        size_t parentLen = (size_t)(lastSep - modPath - 1);   /* drop the trailing `::` */
        char *parent = (char *)arenaAlloc(L->a, parentLen + 1);
        memcpy(parent, modPath, parentLen);
        parent[parentLen] = '\0';
        if (findModFile(L, parent))
            fprintf(stderr,
                    "note:  `%s` is a **name**, not a module -- `use` resolves module paths only.\n"
                    "       To bring it into scope: `use %s::{%s}` (or `use %s::*` for every\n"
                    "       public name of that module).\n",
                    lastSep + 1, parent, lastSep + 1, parent);
    }

    char *rel  = pathToRel(L->a, modPath);
    char *cand = arenaPrintf(L->a, "%s.extc", rel);
    Buf   tried;
    bufInit(&tried, L->a);
    bufPrintf(&tried, "\n        %s/%s", L->rootDir, cand);
    for (size_t i = 0; i < L->searchDirs.len; i++)
        bufPrintf(&tried, "\n        %s/%s", *(const char **)vecAt(&L->searchDirs, i), cand);
    const char *stdDir = L->stdDir;
    if (stdDir && *stdDir) bufPrintf(&tried, "\n        %s/%s", stdDir, cand);

    fprintf(stderr,
            "error: cannot find module `%s` (imported by %s)\n"
            "note:  `use a::b` looks for a file `a/b.extc`. Searched:%s\n"
            "note:  add a search directory with `-I <dir>`, or point $EXTC_STD at the\n"
            "       standard library's directory.\n",
            modPath, importerFile, bufCstr(&tried));
    L->errors++;
    return NULL;
}

/**
 * @brief Report whether `name` is a type this module declares itself.
 *
 * When it is, `a::b` is an associated item of that type and not a module reference at all.
 *
 * @param self  the unit being rewritten.
 * @param name  the left-hand side of an `a::b` form.
 * @return      true when the module declares a struct or enum of that name.
 */
static bool isOwnType(ModUnit *self, const char *name) {
    for (size_t i = 0; i < self->mod.structs.len; i++)
        if (strcmp((*(StructDef **)vecAt(&self->mod.structs, i))->name, name) == 0) return true;
    for (size_t i = 0; i < self->mod.types.len; i++)
        if (strcmp((*(TypeDef **)vecAt(&self->mod.types, i))->name, name) == 0) return true;
    return false;
}

/**
 * @brief Report whether a file for `modPath` exists anywhere on the search path.
 *
 * Silent on purpose, and the two cases it separates need different messages: a module that
 * exists but was not imported is a missing `use`, while a name that matches no file at all
 * is not a module. Reporting "not imported" for the second case sends the user looking in
 * the wrong place.
 *
 * @param L        loader, supplying the search directories.
 * @param modPath  the module path as written.
 * @return         true when some search directory holds the file.
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

/**
 * @brief Find a function declared by this unit.
 *
 * The caller always passes a source name, while a declaration may already carry its
 * generated name, so the rename table is consulted first.
 *
 * @param u     the unit to search.
 * @param name  the source name the caller wrote.
 * @return      the declaration, or NULL when the unit declares nothing of that name.
 */
static FuncDef *unitFunc(ModUnit *u, const char *name) {
    const char *want = renLookup(u, name);
    if (!want) want = name;
    for (size_t i = 0; i < u->mod.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&u->mod.funcs, i);
        if (strcmp(f->name, want) == 0) return f;
    }
    return NULL;
}

/**
 * @brief Find a global declared by this unit.
 *
 * Same as `unitFunc`: a source name goes in, a possibly renamed declaration comes out.
 *
 * @param u     the unit to search.
 * @param name  the source name the caller wrote.
 * @return      the declaration, or NULL when the unit declares nothing of that name.
 */
static GlobalDef *unitGlobal(ModUnit *u, const char *name) {
    const char *want = renLookup(u, name);
    if (!want) want = name;
    for (size_t i = 0; i < u->mod.globals.len; i++) {
        GlobalDef *g = *(GlobalDef **)vecAt(&u->mod.globals, i);
        if (strcmp(g->name, want) == 0) return g;
    }
    return NULL;
}

/**
 * @brief Find a struct declared by this unit, accepting either of its names.
 *
 * `mergeUnit` renames declarations in place, and the root file's qualified names are
 * resolved after that merge, so by the time this runs the table already holds `e1$color`
 * and a search keyed on the source name `color` alone finds nothing: `e1::color` is then
 * rejected as "the module has nothing named color", which blames a name that is nowhere
 * near the real cause.
 *
 * @param u     the unit to search.
 * @param name  the source name the caller wrote.
 * @return      the declaration, or NULL when the unit has no struct of either name.
 */
static StructDef *unitStruct(ModUnit *u, const char *name) {
    g_usCalls++;
    const char *want = renLookup(u, name);
    if (!want) want = name;
    for (size_t i = 0; i < u->mod.structs.len; i++) {
        StructDef *s = *(StructDef **)vecAt(&u->mod.structs, i);
        g_usCmps += 2;
        if (strcmp(s->name, want) == 0 || strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

/**
 * @brief Find a type declaration of this unit, accepting either of its names.
 *
 * Renaming behaves exactly as in `unitStruct`.
 *
 * @param u     the unit to search.
 * @param name  the source name the caller wrote.
 * @return      the declaration, or NULL when the unit has no enum of either name.
 */
static TypeDef *unitType(ModUnit *u, const char *name) {
    g_utCalls++;
    const char *want = renLookup(u, name);
    if (!want) want = name;
    for (size_t i = 0; i < u->mod.types.len; i++) {
        TypeDef *t = *(TypeDef **)vecAt(&u->mod.types, i);
        g_utCmps += 2;
        if (strcmp(t->name, want) == 0 || strcmp(t->name, name) == 0) return t;
    }
    return NULL;
}

/**
 * @brief Find the module this unit imports under the full path `qname`.
 *
 * This has to exist next to the short-name lookup because a `UseDecl` carries two names:
 * `path`, the full path the user wrote (`std::sys::io`), and `shortName`, its last segment
 * (`io`). Matching on the short name alone means a reference written as
 * `std::sys::io::STDOUT` asks with a full path that never matches, and the user is told
 * `std` is not imported even though `use std::sys::io` is right there in the file. So one
 * rule: a short name matches a short name, a full path matches a full path, and both forms
 * must resolve to the same module.
 *
 * @param L      loader.
 * @param self   the unit whose `use` list is searched.
 * @param qname  a full module path as written, e.g. `std::sys::io`.
 * @return       the imported unit, or NULL when no import names that path.
 */
static ModUnit *importedAsPath(Loader *L, ModUnit *self, const char *qname) {
    for (size_t i = 0; i < self->mod.uses.len; i++) {
        UseDecl *u = *(UseDecl **)vecAt(&self->mod.uses, i);
        if (strcmp(u->path, qname) == 0)
            return u->file ? findUnit(L, u->file) : NULL;
    }
    return NULL;
}

/**
 * @brief Find the module this unit imports under the short name `shortName`.
 *
 * @param L          loader.
 * @param self       the unit whose `use` list is searched.
 * @param shortName  the last segment of an imported path, e.g. `io`.
 * @return           the imported unit, or NULL when nothing is imported under that name,
 *                   which is what makes "modules must be imported explicitly" enforceable.
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

/**
 * @brief Rewrite one type **name** (a plain string, not a Type node) into the flat module's naming.
 *
 * `rwType` does this inline for `TY_UNRESOLVED` nodes; an `impl` block's target is a string, so
 * it needs exactly the same treatment -- including the **qualified** form (`io::istream` ->
 * `io$istream`), which the *loader* rewrites and which therefore never reaches the checker in
 * its written shape.
 *
 * Two rules, and the reason the bare one exists at all:
 *   - a bare name is renamed only when **this unit declares it** (`pair` -> `liba$pair`).
 *     Left as the source name on a return type, it reaches the checker while the flat table
 *     holds two structs named `pair`, and the alias table picks one -- the reference is then
 *     silently bound to whichever module registered first, which compiles and has the wrong
 *     type. That is the worst kind of bug, so the rule matches `rwExprName`: only a name this
 *     module declares itself is rewritten;
 *   - a qualified name resolves through this unit's imports, and both a `@private` target and
 *     a missing one are errors here rather than later.
 *
 * @param L     loader whose arena owns the result.
 * @param self  the unit the name was written in.
 * @param name  the type name; NULL is returned unchanged.
 * @return      the rewritten name, or `name` when it is not a declaration of this unit or of
 *              an imported module.
 */
static const char *rwTypeName(Loader *L, ModUnit *self, const char *name) {
    if (!name) return name;
    const char *sep = strstr(name, "::");
    if (!sep) {
        const char *m = renLookup(self, name);
        return m ? m : name;
    }
    const char *shortName = arenaStrndup(L->a, name, (size_t)(sep - name));
    const char *rest = sep + 2;
    /* A `use` suggestion may only name a module, and `rest` can still contain `::`:
     * `alpha::pair` inside `lib::box<alpha::pair>` leaves `rest` as `pair`, but a
     * three-segment path would fold `b::c` into the suggestion. A message reading
     * "add `use alpha::pair`" would name a module that does not exist. */
    const char *restSep = strstr(rest, "::");
    const char *restTop = restSep ? arenaStrndup(L->a, rest, (size_t)(restSep - rest)) : rest;
    ModUnit *target = importedAs(L, self, shortName);
    if (!target) {
        /* State the **rule**, and give the **`use` line that really works** when a file can
         * be found: the spelling in the source (`sys::domain`) is only the name it is
         * **called** by, not the import path. */
        const char *sug1 = suggestImportPath(L, shortName);
        const char *msg1 = sug1
            ? arenaPrintf(L->a, "`%s` is not imported here -- add `use %s` and call it `%s::Name`",
                          shortName, sug1, strrchr(sug1, ':') ? strrchr(sug1, ':') + 1 : sug1)
            : arenaPrintf(L->a, "`%s` is not imported here -- add the `use` line that imports it"
                                " (the name you call it by is the last segment of that path)",
                          shortName);
        ctxError(self->ctx, 0, 1,
                 "Modules are imported explicitly (semantic import, not a textual include).",
                 "%s", msg1);
        L->errors++;
        return rest;
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
    return renOfTarget(target, rest);
}

/**
 * @brief Rewrite the qualified names inside a type, in place: `io::File` becomes the name of
 * the declaration in the merged module.
 *
 * **Every** composite type keeps its element/pointee in `inner` -- `[N]T`, `ref T`, `ptr T`,
 * `dyn T` and (for some spellings) `slice T` -- so the recursion must walk `inner`: `TY_REF`
 * plus `targs` alone leaves `[4]mod::Type` unrewritten, and the name keeps its `::` spelling
 * while the type table holds only the mangled `mod$Type`, which reports "unknown type
 * `mod::Type`" for a spelling that works in every other position.
 *
 * @param L     loader.
 * @param self  the unit the type was written in.
 * @param t     the type, rewritten in place; NULL returns immediately.
 */
static void rwType(Loader *L, ModUnit *self, Type *t) {
    if (!t) return;
    if (t->inner) rwType(L, self, t->inner);
    if (t->kind == TY_FN) {
        for (size_t i = 0; i < t->params.len; i++) rwType(L, self, *(Type **)vecAt(&t->params, i));
        rwType(L, self, t->ret);
    }
    for (size_t i = 0; i < t->targs.len; i++) rwType(L, self, *(Type **)vecAt(&t->targs, i));
    if (t->kind != TY_UNRESOLVED || !t->name) return;
    /* `rwTypeName` holds the name rules; this node is the one caller with a Type. */
    t->name = rwTypeName(L, self, t->name);
}


/**
 * @brief Resolve a type name written in expression position: `mod::Type { ... }` and
 * `mod::Type.variant`.
 *
 * The single test is whether `rest` names a *type* of the module `short`: those two forms
 * are the only way to write a type that another module declares under a name that is also
 * declared here, because the bare name is ambiguous and the loader therefore records no
 * return ticket for it. Without this pass such a type has no writable spelling, and a struct
 * literal or a variant of it cannot be named at all: the parser does not accept a qualified
 * name in a literal, and a lookup that treats the name as a function or a constant reports
 * "module `e1` has nothing named `color`".
 *
 * A qualified name that points at the current module is left alone. The prelude itself
 * contains `pcg32::withStream(...)` where `pcg32` is a struct of that same file, and the
 * prelude is one module: rewriting it to `prelude$pcg32` produces a name the type table never
 * saw, because the type table snapshots the prelude before module loading starts, and the
 * call is then reported as undefined.
 *
 * @param L      loader.
 * @param self   the unit the expression was written in.
 * @param qname  the qualified name to resolve.
 * @param out    receives the resolved declaration name.
 * @return       true when `qname` names a type of an imported module and `out` was filled;
 *               false means "not a qualified type name", so the caller leaves the expression
 *               to the ordinary path.
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
    /* A qualified name that points at this very unit is never rewritten (see above): the
     * prelude writes `pcg32::withStream(...)` for a struct of its own file, and `pcg32` is
     * registered before module loading, so a rewritten `prelude$pcg32` names nothing.
     * `rwQualified` guards the same case by letting a name that equals this file's own
     * name through. */
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

/**
 * @brief Resolve the module part of a deep qualified name such as `std::sys::io::STDOUT`.
 *
 * The parser splits that into the prefix `std::sys::io` and the symbol `STDOUT` and passes
 * the prefix here, so the only question left is whether that prefix is imported. The split
 * is positional -- every segment but the last is a module and the last one is the symbol --
 * so this side has exactly one job: match a full path against the full path recorded by a
 * `use`.
 *
 * @param L      loader.
 * @param self   the unit the name was written in.
 * @param qname  the module prefix, e.g. `std::sys::io`.
 * @return       the imported unit, or NULL when the prefix is not imported.
 */
static ModUnit *rwDeepQName(Loader *L, ModUnit *self, const char *qname) {
    /* Full path first: a short name can collide with another module's while a full path
     * cannot. The short-name form (`io::read`) is covered by `importedAs`. */
    ModUnit *u = importedAsPath(L, self, qname);
    if (!u) u = importedAs(L, self, qname);
    /* Debug switch `EXTC_DBG_QN=1` reports a deep qualified name that matched no
     * imported module. Like the other debug switches, it changes no output on the
     * normal path, and a name that resolved stays silent. */
    if (!u && dbgOn("EXTC_DBG_QN"))
        fprintf(stderr, "[qn] deep `%s` matched no imported module\n", qname);
    return u;
}

/**
 * @brief Resolve `a::b` in expression position, where the parser has built an EX_ASSOC node.
 *
 * The node is read in full before anything is written back: rewriting changes the kind and
 * therefore the union, so `args` and the flags are saved first.
 *
 * @param L     loader.
 * @param self  the unit the expression was written in.
 * @param e     the expression, rewritten in place, possibly into another kind.
 */
static void rwQualified(Loader *L, ModUnit *self, Expr *e) {
    const char *modPrefix = e->u.assoc.modPrefix;
    const char *symName   = e->u.assoc.name;
    const int   saveTargs = (int)e->u.assoc.targs.len;
    const bool  saveCall  = e->u.assoc.isCall;

    const char *mangledType = NULL;    /* set when the middle of the path is a type */

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
             * as not imported right after finding it. */
            e->u.assoc.typeName = modPrefix;
        } else if (rwQualifiedTypeName(L, self, modPrefix, &mangledType) && mangledType) {
            /* `fs::ofstream::new(...)`: the middle of the path is a **type**, not a
             * module, so the deep-path lookup above found nothing. The type name is what
             * the checker needs; the symbol after it (`new`) is resolved from there.
             * Without this the error is "`fs::ofstream` is not imported here -- add
             * `use fs::ofstream`", which is about a module that does not exist. */
            e->u.assoc.typeName = mangledType;
            return;
        } else {
            /* Report the full path with an instruction the user can follow, and name only
             * a module: `std` alone is just a path prefix, so "add `use std`" would point
             * at something that does not exist. `resolveModFile` is not called to tell
             * "does not exist" from "not imported": it prints its own message when it
             * fails, which would give two errors. Just say "not imported" and name the
             * `use` to add; a file that really is missing is reported once that `use`
             * exists, with every path that was tried. */
            const char *sug2 = suggestImportPath(L, modPrefix);
            const char *note = "Modules are imported explicitly (semantic import, not a textual include).";
            const char *msg2 = sug2
                ? arenaPrintf(L->a, "`%s` is not imported here -- add `use %s` and call it `%s::Name`",
                              modPrefix, sug2, strrchr(sug2, ':') ? strrchr(sug2, ':') + 1 : sug2)
                : arenaPrintf(L->a, "`%s` is not imported here -- add the `use` line that imports it"
                                    " (the name you call it by is the last segment of that path)",
                              modPrefix);
            ctxError(self->ctx, e->line, 1, note, "%s", msg2);
            L->errors++;
            return;
        }
    }

    /* `fs::ofstream("out.txt")`: `mod::name(args)` where `name` is a **type** of that
     * module is that type's constructor -- `T(...)` is `T::new(...)`, and from
     * outside the module the type has to be named in full, so the sugar has to be
     * recognized here as well.
     *
     * The order is function first, type second, and no ambiguity is possible: one module
     * cannot declare a function and a type under the same name (that is a duplicate name
     * in the module itself). */
    if (saveCall) {
        const char *tn0 = e->u.assoc.typeName;      /* read before anything is written */
        ModUnit *mowner = target;
        const char *msym = symName ? symName : e->u.assoc.name;
        /* A deep path (`lib::sub::Type(...)`) arrives with the module already in
         * `modPrefix` and the type as the symbol; a short one (`fs::Type(...)`) arrives as
         * `typeName` = module, `name` = type. Both end up asking the same question. */
        if (!mowner && tn0 && !strstr(tn0, "::")) mowner = importedAs(L, self, tn0);
        if (mowner && mowner != self && msym && unitStruct(mowner, msym) &&
            !unitStruct(mowner, msym)->isPrivate) {
            StructDef *csd = unitStruct(mowner, msym);
            /* Not a function of that name: a function would have been left for the
             * checker (and would shadow nothing, since the two cannot share a name). */
            bool isFn = false;
            for (size_t i = 0; i < mowner->mod.funcs.len && !isFn; i++)
                isFn = strcmp((*(FuncDef **)vecAt(&mowner->mod.funcs, i))->name, msym) == 0;
            if (!isFn) {
                e->u.assoc.typeName = csd->name;    /* the mangled type name */
                e->u.assoc.name     = "new";
                return;
            }
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
            anSetQualified(e, true);                   /* the source wrote a qualified name */
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
        /* The name is **finished**: it is the target module's declaration, and the
         * local-name rewrite must not touch it again. Without this flag the callee is
         * looked up in the *calling* module's rename table one pass later -- and when the
         * calling module declares a function of the same name (`std::proc::exit` wrapping
         * `std::sys::process::exit`), the call is rewritten to that local name, i.e. to
         * **itself**: unbounded recursion at run time, with no diagnostic at all. */
        anSetQualified(id, true);
        e->kind = EX_CALL;
        e->u.call.callee = id;
        e->u.call.args   = args;
        anSetQualified(e, true);               /* already qualified: the checker stops asking */
        return;
    }
    if (g) {
        e->kind = EX_IDENT;
        e->u.ident.name = g->name;
        anSetQualified(e, true);                /* the source wrote a qualified name */
        return;
    }
    /* The third thing a qualified name can be is a **type**, and `mod::Type` reaches this
     * function whenever the type is not called but *used*: `io::ioError.closed(fd)` names
     * the enum `ioError` of module `io` and leaves the variant to the checker, which sees a
     * receiver that is a plain identifier and resolves the variant from it (`EX_FIELD` and
     * `EX_METHOD` both have that path, written for the bare `status.ok`).
     *
     * The name is turned into exactly what `rwQualifiedTypeName` produces in type position -
     * the bare, renamed type name - because that is the shape the checker's variant path is
     * written against. Without this branch the value lookup above finds neither a function
     * nor a `let`/`var` and reports the type as a missing export: `module `io` has nothing
     * named `ioError``. */
    if (!f && !g) {
        TypeDef *td = unitType(target, nm);
        if (td) {
            if (td->isPrivate) {
                ctxError(self->ctx, e->line, 1,
                         "`@private` hides it from other modules. Drop the annotation if it is meant"
                         " to be used from here.",
                         "`%s::%s` is private to module `%s`", tn, nm, tn);
                L->errors++;
                return;
            }
            /* Read out before the union is written: `renOfTarget` does not touch `e`. */
            const char *ren = renOfTarget(target, nm);
            e->kind = EX_IDENT;
            e->u.ident.name = ren;
            anSetQualified(e, true);            /* the source wrote a qualified name */
            return;
        }
    }
    ctxError(self->ctx, e->line, 1,
             "A module exports its top-level `fn` / `struct` / `type` / `let`/`var`;"
             " `@private` ones are hidden.",
             "module `%s` has nothing named `%s`", tn, nm);
    L->errors++;
}

/**
 * @brief Rewrite the qualified names inside an expression.
 *
 * An expression carries names of its own, not only children, so every type name is
 * rewritten here, including the one in `EX_CONV` (`mod::T(x)`). Missing the literal's own
 * type name is the worst hole in this pass: with two modules declaring `pair`, the name
 * `liba::pair { ... }` reaches the checker unchanged, the checker looks a bare name up and
 * finds whichever struct was registered first, and the value is silently bound to the wrong
 * type - it compiles without a word when the field names line up. The error names a type
 * that is not in the source, `struct \`alpha$pair\` has no field \`s\``.
 *
 * @param L     loader.
 * @param self  the unit the expression was written in.
 * @param e     the expression, rewritten in place.
 */
static void rwExpr(Loader *L, ModUnit *self, Expr *e) {
    g_exN++;
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
            anSetQualified(e, true);     /* written qualified: stop asking for a qualifier */
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
    case EX_EXT: rwExpr(L, self, e->u.ext_.call); break;   /* `ext f(x)`: spawned call (cloned from EX_REF) */
    case EX_DEREF:  rwExpr(L, self, e->u.deref.operand); break;
    case EX_SIGN:   rwExpr(L, self, e->u.sign.operand); break;
    case EX_CONV:   rwExpr(L, self, e->u.conv.operand); break;
    case EX_TRY:    rwExpr(L, self, e->u.try_.operand); break;
    case EX_INDEX:  rwExpr(L, self, e->u.index.obj); rwExpr(L, self, e->u.index.index); break;
    case EX_SLICE:  rwExpr(L, self, e->u.slice.obj);
                    rwExpr(L, self, e->u.slice.lo);
                    rwExpr(L, self, e->u.slice.hi); break;
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
    case EX_DYN:
        /* A qualified name inside the payload has to be rewritten like any other. */
        rwExpr(L, self, e->u.dynv.payload);
        break;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.enumval.args, i));
        break;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            rwExpr(L, self, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value);
        break;
    /* A lambda is the one node where a **rewriting** pass must go further than an analysis pass:
     * its body is emitted as the generated `call` method's source, so every name and type in it has
     * to be rewritten by the module it was written in. Skipping the body leaves the unmangled name
     * of a module-local function and makes the `call` method an empty body that silently
     * returns 0 (a module file then reports `make=0` with no diagnostic). The field values are walked
     * as well -- they are the reads that happen where the lambda is written. */
    case EX_LAMBDA:
        for (size_t i = 0; i < e->u.lambda.params.len; i++)
            rwType(L, self, (*(Param **)vecAt(&e->u.lambda.params, i))->type);
        if (e->u.lambda.ret) rwType(L, self, e->u.lambda.ret);
        rwStmt(L, self, e->u.lambda.body);
        for (size_t i = 0; i < e->u.lambda.inits.len; i++)
            rwExpr(L, self, (*(FieldInit **)vecAt(&e->u.lambda.inits, i))->value);
        break;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.arraylit.elems, i));
        break;
    default: break;               /* literals, bindings, and null have no children */
    }
}

/**
 * @brief Rewrite the qualified names inside a statement and everything nested in it.
 *
 * @param L     loader.
 * @param self  the unit the statement was written in.
 * @param s     the statement, rewritten in place.
 */
static void rwStmt(Loader *L, ModUnit *self, Stmt *s) {
    g_stN++;
    if (!s) return;
    switch (s->kind) {
    case ST_YIELD:
        rwExpr(L, self, s->u.yield_.value);
        break;
    case ST_TRAP:     /* `trap(msg)`：消息也是一条表达式（可能带模块限定的名字）。 */
        rwExpr(L, self, s->u.trap_.msg);
        break;
    case ST_DOMAIN:   /* `d.run { … }`: 域块 —— 接收者与块体都要改写（和 ST_IF 同一形状）。 */
        rwExpr(L, self, s->u.domain_.callee);
        rwStmt(L, self, s->u.domain_.body);
        break;
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

/**
 * @brief Rename the declarations of a unit and record a return ticket for each of them.
 *
 * The root module is left completely alone. For every other unit, each declaration name is
 * prefixed with the module name and entered in `u->ren`, so that a reference written inside
 * the module can be mapped to the name that was actually emitted. `extern!` declarations
 * keep their names, because their names are linker symbols.
 *
 * Every name is computed and its rename recorded in the same pass. `mangleName` returns a
 * pointer into an arena buffer that a later allocation may reuse, so computing and storing
 * one name at a time would overwrite the previous one.
 *
 * `srcName` is filled here, while `d->name` is still the source name. Filling it in
 * `mergeUnit` would build `alpha::alpha$pair`, because by then the name is already renamed.
 *
 * @param L  loader whose arena owns the generated names.
 * @param u  the unit to rename in place.
 */
static void mangleUnitDecls(Loader *L, ModUnit *u) {
    if (!u->modName || !*u->modName) return;              /* the root module: never renamed */
    Module *src = &u->mod;
    /* One pass per list: compute a declaration's new name and record the rename **right away**.
     * `arenaPrintf` allocates fresh bytes per call and the arena never recycles, so every name
     * stays valid for the life of the process and no cap has to be kept in step with the number
     * of declarations a module renames. */
    for (size_t i = 0; i < src->structs.len; i++) {
        StructDef *d = *(StructDef **)vecAt(&src->structs, i);
        d->srcName = arenaPrintf(L->a, "%s::%s", u->modName, d->name);
        Ren *r = (Ren *)vecPush(&u->ren);
        r->from = d->name; r->to = mangleName(L, u, d->name);
    }
    for (size_t i = 0; i < src->types.len; i++) {
        TypeDef *d = *(TypeDef **)vecAt(&src->types, i);
        d->srcName = arenaPrintf(L->a, "%s::%s", u->modName, d->name);
        Ren *r = (Ren *)vecPush(&u->ren);
        r->from = d->name; r->to = mangleName(L, u, d->name);
    }
    for (size_t i = 0; i < src->globals.len; i++) {
        GlobalDef *d = *(GlobalDef **)vecAt(&src->globals, i);
        Ren *r = (Ren *)vecPush(&u->ren);
        r->from = d->name; r->to = mangleName(L, u, d->name);
    }
    for (size_t i = 0; i < src->funcs.len; i++) {
        FuncDef *d = *(FuncDef **)vecAt(&src->funcs, i);
        Ren *r = (Ren *)vecPush(&u->ren);
        r->from = d->name;
        /* An `extern!` keeps its name: see the note on `externKeepsName`. */
        r->to = externKeepsName(src, d->name) ? d->name : mangleName(L, u, d->name);
        /* When `to` equals `from` nothing is renamed, but the return ticket is still recorded - it
         * maps the name to itself. That keeps `renLookup` returning a value, so a bare reference
         * inside the module is not skipped. */
    }
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
    if (dbgOn("EXTC_DBG_M")) {
        fprintf(stderr, "[mangle] %s: %zu declarations renamed:", u->modName, u->ren.len);
        for (size_t i = 0; i < u->ren.len; i++) { Ren *r = (Ren *)vecAt(&u->ren, i); fprintf(stderr, " %s->%s", r->from, r->to); }
        fprintf(stderr, "\n");
    }
}

/**
 * @brief Report whether this unit exports this name, publicly.
 *
 * `@private` is checked here rather than left to the checker, because the rewrite happens
 * before any visibility rule runs: opening a module must not become a way to reach a
 * private name.
 *
 * @param u     the unit.
 * @param flat  the name as the declarations of the unit carry it.
 * @return      true when a top-level declaration of that name exists and is not `@private`.
 */
static bool unitExports(ModUnit *u, const char *flat) {
    if (!u || !flat) return false;
    Module *m = &u->mod;
    /* By the flat name: the declarations of a loaded unit have already been renamed
     * (`cout` is `io$cout` here), and the rename table is what maps the source name the
     * user wrote onto it. Searching for the source name would find nothing at all. */
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *d = *(StructDef **)vecAt(&m->structs, i);
        if (strcmp(d->name, flat) == 0) return !d->isPrivate;
    }
    for (size_t i = 0; i < m->types.len; i++) {
        TypeDef *d = *(TypeDef **)vecAt(&m->types, i);
        if (strcmp(d->name, flat) == 0) return !d->isPrivate;
    }
    for (size_t i = 0; i < m->globals.len; i++) {
        GlobalDef *d = *(GlobalDef **)vecAt(&m->globals, i);
        if (strcmp(d->name, flat) == 0) return !d->isPrivate;
    }
    for (size_t i = 0; i < m->funcs.len; i++) {
        FuncDef *d = *(FuncDef **)vecAt(&m->funcs, i);
        if (strcmp(d->name, flat) == 0) return !d->isPrivate;
    }
    return false;
}

/**
 * @brief Report whether this `use` lists this name (`use mod::{a, b}`).
 *
 * A wildcard `use mod::*` answers false here: it is handled by its own flag, so that the
 * two forms stay distinguishable.
 *
 * @param u     the use declaration.
 * @param name  the bare name being resolved.
 * @return      true when the name is one of the listed ones.
 */
static bool useListHas(UseDecl *u, const char *name) {
    for (size_t i = 0; i < u->names.len; i++)
        if (strcmp(*(const char **)vecAt(&u->names, i), name) == 0) return true;
    return false;
}

/* Memo for the ownership test below. `unitOwns` is asked 102,383,988 times at N=3200 (call-site
 * histogram of strcmp/strstr via LD_PRELOAD + addr2line) and each call scans all four declaration
 * lists of the unit -- for the root unit that is the whole program, so the rewrite walk is O(N^2)
 * (the load phase x5.8 per doubling, and the source of the 63% of instructions inside strcmp).
 *
 * The answer depends on the four list lengths: they grow while a unit is being merged, and an
 * unguarded memo changes the generated C. Every entry therefore records all four and is re-scanned
 * the moment one of them moves, and a miss -- empty slot, evicted entry, or a full probe window --
 * falls back to the original scan. Fixed size: nothing to allocate. */
typedef struct {
    const ModUnit *u; const char *name; size_t n0, n1, n2, n3; char val; int filled, done;
} OwnMemo;
static OwnMemo g_ownMemo[1 << 16];

/**
 * @brief Hash a unit/name pair into the probe window of the ownership memo.
 *
 * @param u     the unit.
 * @param name  the name to hash.
 * @return      a slot index in the fixed memo table.
 */
static size_t ownHash(const ModUnit *u, const char *name) {
    size_t h = (size_t)(uintptr_t)u * 1000003u + 1469598103934665603u;
    for (const char *q = name; *q; q++) { h ^= (unsigned char)*q; h *= 1099511628211u; }
    return h & 0xFFFFu;
}
/**
 * @brief Look up, or claim, the memo entry for a unit/name pair.
 *
 * @param u       the unit.
 * @param name    the name.
 * @param create  non-zero to claim an empty slot for this pair.
 * @return        the entry, or NULL when the pair is absent (no `create`) or the probe
 *                window is full.
 */
static OwnMemo *ownMemoFind(const ModUnit *u, const char *name, int create) {
    size_t h = ownHash(u, name);
    for (int step = 0; step < 8; step++, h = (h + 1) & 0xFFFFu) {
        OwnMemo *m = &g_ownMemo[h];
        if (m->filled) {
            if (m->u == u && strcmp(m->name, name) == 0) return m;
            continue;
        }
        if (!create) return NULL;
        m->u = u; m->name = name; m->filled = 1; m->done = 0; m->val = 0;
        return m;
    }
    return NULL;
}

/**
 * @brief Report whether this unit declares this name at its top level.
 *
 * For the **root file and the prelude** only, whose declarations are never renamed; a loaded
 * module is asked through its rename table instead. The answer is memoized, and a memo entry
 * is re-scanned the moment one of the four declaration-list lengths moves.
 *
 * @param u     the unit.
 * @param name  the name as written.
 * @return      true when a declaration of that name exists in the unit.
 */
static bool unitOwns(ModUnit *u, const char *name) {
    Module *m = &u->mod;
    OwnMemo *memo = ownMemoFind(u, name, 1);
    if (memo && memo->done && memo->n0 == m->structs.len && memo->n1 == m->types.len &&
        memo->n2 == m->globals.len && memo->n3 == m->funcs.len)
        return memo->val != 0;
    for (size_t i = 0; i < m->structs.len; i++)
        if (strcmp((*(StructDef **)vecAt(&m->structs, i))->name, name) == 0) return true;
    for (size_t i = 0; i < m->types.len; i++)
        if (strcmp((*(TypeDef **)vecAt(&m->types, i))->name, name) == 0) return true;
    for (size_t i = 0; i < m->globals.len; i++)
        if (strcmp((*(GlobalDef **)vecAt(&m->globals, i))->name, name) == 0) return true;
    for (size_t i = 0; i < m->funcs.len; i++)
        if (strcmp((*(FuncDef **)vecAt(&m->funcs, i))->name, name) == 0) {
            if (memo) { memo->n0 = m->structs.len; memo->n1 = m->types.len; memo->n2 = m->globals.len;
                        memo->n3 = m->funcs.len; memo->val = 1; memo->done = 1; }
            return true;
        }
    if (memo) { memo->n0 = m->structs.len; memo->n1 = m->types.len; memo->n2 = m->globals.len;
                memo->n3 = m->funcs.len; memo->val = 0; memo->done = 1; }
    return false;
}

/**
 * @brief Resolve a bare name through this unit's `use mod::*` imports.
 *
 * Only `use mod::*` is consulted: a plain `use mod` deliberately does not put anything in
 * scope, and that rule is not weakened here. The first match wins among the opened modules
 * only when the flat names are equal, which cannot happen for two different modules.
 *
 * @param self  the unit the name was written in.
 * @param name  the bare name.
 * @param line  source line, blamed by an error about `@private` or about two opened modules.
 * @return      the flat name of the exported declaration, or NULL when no opened module has
 *              it. An error is reported (and NULL returned) when two opened modules both
 *              have it: picking one silently would compile and mean the other.
 */
static const char *openLookup(ModUnit *self, const char *name, int line) {
    const char *found = NULL;
    const char *foundMod = NULL;
    for (size_t i = 0; i < self->mod.uses.len; i++) {
        UseDecl *u = *(UseDecl **)vecAt(&self->mod.uses, i);
        if (!u->unit) continue;
        /* `use mod::*` offers every public name; `use mod::{a, b}` offers exactly the
         * listed ones. Everything after this line -- privacy, ambiguity, the rewrite --
         * is shared, so the two forms cannot drift apart. */
        if (!u->wildcard && !useListHas(u, name)) continue;
        ModUnit *dep = (ModUnit *)u->unit;
        const char *flat = renLookup(dep, name);
        if (!flat) continue;
        if (!unitExports(dep, flat)) {
            /* The name is there, but `@private`. Saying "undefined" here would send the
             * reader looking for a typo: opening a module is not a way to reach a private
             * name, and the message has to say so -- the same words the qualified
             * reference gets. */
            ctxError(self->ctx, line, 1,
                     "`@private` means other modules must not name it. Drop the annotation if it is"
                     " meant to be used from here.",
                     "`%s::%s` is private to module `%s`", u->path, name, u->path);
            return NULL;
        }
        if (found && strcmp(found, flat) != 0) {
            ctxError(self->ctx, line, 1,
                     "Write one of them qualified to say which is meant.",
                     "`%s` is exported by both `%s` and `%s`, which are opened here",
                     name, foundMod, u->path);
            return NULL;
        }
        found = flat;
        foundMod = u->path;
    }
    return found;
}

/**
 * @brief Rewrite a bare name written inside a module to the name that module declares.
 *
 * A bare name is what a module writes for its own declarations: `color` in `color.green`,
 * `twice` in a call from `shout`. Once the declarations are renamed, a bare name points at
 * nothing: the error is `call to undefined function \`twice\`` while `greet$twice` is right
 * there in the table.
 *
 * Only names this unit declares itself are rewritten, never "any name some module exports".
 * With the wider rule, a bare name that another module also declares would be quietly
 * rewritten to that other declaration, so the program compiles and means something else -
 * far worse than a rejection.
 *
 * A bare name that this unit declares means this unit's declaration, including a `@private`
 * one: visibility is checked separately, at the point where a qualified reference is
 * required.
 *
 * @param self  the unit whose rename table is consulted.
 * @param e     an identifier expression, updated in place.
 */
static void rwExprName(ModUnit *self, Expr *e) {
    if (!e || e->kind != EX_IDENT) return;
    const char *nm = e->u.ident.name;
    /* In callee position the name may already have become `lib$open` (see
     * `rwQualified`), so this has to be able to fall back to the source name `open`.
     * Without the fallback, the diagnostic that asks for a qualified name turns into
     * "`lib$open` belongs to module lib - write `lib::lib$open`", which is nonsense. */
    if (!nm) nm = e->u.ident.srcName;
    /* A name that a qualified reference already resolved belongs to another module:
     * renaming it by **this** module's table would bind it to a local declaration of the
     * same spelling, which compiles and means something else. */
    if (anQualified(e)) return;
    /* Does this unit declare the name itself? A unit's own declaration wins over
     * anything an opened module offers, so this is asked first.
     *
     * The two cases cannot share one lookup: a loaded module's declarations are already
     * renamed, so its own names are found in the rename table (`cout` -> `io$cout`),
     * while the root file and the prelude are never renamed and their names are searched
     * as written. `renLookup` alone is wrong for the root: it answers the name itself when
     * the unit has no module name, which looks exactly like a match. */
    const char *own = NULL;
    if (self && self->modName && *self->modName) own = nm ? renLookup(self, nm) : NULL;
    else if (self && nm) own = unitOwns(self, nm) ? nm : NULL;
    if (own) { e->u.ident.srcName = nm; e->u.ident.name = own; return; }
    /* Nothing of this module's own: `use mod::*` may have put it in scope. The name is
     * rewritten to the flat one and marked qualified, which is exactly what writing
     * `mod::name` produces -- so the rest of the pipeline needs no second rule. */
    const char *open = (self && nm) ? openLookup(self, nm, e->line) : NULL;
    if (open) { e->u.ident.srcName = nm; e->u.ident.name = open; anSetQualified(e, true); }
}

/**
 * @brief Rewrite one method of a unit into the flat module's naming.
 *
 * Factored out because methods arrive two ways: inside a type's body, and in an `impl` block,
 * which is a declaration of its own until the checker attaches it (so it is not reachable
 * from any struct yet when a unit is merged). Both need the same three rewrites, and a
 * second copy would drift.
 *
 * @param L  loader.
 * @param u  the unit the method was written in.
 * @param m  the method, rewritten in place.
 */
static void rwUnitMethod(Loader *L, ModUnit *u, FuncDef *m) {
    m->modName = u->modName;
    m->ctx     = u->ctx;
    if (m->ret) rwType(L, u, m->ret);
    for (size_t k = 0; k < m->params.len; k++)
        rwType(L, u, (*(Param **)vecAt(&m->params, k))->type);
    rwStmt(L, u, m->body);
}

/**
 * @brief Merge one unit's declarations into the program module.
 *
 * Declarations are renamed first, then every type annotation, expression, and statement in
 * them has its qualified names resolved, and finally the declarations are appended to the
 * merged module. Units are merged in topological order, so a dependency is already present
 * when the declarations that use it arrive.
 *
 * The declarations are rewritten while they still live in `u->mod`, before they are
 * appended, so the rewrite passes see the unit's own name table. Merging mutates them in
 * place: they are the same objects afterwards.
 *
 * @param L  loader owning the merged module.
 * @param u  the unit to merge.
 */
static void mergeUnit(Loader *L, ModUnit *u) {
    Module *src = &u->mod;
    mangleUnitDecls(L, u);
    double t_pre_fn = ldNow();
    for (size_t i = 0; i < src->structs.len; i++) {
        StructDef *s = *(StructDef **)vecAt(&src->structs, i);
        s->modName = u->modName;
        s->ctx     = u->ctx;      /* which file it came from: a diagnostic needs it */
        for (size_t j = 0; j < s->fields.len; j++)
            rwType(L, u, (*(FieldDef **)vecAt(&s->fields, j))->type);
        for (size_t j = 0; j < s->methods.len; j++)
            rwUnitMethod(L, u, *(FuncDef **)vecAt(&s->methods, j));
        *(StructDef **)vecPush(&L->out->structs) = s;
    }
    /* `impl` blocks travel as their own declaration, because the type they extend may well be
     * declared in another module (or be a builtin). What has to happen here is only the naming:
     * the target is a name written in this unit, and this unit's declarations have just been
     * renamed (`pair` -> `liba$pair`), so it goes through the same rename table as every other
     * name. A builtin is never renamed -- `renLookup` only answers for names this unit declares
     * itself. The attachment itself happens in the checker, where the type table exists. */
    /* Traits travel like types: their method signatures are rewritten through this unit's
     * table, so a qualified type name inside a signature resolves exactly as it would in a
     * struct body. The **name** is not mangled: like every other top-level name, a trait is
     * program-global today, and two modules declaring `trait Codec` is a duplicate (the
     * checker reports it). */
    for (size_t i = 0; i < src->traits.len; i++) {
        TraitDef *tr = *(TraitDef **)vecAt(&src->traits, i);
        tr->modName = u->modName;
        for (size_t j = 0; j < tr->methods.len; j++)
            rwUnitMethod(L, u, *(FuncDef **)vecAt(&tr->methods, j));
        *(TraitDef **)vecPush(&L->out->traits) = tr;
    }
    for (size_t i = 0; i < src->impls.len; i++) {
        ImplDef *im = *(ImplDef **)vecAt(&src->impls, i);
        im->modName  = u->modName;
        im->typeName = rwTypeName(L, u, im->typeName);
        for (size_t j = 0; j < im->methods.len; j++)
            rwUnitMethod(L, u, *(FuncDef **)vecAt(&im->methods, j));
        *(ImplDef **)vecPush(&L->out->impls) = im;
    }
    for (size_t i = 0; i < src->types.len; i++) {
        TypeDef *t = *(TypeDef **)vecAt(&src->types, i);
        t->modName = u->modName;
        t->ctx     = u->ctx;
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
        g->ctx     = u->ctx;
        if (g->ann) rwType(L, u, g->ann);
        rwExpr(L, u, g->init);
        *(GlobalDef **)vecPush(&L->out->globals) = g;
    }
    double t_fn = ldNow();
    ldPhase("mg-rest", t_pre_fn);
    for (size_t i = 0; i < src->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&src->funcs, i);
        f->modName = u->modName;
        f->ctx     = u->ctx;
        if (f->ret) rwType(L, u, f->ret);
        for (size_t j = 0; j < f->params.len; j++)
            rwType(L, u, (*(Param **)vecAt(&f->params, j))->type);
        {double t_rs = ldNow(); rwStmt(L, u, f->body); ldAcc("mg-rwstmt", ldNow()-t_rs);}
        *(FuncDef **)vecPush(&L->out->funcs) = f;
    }
    ldPhase("mg-funcs", t_fn);
    ldReport();
}

/* --------------------------------------------------------- recursive loading */

/* Modules that may be imported **only from inside the standard library**.
 *
 * Every entry is a hole. `std::sys::heap` can turn any (pointer, length) into a view
 * (`extc_viewOf`), unmap any range (`munmap`) and map anywhere (`mmap`) -- the rest of the
 * language is built so that a program can neither forge a pointer nor grow a length, and one
 * import would step around all of it (a fabricated length of 1,000,000 over an 8-byte global,
 * indexed at 999,999, dies with SIGSEGV -- and reads silently wrong bytes when the address
 * happens to be mapped).
 *
 * The list is deliberately short, and every entry has a negative test. What a program
 * normally needs is `std::heap`: the plate's doors are **checked** (`open` / `alloc` /
 * `view(off, n)` / `viewAt(p, n)` / `holds(p)`). */
static const char *PRIVILEGED_MODULES[] = { "std::sys::heap", NULL };

/**
 * @brief Report whether a module path names one of the privileged standard-library modules.
 *
 * @param modPath  the module path as written.
 * @return         true when the path is listed in `PRIVILEGED_MODULES`.
 */
static bool isPrivilegedImport(const char *modPath) {
    for (int i = 0; PRIVILEGED_MODULES[i]; i++)
        if (strcmp(modPath, PRIVILEGED_MODULES[i]) == 0) return true;
    return false;
}

/**
 * @brief Collapse `.` / `..` / duplicate separators **textually** (no syscalls, no I/O).
 *
 * The same directory can be spelled two ways: `L.stdDir` is `<dir of extc>/../stdlib`, while
 * `-I /abs/path/stdlib` yields `/abs/path/stdlib/...` for the very same tree. The prefix test
 * in `isStdlibFile` therefore compares normalized paths; with one fixed spelling, passing the
 * standard library through `-I` makes `std/heap.extc` look like an ordinary program file, and
 * its own `use std::sys::heap` is rejected with "only the standard library may", which is a
 * lie about a file that *is* the standard library.
 *
 * @param a  arena that owns the result.
 * @param p  path to normalize.
 * @return   the normalized path, owned by the arena.
 */
static char *pathNorm(Arena *a, const char *p) {
    size_t n = strlen(p);
    char *out = (char *)arenaAlloc(a, n + 2);
    size_t o = 0, i = 0;
    if (n > 0 && p[0] == '/') { out[o++] = '/'; i = 1; }
    while (i < n) {
        size_t j = i;
        while (j < n && p[j] != '/') j++;
        size_t len = j - i;
        if (len == 1 && p[i] == '.') {
            /* `.`: nothing to add */
        } else if (len == 2 && p[i] == '.' && p[i + 1] == '.') {
            /* `..`: drop the previous segment (never past the root) */
            while (o > 1 && out[o - 1] != '/') o--;
            if (o > 1) o--;
        } else if (len > 0) {
            if (o > 0 && out[o - 1] != '/') out[o++] = '/';
            memcpy(out + o, p + i, len);
            o += len;
        }
        i = j + 1;
    }
    if (o == 0) out[o++] = '.';
    out[o] = '\0';
    return out;
}

/**
 * @brief Report whether `file` lives inside the standard library tree.
 *
 * Compares **normalized** paths and requires a directory boundary, so `/x/stdlib2` does not
 * count as inside `/x/stdlib`.
 *
 * @param L     loader, supplying the standard library directory.
 * @param file  path to test; NULL is never inside the tree.
 * @return      true when `file` sits under the standard library directory.
 */
static bool isStdlibFile(Loader *L, const char *file) {
    if (!L->stdDir || !file) return false;
    char *nFile = pathNorm(L->a, file);
    char *nStd = pathNorm(L->a, L->stdDir);
    size_t n = strlen(nStd);
    if (strncmp(nFile, nStd, n) != 0) return false;
    return nFile[n] == '\0' || nFile[n] == '/';
}

/**
 * @brief Load a module and everything it imports, or return the unit already loaded.
 *
 * The unit is registered before its source is parsed, and its state says "loading". A `use`
 * that reaches a module in that state is a cycle, which is refused because each module's
 * types are needed to check the other; the message suggests moving the shared part into a
 * third module. A module that is already loaded is returned as it is, so a diamond of imports
 * loads each file once and the unit list stays a duplicate check. Dependencies are loaded
 * after the module itself is parsed, and each is pushed onto `order` on the way out, which
 * makes `order` a post-order and therefore a topological order.
 *
 * @param L             loader.
 * @param modPath       the module path as written in the `use`.
 * @param importerFile  file containing that `use`, which resolves the path and is blamed.
 * @param line          line of the `use`, blamed by the cycle diagnostic.
 * @return              the unit for this module, or NULL when its file could not be resolved or
 *                      read; all diagnostics are already reported in that case.
 */
static ModUnit *loadUnit(Loader *L, const char *modPath, const char *importerFile, int line) {
    char *file = resolveModFile(L, modPath, importerFile);
    if (!file) return NULL;

    if (isPrivilegedImport(modPath) && !isStdlibFile(L, importerFile)) {
        fprintf(stderr,
                "%s:%d: error: `%s` is the standard library's **privileged layer**: it turns any"
                " (pointer, length) into a view and can map or unmap any range of the address"
                " space.\n"
                "note:  importing it from a program would step around the whole safety net"
                " (bounds checks, escape checks, the plate's range check), so only the standard"
                " library may.\n"
                "note:  what a program normally needs is `std::heap`: `plate::open` / `alloc` /"
                " `view(off, n)` / `viewAt(p, n)` / `holds(p)` are the checked doors.\n",
                importerFile, line, modPath);
        L->errors++;
        return NULL;
    }

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
    char *src = readWholeFile(L->a, file, &len);
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
    if (dbgOn("EXTC_DBG_MOD"))
        fprintf(stderr, "[mod] loaded %s: %zu tokens, %zu funcs, %zu globals\n",
                file, toks.len, u->mod.funcs.len, u->mod.globals.len);
    if (u->ctx->hasError) { L->errors++; u->state = 2; return u; }

    /* `@builtin` = "the runtime has a function with this signature" -- that is, **naming a
     * primitive**. Only the standard library may: one such declaration hands a program a
     * pointer or a length the rest of the language cannot forge (`@builtin fn extc_viewOf(p:
     * ref u8, n: i64) -> mut slice<u8>` plus a length of 1,000,000 over an 8-byte global
     * would otherwise compile and run). */
    for (size_t i = 0; i < u->mod.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&u->mod.funcs, i);
        if (f->isBuiltin && !isStdlibFile(L, u->file)) {
            ctxError(u->ctx, f->line, 1,
                     "Builtin primitives are how the standard library names what the runtime"
                     " implements; a program uses the library's checked doors instead"
                     " (`std::heap`'s `plate::view(off, n)` / `viewAt(p, n)`).",
                     "`@builtin` declarations are standard-library only");
            L->errors++;
            break;
        }
    }

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
        ud->unit = dep;
    }
    u->state = 2;
    *(ModUnit **)vecPush(&L->order) = u;           /* post-order: dependencies are in first */
    return u;
}

/* ------------------------------------------------------------------- entry */

/**
 * @brief Load every module the root file imports and merge them into `out`.
 *
 * `rootCtx` is stored by pointer, never copied: a copy would swallow the diagnostics
 * reported through it. `outCtxs` is used by pointer as well, because its length has to
 * reach the caller.
 *
 * @param a           arena that owns every loaded module.
 * @param out         the merged module; the prelude must already be in it.
 * @param rootm       the root file's own parsed module.
 * @param rootCtx     the root file's source context, which blames errors in that file.
 * @param rootPath    path of the root file; its directory becomes the project root.
 * @param searchDirs  directories from `-I`, or NULL.
 * @param outCtxs     receives the Ctx of every loaded module so the caller can render their
 *                    diagnostics afterwards, or NULL when the caller does not need them.
 * @return            false when any error was reported; the diagnostics are already printed
 *                    and compilation must stop.
 */
bool loadModules(Arena *a, Module *out, Module *rootm, Ctx *rootCtx,
                 const char *rootPath, Vec *searchDirs, Vec *outCtxs) {
    Loader L;
    memset(&L, 0, sizeof L);
    double t_pre = ldNow();
    L.a = a;
    L.out = out;
    L.rootDir = dirOf(a, rootPath);          /* project root */
    /* The standard library directory: `$EXTC_STD` wins, otherwise it is
     * `<directory of the extc binary>/../stdlib`. The prelude is embedded and is not
     * here; a real module such as `std::io` needs a real file. */
    {
        const char *env = getenv("EXTC_STD");
        if (env && *env) { L.stdDir = env; hintIndexSetStdDir(L.stdDir); }
        else {
            char buf[4096];
            ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
            if (n > 0) {
                buf[n] = '\0';
                char *slash = strrchr(buf, '/');
                if (slash) { *slash = '\0'; L.stdDir = arenaPrintf(a, "%s/../stdlib", buf); }
                    hintIndexSetStdDir(L.stdDir);   /* the checker's hint reads INDEX from here */
            }
        }
    }
    vecInit(&L.units, a, sizeof(void *));
    vecInit(&L.order, a, sizeof(void *));
    if (outCtxs) { L.ctxs = outCtxs; if (!outCtxs->arena) vecInit(outCtxs, a, sizeof(void *)); }
    else { L.ctxs = (Vec *)arenaAllocZero(a, sizeof(Vec)); vecInit(L.ctxs, a, sizeof(void *)); }
    if (searchDirs) L.searchDirs = *searchDirs;
    else vecInit(&L.searchDirs, a, sizeof(void *));

    /* The same `@builtin` rule for the **entry file**, which the driver parsed before this call and
     * which therefore never goes through `loadUnit`. */
    for (size_t i = 0; i < rootm->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&rootm->funcs, i);
        if (f->isBuiltin && !isStdlibFile(&L, rootPath)) {
            ctxError(rootCtx, f->line, 1,
                     "Builtin primitives are how the standard library names what the runtime"
                     " implements; a program uses the library's checked doors instead"
                     " (`std::heap`'s `plate::view(off, n)` / `viewAt(p, n)`).",
                     "`@builtin` declarations are standard-library only");
            L.errors++;
            break;
        }
    }

    /* The root file's own imports. */
    for (size_t i = 0; i < rootm->uses.len; i++) {
        UseDecl *u = *(UseDecl **)vecAt(&rootm->uses, i);
        ModUnit *dep = loadUnit(&L, u->path, rootPath, u->line);
        u->file = dep ? dep->file : NULL;
        u->unit = dep;
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
    if (dbgOn("EXTC_DBG_MOD")) fprintf(stderr, "[mod] load done: errors=%d units=%zu order=%zu\n",
                                         L.errors, L.units.len, L.order.len);
    if (L.errors) return false;

    /* Merge the modules in topological order, dependencies first. */
    ldPhase("ld-pre", t_pre);
    {
        double t_mu = ldNow();
        for (size_t i = 0; i < L.order.len; i++)
            mergeUnit(&L, *(ModUnit **)vecAt(&L.order, i));
        ldPhase("ld-merge", t_mu);
    }

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

    /* Collect every `use mod::*` as an (importer, opened) pair. It has to happen here,
     * where both the importing unit and the module it resolved to are known; the
     * checker only has the second one, in `FuncDef.modName`. */
    vecInit(&out->opens, a, sizeof(Open));
    for (size_t i = 0; i < L.order.len; i++) {
        ModUnit *dep = *(ModUnit **)vecAt(&L.order, i);
        for (size_t j = 0; j < dep->mod.uses.len; j++) {
            UseDecl *u = *(UseDecl **)vecAt(&dep->mod.uses, j);
            if (!u->file || (!u->wildcard && u->names.len == 0)) continue;
            ModUnit *opened = findUnit(&L, u->file);
            if (!opened) continue;
            if (u->wildcard) {
                Open *o = (Open *)vecPush(&out->opens);
                o->importer = dep->modName;
                o->opened   = opened->modName;
                o->name     = NULL;                /* every public name */
            } else {
                for (size_t k = 0; k < u->names.len; k++) {
                    Open *o = (Open *)vecPush(&out->opens);
                    o->importer = dep->modName;
                    o->opened   = opened->modName;
                    o->name     = *(const char **)vecAt(&u->names, k);
                }
            }
        }
    }
    for (size_t j = 0; j < rootm->uses.len; j++) {
        UseDecl *u = *(UseDecl **)vecAt(&rootm->uses, j);
        if (!u->file || (!u->wildcard && u->names.len == 0)) continue;
        ModUnit *opened = findUnit(&L, u->file);
        if (!opened) continue;
        if (u->wildcard) {
            Open *o = (Open *)vecPush(&out->opens);
            o->importer = NULL;                 /* the root file */
            o->opened   = opened->modName;
            o->name     = NULL;
        } else {
            for (size_t k = 0; k < u->names.len; k++) {
                Open *o = (Open *)vecPush(&out->opens);
                o->importer = NULL;
                o->opened   = opened->modName;
                o->name     = *(const char **)vecAt(&u->names, k);
            }
        }
    }

    /* The root file's declarations are merged last, and through the **same path** every other
     * unit takes. Its names are never renamed (`modName == NULL` makes `mangleUnitDecls` return
     * immediately and `renLookup` answer NULL). Routing the root through `mergeUnit` is what keeps
     * every declaration kind merged in one place: a kind handled only outside `mergeUnit` is
     * silently dropped for the root file, and a qualified `impl` target then misses `rwTypeName`.
     * `impl` blocks and their targets go through this walk like every other declaration. */
    double t_root = ldNow();
    {
        ModUnit root;
        memset(&root, 0, sizeof root);
        root.file    = rootPath;
        root.mod     = *rootm;
        root.ctx     = rootCtx;   /* the real context, never a copy */
        root.state   = 2;
        root.modName = NULL;      /* the root module: never renamed */
        mergeUnit(&L, &root);
    }
    ldPhase("ld-root", t_root);   /* the root unit's own merge: where the loader's time is */
    ldCount();
    if (dbgOn("EXTC_DBG_MOD")) fprintf(stderr, "[mod] merge done: errors=%d declarations funcs=%zu globals=%zu\n",
                                         L.errors, out->funcs.len, out->globals.len);
    if (L.errors) return false;

    return true;
}
