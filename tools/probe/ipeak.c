/* ipeak.c — sustainable instruction throughput (no memory stalls).
 *
 * Two chain counts (8 and 16 independent accumulators) so the caller can
 * check that wall time scales with the instruction count (issue-bound) and
 * does not change when the chain count changes at fixed total adds.
 *
 *   cc -O2 -fno-tree-vectorize -o /tmp/ipeak tools/probe/ipeak.c
 *   taskset -c 6 /tmp/ipeak 8 400000000        # mode 8:  8 adds/iter
 *   taskset -c 6 /tmp/ipeak 16 400000000       # mode 16: 16 adds/iter
 *   valgrind --tool=callgrind --callgrind-out-file=/tmp/ipeak8.cg \
 *            /tmp/ipeak 8 2000000             # exact Ir for the same binary
 *   callgrind_annotate /tmp/ipeak8.cg | head -3
 *
 * Ir_per_s = Ir(cg) * (N/N_cg) / seconds. This is the ceiling for issue-bound
 * code; GC-ROOFLINE 3.12 uses it as tau_issue = Ir_pop / Ir_per_s.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static long long sink;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

int main(int argc, char **argv) {
    int mode = argc > 1 ? atoi(argv[1]) : 8;
    long long n = argc > 2 ? atoll(argv[2]) : 200000000LL;
    long long a0 = 1, a1 = 2, a2 = 3, a3 = 4, a4 = 5, a5 = 6, a6 = 7, a7 = 8;
    long long b0 = 9, b1 = 10, b2 = 11, b3 = 12, b4 = 13, b5 = 14, b6 = 15, b7 = 16;
    const long long k = 3;
    double t0 = now();
    if (mode == 16) {
        for (long long i = 0; i < n; i++) {
            a0 += k; a1 += k; a2 += k; a3 += k;
            a4 += k; a5 += k; a6 += k; a7 += k;
            b0 += k; b1 += k; b2 += k; b3 += k;
            b4 += k; b5 += k; b6 += k; b7 += k;
            __asm__ __volatile__("" : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3),
                                 "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7));
            __asm__ __volatile__("" : "+r"(b0), "+r"(b1), "+r"(b2), "+r"(b3),
                                 "+r"(b4), "+r"(b5), "+r"(b6), "+r"(b7));
        }
        sink = a0 + b7;
    } else {
        for (long long i = 0; i < n; i++) {
            a0 += k; a1 += k; a2 += k; a3 += k;
            a4 += k; a5 += k; a6 += k; a7 += k;
            __asm__ __volatile__("" : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3),
                                 "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7));
        }
        sink = a0 + a7;
    }
    double t1 = now();
    double s = t1 - t0;
    printf("mode=%d n=%lld sec=%.6f ns_per_iter=%.3f\n", mode, n, s, 1e9 * s / (double)n);
    return 0;
}
