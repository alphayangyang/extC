#include <stdio.h>
#include <stdint.h>
typedef struct { int64_t i,n,val; } Gen;
static int next(Gen *g){ if(g->i>=g->n) return 0; g->val=g->i; g->i++; return 1; }
int main(void){ Gen g={0,1000000,0}; int64_t s=0; while(next(&g)) s+=g.val; printf("%lld\n",(long long)s); return 0; }
