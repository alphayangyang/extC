#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
static bool composite[10000001];
int main(void) {
    int64_t n = 10000000, cnt = 0;
    for (int64_t i = 2; i <= n; i++)
        if (!composite[i]) { cnt++; for (int64_t j = i*i; j <= n; j += i) composite[j] = true; }
    printf("pi = %lld\n", (long long)cnt);
    return 0;
}
