#include <stdint.h>
#include <stdio.h>
int main(void) {
    int64_t s = 0;
    for (int64_t i = 0; i < 300000000; i++) s += (i % 7) + (i % 1000003);
    printf("s = %lld\n", (long long)s);
    return 0;
}
