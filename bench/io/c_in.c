/* C · 输入：手写 fread 缓冲 + 解析（基线之一：通常是最快的）*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static unsigned char BUF[1 << 16];
static size_t BL = 0, BP = 0;
static int FD = 0;

static int refill(void) {
    ssize_t n = read(FD, BUF, sizeof BUF);
    if (n <= 0) { BL = 0; BP = 0; return 0; }
    BL = (size_t)n; BP = 0; return 1;
}
static int64_t nextInt(int *ok) {
    int64_t v = 0; int neg = 0; int any = 0;
    for (;;) {
        if (BP >= BL && !refill()) { *ok = 0; return 0; }
        unsigned char c = BUF[BP];
        if (c == '-' ) { neg = 1; BP++; break; }
        if (c >= '0' && c <= '9') break;
        BP++;
    }
    for (;;) {
        if (BP >= BL && !refill()) { *ok = any; break; }
        unsigned char c = BUF[BP];
        if (c < '0' || c > '9') { BP++; break; }
        v = v * 10 + (c - '0'); any = 1; BP++;
    }
    *ok = any || v != 0;
    return neg ? -v : v;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "用法: in <文件|->\n"); return 2; }
    FD = 0;
    if (argv[1][0] != '-' || argv[1][1]) {
        int fd = open(argv[1], 0);
        if (fd < 0) { perror("open"); return 1; }
        FD = fd;
    }
    int64_t total = 0; int ok = 1;
    while (ok) {
        int o1 = 0, o2 = 0;
        int64_t a = nextInt(&o1);
        if (!o1) break;
        int64_t b = nextInt(&o2);
        if (!o2) break;
        total += a + b;
    }
    printf("%lld\n", (long long)total);
    return 0;
}
