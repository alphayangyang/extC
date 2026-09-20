#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define N 3000000
static int64_t sieve(int64_t n, bool *composite) {
    int64_t cnt = 0;
    for (int64_t i = 2; i <= n; i++) {
        if (i < 0 || i > N) __builtin_trap();
        if (!composite[i]) {
            cnt++;
            for (int64_t j = i * i; j <= n; j += i) { if (j < 0 || j > N) __builtin_trap(); composite[j] = true; }
        }
    }
    return cnt;
}
int main(void) { bool composite[N + 1] = {0}; printf("pi = %lld\n", (long long)sieve(N, composite)); return 0; }
