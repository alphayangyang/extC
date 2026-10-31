/* Probe: every slot-store helper must treat a NULL slot as "no slot to update" (no-op).
 *
 * Before the fix each of these crashed: the `if (slot)` guard returned on a real slot, so the
 * fallback only ever ran with slot == NULL and dereferenced it through `extc_rc_storeRef`.
 * One helper per process so a crash names the culprit; run with no argument to run them all.
 *
 * The runtime text is the one emitted by `src/back/reclaim.c`; the suite drops it here as
 * `extc_reclaim_runtime.inc` and compiles this file against it.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct Node { struct Node *next; } Node;
typedef struct Pair { struct Node *a, *b; } Pair;
typedef struct Wide { uint64_t pad; struct Node *p; } Wide;

#include "extc_reclaim_runtime.inc"

typedef void (*fn)(void);
static void t_storeRef(void) { extc_rc_storeRef((void **)0, (void *)0); }
#if EXTC_RC_NOCOUNT
static void t_sameRegion(void) { extc_rc_storeRefSameRegion((void **)0, (void *)0); }
#endif
static void t_ownedReg(void) { extc_rc_storeRefOwnedReg((void **)0, (void *)0, (ExtcRcRegion *)0); }
static void t_owned(void) { extc_rc_storeRefOwned((void **)0, (void *)0, (void *)0); }
static void t_unknown(void) { extc_rc_storeRefUnknown((void **)0, (void *)0); }
static void t_slot(void) { extc_rc_storeRefSlot((void **)0, (void *)0); }
#ifdef PROBE_LOCAL
static void t_local(void) { extc_rc_storeRefLocal((void **)0, (void *)0); }
#endif

struct ent { const char *name; fn f; };
static const struct ent g_ents[] = {
    { "storeRef", t_storeRef },
#if EXTC_RC_NOCOUNT
    { "storeRefSameRegion", t_sameRegion },
#endif
    { "storeRefOwnedReg", t_ownedReg },
    { "storeRefOwned", t_owned },
    { "storeRefUnknown", t_unknown },
    { "storeRefSlot", t_slot },
#ifdef PROBE_LOCAL
    { "storeRefLocal", t_local },
#endif
};

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : NULL;
    for (size_t i = 0; i < sizeof g_ents / sizeof g_ents[0]; i++) {
        if (only && strcmp(only, g_ents[i].name) != 0) continue;
        fflush(stdout);
        g_ents[i].f();
        printf("  ok %s(NULL) returned\n", g_ents[i].name);
    }
    if (only && argc > 1) {
        size_t i;
        for (i = 0; i < sizeof g_ents / sizeof g_ents[0]; i++)
            if (strcmp(only, g_ents[i].name) == 0) break;
        if (i == sizeof g_ents / sizeof g_ents[0]) {
            fprintf(stderr, "no such helper: %s\n", only);
            return 2;
        }
    }
    return 0;
}
