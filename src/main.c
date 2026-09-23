/* The extC compiler driver: command line, pipeline, and the `--run` shortcut.
 *
 *   extc --dump-tokens foo.extc        lex only
 *   extc foo.extc                      print the generated C to stdout
 *   extc -o foo.c foo.extc             write it to a file
 *   extc --run foo.extc                generate C, compile it, run it
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Each header is listed once, through the layer that actually needs it: `check.h`,
 * `codegen.h`, `modules.h` and `parser.h` all pull in the lower layers themselves. Asking
 * for a lower header again here costs nothing at runtime but does make the preprocessor
 * walk that file a second time, and gcc then reports its prototypes as redundant. */
#include "base.h"
#include "check.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "modules.h"
#include "prelude.h"

/* ------------------------------------------------------------- utilities */

/* Read a whole file into arena memory.
 *
 * Params:
 *   a      - arena that owns the buffer
 *   path   - file to read
 *   outLen - receives the number of bytes read, excluding the terminator
 *
 * Returns:
 *   A NUL-terminated buffer, or NULL when the file cannot be opened or measured, or
 *   is not seekable.
 *
 * Notes:
 *   - The file is not re-read: if it grows between the size measurement and the read
 *     of `n` bytes, the extra bytes are simply not part of the result.
 */
static char *readFile(Arena *a, const char *path, size_t *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);

    char *buf = (char *)arenaAlloc(a, (size_t)n + 1);
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);

    buf[rd] = '\0';
    *outLen = rd;
    return buf;
}

/* Write a byte range to a file, replacing whatever was there.
 *
 * Params:
 *   path - file to write
 *   data - bytes to write
 *   len  - number of bytes
 *
 * Returns:
 *   True when every byte was written; false when the file cannot be opened or a
 *   short write happened.
 */
static bool writeFile(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

/* The file name without its directory and extension: `examples/hello.extc` becomes
 * `hello`.
 *
 * Returns:
 *   An arena-owned copy; the input string is not modified.
 *
 * Notes:
 *   - Only `/` counts as a separator, and the extension is everything after the last
 *     dot of the remaining name, so a dot in a directory name is harmless.
 */
static const char *baseName(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    return arenaStrndup(a, base, n);
}

/* Run a program and wait for it to finish.
 *
 * Params:
 *   argv - NULL-terminated argument vector; argv[0] is the program name, looked up
 *          on PATH
 *
 * Returns:
 *   The exit status of the program, or -1 when it could not be started or was
 *   killed by a signal.
 *
 * Notes:
 *   - The child inherits the three standard streams, so its output appears in place
 *     and a compiled program can still read from stdin.
 *   - A failed exec is reported by the child itself and turned into exit status 127,
 *     which the parent sees as an ordinary exit.
 */
static int runCmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "extc: fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "extc: cannot exec `%s`: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* Print the command line summary to stderr.
 *
 * Params:
 *   argv0 - the name the compiler was invoked under; echoed as the program name
 *
 * Notes:
 *   - The text lists every accepted switch as well as the debug environment
 *     variables, because it is the only documentation the driver ships.
 *   - Writing to stderr keeps stdout free for the generated C, which is what
 *     `extc foo.extc` prints.
 */
static void usage(const char *argv0) {
    fprintf(stderr,
        "extC compiler\n"
        "\n"
        "usage: %s [options] <file.extc>\n"
        "\n"
        "options:\n"
        "  -o <file>        write the generated C to this file (default: stdout)\n"
        "  --run            write generated C to build/<name>.c, compile it, run it\n"
        "                   (uses $CC, default `cc`; creates ./build/ in the CWD)\n"
        "  --check-c        syntax-check the generated C with `$CC -fsyntax-only`\n"
        "  -w               suppress warnings\n"
        "  -I <dir>         add a module search directory (for `use a::b`)\n"
        "  -O0 .. -O3       optimisation level for the generated C (default: -O2)\n"
        "  -march=native    allow host-specific instructions (faster, less portable)\n"
        "  --dump-tokens    lex only; print the token table\n"
        "  --dump-effects   print each function's effect summary (Addr/Cont, arena rule)\n"
        "  --no-line-map    do not emit `#line` directives (default: emit them)\n"
        "\n"
        "debug switches (they never change the output):\n"
        "  EXTC_DBG_ARENA=1     check the arena level the checker computed vs codegen\n"
        "  EXTC_DBG_QN=1        trace how a qualified name (a::b::c) is parsed/resolved\n"
        "  EXTC_DUMP_EFFECTS=1  print each function's effect summary\n"
        "\n"
        "  -h, --help       show this help\n",
        argv0);
}

/* ---------------------------------------------------------------- prelude */

/* Does this module declare `slice` in the shape the compiler expects?
 *
 * A view is recognised by a protocol rather than by a type: the struct is named
 * `slice`, it has exactly one type parameter, and its first two fields are
 * `data: ref T` and `len`. The language knows the protocol and the library provides
 * the methods.
 *
 * Returns:
 *   True when a conforming `slice` exists, false when it is missing or malformed.
 *
 * Notes:
 *   - Checking the shape here turns an implicit contract into an explicit one.
 *     Without it, renaming a field in the prelude would surface much later, as
 *     generated C that does not compile.
 */
static bool viewContractOk(Module *m) {
    for (size_t i = 0; i < m->structs.len; i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        if (strcmp(sd->name, "slice") != 0) continue;
        if (sd->typeParams.len != 1 || sd->fields.len < 2) return false;
        FieldDef *f0 = *(FieldDef **)vecAt(&sd->fields, 0);
        FieldDef *f1 = *(FieldDef **)vecAt(&sd->fields, 1);
        if (strcmp(f0->name, "data") != 0 || f0->type->kind != TY_REF) return false;
        if (strcmp(f1->name, "len") != 0) return false;
        return true;
    }
    return false;
}

/* Parse and check the bundled prelude, then merge it into the main module.
 *
 * The prelude gets a Ctx of its own, whose path renders as <extc prelude>, so an
 * error inside it points at the right position. The prelude ships with the
 * compiler, so a failure here means the compiler is broken rather than the user's
 * program, and the driver reports an internal error.
 *
 * Params:
 *   arena - arena for the prelude's tokens, tree, and diagnostics
 *   tt    - the type table shared with the user file
 *   m     - module the prelude declarations are merged into, ahead of the user's
 *
 * Returns:
 *   False when the prelude failed to compile or does not satisfy the view contract,
 *   in which case the driver stops.
 *
 * Notes:
 *   - The prelude is checked twice, once here and once after being merged. It is
 *     small enough for the cost to be irrelevant, and keeping a separate Ctx is what
 *     makes its error positions always correct.
 */
static bool loadPrelude(Arena *arena, TypeTable *tt, Module *m) {
    size_t len = 0;
    const char *src = preludeSource(&len);

    Ctx ctx;
    ctxInit(&ctx, arena, PRELUDE_PATH, src, len);

    Module pm;
    memset(&pm, 0, sizeof pm);
    moduleInit(&pm, arena);

    Vec toks;
    vecInit(&toks, arena, sizeof(Token));
    lexAll(&ctx, &toks);
    if (!ctx.hasError) parseModule(&ctx, arena, &toks, &pm);
    if (!ctx.hasError) {
        ttRegister(tt, &pm);        /* the same table the user file will use */
        checkModule(&ctx, arena, tt, &pm);
    }

    if (ctx.hasError) {
        Buf diag;
        bufInit(&diag, arena);
        ctxRenderDiag(&ctx, &diag);
        fputs("extc: internal error -- the bundled prelude does not compile\n", stderr);
        fputs(bufCstr(&diag), stderr);
        return false;
    }

    /* Mark them as reserved, so the user cannot redefine them. A clash is an error
     * whose message explains why. */
    for (size_t i = 0; i < pm.structs.len; i++)
        (*(StructDef **)vecAt(&pm.structs, i))->reserved = true;
    for (size_t i = 0; i < pm.types.len; i++)
        (*(TypeDef **)vecAt(&pm.types, i))->reserved = true;
    for (size_t i = 0; i < pm.funcs.len; i++)
        (*(FuncDef **)vecAt(&pm.funcs, i))->reserved = true;

    /* The prelude has to keep the shape the compiler relies on, because `s[i]` is a
     * piece of syntax that works on it. A renamed field would otherwise show up as
     * generated C that does not compile, which says nothing about the real cause. */
    if (!viewContractOk(&pm)) {
        fputs("extc: internal error -- the prelude's `slice` does not have the shape "
              "the compiler expects (`data: ref T` then `len`)\n", stderr);
        return false;
    }

    /* Merge into the main module: the prelude declarations come before the user's. */
    for (size_t i = 0; i < pm.structs.len; i++)
        *(StructDef **)vecPush(&m->structs) = *(StructDef **)vecAt(&pm.structs, i);
    for (size_t i = 0; i < pm.types.len; i++)
        *(TypeDef **)vecPush(&m->types) = *(TypeDef **)vecAt(&pm.types, i);
    for (size_t i = 0; i < pm.funcs.len; i++)
        *(FuncDef **)vecPush(&m->funcs) = *(FuncDef **)vecAt(&pm.funcs, i);
    return true;
}

/* ---------------------------------------------------------------- main */

/* Run the compiler: parse the command line, load, check, generate, and on request
 * compile and run the result.
 *
 * Returns:
 *   0 on success, 1 when the program could not be compiled, 2 when the command line
 *   itself is wrong. Under `--run` the exit status is that of the compiled program.
 */
int main(int argc, char **argv) {
    const char *path = NULL;
    const char *outPath = NULL;
    const char *optLevel = NULL;     /* `-O0` .. `-O3`; the default is `-O2` */
    bool        noWarn = false;      /* `-w`: recorded here because the Ctx does not
                                      * exist yet */
    bool marchNative = false;        /* `-march=native`, off by default: it trades
                                      * portability for speed */
    bool dumpTokens = false;
    bool doRun = false;
    /* `--check-c`: syntax-check the generated C before handing it over. Generated C
     * that does not compile is the worst class of bug this compiler can have, because
     * its promise is that a program which type-checks will build. The check needs
     * neither a run nor the user reaching for a C compiler, and costs one `cc` call. */
    bool doCheckC = false;
    bool lineMap = true;
    /* `-I <dir>` module search directories. The command line is parsed before
     * `arenaInit` runs, so the names are collected in a fixed-size array here and
     * copied into a Vec afterwards; pushing into a Vec that has never been
     * initialised would dereference a garbage pointer. */
    const char *stdDirArgs[16];
    int         nStdDirArgs = 0;

    for (int i = 1; i < argc; i++) {
        /* Optimisation switches. The default stays `-O2`, but the 2x to 4x that
         * `-march=native` gives away for free has to stay reachable: on a matrix
         * multiply it doubles the speed again over `-O2` alone. The price is
         * portability, so it is off unless the user asks for it. */
        if (strncmp(argv[i], "-O", 2) == 0 && argv[i][2] >= '0' && argv[i][2] <= '3'
            && argv[i][3] == 0) {
            optLevel = argv[i];
            continue;          /* do not advance i here: the for loop already does,
                                * and a second increment swallows the next argument */
        }
        if (strcmp(argv[i], "-march=native") == 0) { marchNative = true; continue; }
        if (strcmp(argv[i], "-w") == 0) { noWarn = true; continue; }   /* suppress warnings */
        if (strcmp(argv[i], "--dump-tokens") == 0) {
            dumpTokens = true;
        } else if (strcmp(argv[i], "--dump-effects") == 0) {
            /* Debug switch: print every function's Addr/Cont effect summary. Setting
             * the environment variable here keeps the rest of the driver unaware of the
             * flag. */
            setenv("EXTC_DUMP_EFFECTS", "1", 1);
        } else if (strcmp(argv[i], "--check-c") == 0) {
            doCheckC = true;          /* syntax-check the generated C afterwards */
        } else if (strcmp(argv[i], "--run") == 0) {
            doRun = true;
        } else if (strcmp(argv[i], "--no-line-map") == 0) {
            lineMap = false;
        } else if (strcmp(argv[i], "-I") == 0) {
            if (++i >= argc) { fprintf(stderr, "extc: `-I` needs a directory\n"); return 2; }
            if (nStdDirArgs < 16) stdDirArgs[nStdDirArgs++] = argv[i];
            else { fprintf(stderr, "extc: too many `-I` directories (max 16)\n"); return 2; }
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) { fprintf(stderr, "extc: `-o` needs a file name\n"); return 2; }
            outPath = argv[i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "extc: unknown option `%s`\n", argv[i]);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "extc: only one input file is supported\n");
            return 2;
        }
    }

    if (!path) {
        usage(argv[0]);
        return 2;
    }

    Arena arena;
    arenaInit(&arena, 64 * 1024);

    size_t srcLen = 0;
    char *src = readFile(&arena, path, &srcLen);
    if (!src) {
        fprintf(stderr, "extc: cannot read `%s`\n", path);
        return 1;
    }

    Vec searchDirs;                                       /* the `-I` directories */
    vecInit(&searchDirs, &arena, sizeof(const char *));
    for (int k = 0; k < nStdDirArgs; k++)
        *(const char **)vecPush(&searchDirs) = stdDirArgs[k];

    Ctx ctx;
    ctxInit(&ctx, &arena, path, src, srcLen);
    ctx.noWarn = noWarn;

    Vec toks;
    vecInit(&toks, &arena, sizeof(Token));
    lexAll(&ctx, &toks);

    if (!ctx.hasError && dumpTokens) {
        Buf out;
        bufInit(&out, &arena);
        for (size_t i = 0; i < vecLen(&toks); i++) {
            tokenDescribe((Token *)vecAt(&toks, i), &out);
            bufPutc(&out, '\n');
        }
        fputs(bufCstr(&out), stdout);
        return 0;
    }

    /* One module, initialised once: the prelude and the user file both append to it. */
    Module m;
    memset(&m, 0, sizeof m);
    moduleInit(&m, &arena);

    /* One type table for the whole run. Types are interned and compared by pointer,
     * so a second table would turn the same `bool` into two different pointers. */
    TypeTable *tt = ttNew(&arena, NULL);

    /* 1. the prelude */
    if (!ctx.hasError && !loadPrelude(&arena, tt, &m)) return 1;

    /* 2. the root file is parsed into a module of its own first, because which
     *    modules to load is known only from its `use` declarations */
    Module rootm;
    memset(&rootm, 0, sizeof rootm);
    moduleInit(&rootm, &arena);
    if (!ctx.hasError) parseModule(&ctx, &arena, &toks, &rootm);

    /* 3. load the imported modules recursively, in topological order, merging them
     *    into the main module, so that the checker still sees one flat table: the
     *    loader has already resolved every qualified name */
    Vec moduleCtxs;
    vecInit(&moduleCtxs, &arena, sizeof(Ctx *));
    bool modsOk = true;
    if (!ctx.hasError)
        modsOk = loadModules(&arena, &m, &rootm, &ctx, path, &searchDirs, &moduleCtxs);

    /* Hand the bare-name mappings the loader collected to the type table. The loader
     * does not know TypeTable and ttResolve knows nothing else, so the two are joined
     * here. Dropping this loop leaves `aliases` empty, and a bare name such as `pair`
     * then fails to resolve -- with a message that only says "unknown type pair",
     * which hides the real cause. */
    for (size_t i = 0; i < m.aliases.len; i++)
        *(Alias *)vecPush(&tt->aliases) = *(Alias *)vecAt(&m.aliases, i);
    /* `EXTC_DBG_M=1` prints those bare-name mappings. After mangling, the generated C
     * only mentions `liba$pair`, so a wrong mapping shows up as "unknown type pair",
     * which does not say whether the mapping or the lookup is at fault. Seeing the
     * table settles it. */
    if (getenv("EXTC_DBG_M")) {
        fprintf(stderr, "[mangle] bare-name return tickets: %zu", tt->aliases.len);
        for (size_t i = 0; i < tt->aliases.len; i++) {
            Alias *al = (Alias *)vecAt(&tt->aliases, i);
            fprintf(stderr, " %s=>%s", al->from, al->to);
        }
        fprintf(stderr, "\n");
    }

    /* An error inside a module file was recorded in that file's own Ctx, so it has to be
     * rendered from there to name the right file and quote the right source line. */
    bool modDiag = false;
    for (size_t i = 0; i < moduleCtxs.len; i++) {
        Ctx *mc = *(Ctx **)vecAt(&moduleCtxs, i);
        if (mc && mc->hasError) {
            Buf d;
            bufInit(&d, &arena);
            ctxRenderDiag(mc, &d);
            fputs(bufCstr(&d), stderr);
            modDiag = true;
        }
    }
    if (modDiag || (!modsOk && !ctx.hasError)) {
        /* A failure at this stage ends the run: there is no point checking a
         * half-loaded program. An error in the root file is rendered by the common path
         * below, so it falls through instead of returning here. */
        if (!modDiag && ctx.hasError) { /* the root file's error is rendered below */ }
        else return 1;
    }

    /* Type checking is a pass of its own that writes its results back into the tree.
     * Code generation then performs no type inference at all. */
    if (!ctx.hasError) {
        ttRegister(tt, &m);
        checkModule(&ctx, &arena, tt, &m);
    }

    Buf c;
    bufInit(&c, &arena);
    if (!ctx.hasError) generateC(&ctx, &arena, tt, &m, lineMap, &c);

    if (ctx.hasError) {
        Buf diag;
        bufInit(&diag, &arena);
        ctxRenderDiag(&ctx, &diag);
        fputs(bufCstr(&diag), stderr);
        return 1;
    }

    /* `--check-c`: syntax-check the generated C. This has to run before the
     * `if (!doRun)` block below, which prints the C and returns, or the check would
     * only ever happen under `--run`. */
    if (doCheckC && !doRun) {
        const char *ccx = getenv("CC") ? getenv("CC") : "cc";
        const char *tmp = "/tmp/extc-syntaxcheck.c";
        if (!writeFile(tmp, bufCstr(&c), c.len)) {
            fprintf(stderr, "extc: cannot write the syntax-check file\n");
            return 1;
        }
        char *argv2[] = { (char *)ccx, "-std=c11", "-fsyntax-only", "-w", (char *)tmp, NULL };
        if (runCmd(argv2) != 0) {
            fprintf(stderr, "extc: **the generated C does not compile** -- this is an extc"
                            " bug, not a mistake in your program\n");
            return 1;
        }
    }

    if (!doRun) {
        if (outPath) {
            char *text = bufCstr(&c);
            if (!writeFile(outPath, text, c.len)) {
                fprintf(stderr, "extc: cannot write `%s`\n", outPath);
                return 1;
            }
        } else if (!doCheckC) {      /* under `--check-c` with no `-o`, dumping the C to
                                      * stdout again would only add noise */
            fputs(bufCstr(&c), stdout);
        }
        return 0;
    }

    /* `--run`: write build/<base>.c, compile it, then run the result */
    const char *base = baseName(&arena, path);
    if (mkdir("build", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "extc: cannot create `build/`: %s\n", strerror(errno));
        return 1;
    }

    const char *cPath = arenaPrintf(&arena, "build/%s.c", base);
    const char *binPath = arenaPrintf(&arena, "build/%s", base);
    char *text = bufCstr(&c);
    if (!writeFile(cPath, text, c.len)) {
        fprintf(stderr, "extc: cannot write `%s`\n", cPath);
        return 1;
    }

    const char *cc = getenv("CC") ? getenv("CC") : "cc";


    /* `-fwrapv` makes signed overflow wrap around, and makes that wrap defined.
     *
     * Without it, `x + 1` where x is the largest i32 is signed overflow, which is
     * undefined behaviour: at `-O1` it happens to wrap, but the C standard does not
     * promise that, and the strict-overflow assumptions of `-O2` can delete a test such
     * as `if (x + 1 > x)` altogether.
     *
     * extC promises not to hand the programmer undefined behaviour, so the generated
     * code must not contain any. This flag turns that one case into a defined wrap.
     *
     * What wrap-around means as a language rule is still open -- trapping would be just
     * as defensible -- so the driver takes the smallest step available and agrees with
     * what gcc does in practice, turning "happens to" into "is guaranteed to".
     *
     * The default was raised from `-O1` to `-O2` after measuring it, which costs nothing
     * in semantics: a matrix multiply went from 106 ms to 34 ms (3.1x), a modulo loop
     * from 273 ms to 253 ms, and a prime sieve was unchanged at 29 ms. The numbers are
     * in `bench/`: extC matches C at every optimisation level, `-O3` adds almost
     * nothing over `-O2`, and `-march=native` doubles the matrix multiply again to
     * 15 ms -- at the cost of portability, so it stays off by default.
     *
     * The last mile to C-level performance is therefore flags, not language design. */
    char *ccArgv[16];
    int n = 0;
    ccArgv[n++] = (char *)cc;
    ccArgv[n++] = "-std=c11";
    ccArgv[n++] = (char *)(optLevel ? optLevel : "-O2");
    ccArgv[n++] = "-fwrapv";
    if (marchNative) ccArgv[n++] = "-march=native";
                       /* The compiler derives `_debug`, `_eq`, `_find` and further
                        * helpers for every type the program uses, and the ones nothing
                        * calls would otherwise stay in the binary: the text segment of
                        * hello fell from 3215 to 1446 bytes and that of euler-sieve from
                        * 6852 to 3475. Letting the linker drop the sections nobody
                        * references changes no semantics. */
    ccArgv[n++] = "-ffunction-sections";
    ccArgv[n++] = "-fdata-sections";
    ccArgv[n++] = "-Wl,--gc-sections";
    ccArgv[n++] = "-o";
    ccArgv[n++] = (char *)binPath;
    ccArgv[n++] = (char *)cPath;
    ccArgv[n] = NULL;
    int rc = runCmd(ccArgv);
    if (rc != 0) {
        fprintf(stderr, "extc: C compiler failed (exit %d) on `%s`\n", rc, cPath);
        return 1;
    }

    char *runArgv[] = { (char *)binPath, NULL };
    return runCmd(runArgv);
}
