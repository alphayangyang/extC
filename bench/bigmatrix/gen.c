/* bench/bigmatrix/gen.c -- input generator for the six-language matrix.
 *
 * Every shape reads its data from stdin, and every language reads the *same*
 * file, so the numbers compare like for like.  The generator is the only place
 * that produces randomness; programs must not generate their own.
 *
 * PRNG: pcg32, the same one the existing benches use (bench/heavy/rs.c,
 * bench/stress/stress.c), so the data has the same character as before.
 * Seeds are fixed and written down in SPEC.md; output is plain text.
 *
 * Usage:  ./gen [outdir]        (default: in/)
 *         ./gen small [outdir]  (small sizes, for the brute-force cross-check)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t st, inc;

static uint32_t rnd(void) {
    uint64_t o = st;
    st = o * 6364136223846793005ULL + inc;
    uint32_t x = (uint32_t)(((o >> 18u) ^ o) >> 27u);
    uint32_t r = (uint32_t)(o >> 59u);
    return (x >> r) | (x << ((-r) & 31));
}

static void seed(uint64_t s, uint64_t q) {
    st = 0;
    inc = (q << 1u) | 1u;
    rnd();
    st += s;
    rnd();
}

static uint64_t bounded(uint64_t b) {  /* uniform in [0,b) */
    if (!b) return 0;
    uint64_t t = (0 - b) % b, x = rnd();
    while (x < t) x = rnd();
    return x % b;
}

static FILE *open_out(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.txt", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "gen: cannot write %s\n", path); exit(1); }
    return f;
}

int main(int argc, char **argv) {
    int small = (argc > 1 && strcmp(argv[1], "small") == 0);
    const char *dir = small ? (argc > 2 ? argv[2] : "in-small") : (argc > 1 ? argv[1] : "in");

    /* shape 1: radix -- n u32 values */
    {
        int64_t n = small ? 200000 : 10000000;
        seed(20260924, 11);
        FILE *f = open_out(dir, "radix");
        fprintf(f, "%lld\n", (long long)n);
        for (int64_t i = 0; i < n; i++) fprintf(f, "%u%c", rnd(), (i % 20 == 19) ? '\n' : ' ');
        fprintf(f, "\n");
        fclose(f);
    }

    /* shape 2: cdq -- n triples in [1, 10^6] */
    {
        int64_t n = small ? 2000 : 2000000;
        seed(20260924, 22);
        FILE *f = open_out(dir, "cdq");
        fprintf(f, "%lld\n", (long long)n);
        for (int64_t i = 0; i < n; i++)
            fprintf(f, "%llu %llu %llu\n", (unsigned long long)(bounded(1000000) + 1),
                    (unsigned long long)(bounded(1000000) + 1), (unsigned long long)(bounded(1000000) + 1));
        fclose(f);
    }

    /* shape 3: bt -- parameters only */
    {
        FILE *f = open_out(dir, "bt");
        fprintf(f, "%s\n", small ? "8 10 50" : "16 18 500");
        fclose(f);
    }

    /* shape 4: mandel -- parameters only */
    {
        FILE *f = open_out(dir, "mandel");
        fprintf(f, "%s\n", small ? "400 300 200" : "1600 1200 200");
        fclose(f);
    }

    /* shape 5: rebuild -- rounds and nodes per round */
    {
        FILE *f = open_out(dir, "rebuild");
        fprintf(f, "%s\n", small ? "200 1000" : "20000 1000");
        fclose(f);
    }

    fprintf(stderr, "gen: wrote %s/{radix,cdq,bt,mandel,rebuild}.txt%s\n", dir, small ? " (small)" : "");
    return 0;
}
