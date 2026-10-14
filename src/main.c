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

/* PHASE(name, call)：计时变量关在自己的 `do { } while (0)` 作用域里，不与邻居相撞。 */
/**
 * @brief 阶段计时开关是否打开（环境变量 `EXTC_DBG_TIME`），只读一次环境。
 *
 * @return  打开时为 1，关闭时为 0。
 */
static int timeOn(void) { static int v = -1; if (v < 0) v = getenv("EXTC_DBG_TIME") != NULL; return v; }

/**
 * @brief 进程 CPU 时间，秒。
 *
 * @return  自进程启动以来消耗的 CPU 秒数。
 */
static double nowSec(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }

/**
 * @brief 打一行阶段耗时；开关关着时什么都不做。
 *
 * @param name  阶段名。
 * @param t0    阶段开始时的 CPU 秒数。
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
 * @brief 把一段字节写进文件，覆盖原有内容。
 *
 * @param path  要写入的文件路径。
 * @param data  要写入的字节。
 * @param len   字节数。
 * @return      每个字节都写成功时为 true；文件打不开或出现短写时为 false。
 */
static bool writeFile(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

/**
 * @brief 取出路径里的文件名并去掉扩展名：`examples/hello.extc` 得到 `hello`。
 *
 * 只有 `/` 算目录分隔符；扩展名是名字里最后一个点之后的部分，所以目录名里带点不影响结果。
 *
 * @param a     arena，返回的字符串由它持有。
 * @param path  文件路径；入参字符串不被修改。
 * @return      文件名的 arena 副本。
 */
static const char *baseName(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    return arenaStrndup(a, base, n);
}

/**
 * @brief 运行一个程序并等它结束。
 *
 * 子进程继承三个标准流，所以它的输出就地出现，被编译的程序仍能从 stdin 读。exec 失败由
 * 子进程自己报告，并转成退出码 127，父进程看到的是一次普通退出。
 *
 * @param argv  NULL 结尾的参数向量；argv[0] 是程序名，按 PATH 查找。
 * @return      程序的退出码；启动失败、回收失败或被信号杀死时返回 -1。
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
 * @brief 运行一个程序并捕获它的标准输出。
 *
 * 管道要读到 EOF **之后**才回收子进程：先等子进程，会在它写满一个管道缓冲区时死锁。
 * 子进程的标准错误不动，所以它自己的抱怨会到用户眼前，而不是被吞掉。
 *
 * @param argv  NULL 结尾的参数向量，与 `runCmd` 接受的形式相同。
 * @param out   子进程标准输出追加到的缓冲区。
 * @return      程序的退出码（成功为 0）；启动失败、被信号杀死或管道读失败时返回 -1。
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
 * @brief 读取程序实际加载的每个模块自己的链接需求（`<module>.link`）。
 *
 * 包装 C 库的模块知道自己需要哪个库，用它的程序不该在命令行上重复一遍。需求写在
 * `<module>.extc` 旁边的 `<module>.link` 里，并且**只**为程序加载过（直接或传递 `use`）
 * 的模块读取：不会误链，也不会下载什么 —— 没装的库在链接期由 C 编译器自己报错。
 *
 * 语法（一行一条；空行与 `#` 注释忽略）：
 *   lib <name>        追加 `-l<name>`；`lib :libsqlite3.so.0` 可以写 soname
 *   pkgconfig <name>  追加 `pkg-config --cflags --libs <name>` 的词元
 *   ccflag <flag>     追加一个原样 flag（逃生口，包作者自己负责）
 * 未知指令是错误：被静默忽略的需求正是这里要防的失败。
 *
 * @param a         arena，存放路径、词元与文件内容。
 * @param ctxs      每个已加载模块的 Ctx，即 `loadModules` 填好的那一份。
 * @param args      cc 参数表，flag 追加到它末尾。
 * @param libNames  接收 `lib` 名字，供产物的 `extc-libs:` 行使用。
 * @param pkgNames  接收 `pkgconfig` 名字，供产物的 `extc-pkg-config:` 行使用。
 * @return          成功为 true；出错（坏指令、空参数、pkg-config 失败）时已报告并返回 false。
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
 * @brief 把命令行摘要打印到 stderr。
 *
 * 这段文字列出所有接受的开关以及调试环境变量，因为它是编译器驱动唯一的说明文档。
 * 写 stderr 是为了把 stdout 留给生成的 C —— `extc foo.extc` 打印的正是它。
 *
 * @param argv0  编译器被调用时的名字，作为程序名回显。
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
        /* 下面两个开关会改变输出，所以这段文字不能声称它们都只是诊断：`EXTC_SELFCHECK`
         * 能让一次好构建变红，`EXTC_NO_LEVELPASS` 会改动生成物。声称与代码矛盾比不声称更糟，
         * 所以两个例外在这里点名。 */
        "debug switches -- diagnostics only, **except the marked below**:\n"
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
        "  -h, --help       show this help\n",
        argv0);
    /* 本会话新加的诊断开关：与上面那张大表**分开打印** —— 那张表的行为反复误判过（见 CHANGELOG），
     * 独立一条 fprintf 显然正确，且不再被它的结构牵着走。 */
    fprintf(stderr,
        "  EXTC_RC_NOCOUNT=0    re-enable holder counting (it is **off by default**): the default\n"
        "                       rests on region sweeps alone -- **CHANGES THE OUTPUT**.\n"
        "  EXTC_RC_DBG_LEVEL=1  diagnostic only: print, for every reference store site, whether the\n"
        "                       value and its host object sit in the same arena level.\n"
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
        "                       clock() (CPU time). See prototype-reclaim/sim.c for the method.\n");
}

/* ---------------------------------------------------------------- prelude */

/**
 * @brief 判断模块里声明的 `slice` 是不是编译器期望的形状。
 *
 * 视图靠协议而不是靠类型识别：结构体名叫 `slice`，恰好一个类型参数，前两个字段依次是
 * `data: ref T` 与 `len`。语言知道这个协议，方法由库提供。
 *
 * 在这里查形状，是把一个隐式契约变成显式的：否则 prelude 里某个字段被改名，要到生成的 C
 * 编不过的时候才暴露。
 *
 * @param m  要检查的模块。
 * @return   存在合规的 `slice` 时为 true；缺失或形状不对时为 false。
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

    /* `s[i]` 是作用在 `slice` 上的语法，所以 prelude 必须保持编译器依赖的形状。 */
    if (!viewContractOk(&pm)) {
        fputs("extc: internal error -- the prelude's `slice` does not have the shape "
              "the compiler expects (`data: ref T` then `len`)\n", stderr);
        return false;
    }

    /* 并入主模块：prelude 声明排在最前，用户声明跟在后面。 */
    for (size_t i = 0; i < pm.structs.len; i++)
        *(StructDef **)vecPush(&m->structs) = *(StructDef **)vecAt(&pm.structs, i);
    for (size_t i = 0; i < pm.types.len; i++)
        *(TypeDef **)vecPush(&m->types) = *(TypeDef **)vecAt(&pm.types, i);
    for (size_t i = 0; i < pm.funcs.len; i++)
        *(FuncDef **)vecPush(&m->funcs) = *(FuncDef **)vecAt(&pm.funcs, i);
    return true;
}

/* ---------------------------------------------------------------- main */

/**
 * @brief 跑一次编译：解析命令行，加载、检查、生成，并按请求编译和运行结果。
 *
 * @param argc  参数个数。
 * @param argv  参数向量；argv[0] 是程序名。
 * @return      成功返回 0；程序编不过返回 1；命令行本身有错返回 2。`--run` 下返回的是被
 *              编译程序自己的退出码。
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
    /* `--check-c`：交出去之前先对生成的 C 做语法检查。生成的 C 编不过，是编译器最严重的
     * 一类 bug —— 它的承诺就是类型检查通过的程序必然能构建。 */
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
        /* 优化开关：默认 `-O2`；`-march=native` 用可移植性换速度，所以只在用户明确要求时打开。 */
        if (strncmp(argv[i], "-O", 2) == 0 && argv[i][2] >= '0' && argv[i][2] <= '3'
            && argv[i][3] == 0) {
            optLevel = argv[i];
            continue;          /* do not advance i here: the for loop already does,
                                * and a second increment swallows the next argument */
        }
        if (strcmp(argv[i], "-march=native") == 0) { marchNative = true; continue; }
        if (strcmp(argv[i], "-w") == 0) { noWarn = true; continue; }
        if (strcmp(argv[i], "--dump-tokens") == 0) {
            dumpTokens = true;
        } else if (strcmp(argv[i], "--dump-effects") == 0) {
            /* 调试开关：打印每个函数的 Addr/Cont 效应摘要。在这里设环境变量，驱动的其余部分
             * 不必知道这个 flag。 */
            setenv("EXTC_DUMP_EFFECTS", "1", 1);
        } else if (strcmp(argv[i], "--explain-memory") == 0) {
            /* 每个分配点一行：它落在哪个层，以及它所在循环的每一轮是否释放。检查结束后打印，
             * 那时每个层级都已定稿。 */
            setenv("EXTC_EXPLAIN_MEMORY", "1", 1);
        } else if (strcmp(argv[i], "--check-c") == 0) {
            doCheckC = true;
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

    Vec searchDirs;
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
        /* 加载器（以及它驱动的 AST 改写）夹在 parse 与 check 之间，这里单独给它计时。 */
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

    /* 把链接需求记录进产物：生成的 C 需要 `-lz` 却不说出来就是个构建陷阱 —— 手工编译时
     * 失败是一堵未定义引用的墙，离引起它的 extC 声明很远。注释刻意机器可读（`extc-libs:`），
     * 构建系统可以从产物里读回来。
     *
     * 两个来源都算：已加载模块各自的 `<module>.link` 与命令行上的 flag。注释里的名字排序并
     * 去重 —— 它描述一个集合，两次构建以不同顺序要同样的库必须产出相同的字节；**链接行**
     * 保持声明顺序：静态库从左到右解析符号，重排不是编译器驱动的事。没有可说的内容时这行
     * 不发，什么也不链的程序保持字节相同。 */
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


    /* `-fwrapv` 让有符号溢出回绕，并且让这次回绕成为定义好的行为。
     *
     * 没有它，i32 的最大值加 1 就是有符号溢出，即未定义行为：`-O1` 下碰巧回绕，但 C 标准
     * 不承诺这件事，而 `-O2` 的严格溢出假设可以把 `if (x + 1 > x)` 这样的判断整个删掉。
     * extC 承诺不把未定义行为交给程序员，所以生成的代码里不许有。这个 flag 把那一类情况
     * 变成定义好的回绕：最小的一步，与 gcc 的实际行为一致，把 "happens to" 变成
     * "is guaranteed to"。 */
    Vec ccArgv;
    vecInit(&ccArgv, &arena, sizeof(char *));
    *(char **)vecPush(&ccArgv) = (char *)cc;
    *(char **)vecPush(&ccArgv) = (char *)"-std=c11";
    *(char **)vecPush(&ccArgv) = (char *)(optLevel ? optLevel : "-O2");
    *(char **)vecPush(&ccArgv) = (char *)"-fwrapv";
    if (marchNative) *(char **)vecPush(&ccArgv) = (char *)"-march=native";
                       /* 编译器会为程序用到的每个类型派生 `_debug`、`_eq`、`_find` 等辅助
                        * 函数，没人调用的那些否则会留在二进制里；让链接器丢掉没人引用的段，
                        * 语义不变。 */
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
