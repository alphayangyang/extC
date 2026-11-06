/**
 * @file main.c
 * @brief The extC compiler driver: command line, pipeline, and the `--run` shortcut.
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

#include "version.h"

/* `extc --version` says what it is running on, because a bug report without the target is
 * half a bug report and the cheapest thing to add. */
#if defined(__linux__)
#define EXTC_TARGET_OS "linux"
#elif defined(__APPLE__)
#define EXTC_TARGET_OS "macos"
#elif defined(_WIN32)
#define EXTC_TARGET_OS "windows"
#else
#define EXTC_TARGET_OS "unknown"
#endif
#if defined(__x86_64__) || defined(_M_X64)
#define EXTC_TARGET_ARCH "x86_64"
#elif defined(__aarch64__)
#define EXTC_TARGET_ARCH "aarch64"
#else
#define EXTC_TARGET_ARCH "unknown"
#endif
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

/* PHASE(name, call): the timing variable lives in its own `do { } while (0)` scope, so
 * it cannot collide with a neighbour. */
/**
 * @brief Whether stage timing is on (environment variable `EXTC_DBG_TIME`); the
 *        environment is read once.
 *
 * @return  1 when on, 0 when off.
 */
static int timeOn(void) { static int v = -1; if (v < 0) v = getenv("EXTC_DBG_TIME") != NULL; return v; }

/**
 * @brief Process CPU time, in seconds.
 *
 * @return  CPU seconds consumed since the process started.
 */
static double nowSec(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }

/**
 * @brief Print one line of stage timing; does nothing while the switch is off.
 *
 * @param name  Stage name.
 * @param t0    CPU seconds when the stage started.
 */
static void phase(const char *name, double t0) { if (timeOn()) fprintf(stderr, "[time] %-9s %.3f s\n", name, nowSec() - t0); }
#define PHASE(name, call) do { double t_ = nowSec(); call; phase((name), t_); } while (0)

/* ------------------------------------------------------------- utilities */

/* Whether the memory report should be printed this time.
 *
 * `checkModule` runs once for the prelude and once for the file the user named, and only
 * the second is about the user's program: a report about the prelude would be noise, and
 * printing it twice would look like the program had been analysed twice. */
static bool g_explainRoot = false;

/**
 * @brief Write a run of bytes to a file, overwriting its contents.
 *
 * @param path  Path of the file to write.
 * @param data  Bytes to write.
 * @param len   Byte count.
 * @return      `true` when every byte was written; `false` when the file cannot be
 *              opened or the write is short.
 */
static bool writeFile(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

/**
 * @brief Take the file name out of a path and drop the extension:
 *        `examples/hello.extc` gives `hello`.
 *
 * Only `/` counts as a directory separator; the extension is whatever follows the last
 * dot in the name, so a dot in a directory name does not affect the result.
 *
 * @param a     Arena; it holds the returned string.
 * @param path  File path; the argument string is not modified.
 * @return      Arena copy of the file name.
 */
static const char *baseName(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    return arenaStrndup(a, base, n);
}

/**
 * @brief Run a program and wait for it to finish.
 *
 * The child inherits the three standard streams, so its output appears in place and the
 * compiled program can still read stdin. A failed exec is reported by the child itself
 * and turns into exit code 127; the parent sees an ordinary exit.
 *
 * @param argv  NULL-terminated argument vector; argv[0] is the program name, looked up
 *              along PATH.
 * @return      The program's exit code; -1 when it cannot be started, cannot be reaped,
 *              or is killed by a signal.
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

/**
 * @brief Run a program and capture its standard output.
 *
 * The pipe has to be read to EOF **before** the child is reaped: waiting for the child
 * first deadlocks once it has filled one pipe buffer. The child's standard error is left
 * alone, so its own complaints reach the user instead of being swallowed.
 *
 * @param argv  NULL-terminated argument vector, in the shape `runCmd` accepts.
 * @param out   Buffer the child's standard output is appended to.
 * @return      The program's exit code (0 on success); -1 when it cannot be started, is
 *              killed by a signal, or the pipe read fails.
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

/**
 * @brief Ask pkg-config for one package's flags and append the tokens to a cc argument list.
 *
 * @param a     arena for the split tokens.
 * @param args  the argument list the flags are appended to.
 * @param name  the pkg-config package name.
 * @return      false after reporting the failure; both callers treat it as fatal, because
 *              linking without the flags surfaces much later as a wall of undefined
 *              references that points at the wrong thing.
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

/**
 * @brief Read each loaded module's own link requirements (`<module>.link`).
 *
 * A module that wraps a C library knows which library it needs, and a program using it
 * should not have to repeat that on the command line. The requirements live in
 * `<module>.link` next to `<module>.extc`, and are read **only** for modules the program
 * loaded (directly or through a transitive `use`): nothing is linked by mistake, and
 * nothing is downloaded -- a library that is not installed is reported by the C compiler
 * itself at link time.
 *
 * Syntax (one directive per line; blank lines and `#` comments are ignored):
 *   lib <name>        append `-l<name>`; `lib :libsqlite3.so.0` may spell a soname
 *   pkgconfig <name>  append the tokens of `pkg-config --cflags --libs <name>`
 *   ccflag <flag>     append one raw flag (an escape hatch; the package author owns it)
 * An unknown directive is an error: a requirement silently ignored is exactly the failure
 * this function exists to prevent.
 *
 * @param a         Arena holding paths, tokens, and file contents.
 * @param ctxs      Each loaded module's Ctx, the vector `loadModules` filled in.
 * @param args      cc argument list; flags are appended to its end.
 * @param libNames  Receives `lib` names, for the artifact's `extc-libs:` line.
 * @param pkgNames  Receives `pkgconfig` names, for the artifact's `extc-pkg-config:` line.
 * @return          `true` on success; on error (bad directive, empty argument, pkg-config
 *                  failure) the problem has been reported and `false` is returned.
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

/**
 * @brief Print the declarations of one file as a JSON object (`--symbols-json`).
 *
 * The editor's outline and its "jump to declaration" both need the file's own declaration
 * table; both are cheap to derive from the module the checker already built, so no second
 * parser exists on the tool side. Only declarations that came from `ctx`'s file are printed
 * (the module also holds the prelude and every imported module), and the order is source
 * order, which is what an outline shows.
 *
 * @param out  Stream to write to.
 * @param m    Module that holds every declaration of the program.
 * @param ctx  Context of the file the user named; its `path` is the file being described.
 */
static void emitSymbolsJson(FILE *out, Module *m, Ctx *ctx) {
    bool first = true;
    fputs("{\"symbols\":[", out);
    for (size_t i = 0; i < vecLen(&m->funcs); i++) {
        FuncDef *f = *(FuncDef **)vecAt(&m->funcs, i);
        if (!f || f->ctx != ctx || funcIsMethod(f)) continue;
        fprintf(out, "%s{\"kind\":\"fn\",\"name\":\"%s\",\"line\":%d,\"params\":%zu}",
                first ? "" : ",", f->name, f->line, f->params.len);
        first = false;
    }
    for (size_t i = 0; i < vecLen(&m->structs); i++) {
        StructDef *sd = *(StructDef **)vecAt(&m->structs, i);
        if (!sd || sd->ctx != ctx) continue;
        fprintf(out, "%s{\"kind\":\"struct\",\"name\":\"%s\",\"line\":%d,\"fields\":%zu}",
                first ? "" : ",", sd->name, sd->line, sd->fields.len);
        first = false;
        for (size_t k = 0; k < vecLen(&sd->methods); k++) {
            FuncDef *md = *(FuncDef **)vecAt(&sd->methods, k);
            if (!md || md->ctx != ctx) continue;
            fprintf(out, "%s{\"kind\":\"method\",\"name\":\"%s\",\"line\":%d,\"container\":\"%s\"}",
                    first ? "" : ",", md->name, md->line, sd->name);
            first = false;
        }
    }
    for (size_t i = 0; i < vecLen(&m->types); i++) {
        TypeDef *td = *(TypeDef **)vecAt(&m->types, i);
        if (!td || td->ctx != ctx) continue;
        fprintf(out, "%s{\"kind\":\"type\",\"name\":\"%s\",\"line\":%d,\"variants\":%zu}",
                first ? "" : ",", td->srcName ? td->srcName : td->name, td->line, td->variants.len);
        first = false;
    }
    for (size_t i = 0; i < vecLen(&m->globals); i++) {
        GlobalDef *gd = *(GlobalDef **)vecAt(&m->globals, i);
        if (!gd || gd->ctx != ctx) continue;
        fprintf(out, "%s{\"kind\":\"global\",\"name\":\"%s\",\"line\":%d}", first ? "" : ",",
                gd->name, gd->line);
        first = false;
    }
    for (size_t i = 0; i < vecLen(&m->traits); i++) {
        TraitDef *tr = *(TraitDef **)vecAt(&m->traits, i);
        if (!tr || tr->modName) continue;
        fprintf(out, "%s{\"kind\":\"trait\",\"name\":\"%s\",\"line\":%d,\"methods\":%zu}",
                first ? "" : ",", tr->name, tr->line, tr->methods.len);
        first = false;
    }
    fputs("]}\n", out);
}

/**
 * @brief Print a summary of the command line to stderr.
 *
 * This text lists every accepted switch and every debug environment variable, because it
 * is the compiler driver's only documentation. It goes to stderr so that stdout stays
 * reserved for the generated C -- which is exactly what `extc foo.extc` prints.
 *
 * @param argv0  The name the compiler was invoked under, echoed as the program name.
 */
static void usage(const char *argv0) {
    fprintf(stderr,
        "extC compiler " EXTC_VERSION "\n"
        "\n"
        "usage: %s [options] <file.extc>\n"
        "       %s run [<file.extc> | <dir>] [-- <program arguments>]\n"
        "       %s build [<file.extc> | <dir>] [options]\n"
        "\n"
        "`run` and `build` are the `--run` and `--build` modes without the flags: with no\n"
        "file they take the entry of the project in the current directory. The entry is the\n"
        "first of src/main.extc, main.extc that exists, else the path named by\n"
        "`[build] entry` in packages.toml. Everything after `--` goes to the\n"
        "program, so `extc run -- -m demo x` runs the `@module` entry `demo`.\n"
        "\n"
        "options:\n"
        "  --version, -V    print the compiler version, target and how it finds the library\n"
        "  -o <file>        write the generated C to this file (default: stdout)\n"
        "  --run            write generated C to build/<name>.c, compile it, run it\n"
        "                   (`extc run` is the same mode; it also discovers the entry file\n"
        "                   and passes arguments after `--` to the program)\n"
        "                   (uses $CC, default `cc`; creates ./build/ in the CWD)\n"
        "  --build          same, but stop before running: the binary is `-o <file>` if\n"
        "                   (`extc build` is the same mode, with entry discovery)\n"
        "                   given, otherwise build/<name> (this is what a package's build\n"
        "                   step wants: link flags from <module>.link, no execution)\n"
        "  --check-c        syntax-check the generated C with `$CC -fsyntax-only`\n"
        "  -w               suppress warnings\n"
        "  -I <dir>         add a module search directory (for `use a::b`)\n"
        "  -l <name>        link with `-l<name>` (repeatable; e.g. `-l z`)\n"
        "  -L <dir>         add a library search directory (repeatable)\n"
        "  --ccflag <flag>  append one raw flag to the C compiler command (repeatable);\n"
        "                   e.g. `--ccflag -l:libsqlite3.so.0` or `--ccflag -I/x`\n"
        "  --pkg-config <name>  add `pkg-config --cflags --libs <name>` (repeatable)\n"
        "  -O0 .. -O3       optimisation level for the generated C (default: -O2)\n"
        "  -march=native    allow host-specific instructions (faster, less portable)\n"
        "  --dump-tokens    lex only; print the token table\n"
        "  --dump-effects   print each function's effect summary (Addr/Cont, arena rule)\n"
        "  --no-line-map    do not emit `#line` directives (default: emit them)\n"
        "  --no-fuse        keep adjacent element loops separate\n"
        /* Editor integration: these four exist for `tools/extc-lsp.py` and for build
         * systems that want a check without a translation. They never change what the
         * compiler accepts, only what it prints and how far it goes. */
        "  --diagnostics-json  print diagnostics as one JSON object per line (for editors)\n"
        "  --symbols-json   print this file's declarations as JSON; does not generate C\n"
        "  --check-only     stop after type checking; does not generate C\n"
        "  --source-stdin   read the source from stdin, keeping the named path as its identity\n"
        "\n",
        /* The two switches named below change the output, so this text cannot claim that
         * they are all diagnostics only: `EXTC_SELFCHECK` can turn a good build red, and
         * `EXTC_NO_LEVELPASS` changes the generated program. Claiming something the code
         * contradicts is worse than claiming nothing, so both exceptions are named here. */
        argv0, argv0, argv0);
    /* Split here, not for style: one concatenated literal over 4095 bytes is longer than
     * ISO C99 requires a compiler to support, and gcc says so with -Woverlength-strings.
     * The second half has no `%s`, so it is a plain one-argument fprintf. */
    fprintf(stderr,
        "debug switches -- diagnostics only, **except the marked below**:\n"
        "  EXTC_DBG_ARENA=1     check the arena level the checker computed vs codegen\n"
        "  EXTC_DBG_QN=1        trace how a qualified name (a::b::c) is parsed/resolved\n"
        "  EXTC_DBG_M=1         print the per-module renamed-declaration counts\n"
        "  EXTC_DBG_IMPL=1      print each `impl` block and the type it attaches to\n"
        "  EXTC_DBG_HOME=1      print each function's home/zone flags and its arena sites\n"
        "  EXTC_DBG_OWNER=1     report each result slot first reached from another body\n"
        "  EXTC_DBG_RCCOST=1    print each region's rc site census (reference stores / allocations /\n"
        "                       max root-table slots, split by \"inside a loop\")\n"
        "  EXTC_DUMP_EFFECTS=1  print each function's effect summary\n"
        "  EXTC_NO_LEVELPASS=1  skip the arena level pass -- **CHANGES THE OUTPUT**: it moves arena\n"
        "                       placement in 2 of the 414 golden programs\n"
        "  EXTC_SELFCHECK=1     run the checker's self-check -- **CAN FAIL THE BUILD** (exit 1)\n"
        "  EXTC_RECLAIM=0       turn reclamation off; it is **on by default** -- **CHANGES THE OUTPUT**.\n"
        "                       A program with a governed allocation point (`new T` / `alloc<T>(n)` /\n"
        "                       `@overwrite`) gets the slab allocator / holder-count / region-sweep\n"
        "                       block; `=1` **allows** it (side-by-side runs) but does not force it: a\n"
        "                       unit proved to have no reclaim work emits no reclamation runtime at all\n"
        "  EXTC_RC_SHARDS=N     reclamation's shard count, the pause knob -- **CHANGES THE OUTPUT**.\n"
        "                       Default 1 = one runtime region per logical one; N > 1 splits each\n"
        "                       logical region into N runtime regions that sweep one at a time\n"
        "  EXTC_ARENA_FILL=NN   diagnostic only: fill every **non-zeroed** arena allocation with the\n"
        "                       byte NN (0-255), so \"read of never-written arena memory\" becomes\n"
        "                       deterministic instead of load-dependent -- the arena's own\n"
        "                       `MALLOC_PERTURB_`. Unset (default) writes nothing: byte-identical output\n"
        "  EXTC_RUN_DIR=DIR     `--run`/`--build` write `<DIR>/<name>.c` and `<DIR>/<name>` instead of\n"
        "                       `build/<name>.c` and `build/<name>`. **Does not change the program's\n"
        "                       output** -- it only picks where the artifacts land, so two runs that\n"
        "                       share a source basename (`a/generic.extc` and `b/generic.extc`) do not\n"
        "                       overwrite each other. Default `build`, the path every caller gets\n"

        "\n"
        "  -h, --help       show this help\n");
    /* These further diagnostic switches are printed **separately** from the big table
     * above: one independent fprintf keeps them out of that table's layout. */
    fprintf(stderr,
        "  EXTC_RC_NOCOUNT=0    re-enable holder counting (it is **off by default**): the default\n"
        "                       rests on region sweeps alone -- **CHANGES THE OUTPUT**.\n"
        "  EXTC_RC_DBG_LEVEL=1  diagnostic only: print, for every reference store site, whether the\n"
        "                       value and its host object sit in the same arena level.\n"
        "  EXTC_RC_LOCALSTORE=0 kill switch: turn OFF the local/parameter slot store entry\n"
        "                       `extc_rc_storeRefLocal` (write-only: it skips the two `ownSlab`\n"
        "                       probes the generic entry performs and discards, because the host\n"
        "                       is a slot, not an object). **On by default**; no-count regime\n"
        "                       only; `=0` restores the pre-flip output byte for byte.\n"
        "  EXTC_RC_TIME=1       diagnostic only: the generated program counts how often each\n"
        "                       reclamation component is called and prints the tally to stderr.\n"
        "  EXTC_RC_SWEEPFREE=1  diagnostic only: event regime -- turn holder counting on and stop\n"
        "                       sweeping entirely (reclaim only when a holder count hits zero).\n"
        "  EXTC_DBG_ACYCLIC=1   diagnostic only: at compile time print one line per used function,\n"
        "                       `[acyclic] <name> fn=<0|1> reason=<...>`, plus a summary.\n"
        "  EXTC_DBG_RCIDLE=1    diagnostic only: at compile time print one line per used function,\n"
        "                       `[rcidle] <name> idle=<0|1> reason=<...>` -- the `(a)(b)(c)` proof\n"
        "                       that its region has no reclaim work to do.\n"
        "  EXTC_RC_PAUSE=1      diagnostic only: the generated program times every real sweep (one\n"
        "                       clock read before and after) and prints max/total/collects at exit.\n"
        "                       Uses a monotonic clock when the platform exposes one, else ISO C\n"
        "                       clock() (CPU time). See prototype/reclaim/sim.c for the method.\n");
}

/* ---------------------------------------------------------------- prelude */

/**
 * @brief Tell whether the `slice` declared in a module has the shape the compiler expects.
 *
 * A view is recognised by protocol, not by type: a struct named `slice`, exactly one type
 * parameter, and first two fields `data: ref T` and `len` in that order. The language
 * knows this protocol; the methods come from the library.
 *
 * Checking the shape here makes an implicit contract explicit: otherwise renaming one
 * field in the prelude would surface only when the generated C fails to compile.
 *
 * @param m  Module to check.
 * @return   `true` when a conforming `slice` exists; `false` when it is missing or has
 *           the wrong shape.
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

/**
 * @brief Parse and check the bundled prelude, then merge it into the main module.
 *
 * The prelude gets a Ctx of its own, whose path renders as <extc prelude>, so an
 * error inside it points at the right position. The prelude ships with the
 * compiler, so a failure here means the compiler is broken rather than the user's
 * program, and the driver reports an internal error.
 *
 * The prelude is checked twice, once here and once after being merged; it is small
 * enough for the cost to be irrelevant, and the separate Ctx is what keeps its error
 * positions correct.
 *
 * @param arena  arena for the prelude's tokens, tree, and diagnostics.
 * @param tt     the type table shared with the user file.
 * @param m      module the prelude declarations are merged into, ahead of the user's.
 * @return       false when the prelude failed to compile or does not satisfy the view
 *               contract, in which case the driver stops.
 */
static bool loadPrelude(Arena *arena, TypeTable *tt, Module *m) {
    size_t len = 0;
    const char *src = preludeSource(&len);

    /* The prelude's `Ctx` lives in the arena, not on this frame: bodies of prelude functions
     * are checked **after** this function returns (the deferred pass), and they read the
     * `Ctx` they were parsed with. A stack object would leave those reads pointing at a dead
     * frame -- a use-after-scope that only a sanitized build reports. */
    Ctx *ctx = arenaAlloc(arena, sizeof(Ctx));
    ctxInit(ctx, arena, PRELUDE_PATH, src, len);

    Module pm;
    memset(&pm, 0, sizeof pm);
    moduleInit(&pm, arena);

    Vec toks;
    vecInit(&toks, arena, sizeof(Token));
    lexAll(ctx, &toks);
    if (!ctx->hasError) parseModule(ctx, arena, &toks, &pm);
    if (!ctx->hasError) {
        ttRegister(tt, &pm);        /* the same table the user file will use */
        g_explainRoot = false;      /* a report about the prelude is noise */
        setenv("EXTC_EXPLAIN_ROOT", "0", 1);
        checkModule(ctx, arena, tt, &pm);
    }

    if (ctx->hasError) {
        Buf diag;
        bufInit(&diag, arena);
        ctxRenderDiag(ctx, &diag);
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

    /* `s[i]` is syntax applied to `slice`, so the prelude has to keep the shape the
     * compiler depends on. */
    if (!viewContractOk(&pm)) {
        fputs("extc: internal error -- the prelude's `slice` does not have the shape "
              "the compiler expects (`data: ref T` then `len`)\n", stderr);
        return false;
    }

    /* Merge into the main module: the prelude declarations come first, the user's
     * follow. */
    for (size_t i = 0; i < pm.structs.len; i++)
        *(StructDef **)vecPush(&m->structs) = *(StructDef **)vecAt(&pm.structs, i);
    for (size_t i = 0; i < pm.types.len; i++)
        *(TypeDef **)vecPush(&m->types) = *(TypeDef **)vecAt(&pm.types, i);
    for (size_t i = 0; i < pm.funcs.len; i++)
        *(FuncDef **)vecPush(&m->funcs) = *(FuncDef **)vecAt(&pm.funcs, i);
    return true;
}

/* ---------------------------------------------------------------- project discovery */

/**
 * @brief Does this path name an existing regular file?
 *
 * @param p  Path to test.
 * @return   True when `p` can be opened as a regular file.
 */
static bool pathIsFile(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * @brief Does this path name an existing directory?
 *
 * @param p  Path to test.
 * @return   True when `p` is a directory.
 */
static bool pathIsDir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @brief Read the entry file a project manifest names, if it names one.
 *
 * Only one shape is understood, deliberately: a `[build]` table with a single
 * `entry = "<path>"` key. A project that wants more than this is asking for a build
 * description, not a build convention, and `tools/extpkg.py` is where that lives. The
 * path is taken relative to the manifest's directory.
 *
 * @param a    Arena the returned string is allocated from.
 * @param dir  Directory holding `packages.toml`.
 * @return     The entry path, or NULL when there is no manifest or no `entry` key.
 */
static const char *manifestEntry(Arena *a, const char *dir) {
    const char *manPath = arenaPrintf(a, "%s/packages.toml", dir);
    FILE *f = fopen(manPath, "rb");
    if (!f) return NULL;
    bool inBuild = false;
    const char *found = NULL;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') {
            /* Section header: leave the table when the `[build]` table ends. */
            inBuild = strncmp(s, "[build]", 7) == 0;
            continue;
        }
        if (!inBuild) continue;
        if (strncmp(s, "entry", 5) != 0) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        char quote = *v;
        if (quote != '"' && quote != '\'') continue;
        v++;
        char *end = strchr(v, quote);
        if (!end) continue;
        *end = 0;
        found = arenaPrintf(a, "%s/%s", dir, v);
        break;
    }
    fclose(f);
    return found;
}

/**
 * @brief Find the entry file of a project directory.
 *
 * The order is the convention, and it is short on purpose: `src/main.extc`, then
 * `main.extc`, then whatever `packages.toml` names. A project that wants another spelling
 * writes it in the manifest rather than expecting the driver to guess.
 *
 * @param a    Arena the returned string is allocated from.
 * @param dir  Directory to search.
 * @return     Path to the entry file, or NULL when the directory has none.
 */
static const char *findProjectEntry(Arena *a, const char *dir) {
    static const char *const candidates[] = { "src/main.extc", "main.extc" };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        const char *p = arenaPrintf(a, "%s/%s", dir, candidates[i]);
        if (pathIsFile(p)) return p;
    }
    const char *fromManifest = manifestEntry(a, dir);
    return (fromManifest && pathIsFile(fromManifest)) ? fromManifest : NULL;
}

/* ---------------------------------------------------------------- main */

/**
 * @brief Run one compilation: parse the command line, load, check, generate, and compile
 *        and run the result as requested.
 *
 * @param argc  Argument count.
 * @param argv  Argument vector; argv[0] is the program name.
 * @return      0 on success; 1 when the program does not compile; 2 when the command
 *              line itself is wrong. Under `--run` it is the compiled program's own
 *              exit code.
 */
int main(int argc, char **argv) {
    const char *path = NULL;
    const char *outPath = NULL;
    const char *optLevel = NULL;     /* `-O0` .. `-O3`; the default is `-O2` */
    bool        warnOwnedView = false; /* `-Wowned-view`: stage one of the `slice` migration */
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
    /* `--check-c`: syntax-check the generated C before handing it out. Generated C that
     * does not compile is the worst class of compiler bug -- the compiler's promise is
     * that a program which type-checks can always be built. */
    bool doCheckC = false;
    bool lineMap = true;
    bool noFuse = false;       /* `--no-fuse`: turn off S1 element-chain fusion */
    /* Editor integration (`tools/extc-lsp.py`). `--diagnostics-json` makes every diagnostic
     * a JSON object on stdout (see `ctxSetDiagJson`); `--symbols-json` prints the declaration
     * table of the named file instead of generating C; `--check-only` stops after the
     * checker; `--source-stdin` reads the source from stdin while keeping `path` as the
     * file's identity, which is what lets an editor compile an unsaved buffer without
     * writing it to disk (the module name comes from the path, not from the buffer). */
    bool diagJson = false;
    bool symbolsJson = false;
    bool checkOnly = false;
    bool sourceStdin = false;
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
    /* `extc run` / `extc build`: the same two modes as `--run` / `--build`, spelled the
     * way every other toolchain spells them. `run` also lets the program see its own
     * arguments, which `--run` cannot express: everything after a bare `--` is handed to
     * the program unchanged, so a project whose binary dispatches subcommands with
     * `@module` is driven with `extc run -- -m demo x y`. */
    const char  *progArgs[64];
    int          nProgArgs = 0;
    /* A project directory, or "." -- resolved to an entry file once the arena exists. */
    const char  *discoverDir = NULL;
    /* `--version` is answered here, before the subcommands and before any file is looked
     * for: a version question must not depend on the working directory. */
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("extc %s\n", EXTC_VERSION);
        printf("target %s/%s; C backend: $CC (default `cc`)\n", EXTC_TARGET_OS, EXTC_TARGET_ARCH);
        printf("stdlib: $EXTC_STD, else <directory of extc>/../stdlib\n");
        return 0;
    }
    int          argBegin = 1;
    if (argc > 1 && strcmp(argv[1], "run") == 0) { doRun = true; argBegin = 2; }
    else if (argc > 1 && strcmp(argv[1], "build") == 0) { doBuild = true; argBegin = 2; }

    for (int i = argBegin; i < argc; i++) {
        /* Optimisation switch: the default is `-O2`; `-march=native` trades portability
         * for speed, so it is enabled only when the user asks for it explicitly. */
        if (strncmp(argv[i], "-O", 2) == 0 && argv[i][2] >= '0' && argv[i][2] <= '3'
            && argv[i][3] == 0) {
            optLevel = argv[i];
            continue;          /* do not advance i here: the for loop already does,
                                * and a second increment swallows the next argument */
        }
        if (strcmp(argv[i], "-march=native") == 0) { marchNative = true; continue; }
        if (strcmp(argv[i], "-w") == 0) { noWarn = true; continue; }
        if (strcmp(argv[i], "-Wowned-view") == 0) { warnOwnedView = true; continue; }
        if (strcmp(argv[i], "--dump-tokens") == 0) {
            dumpTokens = true;
        } else if (strcmp(argv[i], "--dump-effects") == 0) {
            /* Debug switch: print each function's Addr/Cont effect summary. The
             * environment variable is set here, so the rest of the driver does not have
             * to know about this flag. */
            setenv("EXTC_DUMP_EFFECTS", "1", 1);
        } else if (strcmp(argv[i], "--explain-memory") == 0) {
            /* One line per allocation site: which level it lands in, and whether each
             * iteration of the loop it sits in frees it. Printed after checking, when
             * every level is final. */
            setenv("EXTC_EXPLAIN_MEMORY", "1", 1);
        } else if (strcmp(argv[i], "--check-c") == 0) {
            doCheckC = true;
        } else if (strcmp(argv[i], "--run") == 0) {
            doRun = true;
        } else if (strcmp(argv[i], "--build") == 0) {
            doBuild = true;
        } else if (strcmp(argv[i], "--no-fuse") == 0) {
            /* Fusion (the level-2 S1 element chain) is on by default; this switch is for
             * differential testing and debugging: with it off, the same source takes the
             * unfused path. */
            noFuse = true;
        } else if (strcmp(argv[i], "--diagnostics-json") == 0) {
            diagJson = true;
        } else if (strcmp(argv[i], "--symbols-json") == 0) {
            symbolsJson = true;
        } else if (strcmp(argv[i], "--check-only") == 0) {
            checkOnly = true;
        } else if (strcmp(argv[i], "--source-stdin") == 0) {
            sourceStdin = true;
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
        } else if (strcmp(argv[i], "--") == 0) {
            /* Everything after the separator belongs to the compiled program, not to
             * `extc`: `extc run -- -m demo x` runs the binary's `demo` entry. */
            for (int k = i + 1; k < argc; k++) {
                if (nProgArgs < (int)(sizeof progArgs / sizeof progArgs[0])) progArgs[nProgArgs] = argv[k];
                nProgArgs++;
            }
            break;
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

    if (path && pathIsDir(path)) {
        /* `extc run src/` and `extc build .`: the argument names the project, not the
         * file. */
        discoverDir = path;
        path = NULL;
    }
    if (!path) {
        /* Only a mode that produces a binary can imply an entry file: plain `extc` with
         * no input still has nothing to do, and printing the usage is the honest answer. */
        if (!doRun && !doBuild) {
            usage(argv[0]);
            return 2;
        }
        if (!discoverDir) discoverDir = ".";
    }

    Arena arena;
    arenaInit(&arena, 64 * 1024);

    if (!path) {
        path = findProjectEntry(&arena, discoverDir);
        if (!path) {
            fprintf(stderr, "extc: no entry file below `%s`\n", discoverDir);
            fprintf(stderr, "  looked for %s/src/main.extc, %s/main.extc, and `[build] entry`"
                            " in %s/packages.toml\n",
                    discoverDir, discoverDir, discoverDir);
            return 2;
        }
    }

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
    char *src;
    if (sourceStdin) {
        /* Read the whole of stdin into the arena: an editor sends an unsaved buffer, and the
         * compiler must see it under the path it would otherwise open (module identity and
         * diagnostic positions both come from that path, not from the stream). */
        size_t cap = 1 << 16, len = 0;
        char *buf = (char *)arenaAlloc(&arena, cap);
        size_t n;
        while ((n = fread(buf + len, 1, cap - len - 1, stdin)) > 0) {
            len += n;
            if (len + 1 >= cap) {
                size_t ncap = cap * 2;
                char *nb = (char *)arenaAlloc(&arena, ncap);
                memcpy(nb, buf, len);
                buf = nb;
                cap = ncap;
            }
        }
        buf[len] = 0;
        src = buf;
        srcLen = len;
    } else {
        src = readWholeFile(&arena, path, &srcLen);
    }
    if (!src) {
        fprintf(stderr, "extc: cannot read `%s`\n", path);
        return 1;
    }
    ctxSetDiagJson(diagJson);

    Vec searchDirs;
    vecInit(&searchDirs, &arena, sizeof(const char *));
    for (int k = 0; k < nStdDirArgs; k++)
        *(const char **)vecPush(&searchDirs) = stdDirArgs[k];

    Ctx ctx;
    ctxInit(&ctx, &arena, path, src, srcLen);
    ctx.noWarn = noWarn;
    ctx.warnOwnedView = warnOwnedView;

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
    checkSetNoFuse(noFuse);
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
        /* The loader (and the AST rewrites it drives) sits between parse and check; it
         * is timed separately here. */
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

    /* Editor integration: the declaration table, or a checker-only run, both stop here.
     * `--symbols-json` implies "do not generate C": the caller wants the outline, not a
     * translation, and keeping the two apart is what makes the LSP response small. */
    if (!ctx.hasError && !bodyDiag && symbolsJson) { emitSymbolsJson(stdout, &m, &ctx); return 0; }
    if (!ctx.hasError && !bodyDiag && checkOnly) return 0;

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

    /* Record the link requirements in the artifact: generated C that needs `-lz` without
     * saying so is a build trap -- compiled by hand it fails with a wall of undefined
     * references, far from the extC declaration that caused it. The comment is
     * deliberately machine-readable (`extc-libs:`), so a build system can read the
     * requirements back out of the artifact.
     *
     * Both sources count: each loaded module's `<module>.link` and the command line's
     * flags. The names in the comment are sorted and de-duplicated -- it describes a set,
     * and two builds that need the same libraries in a different order must produce the
     * same bytes; the **link line** keeps declaration order: a static library resolves
     * symbols left to right, so reordering is not the compiler driver's business. The
     * line is not emitted when there is nothing to say, so a program that links nothing
     * stays byte-identical. */
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

    /* `--run`: write <run dir>/<base>.c, compile it, then run the result.
     *
     * The run directory is `build` unless `EXTC_RUN_DIR` says otherwise. Two `extc --run`
     * invocations that happen to share a source basename (`tests/ops/generic.extc` and
     * `tests/ctor/generic.extc`, say) write the same two paths, so running them at the same
     * time truncates the generated C and replaces the binary under a running program -- the
     * caller gets a spurious failure. A caller that runs several programs at once gives each
     * one its own directory. Unset (the default, and what every existing caller gets) the
     * paths are exactly `build/<base>.c` and `build/<base>`. */
    const char *base = baseName(&arena, path);
    const char *runDir = getenv("EXTC_RUN_DIR");
    if (!runDir || !*runDir) runDir = "build";
    if (mkdir(runDir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "extc: cannot create `%s/`: %s\n", runDir, strerror(errno));
        return 1;
    }

    const char *cPath = arenaPrintf(&arena, "%s/%s.c", runDir, base);
    /* With `--build`, `-o` names the **binary** (with `--run` it still names the generated
     * C, because there is no other output to name). Without `-o`, both modes use
     * `build/<name>` the way `--run` always has. */
    const char *binPath = (doBuild && outPath) ? outPath : arenaPrintf(&arena, "%s/%s", runDir, base);
    char *text = bufCstr(&c);
    if (!writeFile(cPath, text, c.len)) {
        fprintf(stderr, "extc: cannot write `%s`\n", cPath);
        return 1;
    }

    const char *cc = getenv("CC") ? getenv("CC") : "cc";


    /* `-fwrapv` makes signed overflow wrap, and makes that wrap defined behaviour.
     *
     * Without it, the maximum i32 plus 1 is signed overflow, which is undefined
     * behaviour: under `-O1` it happens to wrap, but the C standard does not promise
     * that, and `-O2`'s strict-overflow assumptions can delete a test such as
     * `if (x + 1 > x)` entirely. extC promises not to hand undefined behaviour to the
     * programmer, so the generated code may not contain any. This flag turns that class
     * of case into defined wrap: the smallest step, matching what gcc actually does,
     * turning "happens to" into "is guaranteed to". */
    Vec ccArgv;
    vecInit(&ccArgv, &arena, sizeof(char *));
    *(char **)vecPush(&ccArgv) = (char *)cc;
    *(char **)vecPush(&ccArgv) = (char *)"-std=c11";
    *(char **)vecPush(&ccArgv) = (char *)(optLevel ? optLevel : "-O2");
    *(char **)vecPush(&ccArgv) = (char *)"-fwrapv";
    if (marchNative) *(char **)vecPush(&ccArgv) = (char *)"-march=native";
                       /* The compiler derives helpers such as `_debug`, `_eq`, and
                        * `_find` for every type the program uses; the ones nobody calls
                        * would otherwise stay in the binary. Letting the linker drop
                        * unreferenced sections does not change semantics. */
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

    /* The program's own arguments follow its path, exactly as if it had been started by
     * the shell: `extc run -- a b` is `./build/main a b`. */
    char **runArgv = (char **)arenaAlloc(&arena, sizeof(char *) * (size_t)(nProgArgs + 2));
    runArgv[0] = (char *)binPath;
    for (int k = 0; k < nProgArgs; k++) runArgv[k + 1] = (char *)progArgs[k];
    runArgv[nProgArgs + 1] = NULL;
    return runCmd(runArgv);
}
