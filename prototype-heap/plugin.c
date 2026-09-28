/* prototype-heap/plugin.c —— 模拟一个 dlopen 进来的 C 库：只看得见宿主递来的那张表。
 * 它按**普通指针**语义工作（无痛接入），把结果留在宿主给它的板里，返回**板内指针**。 */
#include <stdint.h>
#include <string.h>

typedef struct extc_heap_api {
    uint64_t size;
    void    *ctx;
    void   *(*alloc)(void *plate, uint64_t n);
    int32_t (*release)(void *plate, void *q);
    int32_t (*resize)(void *plate, void **q, uint64_t n);
    void   *(*copyIn)(void *plate, const void *src, uint64_t n);
    int32_t (*copyOut)(void *plate, const void *q, uint64_t n, void *dst);
    int64_t (*len)(void *plate, const void *q);
} extc_heap_api;

const char *plugin_abi(void) { return "prototype-1"; }

void *plugin_build(const extc_heap_api *api, uint64_t n) {
    if (!api || api->size < sizeof(extc_heap_api)) return 0;      /* 老模块配新宿主：干净拒绝 */
    uint64_t *a = (uint64_t *)api->alloc(api->ctx, n * sizeof(uint64_t));
    if (!a) return 0;
    for (uint64_t i = 0; i < n; i++) a[i] = (i * i) % 1000003u;    /* 普通指针，普通写 */
    return a;
}

int32_t plugin_sum(const extc_heap_api *api, const uint64_t *a, uint64_t n) {
    (void)api;
    int64_t s = 0;
    for (uint64_t i = 0; i < n; i++) s += (int64_t)a[i];
    return (int32_t)(s % 1000000007);
}
