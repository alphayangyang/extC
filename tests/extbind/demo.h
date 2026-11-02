/* tests/extbind/demo.h —— 给"声明式绑定生成器"自检用的假库头文件。
 *
 * 形状刻意覆盖边界：不透明句柄（`?ref void`）、`const char *`（`?ref u8`）、`int64_t`、
 * `void` 返回、返回指针（要 `Ret=0`）、以及一个**没法生成**的变参函数（工具要报出来而不是猜）。 */
#ifndef EXTBIND_DEMO_H
#define EXTBIND_DEMO_H
#include <stdint.h>

typedef struct demo demo;

demo    *demo_open(const char *name);
int64_t  demo_add(demo *h, int64_t a, int64_t b);
void     demo_close(demo *h);
uint64_t demo_count(void);
int      demo_printf(demo *h, const char *fmt, ...);   /* 变参：不生成，工具报出来 */

#endif
