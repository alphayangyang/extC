/* The extC compiler driver: command line, pipeline, and the `--run` shortcut.
 *
 *   extc --dump-tokens foo.extc        lex only
 *   extc foo.extc                      print the generated C to stdout
 *   extc -o foo.c foo.extc             write it to a file
 *   extc --run foo.extc                generate C, compile it, run it
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
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
#include "plan.h"      /* planBuiltinHolder: a builtin-name struct needs no C definition */
#include "lexer.h"
#include "parser.h"
#include "modules.h"
#include "prelude.h"
#include <time.h>

/* Phase timing, off unless EXTC_DBG_TIME=1: "which stage got slow" is the question that
 * matters when a big input takes seconds. Use PHASE(name, call) so the timing variable
 * lives in its own `do { } while (0)` scope and cannot collide with its neighbours. */
static int timeOn(void) { static int v = -1; if (v < 0) v = getenv("EXTC_DBG_TIME") != NULL; return v; }
static double nowSec(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }
static void phase(const char *name, double t0) { if (timeOn()) fprintf(stderr, "[time] %-9s %.3f s\n", name, nowSec() - t0); }
#define PHASE(name, call) do { double t_ = nowSec(); call; phase((name), t_); } while (0)

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
/* Whether the memory report should be printed this time.
 *
 * `checkModule` runs once for the prelude and once for the file the user named, and only
 * the second is about the user's program: a report about the prelude would be noise, and
 * printing it twice would look like the program had been analysed twice. */
static bool g_explainRoot = false;

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

/* Run a program and capture its standard output.
 *
 * Params:
 *   argv - NULL-terminated argument vector, exactly as `runCmd` takes it
 *   out  - buffer the child's standard output is appended to
 *
 * Returns:
 *   The exit status of the program (0 on success), or -1 when it could not be
 *   started, was killed by a signal, or the pipe could not be read.
 *
 * Notes:
 *   - `--pkg-config <name>` uses this to ask pkg-config for a library's cflags and
 *     libs. The child's standard error is left alone, so pkg-config's own
 *     complaints reach the user instead of being swallowed.
 *   - The pipe is read to EOF **before** the child is reaped: waiting first would
 *     deadlock as soon as the child writes more than one pipe buffer.
 */
static int runCapture(char *const argv[], Buf *out) {
    int fds[2];
    if (pipe(fds) != 0) {
        fprintf(stderr, "extc: pipe failed: %s\n", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "extc: fork failed: %s\n", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "extc: cannot exec `%s`: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    char chunk[4096];
    for (;;) {
        ssize_t got = read(fds[0], chunk, sizeof chunk);
        if (got > 0) { bufPutn(out, chunk, (size_t)got); continue; }
        if (got == 0) break;
        if (errno == EINTR) continue;
        close(fds[0]);
        waitpid(pid, NULL, 0);
        return -1;
    }
    close(fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* Ask pkg-config for one package's flags and append the tokens to a cc argument list.
 *
 * Params:
 *   a    - arena for the split tokens
 *   args - the argument list the flags are appended to
 *   name - the pkg-config package name
 *
 * Returns:
 *   False after reporting the failure; the caller decides whether that is fatal. Both
 *   callers treat it as fatal: linking without the flags surfaces much later as a wall
 *   of undefined references, which points at the wrong thing.
 */
static bool appendPkgConfig(Arena *a, Vec *args, const char *name) {
    char *argv[] = { (char *)"pkg-config", (char *)"--cflags", (char *)"--libs",
                     (char *)name, NULL };
    Buf out;
    bufInit(&out, a);
    int rc = runCapture(argv, &out);
    if (rc != 0) {
        fprintf(stderr, "extc: `pkg-config --cflags --libs %s` failed (exit %d)\n", name, rc);
        return false;
    }
    char *text = bufCstr(&out);
    for (char *tok = strtok(text, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n"))
        *(char **)vecPush(args) = tok;
    return true;
}

/* Read the link requirements of every module the program actually loaded.
 *
 * A module that wraps a C library knows which library it needs; every program that
 * uses it should not have to repeat that on the command line. The requirement lives in
 * `<module>.link`, next to `<module>.extc`, and is read **only** for modules the
 * program loaded (`use`d, directly or transitively) -- so nothing is linked by
 * accident, and nothing is downloaded: a library that is not installed fails at the
 * link step with the C compiler's own message.
 *
 * Grammar (one directive per line; blank lines and `#` comments are ignored):
 *   lib <name>        append `-l<name>`; `lib :libsqlite3.so.0` spells a soname
 *   pkgconfig <name>  append the tokens of `pkg-config --cflags --libs <name>`
 *   ccflag <flag>     append one raw flag (escape hatch, the package author's call)
 * An unknown directive is an error: a requirement that is silently ignored is the
 * exact failure this file exists to prevent.
 *
 * Params:
 *   a        - arena for the paths, tokens and file contents
 *   ctxs     - the Ctx of every loaded module, as `loadModules` filled it
 *   args     - the cc argument list the flags are appended to
 *   libNames - receives the `lib` names, for the artifact's `extc-libs:` line
 *   pkgNames - receives the `pkgconfig` names, for the artifact's `extc-pkg-config:` line
 *
 * Returns:
 *   False after reporting an error (bad directive, empty argument, failed pkg-config).
 */
static bool loadModuleLinks(Arena *a, Vec *ctxs, Vec *args, Vec *libNames, Vec *pkgNames) {
    for (size_t i = 0; i < vecLen(ctxs); i++) {
        Ctx *mc = *(Ctx **)vecAt(ctxs, i);
        if (!mc || !mc->path) continue;
        size_t len = strlen(mc->path);
        if (len > 5 && strcmp(mc->path + len - 5, ".extc") == 0) len -= 5;
        const char *linkPath = arenaPrintf(a, "%.*s.link", (int)len, mc->path);
        if (access(linkPath, R_OK) != 0) continue;      /* no requirements: the common case */

        size_t n = 0;
        char *text = readWholeFile(a, linkPath, &n);
        if (!text) {
            fprintf(stderr, "extc: cannot read `%s`\n", linkPath);
            return false;
        }
        int lineNo = 0;
        char *p = text;
        while (*p) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p == '\n') *p++ = 0;
            lineNo++;
            while (*line == ' ' || *line == '\t') line++;
            char *end = line + strlen(line);
            while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
            if (*line == 0 || *line == '#') continue;
            /* Split the directive from its argument; the argument keeps its inner spaces
             * (a `ccflag` may need them), so only the first run of blanks is cut. */
            char *arg = line;
            while (*arg && *arg != ' ' && *arg != '\t') arg++;
            if (*arg) *arg++ = 0;
            while (*arg == ' ' || *arg == '\t') arg++;
            if (strcmp(line, "lib") == 0) {
                if (!*arg) {
                    fprintf(stderr, "extc: %s:%d: `lib` needs a name\n", linkPath, lineNo);
                    return false;
                }
                for (const char *c = arg; *c; c++) {
                    if (isalnum((unsigned char)*c) || *c == '_' || *c == '.' || *c == '+'
                        || *c == '-' || *c == ':') continue;
                    fprintf(stderr, "extc: %s:%d: bad library name `%s`"
                                    " (letters, digits, `_ . + - :` only)\n",
                            linkPath, lineNo, arg);
                    return false;
                }
                *(char **)vecPush(args) = arenaPrintf(a, "-l%s", arg);
                *(const char **)vecPush(libNames) = arg;
            } else if (strcmp(line, "pkgconfig") == 0) {
                if (!*arg) {
                    fprintf(stderr, "extc: %s:%d: `pkgconfig` needs a package name\n",
                            linkPath, lineNo);
                    return false;
                }
                if (!appendPkgConfig(a, args, arg)) return false;
                *(const char **)vecPush(pkgNames) = arg;
            } else if (strcmp(line, "ccflag") == 0) {
                if (!*arg) {
                    fprintf(stderr, "extc: %s:%d: `ccflag` needs a flag\n", linkPath, lineNo);
                    return false;
                }
                *(char **)vecPush(args) = arg;
            } else {
                fprintf(stderr, "extc: %s:%d: unknown directive `%s`"
                                " (expected `lib`, `pkgconfig` or `ccflag`)\n",
                        linkPath, lineNo, line);
                return false;
            }
        }
    }
    return true;
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
        "  --build          same, but stop before running: the binary is `-o <file>` if\n"
        "                   given, otherwise build/<name> (this is what a package's build\n"
        "                   step wants: link flags from <module>.link, no execution)\n"
        "  --check-c        syntax-check the generated C with `$CC -fsyntax-only`\n"
        "  -w               suppress warnings\n"
        "  -I <dir>         add a module search directory (for `use a::b`)\n"
        "  -l <name>        link with `-l<name>` (repeatable; e.g. `-l z`)\n"
        "  -L <dir>         add a library search directory (repeatable)\n"
        "  --ccflag <flag>  append one raw flag to the C compiler command (repeatable)\n"
        "                   escape hatch, e.g. `--ccflag -l:libsqlite3.so.0` (a soname,\n"
        "                   which `-l` cannot spell) or `--ccflag -I/usr/include/cairo`\n"
        "  --pkg-config <name>  add `pkg-config --cflags --libs <name>` (repeatable)\n"
        "  -O0 .. -O3       optimisation level for the generated C (default: -O2)\n"
        "  -march=native    allow host-specific instructions (faster, less portable)\n"
        "  --dump-tokens    lex only; print the token table\n"
        "  --dump-effects   print each function's effect summary (Addr/Cont, arena rule)\n"
        "  --no-line-map    do not emit `#line` directives (default: emit them)\n"
        "\n"
        /* This text used to say "debug switches (they never change the output)". Measured, that
         * was false: `EXTC_NO_LEVELPASS` moves arena placement in 2 of the 414 golden programs, and
         * `EXTC_SELFCHECK` turns a good build red. A documentation claim the code contradicts is
         * worse than no claim -- so the two exceptions are named, and `tools/check_switches.py`
         * keeps "every switch is either documented here or listed there with a reason" true. */
        "debug switches -- diagnostics only, **except the two marked below**:\n"
        "  EXTC_DBG_ARENA=1     check the arena level the checker computed vs codegen\n"
        "  EXTC_DBG_QN=1        trace how a qualified name (a::b::c) is parsed/resolved\n"
        "  EXTC_DBG_M=1         print the per-module renamed-declaration counts\n"
        "  EXTC_DBG_IMPL=1      print each `impl` block and the type it attaches to\n"
        "  EXTC_DBG_HOME=1      print each function's home/zone flags and its arena sites\n"
        "  EXTC_DBG_OWNER=1     report each result slot first reached from another body\n"
        "  EXTC_DUMP_EFFECTS=1  print each function's effect summary\n"
        "  EXTC_NO_LEVELPASS=1  skip the arena level pass -- **CHANGES THE OUTPUT**: it moves arena\n"
        "                       placement in 2 of the 414 golden programs\n"
        "  EXTC_SELFCHECK=1     run the checker's self-check -- **CAN FAIL THE BUILD** (exit 1)\n"
        "  EXTC_DBG_PRIMSCAN=1  decide the runtime primitive block by the old line-shape scan\n"
        "                       instead of the emission-time registry (rollback switch for the\n"
        "                       codegen worklist refactor; the output is meant to be identical)\n"
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
        g_explainRoot = false;      /* a report about the prelude is noise */
        setenv("EXTC_EXPLAIN_ROOT", "0", 1);
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
    for (size_t i = 0; i < pm.structs.len; i++) {
        StructDef *psd = *(StructDef **)vecAt(&pm.structs, i);
        /* A builtin method holder (`impl i64 { ... }` in the prelude) is not a prelude type:
         * it has no fields and no name of its own in the type table, and the program is allowed
         * to add more methods to the same builtin. `reserved` means "the user may not redefine
         * this nor add methods to it", which would be the wrong thing to say here. */
        if (planBuiltinHolder(psd)) continue;
        psd->reserved = true;
    }
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
    /* `--build`: compile and link, but do not execute. A package manager needs exactly
     * this -- the same flag handling as `--run` (including each module's `<module>.link`)
     * without running the program. */
    bool doBuild = false;
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
    /* Link and include flags for the C compiler, collected with the same fixed-size
     * trick as `-I` and for the same reason (the arena does not exist yet). They are
     * turned into `cc` arguments after `arenaInit`, because `--pkg-config` has to run
     * a program and keep its output somewhere. */
    const char *linkArgs[16];        /* `-l <name>` */
    int         nLinkArgs = 0;
    const char *libDirArgs[16];      /* `-L <dir>` */
    int         nLibDirArgs = 0;
    const char *ccFlagArgs[32];      /* `--ccflag <flag>`: passed through verbatim */
    int         nCcFlagArgs = 0;
    const char *pkgConfigArgs[8];    /* `--pkg-config <name>` */
    int         nPkgConfigArgs = 0;

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
        } else if (strcmp(argv[i], "--explain-memory") == 0) {
            /* One line per allocation site: where it lands and whether it is released each
             * round of the loop it sits in. Printed after checking, when every level is
             * final. See `reportMemory`. */
            setenv("EXTC_EXPLAIN_MEMORY", "1", 1);
        } else if (strcmp(argv[i], "--check-c") == 0) {
            doCheckC = true;          /* syntax-check the generated C afterwards */
        } else if (strcmp(argv[i], "--run") == 0) {
            doRun = true;
        } else if (strcmp(argv[i], "--build") == 0) {
            doBuild = true;
        } else if (strcmp(argv[i], "--no-line-map") == 0) {
            lineMap = false;
        } else if (strcmp(argv[i], "-I") == 0) {
            if (++i >= argc) { fprintf(stderr, "extc: `-I` needs a directory\n"); return 2; }
            if (nStdDirArgs < 16) stdDirArgs[nStdDirArgs++] = argv[i];
            else { fprintf(stderr, "extc: too many `-I` directories (max 16)\n"); return 2; }
        } else if (strcmp(argv[i], "-l") == 0) {
            /* A library to link, named the way `cc` names it: `-l z` becomes `-lz`.
             * The name is also recorded in the generated C, in an `extc-libs:` line, so
             * a build system can read the link requirements back out of the artifact
             * instead of keeping a second copy of them. */
            if (++i >= argc) { fprintf(stderr, "extc: `-l` needs a library name\n"); return 2; }
            if (nLinkArgs < 16) linkArgs[nLinkArgs++] = argv[i];
            else { fprintf(stderr, "extc: too many `-l` libraries (max 16)\n"); return 2; }
        } else if (strcmp(argv[i], "-L") == 0) {
            if (++i >= argc) { fprintf(stderr, "extc: `-L` needs a directory\n"); return 2; }
            if (nLibDirArgs < 16) libDirArgs[nLibDirArgs++] = argv[i];
            else { fprintf(stderr, "extc: too many `-L` directories (max 16)\n"); return 2; }
        } else if (strcmp(argv[i], "--ccflag") == 0) {
            /* The escape hatch: anything `cc` understands, including the shapes the two
             * switches above cannot spell (`-l:libsqlite3.so.0`, `-I/usr/include/cairo`,
             * `-Wl,-rpath,...`). Passed through verbatim, one argv entry per flag. */
            if (++i >= argc) { fprintf(stderr, "extc: `--ccflag` needs a flag\n"); return 2; }
            if (nCcFlagArgs < 32) ccFlagArgs[nCcFlagArgs++] = argv[i];
            else { fprintf(stderr, "extc: too many `--ccflag` flags (max 32)\n"); return 2; }
        } else if (strcmp(argv[i], "--pkg-config") == 0) {
            /* `--pkg-config openssl` asks pkg-config for that library's cflags and libs.
             * Resolved after `arenaInit` (it runs a program); see the collection below. */
            if (++i >= argc) { fprintf(stderr, "extc: `--pkg-config` needs a package name\n"); return 2; }
            if (nPkgConfigArgs < 8) pkgConfigArgs[nPkgConfigArgs++] = argv[i];
            else { fprintf(stderr, "extc: too many `--pkg-config` packages (max 8)\n"); return 2; }
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

    /* Turn the link switches into `cc` arguments. The order is fixed: pkg-config
     * output first (its cflags are `-I`/`-D` and its libs are `-l`), then the explicit
     * `-l`, `-L` and raw flags in the order the user wrote them -- the compiler driver
     * is not entitled to reorder a link line, because order decides which symbol a
     * static library resolves. A failing pkg-config is a hard error rather than a
     * warning: silently linking without the flags would surface later as dozens of
     * undefined references, pointing at the wrong thing. */
    Vec ccExtra;
    vecInit(&ccExtra, &arena, sizeof(char *));
    for (int k = 0; k < nPkgConfigArgs; k++)
        if (!appendPkgConfig(&arena, &ccExtra, pkgConfigArgs[k])) return 2;
    for (int k = 0; k < nLinkArgs; k++)
        *(char **)vecPush(&ccExtra) = arenaPrintf(&arena, "-l%s", linkArgs[k]);
    for (int k = 0; k < nLibDirArgs; k++)
        *(char **)vecPush(&ccExtra) = arenaPrintf(&arena, "-L%s", libDirArgs[k]);
    for (int k = 0; k < nCcFlagArgs; k++)
        *(char **)vecPush(&ccExtra) = (char *)ccFlagArgs[k];

    size_t srcLen = 0;
    char *src = readWholeFile(&arena, path, &srcLen);
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
    PHASE("lex", lexAll(&ctx, &toks));

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
    if (!ctx.hasError) PHASE("parse", parseModule(&ctx, &arena, &toks, &rootm));

    /* 3. load the imported modules recursively, in topological order, merging them
     *    into the main module, so that the checker still sees one flat table: the
     *    loader has already resolved every qualified name */
    Vec moduleCtxs;
    vecInit(&moduleCtxs, &arena, sizeof(Ctx *));
    bool modsOk = true;
    if (!ctx.hasError) {
        /* The loader (and the AST rewrite it drives) sits between parse and check and was the one
         * big untimed stretch -- the timers accounted for about 1.2 s of a 3.13 s build before. */
        double t_ld = nowSec();
        modsOk = loadModules(&arena, &m, &rootm, &ctx, path, &searchDirs, &moduleCtxs);
        phase("load", t_ld);
    }

    /* Auto-link: every loaded module's own requirements (`<module>.link`). They are kept
     * apart from the command line's flags so the link order stays predictable -- the
     * program's dependencies in load order first, then whatever the user asked for. */
    Vec modExtra;
    vecInit(&modExtra, &arena, sizeof(char *));
    Vec modLibs;
    vecInit(&modLibs, &arena, sizeof(const char *));
    Vec modPkgs;
    vecInit(&modPkgs, &arena, sizeof(const char *));
    if (modsOk && !loadModuleLinks(&arena, &moduleCtxs, &modExtra, &modLibs, &modPkgs))
        return 2;

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
    if (dbgOn("EXTC_DBG_M")) {
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
        g_explainRoot = true;       /* this is the file the user named */
        setenv("EXTC_EXPLAIN_ROOT", "1", 1);
        PHASE("check", checkModule(&ctx, &arena, tt, &m));
    }

    /* A body that lives in a module is checked under **that file's** context, so a mistake
     * found there is recorded in that file's Ctx, not the entry file's. Rendering only
     * `ctx` would drop it: the compiler would carry on and generate C for a program it
     * just rejected -- a wrong position is bad, silence is worse. Render every context
     * that failed, and do not reach code generation when any of them did. */
    bool bodyDiag = false;
    for (size_t i = 0; i < moduleCtxs.len; i++) {
        Ctx *mc = *(Ctx **)vecAt(&moduleCtxs, i);
        if (!mc || !mc->hasError) continue;
        Buf d;
        bufInit(&d, &arena);
        ctxRenderDiag(mc, &d);
        fputs(bufCstr(&d), stderr);
        bodyDiag = true;
    }

    Buf c;
    bufInit(&c, &arena);
    double t_cg0 = nowSec();
    if (!ctx.hasError && !bodyDiag) generateC(&ctx, &arena, tt, &m, lineMap, &c);
    phase("codegen", t_cg0);

    if (ctx.hasError) {
        Buf diag;
        bufInit(&diag, &arena);
        ctxRenderDiag(&ctx, &diag);
        fputs(bufCstr(&diag), stderr);
        return 1;
    }
    if (bodyDiag) return 1;      /* the module files' errors are already rendered above */

    /* Record the link requirements **in the artifact**. Generated C that needs `-lz`
     * but does not say so is a build trap: compile it by hand and the failure is a wall
     * of undefined references, far from the extC declaration that caused it. The comment
     * is machine-readable on purpose (`extc-libs:`), so a Makefile or a package manager
     * can read the requirement out of the artifact instead of keeping a second copy.
     * Emitted only when there is something to say, so a program that links nothing keeps
     * byte-identical output.
     *
     * Both sources count: the modules the program loaded (each module's own `<module>.link`)
     * and the command line's flags. Inside the comment the names are sorted and deduped --
     * it describes a set, and two builds asking for the same libraries in a different order
     * must produce the same bytes. The **link line** keeps declaration order instead: a
     * static library resolves symbols left to right, so reordering it is not the compiler
     * driver's business. */
    Vec allLibs;
    vecInit(&allLibs, &arena, sizeof(const char *));
    for (size_t k = 0; k < vecLen(&modLibs); k++)
        *(const char **)vecPush(&allLibs) = *(const char **)vecAt(&modLibs, k);
    for (int k = 0; k < nLinkArgs; k++)
        *(const char **)vecPush(&allLibs) = linkArgs[k];
    Vec allPkgs;
    vecInit(&allPkgs, &arena, sizeof(const char *));
    for (size_t k = 0; k < vecLen(&modPkgs); k++)
        *(const char **)vecPush(&allPkgs) = *(const char **)vecAt(&modPkgs, k);
    for (int k = 0; k < nPkgConfigArgs; k++)
        *(const char **)vecPush(&allPkgs) = pkgConfigArgs[k];

    if (vecLen(&allLibs) > 0 || vecLen(&allPkgs) > 0) {
        Buf head;
        bufInit(&head, &arena);
        if (vecLen(&allLibs) > 0) {
            const char **items = (const char **)vecAt(&allLibs, 0);
            size_t n = vecLen(&allLibs);
            for (size_t k = 1; k < n; k++) {          /* insertion sort: n is tiny */
                const char *key = items[k];
                size_t j = k;
                while (j > 0 && strcmp(items[j - 1], key) > 0) { items[j] = items[j - 1]; j--; }
                items[j] = key;
            }
            bufPuts(&head, "/* extc-libs:");
            for (size_t k = 0; k < n; k++) {
                if (k > 0 && strcmp(items[k], items[k - 1]) == 0) continue;
                bufPrintf(&head, " %s", items[k]);
            }
            bufPuts(&head, " */\n");
        }
        if (vecLen(&allPkgs) > 0) {
            const char **items = (const char **)vecAt(&allPkgs, 0);
            size_t n = vecLen(&allPkgs);
            for (size_t k = 1; k < n; k++) {
                const char *key = items[k];
                size_t j = k;
                while (j > 0 && strcmp(items[j - 1], key) > 0) { items[j] = items[j - 1]; j--; }
                items[j] = key;
            }
            bufPuts(&head, "/* extc-pkg-config:");
            for (size_t k = 0; k < n; k++) {
                if (k > 0 && strcmp(items[k], items[k - 1]) == 0) continue;
                bufPrintf(&head, " %s", items[k]);
            }
            bufPuts(&head, " */\n");
        }
        bufPutn(&head, bufCstr(&c), c.len);
        c = head;
    }

    /* `--check-c`: syntax-check the generated C. This has to run before the
     * `if (!doRun)` block below, which prints the C and returns, or the check would
     * only ever happen under `--run`. */
    if (doCheckC && !doRun) {
        const char *ccx = getenv("CC") ? getenv("CC") : "cc";
        char tmp[] = "/tmp/extc-syntaxcheck-XXXXXX";
        int fd = mkstemp(tmp);
        if (fd < 0) {
            fprintf(stderr, "extc: cannot write the syntax-check file\n");
            return 1;
        }
        FILE *f = fdopen(fd, "wb");
        if (!f) {
            close(fd);
            unlink(tmp);
            fprintf(stderr, "extc: cannot write the syntax-check file\n");
            return 1;
        }
        size_t written = fwrite(bufCstr(&c), 1, c.len, f);
        int closed = fclose(f);
        if (written != c.len || closed != 0) {
            unlink(tmp);
            fprintf(stderr, "extc: cannot write the syntax-check file\n");
            return 1;
        }
        Vec checkArgv;
        vecInit(&checkArgv, &arena, sizeof(char *));
        *(char **)vecPush(&checkArgv) = (char *)ccx;
        *(char **)vecPush(&checkArgv) = (char *)"-std=c11";
        *(char **)vecPush(&checkArgv) = (char *)"-fsyntax-only";
        *(char **)vecPush(&checkArgv) = (char *)"-w";
        /* The link/include flags go in here too: a header the program needs must be
         * findable at syntax-check time, or the check would fail on code that compiles
         * perfectly well under `--run`. */
        for (size_t k = 0; k < vecLen(&ccExtra); k++)
            *(char **)vecPush(&checkArgv) = *(char **)vecAt(&ccExtra, k);
        for (size_t k = 0; k < vecLen(&modExtra); k++)
            *(char **)vecPush(&checkArgv) = *(char **)vecAt(&modExtra, k);
        *(char **)vecPush(&checkArgv) = (char *)"-x";
        *(char **)vecPush(&checkArgv) = (char *)"c";
        *(char **)vecPush(&checkArgv) = tmp;
        *(char **)vecPush(&checkArgv) = NULL;
        int checkRc = runCmd((char *const *)vecAt(&checkArgv, 0));
        unlink(tmp);
        if (checkRc != 0) {
            fprintf(stderr, "extc: **the generated C does not compile** -- this is an extc"
                            " bug, not a mistake in your program\n");
            return 1;
        }
    }

    if (!doRun && !doBuild) {
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
    /* With `--build`, `-o` names the **binary** (with `--run` it still names the generated
     * C, because there is no other output to name). Without `-o`, both modes use
     * `build/<name>` the way `--run` always has. */
    const char *binPath = (doBuild && outPath) ? outPath : arenaPrintf(&arena, "build/%s", base);
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
    Vec ccArgv;
    vecInit(&ccArgv, &arena, sizeof(char *));
    *(char **)vecPush(&ccArgv) = (char *)cc;
    *(char **)vecPush(&ccArgv) = (char *)"-std=c11";
    *(char **)vecPush(&ccArgv) = (char *)(optLevel ? optLevel : "-O2");
    *(char **)vecPush(&ccArgv) = (char *)"-fwrapv";
    if (marchNative) *(char **)vecPush(&ccArgv) = (char *)"-march=native";
                       /* The compiler derives `_debug`, `_eq`, `_find` and further
                        * helpers for every type the program uses, and the ones nothing
                        * calls would otherwise stay in the binary: the text segment of
                        * hello fell from 3215 to 1446 bytes and that of euler-sieve from
                        * 6852 to 3475. Letting the linker drop the sections nobody
                        * references changes no semantics. */
    *(char **)vecPush(&ccArgv) = (char *)"-ffunction-sections";
    *(char **)vecPush(&ccArgv) = (char *)"-fdata-sections";
    *(char **)vecPush(&ccArgv) = (char *)"-Wl,--gc-sections";
    *(char **)vecPush(&ccArgv) = (char *)"-o";
    *(char **)vecPush(&ccArgv) = (char *)binPath;
    *(char **)vecPush(&ccArgv) = (char *)cPath;
    /* Order on the link line: **what the user wrote, then what the modules need**.
     * The linker resolves left to right with `--as-needed`, so a library placed before
     * the object that needs it is dropped: `app.c --ccflag shim.c` (a source the user
     * passed) must come before a module's `-l`, or the shim's undefined symbols survive
     * the library. This is the classic "objects before libraries" rule, and module flags
     * keep load order among themselves (the loader loads dependencies first). */
    for (size_t k = 0; k < vecLen(&ccExtra); k++)
        *(char **)vecPush(&ccArgv) = *(char **)vecAt(&ccExtra, k);
    for (size_t k = 0; k < vecLen(&modExtra); k++)
        *(char **)vecPush(&ccArgv) = *(char **)vecAt(&modExtra, k);
    *(char **)vecPush(&ccArgv) = NULL;
    int rc = runCmd((char *const *)vecAt(&ccArgv, 0));
    if (rc != 0) {
        fprintf(stderr, "extc: C compiler failed (exit %d) on `%s`\n", rc, cPath);
        return 1;
    }

    if (doBuild) return 0;              /* the binary is the output; do not run it */

    char *runArgv[] = { (char *)binPath, NULL };
    return runCmd(runArgv);
}
