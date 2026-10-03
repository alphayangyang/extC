/* 自足靶子（食言版）：声明写着"我不留你的指针"，它留了 —— 存进静态变量，下次调用再读。
 * 与它签的 `Addr=0 Cont=0` 相反，所以 extpkg verify 必须当场证伪（隔离页上的硬缺页）。 */
#include <stdint.h>

static const char *kept;

int64_t extc_demo_take(const char *p, int64_t n) {
    kept = p;
    int64_t s = 0;
    for (int64_t i = 0; i < n; i++) s += (unsigned char)p[i];
    return s;
}

int64_t extc_demo_read_later(void) { return kept ? (int64_t)(unsigned char)kept[0] : -1; }
