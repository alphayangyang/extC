#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
static int64_t sieve(int64_t n, bool *composite) {
    int64_t cnt = 0;
    for (int64_t i = 2; i <= n; i++) {
        if (!composite[i]) {
            cnt++;
            for (int64_t j = i * i; j <= n; j += i) composite[j] = true;
        }
    }
    return cnt;
}
int main(void) { bool composite[3000001] = {0}; printf("pi = %lld\n", (long long)sieve(3000000, composite)); return 0; }
