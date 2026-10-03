/* A forward, monotone data-flow analysis that computes how deep the storage is that
 * each binding may point at.
 *
 * Why this exists as its own module: the escape checker needs one number per binding,
 * "the depth of the deepest live reference it can hold", and that number has to be an
 * upper bound. Deriving it while checking, with destructive updates and no join at
 * control-flow merges, cannot give an upper bound: at a merge the later write simply
 * overwrites the earlier one, so the answer depends on the order the branches happen
 * to be visited. See docs/topics/ARENA-SOUNDNESS.md section 9.2 for the measurements
 * that led here.
 *
 * The lattice and the transfer functions are the ones specified in section 9.3, item
 * 3-a:
 *
 *   lattice   the block depths a value can reach, ordered by "is at least as deep",
 *             plus a bottom element that means "no live reference"
 *   join      maximum: a value that may come from either branch can be as deep as the
 *             deeper branch
 *   transfer  a store raises the depth of the destination to that of the source
 *   loops     iterated to a fixed point, which terminates because the transfer
 *             functions are monotone and the lattice has finite height
 *
 * The result is the same for every traversal order, which is the property the current
 * implementation lacks.
 */
#ifndef EXTC_DATAFLOW_H
#define EXTC_DATAFLOW_H

#include "ast.h"

#define DFA_MAX_VARS 256     /* bindings tracked per function */

/* The checker is passed through a pointer; `struct Checker` is the tag of the typedef in
 * `check_internal.h`, so this declaration and the definition agree on the type. The tag
 * has to be introduced at file scope, or it would be a distinct type visible only inside
 * the parameter list. */
struct Checker;

/* Depth of a binding, and the depth of each of its reference-carrying fields. */
typedef struct {
    const char *cname;      /* the C name of the binding, which is unique per function */
    int         depth;      /* depth of the deepest reference the binding can hold */
    int         nfields;
    struct { const char *name; int depth; } fields[4];
} DfVar;

/* The data-flow facts at the program point where the walk stopped. */
typedef struct {
    DfVar  vars[DFA_MAX_VARS];
    int    nvars;
    int    nblocks;         /* number of basic blocks, for diagnostics */
    int    rounds;          /* fixed-point iterations, for diagnostics */
    bool   overflow;        /* the variable table was too small, so the answer is not
                             * usable and the caller must keep its own numbers */
} DfResult;

/* Analyze one function body and fill `out`.
 *
 * Params:
 *   c   - checker, for name resolution and the type table
 *   f   - the function whose body is analyzed
 *   out - filled with the final facts; `overflow` is set when they are incomplete
 *
 * Notes:
 *   - Runs after the function body has been checked, so expression types and resolved
 *     names are available on the tree.
 *   - Reads the AST only; it never writes to it.
 */
void dfAnalyze(struct Checker *c, FuncDef *f, DfResult *out);

/* Depth recorded for a binding, or `-1` when the analysis has no entry for it. */
int dfLookup(const DfResult *r, const char *cname);

/* Depth of the value `e` under the fixed point in `r`, and the number of fields `r`
 * knows for the binding rooted at `e` (0 when it knows none). Used to repair the field
 * table, which is written while the body is checked and therefore holds pre-fixed-point
 * answers.
 *
 * Params:
 *   c    - checker, for name resolution
 *   r    - the analysis result
 *   e    - expression to measure
 *
 * Returns:
 *   The upper bound on the depth of the references `e` can carry. */
int dfValueDepth(struct Checker *c, const DfResult *r, Expr *e);

#endif /* EXTC_DATAFLOW_H */
