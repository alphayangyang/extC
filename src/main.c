/* extC 编译器驱动（week-0）。 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"
#include "lexer.h"

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

static void usage(const char *argv0) {
    fprintf(stderr,
        "extC compiler (week-0)\n"
        "\n"
        "usage: %s [options] <file.extc>\n"
        "\n"
        "options:\n"
        "  --dump-tokens   只做词法分析，打印 token 表\n"
        "  -h, --help      显示这份帮助\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    bool dumpTokens = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-tokens") == 0) {
            dumpTokens = true;
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

    if (ctx.hasError) {
        Buf diag;
        bufInit(&diag, &arena);
        ctxRenderDiag(&ctx, &diag);
        fputs(bufCstr(&diag), stderr);
        return 1;
    }

    if (dumpTokens) {
        Buf out;
        bufInit(&out, &arena);
        for (size_t i = 0; i < vecLen(&toks); i++) {
            tokenDescribe((Token *)vecAt(&toks, i), &out);
            bufPutc(&out, '\n');
        }
        fputs(bufCstr(&out), stdout);
        return 0;
    }

    fprintf(stderr, "extc: parser 还没接上 —— 先用 --dump-tokens 看词法结果\n");
    return 0;
}
