/* bench/gc/io/read_ints.c —— 读 n 个整数求和，**extC 那版的 C 对照**
 *
 * 形状必须一样才可比：自己管 64KB 块 + 下标式解析（**不是** `getchar_unlocked`、
 * 也不是 `scanf`/`fread` 那一族 —— 那些用的是 libc 的缓冲，量出来的就不是同一件事 ✗）。
 * 旗子跟 extC 生成的那份 C 一样（`-O2 -fwrapv`，见 src/main.c）。
 *
 * 语义与 `stdlib/std/io.extc::nextInt` 逐字符一致：
 *   · 前导空白跳过（词法空白，不是 locale 那套）· 只认 `-`，不收 `+`
 *   · 数字后不是数字就停，那个字节推回（`12,34` 连读得到 12 和 34）
 *   · 数字后的换行 / CRLF 吃掉，别的空白推回
 * ⚠️ 推回用局部变量记住那个字节，**不是把 pos 往回减** —— 减 pos 在"正好补完缓冲"时
 *    会退到缓冲之前（extC 版实测过 `index -1 out of range`）✓ */
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define BLK 65536

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

static int64_t nextInt(void) {
    int64_t v = 0, pushed = 0, b;
    int neg = 0, got = 0, seen = 0, bad = 0;
    for (;;) {
        if (pushed == -2) { pushed = 0; b = -1; }
        else if (pushed > 0) { b = pushed; pushed = 0; }
        else if (pos >= len) { if (fill() == 0) b = -1; else b = buf[pos++]; }
        else b = buf[pos++];

        if (!seen) {
            if (b < 0) break;                       /* EOF：没有数字 ⇒ 0 */
            if (!isSpace(b)) { seen = 1; if (b == 45) neg = 1; else pushed = b; }
            continue;
        }
        if (bad) { if (b < 0) pushed = -2; else pos--; break; }
        if (b < 0) break;                           /* EOF：数字到此为止 */
        if (b >= 48 && b <= 57) { got = 1; v = v * 10 + (b - 48); continue; }
        if (!got) continue;                         /* 前导分隔符吃掉 */
        if (b == 10) break;
        if (b == 13) { if (pos < len && buf[pos] == 10) pos++; break; }
        bad = 1;                                    /* 下一轮把它推回 */
    }
    if (!got) return 0;
    return neg ? -v : v;
}

int main(void) {
    int64_t n = nextInt(), s = 0;
    for (int64_t i = 0; i < n; i++) s += nextInt();
    printf("sum = %lld\n", (long long)s);
    return 0;
}
