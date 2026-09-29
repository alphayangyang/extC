/* 不规范模块 ⑤：**改宿主的表**。宿主把表按 `mut ref` 交给我（模块确实需要它），那我顺手把
 * 里面的槽改掉 —— 之后宿主经那个槽调用，跑到我的代码上。 */
#include <stdint.h>
struct api {
    int64_t (*slots[4])(int64_t);
    void    *ctx;
};
static int64_t hijack(int64_t x) { return x + 1000; }
void rewrite(struct api *a) { a->slots[0] = hijack; }
