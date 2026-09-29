/* 不规范模块 ①：**越界写**。宿主给一块 n 字节，我偏写 n+64 字节。
 * 危害能到哪，取决于那块内存是谁的 —— 这正是板存在的理由之一。 */
#include <stdint.h>
int64_t scribble(uint8_t *p, int64_t n) {
    for (int64_t i = 0; i < n + 64; i++) p[i] = 0xEE;
    return n;
}
