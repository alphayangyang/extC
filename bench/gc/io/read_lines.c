/* bench/gc/io/read_lines.c —— 读 n 行，**extC nextLine 那版的 C 对照**
 *
 * 同一个形状：自己管 64KB 块 + 下标式扫（不调 `fgets`/`getline` —— 那些是 libc 的缓冲 ✗）。
 * 语义与 `stdlib/std/io.extc::nextLine` 一致：不含换行 · EOF 收尾（末行没换行也算一行）·
 * 行比 `out` 长 ⇒ 报错退出（**不静默切一半**）。 */
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define BLK 65536
#define CAP 256

static unsigned char buf[BLK];
static int64_t len = 0, pos = 0;

static int64_t fill(void) {
    ssize_t n = read(0, buf, BLK);
    if (n <= 0) return 0;
    len = n;
    pos = 0;
    return n;
}

/* 读到的字节数；0 = EOF；-1 = 行比 cap 长 */
static int64_t nextLine(unsigned char *out, int64_t cap) {
    int64_t n = 0;
    for (;;) {
        if (pos >= len) { if (fill() == 0) return n; }
        while (pos < len) {
            int64_t b = buf[pos++];
            if (b == 10) return n;
            if (n >= cap) return -1;
            out[n++] = (unsigned char)b;
        }
    }
}

int main(void) {
    unsigned char out[CAP];
    int64_t total = 0, lines = 0, k;
    while ((k = nextLine(out, CAP)) > 0) { total += k; lines++; }
    if (k < 0) return 1;
    printf("lines = %lld bytes = %lld\n", (long long)lines, (long long)total);
    return 0;
}
