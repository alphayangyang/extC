#include <stdint.h>
#include <stdio.h>
static int32_t a[262144], b[262144], c[262144];
int main(void) {
    int64_t n = 512;
    for (int64_t i = 0; i < 262144; i++) { a[i] = 1; b[i] = 2; }
    for (int64_t x = 0; x < n; x++)
        for (int64_t y = 0; y < n; y++) {
            int32_t s = 0;
            for (int64_t k = 0; k < n; k++) s += a[x*n+k] * b[k*n+y];
            c[x*n+y] = s;
        }
    printf("c[0] = %d\n", c[0]);
    return 0;
}
