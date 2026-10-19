/* extlib/blas/tests/lp64shim.c —— 一个最小的 LP64 CBLAS：给判据造一条 LP64 路径
 *
 * 本机（以及大多数「只装了 numpy 轮子」的机器）只有 numpy 自带的那份 ILP64 OpenBLAS，
 * 符号是 scipy_cblas_sgemm64_。绑定层有两条 ABI 分支，只测 ILP64 等于有一条从不执行。
 * 这个 shim 用 32 位维度导出标准 LP64 名字（cblas_sgemm 等），让判据两条分支都跑一遍：
 * 它算得慢没关系，判据只看结果对不对。
 *
 * 只实现行主序 + 不转置（绑定层自己只会这样调）。 */
#include <stddef.h>

static void sgemm_naive(int m, int n, int k, float alpha,
                        const float *a, const float *b, float beta, float *c) {
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            float acc = 0.0f;
            for (int t = 0; t < k; t++) acc += a[(size_t)i * k + t] * b[(size_t)t * n + j];
            c[(size_t)i * n + j] = alpha * acc + beta * c[(size_t)i * n + j];
        }
    }
}

static void dgemm_naive(int m, int n, int k, double alpha,
                        const double *a, const double *b, double beta, double *c) {
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            double acc = 0.0;
            for (int t = 0; t < k; t++) acc += a[(size_t)i * k + t] * b[(size_t)t * n + j];
            c[(size_t)i * n + j] = alpha * acc + beta * c[(size_t)i * n + j];
        }
    }
}

void cblas_sgemm(int layout, int ta, int tb, int m, int n, int k, float alpha,
                 const float *a, int lda, const float *b, int ldb, float beta,
                 float *c, int ldc) {
    (void)ta; (void)tb; (void)lda; (void)ldb; (void)ldc; (void)layout;
    sgemm_naive(m, n, k, alpha, a, b, beta, c);
}

void cblas_dgemm(int layout, int ta, int tb, int m, int n, int k, double alpha,
                 const double *a, int lda, const double *b, int ldb, double beta,
                 double *c, int ldc) {
    (void)ta; (void)tb; (void)lda; (void)ldb; (void)ldc; (void)layout;
    dgemm_naive(m, n, k, alpha, a, b, beta, c);
}

void cblas_sgemv(int layout, int tr, int m, int n, float alpha, const float *a,
                 int lda, const float *x, int incx, float beta, float *y, int incy) {
    (void)layout; (void)lda; (void)incx; (void)incy;
    for (int i = 0; i < m; i++) {
        float acc = 0.0f;
        for (int j = 0; j < n; j++) {
            if (tr == 111) acc += a[(size_t)i * n + j] * x[j];
            else acc += a[(size_t)j * m + i] * x[j];
        }
        y[i] = alpha * acc + beta * y[i];
    }
}

void cblas_dgemv(int layout, int tr, int m, int n, double alpha, const double *a,
                 int lda, const double *x, int incx, double beta, double *y, int incy) {
    (void)layout; (void)lda; (void)incx; (void)incy;
    for (int i = 0; i < m; i++) {
        double acc = 0.0;
        for (int j = 0; j < n; j++) {
            if (tr == 111) acc += a[(size_t)i * n + j] * x[j];
            else acc += a[(size_t)j * m + i] * x[j];
        }
        y[i] = alpha * acc + beta * y[i];
    }
}
