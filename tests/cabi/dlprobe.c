/* tests/cabi/dlprobe.c —— 被 `std::dl` 加载的那个 C 模块（编成 .so，见 run.sh）。
 *
 * 它故意小到能一眼看完：一个标量函数（第 ③ 块的验收）与一个"经函数指针调用"的函数
 * （第 ④ 块：extC 侧拿到的地址要变成 `fn` 才能调）。 */
#include <stdint.h>

int64_t triple(int64_t x) { return x * 3; }

/* 经代码指针调用：`f(a, b)`。第 ④ 块用它证明"表里的槽真的能调"。 */
int64_t callThrough(int64_t (*f)(int64_t, int64_t), int64_t a, int64_t b) { return f(a, b); }

int64_t addPair(int64_t a, int64_t b) { return a + b; }
