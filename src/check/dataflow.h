/**
 * @file dataflow.h
 * @brief Forward, monotone data-flow analysis: computes how deep the storage a binding may
 * point at can be.
 *
 * It is a separate module because the escape check needs one number per binding -- "the
 * deepest live reference it can hold" -- and that number must be an **upper bound**.
 * Deriving it while checking, with destructive updates and no join at control-flow merges,
 * cannot give an upper bound: a store after a merge overwrites one written before it, and
 * the answer depends on the order in which the branches are visited.
 *
 * The lattice and transfer function are:
 *
 *   lattice    the block depth a value can reach, ordered by "at least as deep", plus a
 *              bottom element meaning "no live reference"
 *   join       takes the maximum: a value that may come from either branch can be as deep
 *              as the deeper branch
 *   transfer   one store raises the target's depth to the source's depth
 *   loops      iterate to a fixed point; the transfer function is monotone and the lattice
 *              has finite height, so this always terminates
 *
 * The result is independent of traversal order.
 */
#ifndef EXTC_DATAFLOW_H
#define EXTC_DATAFLOW_H

#include "ast.h"

#define DFA_MAX_VARS 256     /* number of bindings tracked per function */

/* The checker is passed through a pointer; `struct Checker` is the tag of the typedef in
 * `check_internal.h`, so this declaration and the definition agree on the type. The tag
 * has to be introduced at file scope, or it would be a distinct type visible only inside
 * the parameter list. */
struct Checker;

/**
 * @brief The depth of a binding, and the depth of each of its reference-carrying fields.
 */
typedef struct {
    const char *cname;      /* the C name of the binding, which is unique per function */
    int         depth;      /* depth of the deepest reference the binding can hold */
    int         nfields;
    struct { const char *name; int depth; } fields[4];
} DfVar;

/**
 * @brief The data-flow facts at the program point where the walk stopped.
 */
typedef struct {
    DfVar  vars[DFA_MAX_VARS];
    int    nvars;
    int    nblocks;         /* number of basic blocks, for diagnostics */
    int    rounds;          /* fixed-point iterations, for diagnostics */
    bool   overflow;        /* the variable table was too small, so the answer is not
                             * usable and the caller must keep its own numbers */
} DfResult;

/**
 * @brief Analyze one function body and fill `out`.
 *
 * Runs after the body has been checked, so expression types and resolved names are already
 * on the tree; it only reads the AST and never writes it.
 *
 * @param c    Checker used for name lookup and the type table.
 * @param f    Function whose body is analyzed.
 * @param out  Receives the final facts; `overflow` is set when they are incomplete.
 */
void dfAnalyze(struct Checker *c, FuncDef *f, DfResult *out);

/**
 * @brief Return the depth recorded for one binding.
 *
 * @param r      Analysis result.
 * @param cname  The binding's C name.
 * @return       The depth, or `-1` when the result has no entry for it.
 */
int dfLookup(const DfResult *r, const char *cname);

/**
 * @brief Compute the depth of expression `e` under the fixed point in `r`.
 *
 * It also yields the number of fields of the binding rooted at `e` that `r` knows about (0
 * when unknown). This repairs the field table: that table is written while the body is
 * checked, so it holds answers from before the fixed point.
 *
 * @param c  Checker used for name lookup.
 * @param r  Analysis result.
 * @param e  Expression to measure.
 * @return   Upper bound on the depth of the references `e` can carry; 0 when no analysis
 *           result is available.
 */
int dfValueDepth(struct Checker *c, const DfResult *r, Expr *e);

#endif /* EXTC_DATAFLOW_H */
