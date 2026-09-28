#include <stdio.h>
#include <stdint.h>
int main(void){ int64_t s=0; for(int64_t i=0;i<1000000;i++) s+=i; printf("%lld\n",(long long)s); return 0; }
