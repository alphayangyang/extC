/**
 * @file reclaim.c
 * @brief Emits the reclaim runtime: splices the bitmap-slab, zero-the-counts and
 *        regional-sweep C text into the generated file.
 *
 * Every line of the text is one entry of `RECLAIM_RT`, concatenated in emission order; the
 * concatenation is the runtime that really runs in the generated file. The text is cut per line
 * for two reasons: one literal above 4095 characters trips `-Woverlength-strings` (adjacent
 * literals are concatenated and count as one, so concatenating does not help), and this runtime
 * is readable generated output, where one line per entry reviews better than one escaped block.
 *
 * The shape and soundness obligations of the text itself are stated in the block comment at the
 * head of the text: they are criteria, not optimization.
 *
 * Emission is tiered: the title line of every section carries a stable label `[rt:name]`, and
 * this compile's tier decides whether the section is emitted at all (no frame-root chain means
 * no chain-pushing code; no counting means no counter arrays). Dropping a section is equivalent
 * to letting the preprocessor drop it, except that the generated file keeps no code this tier
 * can never reach.
 */

#include "reclaim.h"

#include <stdio.h>    /* snprintf: builds the tier macro names */
#include <stdlib.h>   /* getenv: called by the emitted runtime, not by this file */
#include <string.h>   /* strncmp/strstr/strchr/memcmp: tier scan and labels */

/** @brief Whether a section's label is emitted at this compile's tier.
 *
 * A section labelled `[rt:counters]` is emitted only in the counting tier and `[rt:chain]` only
 * once the frame-root chain is wired; every other section is always emitted. Dropping a section
 * leaves the semantics unchanged: that tier never reaches the code, so emitting it would only
 * add pages to read.
 *
 * @param line    one line of the runtime text.
 * @param timeOn  the counting tier is on.
 * @param chainOn the frame-root chain is emitted.
 * @return 1 to emit the section, 0 to drop it.
 */
static int rtSectionOn(const char *line, int timeOn, int chainOn) {
    const char *p = strstr(line, "[rt:");
    if (p == NULL) return 1;
    p += 4;
    const char *e = strchr(p, ']');
    if (e == NULL) return 1;
    size_t n = (size_t)(e - p);
    if (n == 8 && memcmp(p, "counters", 8) == 0) return timeOn;   /* counter arrays exist only in the counting tier */
    if (n == 5 && memcmp(p, "chain", 5) == 0) return chainOn;     /* the frame-root chain has no emitter pushing onto it */
    return 1;
}

/** @brief Match a tier-switch `#if`: one macro name, optionally negated, known at this
 *         compile.
 *
 * Returns 1 when the condition resolves (*val holds its truth value); the emitter then drops the
 * `#if` directive along with the branch it rules out. Conditions that do not resolve
 * (`#ifndef`, `#if X > N`, combinations) return 0 and are emitted verbatim, so the compiler and
 * -D still decide — those knobs switch behavior or run comparisons through -D overrides.
 *
 * @param p         condition text, with the leading `#if` already stripped.
 * @param noCount   value of EXTC_RC_NOCOUNT.
 * @param timeOn    value of EXTC_RC_TIME.
 * @param sweepFree value of EXTC_RC_SWEEP_FREE.
 * @param[out] val  receives the truth value of the condition when it resolves.
 * @return 1 if the condition resolves, 0 if it is left to the preprocessor.
 */
static int rtCfgIf(const char *p, int noCount, int timeOn, int sweepFree, int *val) {
    static const char *const suffixes[3] = { "NOCOUNT", "TIME", "SWEEP_FREE" };
    const int vals[3] = { noCount, timeOn, sweepFree };
    while (*p == ' ') p++;
    int neg = 0;
    if (*p == '!') { neg = 1; p++; while (*p == ' ') p++; }
    for (int k = 0; k < 3; k++) {
        /* The macro name is built from prefix + suffix instead of one whole literal: these are
         * macros the generated file defines, not switches of the compiler itself (a whole literal
         * would be taken for a new switch by the static scan of the switch table). */
        char name[32];
        int n = snprintf(name, sizeof name, "EXTC_RC_%s", suffixes[k]);
        if (n <= 0 || (size_t)n >= sizeof name) continue;
        if (strncmp(p, name, (size_t)n) != 0) continue;
        const char *q = p + n;
        while (*q == ' ' || *q == '\n' || *q == '\r') q++;
        if (*q != '\0') continue;
        *val = neg ? !vals[k] : vals[k];
        return 1;
    }
    return 0;
}
/* The emitted text, one line per entry. Every line carries its own newline, so the
 * concatenation is byte-identical to the source text. */
static const char *const RECLAIM_RT[] = {
/* rt-split:begin
 * Section files under src/back/rt/, in emission order. The split is textual: one array
 * entry per line, and the concatenation is byte-identical to the emitted text.
 * tools/emitted_identity.py is the judge; tools/reclaim_source.py renders this file with
 * the includes expanded, which is what the source-text gates read. Lines between these
 * two markers are this file's own notes, not array entries, and the renderer drops them.
 * rt-split:end */
#include "rt/00-head.inc"
#include "rt/10-counters.inc"
#include "rt/20-types.inc"
#include "rt/21-wmark.inc"
#include "rt/30-helpers.inc"
#include "rt/31-slab.inc"
#include "rt/32-deferred.inc"
#include "rt/33-hugefree.inc"
#include "rt/40-pause.inc"
#include "rt/41-budget.inc"
#include "rt/50-xregion.inc"
#include "rt/51-closing.inc"
#include "rt/52-capacity-release.inc"
#include "rt/53-slot-release.inc"
#include "rt/54-cascade.inc"
#include "rt/55-nocount.inc"
#include "rt/56-shade-cache.inc"
#include "rt/57-xentry.inc"
#include "rt/58-chain.inc"
#include "rt/59-tlsdepth.inc"
#include "rt/60-mark.inc"
#include "rt/61-release.inc"
#include "rt/62-sweep.inc"
#include "rt/63-owe.inc"
};

/**
 * @brief Report a malformed directive in the bundled runtime text and stop.
 *
 * The text is a constant of this file, so a conditional it cannot balance is a generator
 * bug, never something the program under compilation caused. Emitting the text as-is would
 * hand the C compiler a file this generator never checked.
 *
 * @param lineNo 1-based entry index in RECLAIM_RT.
 * @param what   the malformed directive, for the message.
 * @return       never returns.
 */
static void rtTextBug(size_t lineNo, const char *what) {
    fprintf(stderr, "extc: internal error -- reclaim runtime text: %s (line %zu)\n", what, lineNo);
    exit(1);
}

/**
 * @brief Append the reclaim runtime's C text to `out`.
 *
 * Emission is split by this compile's tier: the three switch macros are defined first (the
 * `#if`s in the text and the code after the generated file are chosen by them), then the text is
 * concatenated line by line. Sections this tier does not use (counters, frame-root chain) and
 * `#if`s whose branch is fixed are dropped instead of emitted; dropping equals letting the
 * preprocessor discard them, except that the generated file keeps no code this tier can never
 * reach.
 *
 * @param a          unused: the runtime is pure text.
 * @param out        output buffer receiving the generated code.
 * @param noCount    EXTC_RC_NOCOUNT: counting entry points compile to no-ops.
 * @param timeOn     EXTC_RC_TIME: per-operation counters in the runtime.
 * @param sweepFree  EXTC_RC_SWEEP_FREE: event tier, shouldSweep is always 0.
 * @param localStore EXTC_RC_LOCALSTORE: store-only entry for local slots (meaningful only in the
 *                   no-count tier).
 * @param frameRoots frame-root chain: 1 emits chain push/lookup code; no emitter pushes onto the
 *                   chain, so callers pass 0.
 */
void reclaimEmitRuntime(Arena *a, Buf *out, int noCount, int timeOn, int sweepFree,
                        int localStore, int frameRoots) {
    (void)a;
    /* Define the macros first: which branch of #if EXTC_RC_NOCOUNT / #if EXTC_RC_TIME below is
     * taken is decided by them. EXTC_RC_NOCOUNT is normally 1 — counting entry points compile to
     * no-ops and reclaim relies on regional sweep alone. */
    bufPuts(out, noCount ? "#define EXTC_RC_NOCOUNT 1\n" : "#define EXTC_RC_NOCOUNT 0\n");
    bufPuts(out, timeOn ? "#define EXTC_RC_TIME 1\n" : "#define EXTC_RC_TIME 0\n");
    bufPuts(out, sweepFree ? "#define EXTC_RC_SWEEP_FREE 1\n" : "#define EXTC_RC_SWEEP_FREE 0\n");
    enum { RT_CFG_MAX_DEPTH = 16 };   /* #if nesting the tables below can track */
    int skipSec = 0;        /* drop the current section whole */
    int sup = 0;            /* inside a dropped #if layer: emit the current branch */
    int depth = 0;
    int resolvable[RT_CFG_MAX_DEPTH];     /* this #if layer's tier is known (known => the whole directive is dropped) */
    int skipped[RT_CFG_MAX_DEPTH];        /* known layer: is the current branch the dropped one */
    size_t nlines = sizeof RECLAIM_RT / sizeof RECLAIM_RT[0];
    for (size_t i = 0; i < nlines; i++) {
        const char *line = RECLAIM_RT[i];
        if (strstr(line, "[rt:") != NULL) skipSec = !rtSectionOn(line, timeOn, frameRoots);
        if (skipSec) continue;
        if (strncmp(line, "#if", 3) == 0) {
            if (depth == RT_CFG_MAX_DEPTH) {
                char msg[80];
                snprintf(msg, sizeof msg, "#if nesting deeper than the %d-layer table",
                         RT_CFG_MAX_DEPTH);
                rtTextBug(i + 1, msg);
            }
            /* `#ifdef`/`#ifndef` and conditions rtCfgIf cannot resolve are left to the
             * preprocessor: the layer is tracked (so its #else/#endif still pair up) but
             * emitted verbatim, marked by `known = -1`. */
            int known = -1;
            int v = 0;
            if (strncmp(line, "#ifdef", 6) != 0 && strncmp(line, "#ifndef", 7) != 0)
                known = rtCfgIf(line + 3, noCount, timeOn, sweepFree, &v);
            resolvable[depth] = known;
            skipped[depth] = known == 1 && !v;
            if (skipped[depth]) sup++;
            depth++;
            if (known != 1 && !sup) bufPuts(out, line);
            continue;
        }
        if (strncmp(line, "#else", 5) == 0) {
            if (depth == 0) rtTextBug(i + 1, "unbalanced #else");
            if (resolvable[depth - 1] == 1) {
                sup += skipped[depth - 1] ? -1 : 1;
                skipped[depth - 1] = !skipped[depth - 1];
                continue;
            }
            if (!sup) bufPuts(out, line);
            continue;
        }
        if (strncmp(line, "#endif", 6) == 0) {
            if (depth == 0) rtTextBug(i + 1, "unbalanced #endif");
            depth--;
            if (resolvable[depth] == 1) {
                if (skipped[depth]) sup--;
                continue;
            }
            if (!sup) bufPuts(out, line);
            continue;
        }
        if (!sup) bufPuts(out, line);
    }
    /* A leftover layer means the text ends inside a conditional: the emitted file would
     * silently keep whatever a missing #endif swallowed, so stop instead. */
    if (depth != 0) rtTextBug(nlines, "unterminated #if");
    /* EXTC_RC_LOCALSTORE (compile-time, off by default): store-only entry for local and
     * parameter slots. In the no-count tier, storeRefOwned(slot, v, owner=0) does two ownSlab
     * checks and then does nothing because so == 0, so those two lookups are wasted. This entry
     * skips them; it is meaningful only in the no-count tier. */
    if (localStore && noCount) {
        bufPuts(out,
            "/* Write-only store entry for a local/param slot: in the no-count tier the two\n"
            " * ownSlab probes would find no owner, so skip them (the slot is not an rc slot).\n"
            " * No barrier: shadeOld covers the old edge that the overwrite deletes. */\n"
            "__attribute__((unused)) static inline void extc_rc_storeRefLocal(void **slot, void *v) {\n"
            "    if (!slot) return;   /* skip:slot-null: no slot to update */\n"
            "    *slot = v;\n"
            "}\n");
    }
}
