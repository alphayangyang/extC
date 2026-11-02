/* tests/extbind/demo.c —— 头文件对应的小实现（编成 libdemo.a 让绑定真的链接得上）。 */
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct demo { int n; };

struct demo *demo_open(const char *name) {
    struct demo *d = malloc(sizeof *d);
    d->n = (int)strlen(name);
    return d;
}

int64_t demo_add(struct demo *h, int64_t a, int64_t b) {
    h->n++;
    return a + b;
}

void demo_close(struct demo *h) { free(h); }

uint64_t demo_count(void) { return 1; }

int64_t demo_alloc(struct demo **out, const char *name) {
    *out = demo_open(name);
    return *out ? 0 : -1;
}

int demo_printf(struct demo *h, const char *fmt, ...) {
    va_list ap;
    (void)h; (void)fmt; (void)ap;
    return 0;
}
