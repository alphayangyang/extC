/* 内嵌的 extC prelude。
 *
 * 源码在 stdlib/prelude.extc，由 tools/embed.c 在构建时变成
 * build/prelude_data.c（一个字节数组），链接进编译器。
 * 这样编译器仍然是**单个可执行文件**，运行时不依赖任何外部路径。
 */
#ifndef EXTC_PRELUDE_H
#define EXTC_PRELUDE_H

#include <stddef.h>

/* 返回 prelude 的源码文本（不是 NUL 结尾，长度用 *len 给出） */
const char *preludeSource(size_t *len);

/* 它显示在诊断里的文件名 */
#define PRELUDE_PATH "<extc prelude>"

#endif /* EXTC_PRELUDE_H */
