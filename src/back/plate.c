/* plate.c —— `extc_viewOf`：把"一个字节指针 + 长度"变成 `mut slice<u8>`。
 *
 * 一道门的三个理由见 plate.h（视图造不出来、泛型类型没有字面量、`ref void` → `ref T` 不开）。
 * 语义上它就是那个两词的结构体；**没有任何检查** —— 谁给的指针、它指向哪里、长度多大，
 * 由调用它的那层负责（板：`plate::holds` 一次区间检查；C 边界：声明处的签字）。 */
#include "plate.h"

void plateEmitViewOf(Arena *a, Buf *out) {
    (void)a;
    /* `slice_u8` 的字段名与 prelude 里 `struct slice<T> { data, len }` 一致；类型的定义在
     * 结构体那一段（本段发射在原型池之后、函数体之前），所以这里能直接用。 */
    bufPuts(out,
        "\n/* ---- plate (`std::sys::heap`) ----\n"
        " * A view over a pointer and a length. The language cannot build one -- a view's type is a\n"
        " * generic instance, so no struct literal spells it, and `ref void` -> `ref T` is refused on\n"
        " * purpose -- so the privileged layer owns this one three-line door.\n"
        " */\n"
        "static slice_u8 extc_viewOf(uint8_t *p, int64_t n) {\n"
        "    slice_u8 v = { p, n };\n"
        "    return v;\n"
        "}\n\n");
}

void plateEmitMemCopy(Arena *a, Buf *out) {
    (void)a;
    /* 同一个门里的第二件：**整块拷贝**。`memcpy` 的声明会与 `<string.h>` 撞（生成文件 include 了
     * 它），所以它也只能从这条出口走 —— 与 `extc_memEq` 同一个理由（见 src/memfind.c 的文件头）。
     * 放在**板的运行期块**里而不是字节搜索那一块：那块的触发点是 `extern!("extc-mem")` 的声明，
     * 而 `std::string` 的用户全都声明它 ⇒ 加进去会让 21 个产物逐字节变化（golden 当场抓到）；
     * 板的触发点是自己的声明，加什么都不会动到别人。 */
    bufPuts(out,
        "/* Bulk copy (`memcpy` behind a door extC can declare). Returns how many bytes it copied;\n"
        " * `n <= 0` copies nothing. The plate\'s `copyIn`/`copyOut` want a **bulk** copy: a byte loop\n"
        " * would pay one bounds check per byte, which is the thing this door exists to avoid. */\n"
        /* **非 static**：它的 `extern!` 声明会发一份非 static 的原型（`extc_memEq` 同理），
         * 两边不一致 gcc 直接报 "static declaration follows non-static declaration"（实测）。 */
        "int64_t extc_memCopy(uint8_t *dst, uint8_t *src, int64_t n) {\n"
        "    if (n <= 0) return 0;\n"
        "    memcpy((void *)dst, (const void *)src, (size_t)n);\n"
        "    return n;\n"
        "}\n\n");
}
