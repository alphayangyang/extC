/* Shape 5: rebuild -- C reference (malloc + free per node).
 *
 * Read `rounds k`; each round builds a k-node chain, sums it, then frees all of
 * it.  The allocator must stay opaque to the optimizer -- gcc proves the whole
 * malloc/free pair away otherwise (measured: zero allocator calls in the
 * binary), and then the shape measures nothing. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "fastio.h"

typedef struct node { int64_t v; struct node *next; } node;

static void *(*volatile v_alloc)(size_t) = malloc;
static void (*volatile v_free)(void *) = free;

int main(void) {
    int64_t rounds = bm_i64(), k = bm_i64();
    int64_t acc = 0;
    for (int64_t r = 0; r < rounds; r++) {
        node *h = NULL;
        for (int64_t i = 0; i < k; i++) {
            node *c = (node *)v_alloc(sizeof(node));
            c->v = i;
            c->next = h;
            h = c;
        }
        for (node *p = h; p; p = p->next) acc += p->v;
        while (h) { node *nx = h->next; v_free(h); h = nx; }
    }
    printf("acc=%lld\n", (long long)acc);
    return 0;
}
