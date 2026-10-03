/* The time runtime (see time.h for why this is here rather than in extC).
 *
 * Two clocks, because they answer different questions and mixing them up is a classic bug:
 *   - `extc_time_mono_ns`  == CLOCK_MONOTONIC : never goes backwards, immune to wall-clock jumps.
 *                                              The one to measure elapsed time with.
 *   - `extc_time_real_ns`  == CLOCK_REALTIME  : "what time is it", can jump (NTP, DST, admin).
 *
 * A failed `clock_gettime` returns -1 rather than 0: 0 is a legitimate value for "the epoch" on the
 * realtime clock, and a wrong 0 would look like a valid timestamp.
 */
#include "extctime.h"

void timeEmitRuntime(Arena *a, Buf *out) {
    (void)a;
    bufPuts(out,
        "/* ---- time: a monotonic clock, a wall clock and a sleep, all in i64 nanoseconds ---- */\n"
        "#include <time.h>\n"
        "static int64_t extc_time_from(clockid_t which) {\n"
        "    struct timespec ts;\n"
        "    if (clock_gettime(which, &ts) != 0) return -1;\n"
        "    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;\n"
        "}\n"
        "/* Elapsed-time clock: never jumps backwards. */\n"
        "int64_t extc_time_mono_ns(void) { return extc_time_from(CLOCK_MONOTONIC); }\n"
        "/* Wall clock: \"what time is it\", can jump. */\n"
        "int64_t extc_time_real_ns(void) { return extc_time_from(CLOCK_REALTIME); }\n");
    bufPuts(out,
        "/* Sleep at least `ns` nanoseconds. A signal may cut a sleep short, and `nanosleep` reports\n"
        " * what is left in the same struct -- keep going until nothing is left. No `errno` needed:\n"
        " * any other failure leaves no time remaining, so the loop ends either way. */\n"
        "void extc_time_sleep_ns(int64_t ns) {\n"
        "    if (ns <= 0) return;\n"
        "    struct timespec ts;\n"
        "    ts.tv_sec  = (time_t)(ns / 1000000000LL);\n"
        "    ts.tv_nsec = (long)(ns % 1000000000LL);\n"
        "    while (nanosleep(&ts, &ts) != 0 && (ts.tv_sec > 0 || ts.tv_nsec > 0)) { }\n"
        "}\n");
}
