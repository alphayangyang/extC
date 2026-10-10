/**
 * @file prelude.h
 * @brief 内置的 extC prelude。
 *
 * 它的源码住在 stdlib/prelude.extc，构建时由 tools/embed.c 变成 build/prelude_data.c 里的
 * 普通字节数组，链进编译器。编译器因此保持单个可执行文件，运行期不需要外部路径。
 */
#ifndef EXTC_PRELUDE_H
#define EXTC_PRELUDE_H

#include <stddef.h>

/**
 * @brief 取 prelude 源文本。
 *
 * 文本**不以 NUL 结尾**：长度经 `*len` 返回，所以它是以字节而不是字符串交出去的。它住在
 * 静态存储里，整个运行期都在，调用方既不释放也不该指望终止符。
 *
 * @param len  接收字节长度。
 * @return     链进编译器的那个字节数组。
 */
const char *preludeSource(size_t *len);

/* 诊断指向 prelude 时显示的文件名。 */
#define PRELUDE_PATH "<extc prelude>"

#endif /* EXTC_PRELUDE_H */
