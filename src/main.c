/* extC 编译器驱动（week-0）。
 *
 *   extc --dump-tokens foo.extc        只做词法分析
 *   extc foo.extc                      把生成的 C 打到 stdout
 *   extc -o foo.c foo.extc             写文件
 *   extc --run foo.extc                生成 C -> gcc -> 直接跑
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base.h"
#include "check.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "prelude.h"
#include "types.h"

/* ---------------------------------------------------------------- 工具 */

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

static bool writeFile(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

/* 取文件名去掉目录和扩展名：examples/hello.extc -> hello */
static const char *baseName(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    return arenaStrndup(a, base, n);
}

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

static void usage(const char *argv0) {
    fprintf(stderr,
        "extC compiler\n"
        "\n"
        "usage: %s [options] <file.extc>\n"
        "\n"
        "options:\n"
        "  -o <file>       write the generated C to this file (default: stdout)\n"
        "  --run           compile the generated C, then run it\n"
        "  --dump-tokens   lex only; print the token table\n"
        "  --no-line-map   do not emit `#line` directives (default: emit them)\n"
        "  -h, --help      show this help\n",
        argv0);
}

/* ---------------------------------------------------------------- prelude

 * 把编译器自带的 prelude 解析 + 检查一遍，然后合并进主 Module。
 *
 * 它用**自己的 Ctx**（路径显示成 <extc prelude>），这样它出错时错误位置是对的。
 * 而 prelude 是编译器自带的 —— 它出错就说明**编译器坏了**，不是用户的问题，
 * 所以这里报 internal error。
 *
 * 注：prelude 会被检查两遍（这里一遍、合并后一遍）。它很小，代价可忽略；
 * 换来的是「prelude 的错误位置永远正确」。
 */
/* 编译器认 `slice` 的协议：第一个字段是 `data: ref T`，第二个是 `len`。
 * 见 ARRAYS.md「语言认识协议，库提供方法」。 */
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
        ttRegister(tt, &pm);        /* 跟用户文件共用同一张表 */
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

    /* 标记成「保留定义」—— 用户不能重定义它们（重名即报错，而且错误信息会说清原因）*/
    for (size_t i = 0; i < pm.structs.len; i++)
        (*(StructDef **)vecAt(&pm.structs, i))->reserved = true;
    for (size_t i = 0; i < pm.types.len; i++)
        (*(TypeDef **)vecAt(&pm.types, i))->reserved = true;
    for (size_t i = 0; i < pm.funcs.len; i++)
        (*(FuncDef **)vecAt(&pm.funcs, i))->reserved = true;

    /* 契约检查：编译器认 `slice` 的协议（`data` + `len`，因为 `s[i]` 是一条语法）。
     * prelude 必须真的这么写 —— 否则「改个字段名」会变成「生成的 C 编译不过」
     * 这种莫名其妙的错误。**把隐式契约变成显式检查。** */
    if (!viewContractOk(&pm)) {
        fputs("extc: internal error -- the prelude's `slice` does not have the shape "
              "the compiler expects (`data: ref T` then `len`)\n", stderr);
        return false;
    }

    /* 合并进主 Module：prelude 的声明排在用户的前面 */
    for (size_t i = 0; i < pm.structs.len; i++)
        *(StructDef **)vecPush(&m->structs) = *(StructDef **)vecAt(&pm.structs, i);
    for (size_t i = 0; i < pm.types.len; i++)
        *(TypeDef **)vecPush(&m->types) = *(TypeDef **)vecAt(&pm.types, i);
    for (size_t i = 0; i < pm.funcs.len; i++)
        *(FuncDef **)vecPush(&m->funcs) = *(FuncDef **)vecAt(&pm.funcs, i);
    return true;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    const char *path = NULL;
    const char *outPath = NULL;
    bool dumpTokens = false;
    bool doRun = false;
    bool lineMap = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-tokens") == 0) {
            dumpTokens = true;
        } else if (strcmp(argv[i], "--run") == 0) {
            doRun = true;
        } else if (strcmp(argv[i], "--no-line-map") == 0) {
            lineMap = false;
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

    Ctx ctx;
    ctxInit(&ctx, &arena, path, src, srcLen);

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

    /* Module 只初始化一次；prelude 和用户文件都**追加**进同一个它 */
    Module m;
    memset(&m, 0, sizeof m);
    moduleInit(&m, &arena);

    /* **全程一张类型表** —— 类型是驻留的、相等是指针比较，
     * 分成两张表会让同一个 bool 变成两个指针。 */
    TypeTable *tt = ttNew(&arena, NULL);

    /* ① prelude */
    if (!ctx.hasError && !loadPrelude(&arena, tt, &m)) return 1;

    /* ② 用户文件 */
    if (!ctx.hasError) parseModule(&ctx, &arena, &toks, &m);

    /* 类型检查：一遍**独立**的 pass，把结果写回 AST。
     * 之后的代码生成不再做任何类型推理（T1）。 */
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

    if (!doRun) {
        if (outPath) {
            char *text = bufCstr(&c);
            if (!writeFile(outPath, text, c.len)) {
                fprintf(stderr, "extc: cannot write `%s`\n", outPath);
                return 1;
            }
        } else {
            fputs(bufCstr(&c), stdout);
        }
        return 0;
    }

    /* --run：写 build/<base>.c -> gcc -> 跑 */
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
    /* `-fwrapv`：让**有符号溢出绕回**成为确定的语义。
     *
     * 不加的话，`x + 1`（x 是 i32 的最大值）在生成的 C 里是**有符号溢出 = UB**：
     * `-O1` 下碰巧绕回，但 C 标准不保证，而且 `-O2` 的 strict-overflow 假设
     * 会让 `if (x + 1 > x)` 这类判断被优化掉。
     *
     * extC 的承诺是「不给 UB」，所以生成的代码里一个 UB 都不该留 ——
     * 这一行把它从 UB 变成**确定的绕回**。
     *
     * ⚠️ 但「溢出绕回」这个**语言语义本身还没定案**（也可以选 trap）。
     *    在主人拍板前取的是最小干预：跟 gcc 的实际行为一致，
     *    只把「碰巧」变成「保证」。见 DECISIONS 的待定项。
     *
     * `-O2`（2026-09-20 从 `-O1` 提上来）：实测**白赚**，而且不动语义 ——
     *   矩阵乘 106 → 34 ms（**3.1×**）· 取模 273 → 253 ms · 筛素数不变（29 ms）
     *   （`bench/` 里有全套数据：extC 和 C 在**每个优化级别**上都持平，
     *     `-O3` 相比 `-O2` 几乎没有额外收益，`-march=native` 能让矩阵乘再翻倍到 15 ms
     *     —— 但那会牺牲可移植性，先不开。）
     *
     * ⇒ 性能跟 C 同级的最后一公里其实是**旗子**，不是语言 ✓ */
    char *ccArgv[] = { (char *)cc, "-std=c11", "-O2", "-fwrapv",
                       /* 编译器会给用到的每种类型**自动派生** `_debug` / `_eq` /
                        * `_find` …… 程序里没用到的那部分本来会留在二进制里
                        * （实测：hello 的 text 3215 → 1446 字节，euler-sieve 6852 → 3475）。
                        * 让链接器把没人引用的段丢掉 —— 零语义变化，白赚 ✓ */
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-o",
                       (char *)binPath, (char *)cPath, NULL };
    int rc = runCmd(ccArgv);
    if (rc != 0) {
        fprintf(stderr, "extc: C compiler failed (exit %d) on `%s`\n", rc, cPath);
        return 1;
    }

    char *runArgv[] = { (char *)binPath, NULL };
    return runCmd(runArgv);
}
