#include <stdint.h>
#include <stdio.h>
static uint8_t img[1920000];
int main(void){ int64_t w=1600,h=1200,acc=0;
  for(int64_t y=0;y<h;y++) for(int64_t x=0;x<w;x++){
    double cr=(double)x/(double)w*3.5-2.5, ci=(double)y/(double)h*2.0-1.0, zr=0, zi=0; int64_t it=0;
    while(it<200){ double t=zr*zr-zi*zi+cr; zi=2.0*zr*zi+ci; zr=t;
      if(zr*zr+zi*zi>4.0) break; it++; }
    img[y*w+x]=(uint8_t)(it&255); acc+=it; }
  printf("mandel = %lld 首字节 = %d\n",(long long)acc, img[0]); }
