/* memfind.c —— `extc_memFind` 与 `extc_memEq`：字节搜索/整块比较的运行期。
 *
 * 为什么要有这一层（量出来的理由，见 `bench/string_vs_cpp/`）：extC 的 `string::find` 原来是
 * 纯 extC 的 Two-Way，逐字节走 `hay[i]` —— 而每一次下标都要过一次边界检查。缩样探针上
 * callgrind 给出的是 **25.5 条指令/字节**，其中 30% 集中在 `if (i < 0 || i >= v.len) extc_trap(...)`
 * 那一行；libstdc++ 那边 `memchr`/`memcmp` 是向量化的、且没有逐字节检查。
 * 算法没错，输在"每个字节一次检查"。
 *
 * `memmem` 是 libc 里的 Two-Way（glibc 走 `__memmem`，短模式先 `memchr` 找首字节、
 * 长模式用周期位移），语义与原来的实现一致：返回**最左**匹配。
 *
 * 两条纪律：
 *   ① 判据不变 —— 偏移、`-1`、空模式 `0`，一条不少；自重叠模式（`aa` 在 `aaaa` 里）必须是 0。
 *   ② 可移植 —— `memmem` 是 GNU/BSD 扩展，非 GNU 平台走下面 `#else` 的 `memchr` + `memcmp`
 *      朴素回退（慢，但语义相同）。
 *
 * `extc_memEq` 是同一扇门的第二个出口：C 的 `memcmp` 签名里 `const void *` 与 `size_t`
 * extC 都写不出来，而**手写声明会与 `<string.h>` 撞车**（生成文件里 include 了它，
 * gcc 报 `conflicting types for 'memcmp'` —— 实测过）。所以它被封在这个整数出口后面。 */

#include "memfind.h"

void memfindEmitRuntime(Arena *a, Buf *out) {
    (void)a;
    /* 第一个字面量：头部注释 + `extc_memFind`。分开写是因为 C99 只保证 4095 字符的字面量
     * （`-Woverlength-strings`），pools.c 那边踩过同一个坑。 */
    bufPuts(out,
        "\n/* ---- byte search (`std::sys::mem`) ----\n"
        " * `memmem` under a door the language can declare: extC has no `const` and no `size_t`,\n"
        " * and the C library's own prototypes use both, so a hand-written declaration of\n"
        " * `memmem`/`memcmp` cannot be made to agree with <string.h>. Only integers and one\n"
        " * pointer cross this boundary.\n"
        " *\n"
        " * The parameter type is `uint8_t *`, not `const uint8_t *`: the prototype the\n"
        " * `extern!(\"extc-mem\")` declaration produces is `uint8_t *`, and a definition that\n"
        " * disagreed with it would be a conflicting-declaration error in the generated C. The\n"
        " * body reads both buffers and writes neither.\n"
        " *\n"
        " * `memmem` returns the LEFTMOST match, which is the promise `string::find` makes; the\n"
        " * fallback below keeps that too (scan with `memchr`, verify with `memcmp`, advance one\n"
        " * byte). */\n"
        "#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)\n"
        "#define EXTC_HAVE_MEMMEM 1\n"
        "/* `memmem` is a GNU/BSD extension: under `-std=c11` (strict ANSI) even glibc's <string.h>\n"
        " * leaves it undeclared. <string.h> has been read long before this block, so the\n"
        " * declaration is written out here instead of turning on `_GNU_SOURCE` -- a feature macro\n"
        " * is read by every other header in the translation unit, and changing how the whole file\n"
        " * is compiled to get one function is the wrong price. The signature is the one glibc,\n"
        " * musl and the BSDs agree on, so a translation unit that defines `_GNU_SOURCE` itself\n"
        " * sees the same declaration twice and stays valid. */\n"
        "extern void *memmem(const void *haystack, size_t haystacklen,\n"
        "                    const void *needle, size_t needlelen);\n"
        "#endif\n"
        "int64_t extc_memFind(uint8_t *hay, int64_t hayLen, uint8_t *needle, int64_t needleLen) {\n"
        "    /* The empty pattern matches at offset 0 -- and this is asked BEFORE either buffer is\n"
        "     * touched, so a zero-length slice with a null payload is a legal argument. */\n"
        "    if (needleLen <= 0) return 0;\n"
        "    if (hayLen < needleLen) return -1;   /* covers the empty haystack as well */\n"
        "#ifdef EXTC_HAVE_MEMMEM\n"
        "    {\n"
        "        const uint8_t *p = (const uint8_t *)memmem((const void *)hay, (size_t)hayLen,\n"
        "                                                  (const void *)needle, (size_t)needleLen);\n"
        "        return p ? (int64_t)(p - hay) : -1;\n"
        "    }\n"
        "#else\n"
        "    {\n"
        "        const uint8_t *end = hay + (hayLen - needleLen);\n"
        "        const uint8_t *p = hay;\n"
        "        for (;;) {\n"
        "            const uint8_t *q = (const uint8_t *)memchr(p, (int)needle[0],\n"
        "                                                       (size_t)(end - p) + 1);\n"
        "            if (!q) return -1;\n"
        "            if (memcmp(q, needle, (size_t)needleLen) == 0) return (int64_t)(q - hay);\n"
        "            p = q + 1;\n"
        "        }\n"
        "    }\n"
        "#endif\n"
        "}\n");
    /* 第二个字面量：`extc_memEq`。 */
    bufPuts(out,
        "/* `memcmp` behind the same door: 1 = the `n` bytes are equal, 0 = they differ. `n <= 0`\n"
        " * is equal by definition, so a caller may pass a zero-length slice's payload. */\n"
        "int64_t extc_memEq(uint8_t *a, uint8_t *b, int64_t n) {\n"
        "    if (n <= 0) return 1;\n"
        "    return (int64_t)(memcmp((const void *)a, (const void *)b, (size_t)n) == 0);\n"
        "}\n");
}
