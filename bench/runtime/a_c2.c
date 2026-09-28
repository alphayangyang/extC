#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(void){ long sum=0;
  for(long i=0;i<20000;i++){ unsigned char*b=malloc(16384); memset(b,0,16384); b[0]=3; b[16383]=5; sum+=b[0]+b[16383]; free(b); }
  printf("%ld\n",sum); return 0; }
