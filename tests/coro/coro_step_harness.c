/* B1 的差分判据：直接驱动**生成的** step 函数（`#include` 生成物，static 才可见 ✓）
 * 手写状态机（tests/coro/statemachine.extc 那一族的形状）产出同一个序列 ⇒ 逐字节比对 ✓ */
#include <stdio.h>
/* The generated unit has its own `main`; this harness drives the generated step function directly,
 * so the generated entry point is renamed out of the way before the include. */
#define main __extc_generated_main
#include "coro_step_gen.c"
#undef main

int main(void) {
    struct counter$frame f = {0};
    f.n = 3;
    while (counter$step(&f)) printf("%lld ", (long long)f.ret);
    printf("\n");
    return 0;
}
