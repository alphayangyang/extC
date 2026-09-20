#include <stdint.h>
#include <stdio.h>
static void mul(int64_t n, int32_t *a, int32_t *b, int32_t *c) {
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j < n; j++) {
            int32_t s = 0;
            for (int64_t k = 0; k < n; k++) s += a[i*n+k] * b[k*n+j];
            c[i*n+j] = s;
        }
}
int main(void) {
    int32_t a[40000], b[40000], c[40000];
    for (int i = 0; i < 40000; i++) { a[i] = 1; b[i] = 2; }
    mul(200, a, b, c);
    printf("c[0] = %d\n", c[0]);
    return 0;
}
