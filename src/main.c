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
#include "codegen.h"
#include "lexer.h"
#include "parser.h"

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
        "extC compiler (week-0)\n"
        "\n"
        "usage: %s [options] <file.extc>\n"
        "\n"
        "options:\n"
        "  -o <file>       把生成的 C 写到这个文件（默认打到 stdout）\n"
        "  --run           生成 C、用 C 编译器编译，然后运行\n"
        "  --dump-tokens   只做词法分析，打印 token 表\n"
        "  --no-line-map   不生成 `#line` 指令（默认生成）\n"
        "  -h, --help      显示这份帮助\n",
        argv0);
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

    Module m;
    if (!ctx.hasError) parseModule(&ctx, &arena, &toks, &m);

    Buf c;
    bufInit(&c, &arena);
    if (!ctx.hasError) generateC(&ctx, &arena, &m, lineMap, &c);

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
    char *ccArgv[] = { (char *)cc, "-std=c11", "-O1", "-o", (char *)binPath, (char *)cPath, NULL };
    int rc = runCmd(ccArgv);
    if (rc != 0) {
        fprintf(stderr, "extc: C compiler failed (exit %d) on `%s`\n", rc, cPath);
        return 1;
    }

    char *runArgv[] = { (char *)binPath, NULL };
    return runCmd(runArgv);
}
