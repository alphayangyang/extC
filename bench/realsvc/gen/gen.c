/* gen/gen.c — the one dataset every realsvc arm reads.
 *
 *   cc -O2 -std=c11 -o build/gen gen/gen.c
 *   ./build/gen data/log.tsv [lines] [seed]
 *
 * The generator is deliberately plain: xorshift64* drawn in a fixed order, one
 * record per line, TAB separated. Same (lines, seed) always produces the same
 * bytes, so a checksum disagreement is never the dataset's fault.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t gState;

static uint64_t next(void) {
    uint64_t s = gState;
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    gState = s;
    return s * UINT64_C(2685821657736338717);
}

static const int kStatus[16] = {200, 200, 200, 200, 200, 200, 200, 200,
                                201, 204, 301, 400, 403, 404, 500, 502};

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "data/log.tsv";
    long lines = argc > 2 ? strtol(argv[2], NULL, 10) : 2000000L;
    uint64_t seed = argc > 3 ? strtoull(argv[3], NULL, 10) : UINT64_C(0x9E3779B97F4A7C15);
    if (lines <= 0) {
        fprintf(stderr, "gen: lines must be positive\n");
        return 2;
    }
    gState = seed;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "gen: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    static char buf[1 << 22];
    setvbuf(f, buf, _IOFBF, sizeof buf);
    for (long i = 0; i < lines; i++) {
        long long ts = 1712000000LL + i / 1000;
        // Activity is heavy-tailed: 90% of the records come from 10% of the uids,
        // so a TTL window leaves a small live set and a lot of dead sessions.
        long long uid = (next() % 10 < 9) ? (long long)(next() % 20000)
                                         : 20000 + (long long)(next() % 180000);
        long long site = (long long)(next() % 16);
        long long pathIdx = (long long)(next() % 24);
        long long status = kStatus[next() % 16];
        long long lat;
        if (next() % 1000 == 0) {
            lat = 100000 + (long long)(next() % 200000);
        } else {
            lat = (long long)(next() % 2000);
        }
        long long bytes = (long long)(next() % 65536);
        if (fprintf(f, "%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\n", ts, uid, site, pathIdx, status, lat,
                    bytes) < 0) {
            fprintf(stderr, "gen: write failed: %s\n", strerror(errno));
            fclose(f);
            return 1;
        }
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "gen: close failed: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "gen: %s lines=%ld seed=%llu\n", path, lines, (unsigned long long)seed);
    return 0;
}
