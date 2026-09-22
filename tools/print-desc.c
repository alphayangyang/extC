/* ⭐ 通用打印器原型（描述表方案）—— `PLAN-REGION` 之后的**编译时长优化**第二步
 *
 * 现在 extC 给**每个用到类型**派生一份 `_debug` 代码（我压测程序里 2028 个、占 23% 行 ✗）。
 * 正确形状是：**每类型一份「数据」（描述表）+ 全程序一个「通用打印器」（代码）** ✓
 *   · 描述表是编译期常量（进 .rodata）⇒ 每类型成本从"一份代码"降到"几行数据" ✓
 *   · 字段偏移由 codegen 用 `offsetof(T, f)` 填 —— 编译器自己知道布局 ✓
 *   · **没有运行时反射**：描述表是静态数据，跟 extC 的 P′ 一致 ✓
 *
 * 同一张表以后还能喂给 `extc_eq`（结构化 ==）· `extc_hash`（将来的 map）· 序列化 ✓
 *
 * 这份文件是**原型 + 对拍器**：手搭一份描述表，打印同样的数据，
 * 输出必须跟现编译器 `println(p)` 的结果**逐字节一致** ⇒ 用 `tests/run` 那套 expect 一比就知道 ✓
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct Desc Desc;
typedef enum { D_I32, D_I64, D_BOOL, D_STRUCT, D_SLICE } DKind;

typedef struct { const char *name; size_t off; const Desc *d; } DField;
typedef struct { const char *name; size_t count; } DSlice;

struct Desc {
    DKind       kind;
    const char *name;     /* 打印时用的类型名（跟现在的 `_debug` 一致 ✓）*/
    size_t      size;     /* 元素/字段大小 */
    const Desc *elem;     /* 切片元素 */
    size_t      nfields;
    const DField *fields;
};

/* ---------------- 通用打印器（全程序**一份**）---------------- */
static void extc_print(const void *p, const Desc *d) {
    switch (d->kind) {
    case D_I32:  printf("%d", *(const int32_t *)p); return;
    case D_I64:  printf("%lld", (long long)*(const int64_t *)p); return;
    case D_BOOL: printf("%s", *(const bool *)p ? "true" : "false"); return;
    case D_STRUCT:
        printf("%s { ", d->name);
        for (size_t i = 0; i < d->nfields; i++) {
            if (i) printf(", ");
            printf("%s: ", d->fields[i].name);
            extc_print((const char *)p + d->fields[i].off, d->fields[i].d);
        }
        printf(" }");
        return;
    case D_SLICE: {                       /* 视图：`{ data, len }` ⇒ 打印成 `[a, b]` ✓ */
        const char *data = *(const char *const *)p;
        int64_t len = *(const int64_t *)((const char *)p + sizeof(void *));
        printf("[");
        for (int64_t i = 0; i < len; i++) {
            if (i) printf(", ");
            extc_print(data + i * d->size, d->elem);
        }
        printf("]");
        return;
    }
    }
}

/* ---------------- 对拍：手搭一份 `pair { a: i32, b: i64, xs: slice<i32> }` 的描述 ---------------- */
typedef struct { int32_t a; int64_t b; struct { const int32_t *data; int64_t len; } xs; } pair_t;

static const Desc D_I32D = { D_I32, "i32", sizeof(int32_t), NULL, 0, NULL };
static const Desc D_I64D = { D_I64, "i64", sizeof(int64_t), NULL, 0, NULL };
static const Desc D_SLICE_I32 = { D_SLICE, "slice<i32>", sizeof(int32_t), &D_I32D, 0, NULL };
static const DField PAIR_FIELDS[] = {
    { "a",  offsetof(pair_t, a),  &D_I32D },
    { "b",  offsetof(pair_t, b),  &D_I64D },
    { "xs", offsetof(pair_t, xs), &D_SLICE_I32 },
};
static const Desc D_PAIR = { D_STRUCT, "pair", sizeof(pair_t), NULL,
                             sizeof(PAIR_FIELDS) / sizeof(PAIR_FIELDS[0]), PAIR_FIELDS };

int main(void) {
    int32_t arr[3] = { 1, 2, 3 };
    pair_t  p = { 7, 99, { arr, 3 } };
    extc_print(&p, &D_PAIR);
    printf("\n");
    return 0;
}
