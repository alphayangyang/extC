/* bench/gc/io/gen.c -- regenerate the two reader-benchmark inputs.
 *
 * Why this exists: the two files used to be committed, and they are 36 MB together
 * (24 MB of lines, 12 MB of integers) -- nine tenths of a fresh clone, for data that
 * is generated in a fraction of a second. They are ignored now; this tool is how you
 * get them back.
 *
 * The lines file is reproduced **byte for byte**: it is exactly
 *     line <i> of the input\n     for i in 0..999999
 * which is what the committed file contained (verified with cmp before removing it).
 * So the nextLine/nextToken numbers stay comparable across the change.
 *
 * The integers file is NOT byte-identical: its generator was never committed, and the
 * seed could not be recovered (a search over plausible seeds and streams found no
 * match). This tool reproduces its *shape* -- 3000000 values, uniform in 1..1000,
 * space separated, 20 per line, first line the count -- which is what the reader
 * benchmark measures. Numbers measured on the old file and on this one differ only
 * inside the noise floor, but if you need the exact old file it is still in git
 * history:  git show <commit>:bench/gc/io/in_ints.txt > in_ints.txt
 *
 * Usage:  cc -O2 -o gen gen.c && ./gen [outdir]     (default: .)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t st, inc;

static uint32_t rnd(void) {
    uint64_t old = st;
    st = old * 6364136223846793005ULL + inc;
    uint32_t x = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t r = (uint32_t)(old >> 59u);
    return (x >> r) | (x << ((-r) & 31));
}

static void seed(uint64_t s, uint64_t q) {
    st = 0;
    inc = (q << 1u) | 1u;
    rnd();
    st += s;
    rnd();
}

static uint64_t bounded(uint64_t b) {
    if (!b) return 0;
    uint64_t t = (0 - b) % b, x = rnd();
    while (x < t) x = rnd();
    return x % b;
}

static FILE *open_out(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "gen: cannot write %s\n", path); exit(1); }
    return f;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    const int64_t N = 3000000, LINES = 1000000;

    /* reader benchmark shape 1: n integers, uniform in 1..1000 */
    seed(20260920, 54);
    FILE *f = open_out(dir, "in_ints.txt");
    fprintf(f, "%lld\n", (long long)N);
    for (int64_t i = 0; i < N; i++)
        fprintf(f, "%llu%c", (unsigned long long)(bounded(1000) + 1), (i % 20 == 19) ? '\n' : ' ');
    fprintf(f, "\n");
    fclose(f);

    /* reader benchmark shape 2: 1000000 lines, byte-identical to the old file */
    f = open_out(dir, "in_lines.txt");
    for (int64_t i = 0; i < LINES; i++)
        fprintf(f, "line %lld of the input\n", (long long)i);
    fclose(f);

    fprintf(stderr, "gen: wrote %s/{in_ints.txt,in_lines.txt}\n", dir);
    return 0;
}
