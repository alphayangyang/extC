/* Shape 3: bt -- C reference.
 * Read `maxdepth bigdepth iters`; build perfect binary trees bottom-up,
 * count nodes; then count one big tree `iters` times. Same as bench/heavy/bt.c. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "fastio.h"

typedef struct tree { struct tree *left, *right; } tree;

static tree *bottomUp(int64_t depth) {
    tree *t = (tree *)calloc(1, sizeof(tree));
    if (depth > 0) {
        t->left = bottomUp(depth - 1);
        t->right = bottomUp(depth - 1);
    }
    return t;
}
static int64_t check(tree *t) {
    if (!t->left) return 1;
    return 1 + check(t->left) + check(t->right);
}
static void freeTree(tree *t) {
    if (!t) return;
    freeTree(t->left);
    freeTree(t->right);
    free(t);
}

int main(void) {
    int64_t maxdepth = bm_i64(), bigdepth = bm_i64(), iters = bm_i64();
    int64_t total = 0;
    for (int64_t d = 4; d <= maxdepth; d++) {
        tree *t = bottomUp(d);
        total += check(t);
        freeTree(t);
    }
    tree *big = bottomUp(bigdepth);
    for (int64_t i = 0; i < iters; i++) total += check(big);
    printf("trees=%lld\n", (long long)total);
    return 0;
}
