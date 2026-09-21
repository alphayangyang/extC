#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct tree { struct tree *left, *right; } tree;
static tree *bottomUp(int depth){ tree *t=calloc(1,sizeof(tree));
  if(depth>0){ t->left=bottomUp(depth-1); t->right=bottomUp(depth-1);} return t; }
static int64_t check(tree *t){ if(!t->left) return 1; return 1+check(t->left)+check(t->right); }
static void freeTree(tree *t){ if(!t) return; freeTree(t->left); freeTree(t->right); free(t); }
int main(void){ int64_t total=0;
  for(int d=4;d<=16;d++){ tree *t=bottomUp(d); total+=check(t); freeTree(t); }
  tree *big=bottomUp(18); for(int i=0;i<500;i++) total+=check(big);
  printf("trees = %lld\n",(long long)total); }
