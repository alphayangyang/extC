/* 自足靶子（诚实版）：只在调用期间读，不留指针 —— 与它签的 `Addr=0 Cont=0` 一致。 */
#include <stdint.h>

int64_t extc_demo_consume(const char *p, int64_t n) {
    int64_t s = 0;
    for (int64_t i = 0; i < n; i++) s += (unsigned char)p[i];
    return s;
}
