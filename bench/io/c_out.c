/* C · 输出：手写缓冲 + write（基线之一）*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

static char BUF[1 << 16];
static size_t BL = 0;
static int FD = 1;

static void flushOut(void) { if (BL) { ssize_t w = write(FD, BUF, BL); (void)w; BL = 0; } }
static void putChar(char c) { if (BL == sizeof BUF) flushOut(); BUF[BL++] = c; }
static void putI64(int64_t v) {
    if (v == 0) { putChar('0'); return; }
    if (v < 0) { putChar('-'); v = -v; }
    char t[24]; int n = 0;
    while (v > 0) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) putChar(t[--n]);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "用法: out <文件|-> <n>\n"); return 2; }
    if (!(argv[1][0] == '-' && !argv[1][1])) {
        int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("open"); return 1; }
        FD = fd;
    }
    long long n = atoll(argv[2]);
    for (long long i = 0; i < n; i++) { putI64(i); putChar(' '); putI64(i * i); putChar('\n'); }
    flushOut();
    return 0;
}
