/* The results side: analysis results live here, addressed by a dense node id.
 *
 * Why a separate layer (docs/topics/AST-DECOUPLING.md): the syntax tree must be frozen
 * once the parser has built it, so everything the later phases work out about it has to
 * live beside the tree rather than on it. What the mainstream compilers do, and what
 * this file follows:
 *
 *   - one result slot per owner, not N fields per node (rustc `TypeckResults`,
 *     Clang `AnalysisDeclContext`);
 *   - addressed by a **dense integer id** assigned once per node, so lookup is an array
 *     index and not a pointer hash (rustc `ItemLocalId`: "a dense range of integers
 *     starting at zero ... can be implemented by a `Vec` instead of a tree or hash map");
 *     Cranelift's `PrimaryMap`/`SecondaryMap` is the same idea;
 *   - no pointer may be used as a cache key once nodes can disappear (LLVM's pass-manager
 *     docs warn about exactly that), which is why the id -- not the pointer -- is what a
 *     result is stored against.
 *
 * An id is assigned the first time a node needs results and never moves. `NodeId` is a
 * plain integer on purpose: it can be stored, compared, logged, and later exported
 * without dragging a pointer along.
 *
 * The API has three entry points:
 *
 *   nodeIdOf(node, create)   - the id of a node (create = assign one if new)
 *   resultsOf(node, create)  - the result slot of a node, by pointer
 *   resultsById(id)          - the result slot of a node, by id
 *
 * A lookup that does not create returns NULL for a node with no results yet, and every
 * accessor in `plan.c` treats NULL as "default value" -- the same semantics the old
 * pointer table had.
 *
 * What is deliberately *not* here yet (P2/P3 of the plan): owner-guarded accessors
 * (rustc's `validate_hir_id_for_typeck_results`) and per-owner result scopes. Both need
 * the checker's node kinds, which move with the freeze; the storage shape below is what
 * they will be built on.
 */
#ifndef EXTC_RESULTS_H
#define EXTC_RESULTS_H

#include "ast.h"

/* A dense node index. 0 is never handed out, so it doubles as "no id". */
typedef unsigned NodeId;

/* Which kind of node a result slot belongs to. The first writer stamps it, and every
 * later access checks it -- rustc does the same with `validate_hir_id_for_typeck_results`
 * ("this table is from function A, this node is from function B"). A mismatch is a
 * compiler bug, not a program error, so it aborts under `EXTC_DBG=1` rather than
 * returning a default. */
typedef enum { RKIND_NONE = 0, RKIND_EXPR, RKIND_STMT, RKIND_FUNC, RKIND_OTHER } ResultKind;

/* Everything the plan side knows about one node, in one object -- the "one result slot
 * per owner" shape. A node that is only a call site uses two of these fields; a function
 * uses another set; nothing forces a node to spread its results across the tree. */
typedef struct {
    /* expression sites */
    int  arenaLevel, zoneLevel, arenaArg;
    bool needTemp;
    /* function summary */
    bool usesHome, mayUseArena, makesPool;
    /* statements: this loop's condition allocates (released per round) */
    bool condAllocs;
    /* A desugared `for` carries its step statement as **syntax** (`Stmt.forStep`, written by
     * the parser). The checker drops that step when it retargets the loop onto an iterator,
     * and that decision is a result, not syntax -- so it is recorded here. */
    bool forStepDropped;
    /* coroutines */
    bool isCoro, coroNeedsZone, coroBoxed;
    Type *yieldType, *coroFrameType;
    int  coroProto;
    /* identity */
    const char *instName;
    FuncDef    *tmpl;          /* instance -> template */
    /* which of the fields above were written; an unset field reads as its default */
    unsigned setMask;
    ResultKind kind;           /* which kind of node this slot belongs to (see ResultKind) */
    const FuncDef *owner;      /* the body whose results this slot holds (NULL = module level) */
    bool shared;               /* more than one body legitimately reaches this slot */
    int line;                  /* where the first write happened, for the guard's message */
} NodeResults;

/* The id of `node`, assigning one when `create` is set. Returns 0 for a NULL node. */
NodeId nodeIdOf(const void *node, bool create);

/* ---- owners: the partition of the result space ------------------------------------
 *
 * An id is dense, and a dense id is only useful if its scope is structural: rustc makes an
 * `HirId` a pair of (owner, local index) and guards every access with
 * `validate_hir_id_for_typeck_results`, so a table from one body can never be indexed by a
 * node of another. The same idea, minimally:
 *
 *   - a **pool** is a region of the id space with one owner;
 *   - the owner of a slot is stamped when the slot is first created, from the *current*
 *     owner (see `resultsEnterOwner`);
 *   - `resultsAs` compares the two whenever both are known. A mismatch is a compiler bug
 *     (a result written while checking one body and read while checking another), so it is
 *     reported under `EXTC_DBG_OWNER=1` and ignored otherwise. It is **observation, not a
 *     verdict**: a node inside a generic body is reached by every instance of that body,
 *     and the analysis currently writes per-instance results onto it -- the coupling T4
 *     removes. The count is T4's worklist.
 *
 * **Sharing is a first-class outcome, not a failure.** A node inside a generic body is
 * reached by every instance of that body (the deferred-call fixpoint resolves the same
 * call site once per instance) -- and that is by design: the tree is shared, which is why
 * the answers live beside it. The first time a slot stamped for one body is reached from
 * another, the slot is marked `shared` and the check stops for it. That is deterministic:
 * the same sources produce the same set of shared slots. What the check still catches is
 * the dangerous case -- a slot that only *one* body ever reaches being touched from a
 * second one.
 *
 * `resultsEnterOwner(NULL)` means "not inside a body" -- module-level work. A slot created
 * there has no owner, and no access to it can mismatch.
 */
void resultsEnterOwner(const FuncDef *owner);
void resultsLeaveOwner(void);
const FuncDef *resultsCurrentOwner(void);

/* How many accesses were made from a body other than the one that created the slot.
 * Always counted (one branch per access); the compiler may use it as a signal. */
size_t resultsOwnerMismatches(void);

/* The result slot of `node`: NULL when it has none and `create` is false. */
NodeResults *resultsOf(const void *node, bool create);

/* The result slot of `node` for a writer/reader that says which kind of node it is.
 * `kind` may be RKIND_NONE to skip the check. A slot first used under one kind and then
 * touched under another is a compiler bug: it aborts under `EXTC_DBG=1` and is ignored
 * in a release build (the value is still returned, so behaviour does not change). */
NodeResults *resultsAs(const void *node, bool create, ResultKind kind, int line);

/* The result slot of an id, or NULL for id 0 / an id out of range. */
NodeResults *resultsById(NodeId id);

/* How many ids have been handed out (the size of the dense range). Diagnostics and the
 * per-owner work in P2/P3 use it; nothing in the compiler depends on the number. */
size_t resultsCount(void);

#endif /* EXTC_RESULTS_H */
