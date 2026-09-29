/* tests/cbindgen/demo.h —— 给生成器自检用的**假头文件**（不需要任何真实库）。
 *
 * 它故意把几种形状都放进来：不透明句柄、`const void *`、函数指针、隐式枚举值（`DEMO_TWO` 后面
 * 没有 `= 2`）、`unsigned long`/`uint64_t`/`float`/`double`、`void` 返回。生成器对这些的映射
 * 都有判据盯着（见 run.sh 的 grep 清单）。 */
#ifndef DEMO_H
#define DEMO_H
#include <stdint.h>

typedef enum { DEMO_ONE = 1, DEMO_TWO, DEMO_THREE } demo_kind;

typedef struct demo_opaque demo_opaque;          /* 不透明：extC 侧只能是 `?ref void` */

typedef void (*demo_cb)(void *closure, const char *msg);

demo_opaque *demo_open(const char *name, int flags);
int          demo_write(demo_opaque *h, const void *data, unsigned long n);
void         demo_set_callback(demo_opaque *h, demo_cb fn, void *closure);
double       demo_scale(double x, float y);
void         demo_close(demo_opaque *h);
uint64_t     demo_count(void);

#endif
