/* tests/cabi/platetable.c —— 第 5 步 ①：**板的门走表**（C-ABI.md §9.12）
 *
 * 与 `heapmod.c`（第 3 步 ④）的分工一样：宿主给表，模块只用表。区别是宿主那张板现在是
 * `std::heap` 的**真板**（4 GiB 保留 + 按需 commit + close 即 munmap），而门就是板的门
 * （`holds` / `copyOut`）—— 模块拿到的每一块内存都从板上过，还回来的指针也要过那道门。
 *
 * `ctx` 在 C 这边是 `void *`；extC 侧把它的**真类型**写出来（`mut ref plate`）—— 指针的指向
 * 类型在**声明处**定，这是 C-ABI 那条线一贯的口径，也是这个模块能回调宿主的原因。
 *
 * `struct plate_api` 是 C 侧手写的，布局与 extC 侧那个 `@frozen struct plate_api` 一致。 */
#include <stdint.h>

struct plate_api {
    uint64_t size;
    void    *ctx;
    uint8_t *(*alloc)(uint64_t n, void *ctx);            /* 宿主的门：从板里要一块 */
    int64_t  (*copyOut)(uint8_t *p, int64_t n, void *ctx); /* 宿主的门：这块在板内吗（顺带记账） */
};

/* **板外的**内存：判据的牙。宿主拿到这个指针时 `holds` 必须为假。 */
static uint8_t outside[8];

/* 模块入口：经表申请、写进去、回调宿主的门验一次，返回写了多少字节。 */
int64_t init(struct plate_api *a) {
    uint8_t *p = a->alloc(16, a->ctx);
    if (!p) return -1;
    for (int i = 0; i < 16; i++) p[i] = (uint8_t)(i + 1);
    if (a->copyOut(p, 16, a->ctx) != 16) return -2;      /* 门要认这块 */
    return 16;
}

/* 把一块**板内**内存交回宿主（宿主自己再验一次）。 */
uint8_t *platePtr(struct plate_api *a) {
    uint8_t *p = a->alloc(8, a->ctx);
    if (p) p[0] = 42;
    return p;
}

/* 交回一块**板外**内存：宿主必须挡住它。 */
uint8_t *badPtr(struct plate_api *a) {
    (void)a;
    outside[0] = 9;
    return outside;
}
