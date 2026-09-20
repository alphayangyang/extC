#include <stdint.h>
#include <stdio.h>
#define N 200
static void mul(int64_t n, int32_t *a, int32_t *b, int32_t *c) {
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j < n; j++) {
            int32_t s = 0;
            for (int64_t k = 0; k < n; k++) {
                if (i*n+k < 0 || i*n+k > N*N-1 || k*n+j < 0 || k*n+j > N*N-1) __builtin_trap();
                s += a[i*n+k] * b[k*n+j];
            }
            if (i*n+j < 0 || i*n+j > N*N-1) __builtin_trap();
            c[i*n+j] = s;
        }
}
int main(void) {
    int32_t a[N*N], b[N*N], c[N*N];
    for (int i = 0; i < N*N; i++) { a[i] = 1; b[i] = 2; }
    mul(N, a, b, c);
    printf("c[0] = %d\n", c[0]);
    return 0;
}
