/* tests/cabi/heapmod.c —— 第 ④ 块的那个 C 模块（编成 .so，见 run.sh）。
 *
 * 它是 `prototype-heap/` 的形状在一个测试里的最小复现：**宿主**给一张表（分配器 + 长度 + 记录），
 * 模块从表里拿一块内存、写进去、回调宿主记账，然后把**板内指针**还给宿主，由宿主做那一次
 * 区间检查。
 *
 * `struct heapapi` 是 C 侧手写的，布局与 extC 侧那个 `@frozen struct heapapi` 一致 —— 一致这件事
 * 由 `@frozen` 的承诺（`C-ABI.md` §9.8）与 tests/frozen 的镜像测试共同担保。 */
#include <stdint.h>

struct heapapi {
    int64_t   size;
    void     *ctx;
    uint8_t  *(*alloc)(int64_t n, void *ctx);
    int64_t   (*len)(void *ctx);
    int64_t   (*log)(uint8_t *p, int64_t n, void *ctx);
};

/* **有牙**的那一个：返回一个**板外**的指针（一个 C 静态变量）。宿主的区间检查必须把它挡住 ——
 * 判据不能只是"合法路径跑得通"，还得证明那条检查真的在检查。 */
static uint8_t outside;
uint8_t *badPtr(struct heapapi *a) {
    (void)a;
    outside = 9;
    return &outside;
}

/* 模块的入口：宿主 dlsym 到它、把它转成 `fn(mut ref heapapi) -> ref u8` 再调。 */
uint8_t *init(struct heapapi *a) {
    uint8_t *p = a->alloc(8, a->ctx);      /* 从**宿主的板**里拿 */
    p[0] = 42;
    p[1] = 7;
    a->log(p, 2, a->ctx);                  /* 回调宿主（经表里签过字的槽）*/
    return p;                              /* 把板内指针还回去，宿主自己验 */
}
