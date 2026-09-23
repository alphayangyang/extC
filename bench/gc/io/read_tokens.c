/* bench/gc/io/read_tokens.c —— 按词读，**extC nextToken 那版的 C 对照**
 *
 * 同形状：64KB 块 + 下标式扫。语义与 `stdlib/std/io.extc::nextToken` 一致：
 * 前导空白吃掉 · 词尾那个空白**不消费**（留着给下一次）· EOF 收尾 · 词比 `out` 长 ⇒ 报错。 */
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define BLK 65536
#define CAP 64

static unsigned char buf[BLK];
static int64_t len = 0, pos = 0;

static int64_t fill(void) {
    ssize_t n = read(0, buf, BLK);
    if (n <= 0) return 0;
    len = n;
    pos = 0;
    return n;
}

static int isSpace(int64_t b) {
    return b == 32 || b == 9 || b == 10 || b == 13 || b == 11 || b == 12;
}

static int64_t nextToken(unsigned char *out, int64_t cap) {
    int64_t n = 0;
    int started = 0;
    for (;;) {
        if (pos >= len) { if (fill() == 0) return n; }
        while (pos < len) {
            int64_t b = buf[pos];
            if (!started) { if (isSpace(b)) { pos++; continue; } started = 1; }
            if (isSpace(b)) return n;
            if (n >= cap) return -1;
            out[n++] = (unsigned char)b;
            pos++;
        }
    }
}

int main(void) {
    unsigned char out[CAP];
    int64_t toks = 0, total = 0, k;
    while ((k = nextToken(out, CAP)) > 0) { toks++; total += k; }
    if (k < 0) return 1;
    printf("tokens = %lld bytes = %lld\n", (long long)toks, (long long)total);
    return 0;
}
