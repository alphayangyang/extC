/* The compile plan: the one read port through which code generation sees what the
 * checker decided.
 *
 * Why it exists: an AST node used to carry three different things -- syntax from the
 * parser, analysis results from the checker, and the plan code generation reads. Which
 * field was valid when was then a matter of pass order, and a wrong order produced wrong
 * C silently (an instance materialized by rewriting a call site is the classic case: the
 * callee pointer is only final after the deferred-call fixpoint). The plan now lives in a
 * side table keyed by node pointer (`plan.c`), and everything reaches it through the
 * accessors below. Per field, the comment states who writes it, when it is valid, and
 * what goes wrong when it is stale.
 *
 * Current state: every field family is in the side table except two, and those two
 * accessors read the node because the field is still declared on it: the resolved callee
 * (`Expr.func`, `planCallee`) and the `for` step (`Stmt.forStep`, `planForStep`).
 * `Expr.func` waits for the instance set to become explicit: moving it is not a storage
 * change, because that pointer is produced by the instance-materialization side effect
 * itself. `forStep` is half syntax -- the parser owns one half -- so staying on the node is
 * the right answer, not a step not yet taken.
 *
 * Rules for callers:
 *   - the checker writes a plan field only through its setter; code generation reads it
 *     only through its accessor. A direct read of a moved field in codegen fails the
 *     `tools/check_plan_seam.py` ratchet, and so does declaring one of those fields on an
 *     AST node again.
 *   - a new plan field goes into the side table with a setter and an accessor, never onto
 *     a node.
 */
#ifndef EXTC_PLAN_H
#define EXTC_PLAN_H

#include "ast.h"

/* ---- identity and instances ------------------------------------------------------- */

/* The instance a call site resolved to (NULL for a builtin, an operator, or one the
 * checker never resolved).
 * Writer: the checker's deferred-call fixpoint, which rewrites `Expr.func`.
 * Valid: after that fixpoint has run in the POST phase; before it, the pointer can still
 * be the template or provisional.
 * Stale: generated C calls a C function that was never emitted.
 *
 * Still stored on the AST: rewriting the call site is how the checker materializes an
 * instance, so this pointer has to move together with the explicit instance set. */
FuncDef *planCallee(const Expr *e);

/* The template this instance was materialized from (NULL when `f` is not an instance).
 * Writer: instance materialization, which is the only place an instance is created.
 * Stale: names and type arguments are taken from the wrong function. */
FuncDef *planTemplate(const FuncDef *f);

/* The C name of an instance.
 * Writer: instance materialization. Stale: two instances collide on one C name, or the
 * emitted definition and the call site disagree. */
const char *planInstName(const FuncDef *f);

/* ---- arena and zone plan ----------------------------------------------------------- */

/* The arena level an allocation site (`new`, or a call that may allocate) must use.
 * Writer: the arena channel of the escape/level solver.
 * Stale: the object is placed in an arena that dies before the last reference to it, so
 * generated C has a use-after-free. */
int  planArenaLevel(const Expr *e);
/* The arena argument a call passes down: either the caller's own arena or an explicit
 * level. Writer: the arena channel of the solver. */
int  planArenaArg(const Expr *e);
/* The zone level a pool site must use; meaningful only for a callee that builds a pool.
 * Writer: the zone channel of the solver (which sees `makesPool` for the callee).
 * Stale: the pool is reclaimed too early or too late. */
int  planZoneLevel(const Expr *e);
/* Three per-function facts: does it use the hidden home arena, may it allocate at all,
 * and does it build a pool?
 *
 * Precondition (not enforced here): `usesHome` and `mayUseArena` are final only after the
 * summary pass. `makesPool` additionally waits for the call-graph closure, which is why
 * the level solver replays its last round instead of solving once.
 *
 * `usesHome` answers "does this function take the caller's home arena as a hidden
 * argument", which is the narrow per-node question a call site is settled with; the other
 * question -- does this body reach something that needs a home arena -- is `needsHome`,
 * and the two are deliberately distinct. `mayUseArena` is an optimization: when it is
 * false, this body allocates nothing of its own, so code generation drops its arena array
 * and its release calls. Stale values give the callee an arena that dies too early, or
 * one it holds longer than it must.
 *
 * Known divergence, not yet fixed: `funcTakesHomeZone` in `ast.h` answers "does this call
 * hand the callee a zone?" from the raw `mayUseArena` flag, while the checker decides to
 * promote a call into a zone from a richer predicate that also accepts pool-object
 * methods, extern pool constructors, and associated pool owners. A call promoted to the
 * zone level can therefore reach code generation with no zone argument emitted. Closing
 * it needs one shared predicate instead of two spellings. */
bool planUsesHome(const FuncDef *f);
bool planMayUseArena(const FuncDef *f);
bool planMakesPool(const FuncDef *f);
/* Does this loop's condition or bound allocate? Code generation then wraps the loop in a
 * per-iteration arena so each round's allocation is reclaimed at the end of the round.
 * Writer: the checker. */
bool planCondAllocs(const Stmt *s);
/* The step statement of a desugared `for`, carried on the loop body because `continue`
 * has to reach it. Half syntax, half plan: the parser builds it, and the checker may
 * replace it while lowering, so it stays on the AST.
 * Writer: the parser. */
Stmt *planForStep(const Stmt *s);
/* Does this call site need a temporary for its arguments (evaluation order)?
 * Writer: the checker. */
bool planNeedTemp(const Expr *e);

/* ---- coroutine family -------------------------------------------------------------- */

/* Is this function a coroutine (its value is a frame handle, not a C function)?
 * Writer: the checker. */
bool   planIsCoro(const FuncDef *f);
/* The type of the value a `yield` hands out. Writer: the checker.
 * Stale: the payload is laid out as the wrong type. */
Type  *planYieldType(const FuncDef *f);
/* The C struct type of this coroutine's frame. Writer: the checker.
 * Stale: the frame layout does not match the fields code generation emits. */
Type  *planCoroFrameType(const FuncDef *f);
/* Does this coroutine need a zone of its own? Releasing a task's place goes by identity,
 * not by depth.
 * Writer: the checker. Stale: the task's place is released while the task is still alive,
 * or is never released. */
bool   planCoroNeedsZone(const FuncDef *f);
/* Which protocol method a handle call maps to, as an index resolved by the checker and
 * consumed by code generation while driving the handle. */
int planCoroProto(const FuncDef *f);
/* Was this frame ever coerced into a handle? Such a coroutine always gets a task -- it
 * can be driven even when it is never spawned explicitly -- so it counts as needing a
 * zone. Writer: the checker, at the coercion. */
bool   planCoroBoxed(const FuncDef *f);

/* ---- setters (written by the checker) ----------------------------------------------
 *
 * The read points were closed first; the other half of the coupling is who writes a plan
 * field and when. Every write goes through the setter below, so that writes are the one
 * place that can grow an assertion or change storage. A setter with a NULL node is a
 * no-op: some plan questions are asked about nodes that do not exist. */
void planSetArenaLevel(Expr *e, int v);
void planSetZoneLevel(Expr *e, int v);
void planSetArenaArg(Expr *e, int v);
void planSetNeedTemp(Expr *e, bool v);
/* The function-summary family. `makesPool` is final only after the call graph is closed,
 * which is why the level solver replays its last round; `mayUseArena` decides whether the
 * function receives the hidden home arena. */
void planSetUsesHome(FuncDef *f, bool v);
void planSetMayUseArena(FuncDef *f, bool v);
void planSetMakesPool(FuncDef *f, bool v);
/* Allocation in a loop condition: release it per round. `forStep` stays on the AST -- the
 * parser owns half of it. */
void planSetCondAllocs(Stmt *st, bool v);

/* The coroutine family. */
void planSetIsCoro(FuncDef *f, bool v);
void planSetYieldType(FuncDef *f, Type *v);
void planSetCoroFrameType(FuncDef *f, Type *v);
void planSetCoroNeedsZone(FuncDef *f, bool v);
void planSetCoroProto(FuncDef *f, int v);
/* A handle of this coroutine was made somewhere: it gets a task even when it is never
 * spawned explicitly, which is one of the two reasons a coroutine needs a zone. Written at
 * the coercion, which is the only place that knows. */
void planSetCoroBoxed(FuncDef *f, bool v);
/* The C name of an instance. When migrating a field named `tmpl`, check the declaring
 * struct first: two structs carry a member with that name (`FuncDef` in `ast.h`,
 * `CallCheck` in `check_internal.h`), so a rename or a move by field name alone catches
 * the wrong one. `tools/check_tmpl_owners.py --verify` answers the ownership question
 * per use. */
void planSetInstName(FuncDef *f, const char *name);
/* The instance -> template back pointer, written once per instance by instance
 * materialization. `FuncDef` must not carry this as a field: the same pointer decides
 * names, type arguments and which body is shared, and it has to be written through the
 * plan so that "who writes it, and when" stays one place. An instance is a shallow copy
 * of its template, so the copy no longer brings the pointer along -- this write is what
 * establishes it, and it must stay after the copy. */
void planSetTemplate(FuncDef *f, FuncDef *tmpl);

#endif /* EXTC_PLAN_H */
