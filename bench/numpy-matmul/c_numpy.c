/* C + numpy 做矩阵乘法 —— **对照组**：和 extC 那份走完全一样的路（dlopen libpython、
 * 拿 `_ARRAY_API` 表、经表的槽 280 调 `PyArray_MatrixProduct2`），只是把语言换成 C。
 * 用途：分辨"extC 的胶水有没有额外代价"与"numpy/Python 本身的代价"。 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

typedef void *(*fn3)(void *, void *, void *);
typedef void *(*fn1)(void *);
typedef void *(*fn0)(void);

static void *sym(void *h, const char *n) {
    void *p = dlsym(h, n);
    if (!p) { fprintf(stderr, "missing symbol %s\n", n); exit(1); }
    return p;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 256;
    long reps = argc > 2 ? atol(argv[2]) : 20;

    /* `RTLD_GLOBAL` 不是可选项：numpy 的扩展 `.so` 要解析 Python 符号，而 libpython 若是**非全局**
     * 加载的，那些符号就找不到 ⇒ `import numpy` 直接 ImportError ✗（实测）。extC 那份是
     * `extern!("c")` **链接** libpython ⇒ 天然全局 ✓ —— 两条路在这里的区别值得记着。 */
    void *h = dlopen("libpython3.14.so.1.0", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    ((void (*)(void))sym(h, "Py_Initialize"))();

    void *(*import)(const char *) = sym(h, "PyImport_ImportModule");
    void *(*getattr)(void *, const char *) = sym(h, "PyObject_GetAttrString");
    void *(*capsule)(void *, const char *) = sym(h, "PyCapsule_GetPointer");
    void *(*tup_new)(long) = sym(h, "PyTuple_New");
    void *(*long_new)(long) = sym(h, "PyLong_FromLong");
    int (*setitem)(void *, long, void *) = sym(h, "PyTuple_SetItem");
    void *(*call)(void *, void *) = sym(h, "PyObject_CallObject");

    void *mod = import("numpy._core._multiarray_umath");
    void *cap = getattr(mod, "_ARRAY_API");
    void **tbl = capsule(cap, NULL);                       /* ← 表的每一格按编号（读自头文件） */
    fn3 matmul = (fn3)tbl[280];                            /* PyArray_MatrixProduct2 */
    fn0 version = (fn0)tbl[0];

    void *np = import("numpy");
    void *ones = getattr(np, "ones");
    void *dims = tup_new(2);
    setitem(dims, 0, long_new(n));
    setitem(dims, 1, long_new(n));
    void *args = tup_new(1);
    setitem(args, 0, dims);
    void *a = call(ones, args), *b = call(ones, args), *c = call(ones, args);
    if (!a || !b || !c) { fprintf(stderr, "alloc failed\n"); return 1; }

    if (!matmul(a, b, c)) { fprintf(stderr, "matmul failed\n"); return 1; }   /* 预热 */
    double best = 1e30;
    for (int r = 0; r < 3; r++) {
        double t0 = now_ms();
        for (long i = 0; i < reps; i++)
            if (!matmul(a, b, c)) { fprintf(stderr, "matmul failed\n"); return 1; }
        double per = (now_ms() - t0) / (double)reps;
        if (per < best) best = per;
    }
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("n=%ld best_ms=%.6f peak_rss_kb=%ld\n", n, best, (long)ru.ru_maxrss);
    return 0;
}
