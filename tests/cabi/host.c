/* tests/cabi/host.c —— C 那边的宿主：**手写**的声明与结构体。
 *
 * 这是 `@export` 的验收方式：同一份产物既当普通 C 文件链接进来（这里），又单独编成 `.so`
 * 用 `nm -D` 查符号（run.sh），两条都是 C 侧真的在用那些符号。
 *
 * `struct pair2` 在这里是 C 手写的一份等价结构体。它和产物里那个同名结构体布局一致，
 * 靠的是产物那边 `@frozen` 的承诺（tests/frozen 的镜像测试把同一件事钉在编译期：
 * `sizeof` 与每个字段的 `offsetof` 相等）。这是两个翻译单元，C 不检查跨单元的类型，
 * 所以"对得上"是**约定** —— `@frozen` 就是把那份约定写下来的地方。 */
#include <stdint.h>
#include <stdio.h>

struct pair2 {
    int64_t lo;
    int64_t hi;
};

int64_t triple(int64_t x);
int64_t mid(struct pair2 *p, int64_t b);
struct pair2 makePair(int64_t lo, int64_t hi);

int main(void) {
    struct pair2 in = { 10, 18 };
    struct pair2 out = makePair(3, 4);
    printf("triple=%lld mid=%lld pair=(%lld,%lld)\n",
           (long long)triple(14), (long long)mid(&in, 4),
           (long long)out.lo, (long long)out.hi);
    return 0;
}
