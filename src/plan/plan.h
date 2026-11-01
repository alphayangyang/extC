/**
 * @file plan.h
 * @brief The compilation plan: the one read port through which code generation learns what the
 * checker decided.
 *
 * The plan lives in side tables addressed by node, and every read and write goes through the
 * accessors and setters below. Each field's comment states who writes it, when it is valid, and
 * what goes wrong in the emitted output when it is stale.
 *
 * Layering contract and invariants:
 *   - The checker writes plan fields only through setters; code generation reads them only
 *     through accessors. codegen reading a plan field directly is caught by
 *     `tools/check_plan_seam.py`.
 *   - A new plan field goes into the side table (with a setter and an accessor); do not add it to
 *     an AST node.
 *   - A few fields still read AST nodes: the resolved callee (`Expr.func`, `planCallee`) and the
 *     `for` step (`Stmt.forStep`, `planForStep`). The pointer in `Expr.func` is produced by
 *     instance materialisation itself, and moving it into the side table waits for the instance
 *     set to become explicit; `forStep` is half syntax (the parser owns the other half), so
 *     leaving it on the node is a division of labour.
 */
#ifndef EXTC_PLAN_H
#define EXTC_PLAN_H

#include "ast.h"
#include "results.h"

/* ---- identity and instances ------------------------------------------------------- */

/**
 * @brief Get the callee a call site currently resolves to.
 *
 * One call site inside a generic body is shared by every instance of that body; code generation
 * uses `planEnterFunc` / `planEnterInst` to bracket the function body or type instance it is
 * emitting, and this function prefers the resolution recorded for that body. A call site that
 * belongs to no instance returns the one recorded on the node.
 *
 * @param e  The call-site (or function-value) expression.
 * @return   The resolved function definition; NULL for a builtin, an operator, a call that is not
 *           resolved yet, and a call with no recorded resolution.
 */
FuncDef *planCallee(const Expr *e);

/**
 * @brief Tell whether a function is ever called.
 *
 * Code generation emits only the functions that are called, plus the roots: `main`, `@export`
 * entries, and `dyn` table entries. A function with no recorded call reads as unused, which is the
 * safe direction: a missed call site cannot leave an unreferenced function body in the output.
 *
 * @param f  The function definition.
 * @return   true when a call is recorded; false when there is none.
 */
bool planUsed(const FuncDef *f);

/**
 * @brief Record whether a function is ever called.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether it is called.
 */
void planSetUsed(FuncDef *f, bool v);

/**
 * @brief Record that method `m` was used on this receiver **type instance**.
 *
 * The method bodies of a generic struct are shared by all instances, so the template-level
 * `planUsed` has a single bit; but the checker's instance replay and code generation both expand
 * per instance. Without this bit, one `slice<u8> == ...` anywhere would require the element type
 * of `slice<map::bnode<...>>` to define `!=` as well.
 *
 * The key is the type instance's name (the mangled name `ttGeneric` puts in), compared as a string
 * only, without depending on the type layer.
 *
 * @param m         The method/operator template definition; NULL ignores this write.
 * @param recvName  The receiver type instance's name; NULL ignores this write.
 */
void planSetInstUsed (const FuncDef *m, const char *recvName);

/**
 * @brief Was method `m` used on this receiver type instance.
 *
 * @param m         The method/operator template definition.
 * @param recvName  The receiver type instance's name.
 * @return          true when recorded; false when there is no record (conservative: no redundant
 *                  instance body is emitted).
 */
bool planInstUsed (const FuncDef *m, const char *recvName);

/**
 * @brief How many "(method, instance) used" records exist (the instance-use closure reads it to
 * detect growth).
 *
 * @return  The number of records.
 */
size_t planInstUseCount (void);


/**
 * @brief Will there be any `dec` in this function's dynamic execution range (`false` = none, so
 * the `+1` of `initRef` can be dropped)?
 *
 * The default is `false` (unknown => count as usual); only the check side's call-graph fixpoint
 * sets it to `true`.
 *
 * @param f  The function definition.
 * @return   `true` when the `+1` can be dropped.
 */
bool planRcNoDec(const FuncDef *f);

/**
 * @brief Record the conclusion above.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The conclusion.
 */
void planSetRcNoDec(FuncDef *f, bool v);

/**
 * @brief "The heap reference graph this function builds has no back edges, and references live
 * only in counted rc slots".
 *
 * This is the static precondition of the criterion: when it holds, the event band (counting) is
 * complete and needs no sweep. It answers true only when criteria (i)(ii)(iii) (acyclic) and (iv)
 * (references land only in counted heap slots) both hold; the default is `false` (not computed /
 * does not hold => keep sweeping).
 *
 * @param f  The function definition.
 * @return   `true` when it holds; `false` when there is no record.
 */
bool planFnAcyclic(FuncDef *f);

/**
 * @brief Record that "the heap reference graph this function builds has no back edges"
 * (criteria (i)(ii)(iii)).
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The conclusion.
 */
void planSetFnAcyclic(FuncDef *f, bool v);

/**
 * @brief "References in this function live only in counted rc slots" (criterion (iv)).
 *
 * Separate from `planSetFnAcyclic` because the closures over the call graph differ ((i') requires
 * the callees to be acyclic as a whole, (iv) looks only at the callees' (iv)).
 *
 * @param f  The function definition.
 * @return   `true` when it holds; `false` when there is no record.
 */
bool planFnRefsCounted(FuncDef *f);

/**
 * @brief Record that "references in this function live only in counted rc slots" (criterion
 * (iv)).
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The conclusion.
 */
void planSetFnRefsCounted(FuncDef *f, bool v);

/**
 * @brief "No live reference is dropped in this function": criteria (a) each ref field is written at
 * most once per object, (b) ref-typed locals and parameters are not reassigned, (c) no
 * reference-dropping operation (container delete/replace, `@overwrite` reusing the same cell,
 * free-class builtins, and callees that transitively violate (a)/(b)/(c)).
 *
 * The conjunction holds for the whole unit (`unitReclaimIdle` in codegen.c) => reclaim has nothing
 * to do => emit no rc runtime. The default is `false` (not computed / does not hold => emit as
 * usual), which is the safe direction.
 *
 * @param f  The function definition.
 * @return   `true` when it holds; `false` when there is no record.
 */
bool planFnReclaimIdle(FuncDef *f);

/**
 * @brief Record that "no live reference is dropped in this function" (criteria (a)(b)(c), as
 * above).
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The conclusion.
 */
void planSetFnReclaimIdle(FuncDef *f, bool v);

/**
 * @brief The "may form a cycle" group number of an allocation site; 0 means it belongs to no
 * cross-site possible cycle.
 *
 * Written by `applyCycleMerges` in check_escape.c (the site union-find) and read by the shard
 * selection in codegen.c.
 *
 * @param e  The allocation-site expression.
 * @return   The group number (from 1); 0 when there is no record or it belongs to no cross-site
 *           cycle.
 */
int planSiteCycleGroup(const Expr *e);

/**
 * @brief Record an allocation site's "may form a cycle" group number (as above).
 *
 * @param e  The allocation-site expression; NULL ignores this write.
 * @param g  The group number (from 1).
 */
void planSetSiteCycleGroup(Expr *e, int g);

/**
 * @brief Does the unit this function belongs to have a **cross-site** possible cycle (when it does,
 * that unit refuses sharding)?
 *
 * Written by `applyCycleMerges` in check_escape.c and read by `scanUnitReclaim` in codegen.c.
 *
 * @param f  The function definition.
 * @return   `true` when a cross-site possible cycle exists; `false` when there is no record.
 */
bool planFnCrossSiteCycle(const FuncDef *f);

/**
 * @brief Record that "the unit this function belongs to has a cross-site possible cycle" (as
 * above).
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The conclusion.
 */
void planSetFnCrossSiteCycle(FuncDef *f, bool v);

/**
 * @brief The classification of "write into a reference slot" at an assignment site (`SR_*`; see
 * check_stmt.c).
 *
 * The checker records it while the scope is still alive, and `computeFnAcyclic` in check_top.c
 * reads it after the body has been checked.
 *
 * @param s  The assignment statement.
 * @return   The classification; `0` when there is no record.
 */
int anStoreRefKind(const Stmt *s);

/**
 * @brief Record the reference-slot store classification of an assignment site.
 *
 * @param s  The assignment statement; NULL ignores this write.
 * @param v  The `SR_*` classification.
 */
void anSetStoreRefKind(Stmt *s, int v);

/**
 * @brief The reason string for this function being rejected (`reason=` of `EXTC_DBG_ACYCLIC=1`).
 *
 * @param f  The function definition.
 * @return   The reason string; NULL when it was judged true or not computed yet.
 */
const char *anAcyclicWhy(const FuncDef *f);

/**
 * @brief Record the reason string for this function being rejected.
 *
 * @param f    The function definition; NULL ignores this write.
 * @param why  The reason string (a constant string in the arena).
 */
void anSetAcyclicWhy(FuncDef *f, const char *why);

/**
 * @brief Get the C name used when emitting a binding or a variable statement.
 *
 * Parameters are not here: they are stored by value and carry their own field, so this function
 * does not accept a parameter node.
 *
 * @param node  A binding expression or variable declaration statement node.
 * @return      The recorded C name; NULL when there is no record or the record is the empty
 *              string, in which case the caller should fall back to the declaration's own name.
 */
const char *planCName(const void *node);

/**
 * @brief Record the C name used when emitting a binding or a variable statement.
 *
 * @param node  A binding expression or variable declaration statement node; NULL ignores this
 *              write.
 * @param name  The C name.
 * @param kind  The node's kind (`RKIND_EXPR` or `RKIND_STMT`), used for the result slot's type
 *              check.
 */
void planSetCName(void *node, const char *name, ResultKind kind);

/* ---- struct-definition facts -------------------------------------------------------
 *
 * `builtinHolder` / `coroOf` are plan facts because code generation reads them; the two `anXxx`
 * fields are the checker's own working state, with no later stage reading them. They share one
 * slot but have different readers (the analysis-side list is `planAnalysisFields`). */

/**
 * @brief Tell whether this struct definition is the synthetic placeholder struct of a builtin type.
 *
 * Such a placeholder has no fields and no layout, so no struct is declared for it when emitting C;
 * its methods are emitted as ordinary functions as usual.
 *
 * @param sd  The struct definition.
 * @return    true for a builtin placeholder struct; false otherwise.
 */
bool     planBuiltinHolder(const StructDef *sd);

/**
 * @brief Record whether a struct definition is the synthetic placeholder struct of a builtin type.
 *
 * @param sd  The struct definition; NULL ignores this write.
 * @param v   Whether it is a builtin placeholder struct.
 */
void     planSetBuiltinHolder(StructDef *sd, bool v);

/**
 * @brief Get the coroutine function a coroutine frame struct belongs to.
 *
 * The frame struct is synthesised by the checker for each coroutine; when emitting C, the function
 * name recorded here joins the frame type to the `$step` / `$next` functions.
 *
 * @param sd  The struct definition (a synthesised frame struct).
 * @return    The owning coroutine function; NULL when this struct is not a coroutine frame.
 */
FuncDef *planCoroOf(const StructDef *sd);

/**
 * @brief Record the coroutine function a coroutine frame struct belongs to.
 *
 * @param sd  The synthesised frame struct; NULL ignores this write.
 * @param f   The owning coroutine function.
 */
void     planSetCoroOf(StructDef *sd, FuncDef *f);

/**
 * @brief Get the user-written signature recorded on a lambda's environment struct.
 *
 * The environment struct is compiler-synthesised; diagnostics show this string, so the user sees
 * the `fn(i64) -> i64` they wrote rather than the generated type name.
 *
 * @param sd  The struct definition (the lambda's environment struct).
 * @return    The recorded signature string; NULL when it is not a lambda environment struct or
 *            there is no record.
 */
const char *anLamSig(const StructDef *sd);

/**
 * @brief Record the user-written signature on a lambda's environment struct.
 *
 * @param sd   The lambda's environment struct; NULL ignores this write.
 * @param sig  The signature string the user wrote.
 */
void        anSetLamSig(StructDef *sd, const char *sig);

/**
 * @brief Tell whether this struct has a method that creates a pool.
 *
 * This is the struct-level summary: when one method creates a pool, its associated functions (the
 * constructor family) may create pools too, and pool hierarchy and hidden zone-parameter decisions
 * have to take this layer into account.
 *
 * @param sd  The struct definition.
 * @return    true when a method creates a pool; false when there is no record.
 */
bool        anMakesPoolAny(const StructDef *sd);

/**
 * @brief Record whether this struct has a method that creates a pool.
 *
 * @param sd  The struct definition; NULL ignores this write.
 * @param v   Whether a method creates a pool.
 */
void        anSetMakesPoolAny(StructDef *sd, bool v);

/* ---- the effect closure's state ----------------------------------------------------
 *
 * `effState` is the closure's working state (`EFF_NONE` = not computed, `EFF_DONE` = finished,
 * `EFF_IN_PROGRESS` = being computed); `effComplete` / `effUnknown` are the results ("is this
 * summary trustworthy"). Code generation reads none of the three; they belong to the analysis
 * side. */

/**
 * @brief Get the effect closure's computation state.
 *
 * The value is `EFF_NONE` (not computed yet), `EFF_DONE` (computed; see `anEffComplete` for
 * whether it is trustworthy), or `EFF_IN_PROGRESS` (being computed -- meeting it again during
 * recursion is a cycle, and the summary is treated as incomplete). Only the effect closure itself
 * reads and writes this value.
 *
 * @param f  The function definition.
 * @return   The state value; `EFF_NONE` when there is no record.
 */
int  anEffState(const FuncDef *f);

/**
 * @brief Record the effect closure's computation state.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The state value: `EFF_NONE`, `EFF_DONE`, or `EFF_IN_PROGRESS`.
 */
void anSetEffState(FuncDef *f, int v);

/**
 * @brief Tell whether a function's effect summary is trustworthy.
 *
 * When the closure merges its callees' summaries, a single callee that cannot be finished (it is
 * on a cycle, or calls something that was never resolved) sets this to false; call sites then take
 * the most conservative check.
 *
 * @param f  The function definition.
 * @return   true when the summary is trustworthy; false when there is no record.
 */
bool anEffComplete(const FuncDef *f);

/**
 * @brief Record whether a function's effect summary is trustworthy.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether the summary is trustworthy.
 */
void anSetEffComplete(FuncDef *f, bool v);

/**
 * @brief Tell whether a function's effect summary contains an unresolved call.
 *
 * When it does, the summary never completes and call sites are treated at the most conservative
 * level.
 *
 * @param f  The function definition.
 * @return   true when an unresolved call exists; false when there is no record.
 */
bool anEffUnknown(const FuncDef *f);

/**
 * @brief Record whether a function's effect summary contains an unresolved call.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether an unresolved call exists.
 */
void anSetEffUnknown(FuncDef *f, bool v);

/* ---- effect summary ----------------------------------------------------------------
 *
 * The masks after the transitive closure: one call to some function may store a parameter's
 * address/contents into the caller's frame (`Addr`/`Cont`), into this function's own home arena
 * (`home…`), or elsewhere (`otherMask`). Only the checker reads them. The closure's own state and
 * the call graph it walks (`callees`) are a different matter, belonging to pass state. */

/**
 * @brief Get the parameter bit mask for "may store a parameter's address into the caller's frame".
 *
 * Bit i means the address of parameter i (or of a field reachable through it) may be stored into
 * the caller's frame, so the storage that argument points at must outlive the destination. Only
 * the checker reads it.
 *
 * @param f  The function definition.
 * @return   The bit mask; 0 when there is no record.
 */
uint64_t anAddrMask(const FuncDef *f);

/**
 * @brief Record the parameter bit mask for "may store a parameter's address into the caller's
 * frame".
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The bit mask.
 */
void anSetAddrMask(FuncDef *f, uint64_t v);

/**
 * @brief Get the parameter bit mask for "may store a pointer read out of a parameter into the
 * caller's frame".
 *
 * Bit i means a pointer read out of parameter i may be stored into the caller's frame, so the
 * storage it points at must outlive the destination. Only the checker reads it.
 *
 * @param f  The function definition.
 * @return   The bit mask; 0 when there is no record.
 */
uint64_t anContMask(const FuncDef *f);

/**
 * @brief Record the parameter bit mask for "may store a pointer read out of a parameter into the
 * caller's frame".
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The bit mask.
 */
void anSetContMask(FuncDef *f, uint64_t v);

/**
 * @brief Get the bit mask for "flows that cannot be attributed to a parameter".
 *
 * Anything unclear is recorded here; a call site that reads a non-zero value takes the strictest
 * level. Only the checker reads it.
 *
 * @param f  The function definition.
 * @return   The bit mask; 0 when there is no record.
 */
uint64_t anOtherMask(const FuncDef *f);

/**
 * @brief Record the bit mask for "flows that cannot be attributed to a parameter".
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The bit mask.
 */
void anSetOtherMask(FuncDef *f, uint64_t v);

/**
 * @brief Get the parameter bit mask for "may store a parameter's address into this function's own
 * home arena".
 *
 * Bit i means the address of parameter i (or of a field reachable through it) may be stored into
 * this function's own home arena. Only the checker reads it.
 *
 * @param f  The function definition.
 * @return   The bit mask; 0 when there is no record.
 */
uint64_t anHomeAddrMask(const FuncDef *f);

/**
 * @brief Record the parameter bit mask for "may store a parameter's address into this function's
 * own home arena".
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The bit mask.
 */
void anSetHomeAddrMask(FuncDef *f, uint64_t v);

/**
 * @brief Get the parameter bit mask for "may store a pointer read out of a parameter into this
 * function's own home arena".
 *
 * Bit i means a pointer read out of parameter i may be stored into this function's own home arena.
 * Only the checker reads it.
 *
 * @param f  The function definition.
 * @return   The bit mask; 0 when there is no record.
 */
uint64_t anHomeContMask(const FuncDef *f);

/**
 * @brief Record the parameter bit mask for "may store a pointer read out of a parameter into this
 * function's own home arena".
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The bit mask.
 */
void anSetHomeContMask(FuncDef *f, uint64_t v);

/**
 * @brief Tell whether a function stores the address of one of its frame's locals somewhere.
 *
 * Such a call is one the call site must reject: a local's storage does not outlive this frame,
 * while the stored address does.
 *
 * @param f  The function definition.
 * @return   true when it stores a frame-local address; false when there is no record.
 */
bool     anAddrFromLocal(const FuncDef *f);

/**
 * @brief Record whether a function stores the address of one of its frame's locals somewhere.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether it stores a frame-local address.
 */
void anSetAddrFromLocal(FuncDef *f, bool v);

/**
 * @brief Get the number code generation's prelude assigns to this coroutine.
 *
 * The `kind` field of the handle and the scheduling domain's task table both use this number to
 * dispatch to the matching `$step` / `$next`.
 *
 * @param f  The coroutine function definition.
 * @return   The number; 0 when there is no record.
 */
int      planCoroKind(const FuncDef *f);

/**
 * @brief Record the number code generation's prelude assigns to this coroutine.
 *
 * @param f  The coroutine function definition; NULL ignores this write.
 * @param v  The number.
 */
void planSetCoroKind(FuncDef *f, int v);

/**
 * @brief Tell whether this function's body is named from outside by a `dyn` vtable thunk.
 *
 * Such a reference is invisible to the trimming rule "emit only what some name mentions", so it
 * has to be recorded here and the function body has to be kept.
 *
 * @param f  The function definition.
 * @return   true when a `dyn` vtable names it; false when there is no record.
 */
bool     planDynTable(const FuncDef *f);

/**
 * @brief Record whether this function's body is named from outside by a `dyn` vtable thunk.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether a `dyn` vtable names it.
 */
void planSetDynTable(FuncDef *f, bool v);

/**
 * @brief Get the number of `@overwrite` reuse sites in this function body.
 *
 * When emitting C, the count decides whether hidden storage has to be prepared for the reuse sites
 * and passed along. Only the checker writes it.
 *
 * @param f  The function definition.
 * @return   The number of sites; 0 when there is no record.
 */
int      planOwSites(const FuncDef *f);

/**
 * @brief Record the number of `@overwrite` reuse sites in this function body.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The number of sites.
 */
void planSetOwSites(FuncDef *f, int v);

/**
 * @brief Tell whether the storage of `@overwrite` reuse sites lives in this function's own frame.
 *
 * True means each call has its own storage (for `main`, or for a function that recurses into
 * itself); false means the storage is held by the call site's frame and passed in as an opaque
 * hidden parameter.
 *
 * @param f  The function definition.
 * @return   true when the storage lives in this function's frame; false when there is no record.
 */
bool     planOwLocal(const FuncDef *f);

/**
 * @brief Record whether the storage of `@overwrite` reuse sites lives in this function's own frame.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether the storage lives in this function's frame.
 */
void planSetOwLocal(FuncDef *f, bool v);

/**
 * @brief Get a coroutine declaration's return type (the one not yet substituted with type
 * arguments).
 *
 * Materialising a generic instance uses it to substitute the type arguments into the instance's
 * real return type.
 *
 * @param f  The coroutine function definition (the template).
 * @return   The declared return type; NULL when it is not a coroutine or there is no record.
 */
Type    *planCoroRetProto(const FuncDef *f);

/**
 * @brief Record a coroutine declaration's return type (the one not yet substituted with type
 * arguments).
 *
 * @param f  The coroutine function definition; NULL ignores this write.
 * @param t  The declared return type; NULL when it is not a coroutine.
 */
void     planSetCoroRetProto(FuncDef *f, Type *t);

/**
 * @brief Tell whether this expression needs one dereference layer when emitted.
 *
 * The value's type is `ref T` while its position needs a `T`, so the generated C writes `*(...)`.
 *
 * @param e  The expression.
 * @return   true when a dereference is needed; false when there is no record.
 */
bool     planDeref(const Expr *e);

/**
 * @brief Record whether this expression needs one dereference layer when emitted.
 *
 * @param e  The expression; NULL ignores this write.
 * @param v  Whether a dereference is needed.
 */
void planSetDeref(Expr *e, bool v);

/**
 * @brief Tell whether this coroutine call has to be boxed into a handle for storage.
 *
 * True means the coroutine's value is stored somewhere that outlives the current C scope: the
 * generated C puts the frame into the task's own place and makes the value a `{frame, kind, task}`
 * handle.
 *
 * @param e  The coroutine call expression.
 * @return   true when boxing is needed; false when there is no record.
 */
bool     planBoxedCoro(const Expr *e);

/**
 * @brief Record whether this coroutine call has to be boxed into a handle for storage.
 *
 * @param e  The coroutine call expression; NULL ignores this write.
 * @param v  Whether boxing is needed.
 */
void planSetBoxedCoro(Expr *e, bool v);

/**
 * @brief Tell whether this function is started as a task by `ext`.
 *
 * When emitting C, only such a coroutine gets the adapter the scheduling domain needs, plus the
 * forward declarations of `$step` / `$next`.
 *
 * @param f  The function definition.
 * @return   true when it is an `ext` target; false when there is no record.
 */
bool     planIsExtTarget(const FuncDef *f);

/**
 * @brief Record whether this function is started as a task by `ext`.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether it is an `ext` target.
 */
void planSetIsExtTarget(FuncDef *f, bool v);

/**
 * @brief Tell whether this function is used as a worker by `parallel::run`.
 *
 * The language layer has no function pointers, so when emitting C only such a function gets a
 * static trampoline whose address is taken.
 *
 * @param f  The function definition.
 * @return   true when it is used as a worker; false when there is no record.
 */
bool     planIsParWorker(const FuncDef *f);

/**
 * @brief Record whether this function is used as a worker by `parallel::run`.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether it is used as a worker.
 */
void planSetIsParWorker(FuncDef *f, bool v);

/**
 * @brief Tell whether `new` inside this function has to use a thread-local arena.
 *
 * A worker's signature is fixed and its allocations cannot escape, so thread-local is safe; the
 * frame arena, by contrast, belongs to the main thread, and two workers using it at once would be a
 * data race. Every function on this call chain records the flag.
 *
 * @param f  The function definition.
 * @return   true when the thread-local arena is used; false when there is no record.
 */
bool     planParTlsArena(const FuncDef *f);

/**
 * @brief Record whether `new` inside this function has to use a thread-local arena.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether the thread-local arena is used.
 */
void planSetParTlsArena(FuncDef *f, bool v);

/**
 * @brief Tell whether the output view this worker receives is the whole one (rather than the
 * `[lo, hi)` slice).
 *
 * The one-dimensional `par` slices the output view by the worker's row range, and the worker writes
 * `out[i - lo]`; a worker of a two-dimensional fold has to write absolute slots
 * `out[r * cols + c]`, where a slice does not line up with the row numbers, so that shape passes
 * the whole view instead.
 *
 * @param f  The function definition.
 * @return   true when the whole output view is passed; false when there is no record.
 */
bool     planParFullOut(const FuncDef *f);

/**
 * @brief Record whether this worker's output view is passed whole.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether the whole output view is passed.
 */
void planSetParFullOut(FuncDef *f, bool v);

/* ---- per-function analysis facts ---------------------------------------------------
 *
 * These facts are stored in node slots. `funcInstance` shallow-copies with `*in = *tmpl`, and slots
 * do not travel with the copy, so `planInheritFuncFacts` must inherit them onto the instance
 * immediately after the copy. */

/**
 * @brief Carry a template function's per-function analysis facts onto a newly materialised
 * instance.
 *
 * The instance is a shallow copy of the template (`*in = *tmpl`), and result slots are indexed by
 * node pointer, so the copy does not bring them along; this function must therefore be called
 * right after the copy to merge the template's analysis block and written-bit masks into the
 * instance's slot.
 *
 * @param in    The instance created by the shallow copy.
 * @param tmpl  The template function.
 */
void planInheritFuncFacts(FuncDef *in, const FuncDef *tmpl);

/**
 * @brief Tell whether this function is an associated function.
 *
 * A function without a `self` parameter belongs to a type but does not act on a value
 * (`point::origin()`), so a call site has to write the full type name.
 *
 * @param f  The function definition.
 * @return   true when it is an associated function; false when there is no record.
 */
bool anIsAssoc(const FuncDef *f);

/**
 * @brief Record whether this function is an associated function.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether it is an associated function.
 */
void anSetIsAssoc(FuncDef *f, bool v);

/**
 * @brief Tell whether this function body was already checked in the scope where the lambda is
 * written.
 *
 * The `call` method body a lambda synthesises is checked in place, in the scope holding the capture
 * set, so this pass over struct methods must skip it.
 *
 * @param f  The function definition (the lambda's `call` method).
 * @return   true when it was already checked; false when there is no record.
 */
bool anLamChecked(const FuncDef *f);

/**
 * @brief Record whether this function body was already checked in the scope where the lambda is
 * written.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether it was already checked.
 */
void anSetLamChecked(FuncDef *f, bool v);

/**
 * @brief Tell whether this lambda's return type is still unwritten and has to be decided by the
 * first `return`.
 *
 * While true, the first `return` carrying a value fills its type in; once filled, the flag is set
 * back to false.
 *
 * @param f  The function definition (the lambda's `call` method).
 * @return   true when the return type is to be inferred; false when there is no record.
 */
bool anLamInferRet(const FuncDef *f);

/**
 * @brief Record whether this lambda's return type is still unwritten and has to be decided by the
 * first `return`.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether the return type is to be inferred.
 */
void anSetLamInferRet(FuncDef *f, bool v);

/**
 * @brief Tell whether this function needs a home arena of its own.
 *
 * The body contains `new`, and the return type contains a reference or a view, or the function
 * stores outward through an out-parameter: such a function takes one more hidden home arena
 * parameter. The question is closed over the call graph, so "needs" is transitive.
 *
 * @param f  The function definition.
 * @return   true when it needs its own home arena; false when there is no record.
 */
bool anNeedsHome(const FuncDef *f);

/**
 * @brief Record whether this function needs a home arena of its own.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  Whether it needs its own home arena.
 */
void anSetNeedsHome(FuncDef *f, bool v);

/**
 * @brief Get the memo state of "does this function allocate".
 *
 * 0 means not computed, 1 that it allocates (it has `new` directly, or a callee allocates), 2 that
 * it does not, and 3 that it is being computed -- on a cycle it is treated as allocating, which is
 * the conservative direction.
 *
 * @param f  The function definition.
 * @return   The state value; 0 when there is no record.
 */
int  anAllocState(const FuncDef *f);

/**
 * @brief Record the memo state of "does this function allocate".
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The state value: 0 not computed, 1 yes, 2 no, 3 being computed.
 */
void anSetAllocState(FuncDef *f, int v);

/**
 * @brief Get the memo state of "does this function print".
 *
 * 0 means not computed, 1 that it prints (it has `print` / `println` directly, or a callee prints),
 * 2 that it does not, and 3 that it is being computed -- on a cycle it is treated as printing.
 *
 * @param f  The function definition.
 * @return   The state value; 0 when there is no record.
 */
int  anMayPrintState(const FuncDef *f);

/**
 * @brief Record the memo state of "does this function print".
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The state value: 0 not computed, 1 yes, 2 no, 3 being computed.
 */
void anSetMayPrintState(FuncDef *f, int v);

/**
 * @brief Get the number of fresh local names recorded while collecting this function's effects.
 *
 * "Fresh" means the object the name was bound to at the time was produced by `new`; storing such a
 * value into a container needs no lifetime constraint.
 *
 * @param f  The function definition.
 * @return   The number of names recorded; 0 when there is no record.
 */
int  anFreshCount(const FuncDef *f);

/**
 * @brief Record the number of fresh local names recorded while collecting this function's effects.
 *
 * @param f  The function definition; NULL ignores this write.
 * @param v  The number of names.
 */
void anSetFreshCount(FuncDef *f, int v);

/**
 * @brief Get the list of allocation sites recorded for a function.
 *
 * The list holds every expression in the body that may allocate: `new`, plus calls that may
 * allocate storage. The allocation-placement stage decides the arena level per site from it; this
 * is the checker's own working state, and later stages do not read it. A generic instance does not
 * collect its own list and points at its template's directly.
 *
 * @param f  The function definition.
 * @return   The allocation-site vector; an empty vector when `f` is NULL or there is no record.
 */
Vec  anArenaSites(const FuncDef *f);

/**
 * @brief Record a function's list of pending arena sites.
 *
 * The list holds both allocation-point expressions and call sites whose arena argument is not
 * decided yet; it is handed to a pass after the analysis finishes so that the level can be decided
 * per site. An instantiated function copies the same list over from its template.
 *
 * @param f  The target function.
 * @param v  The site vector, with `Expr *` elements.
 */
void anSetArenaSites(FuncDef *f, Vec v);

/**
 * @brief Get the number of parameter symbols registered for a function.
 *
 * @param f  The target function.
 * @return   The registered count; 0 when there is no record, capped at `EFF_MAX_PARAMS`.
 */
int  anParamSymCount(const FuncDef *f);

/**
 * @brief Set the number of parameter symbols registered for a function.
 *
 * @param f  The target function.
 * @param v  The new count.
 */
void anSetParamSymCount(FuncDef *f, int v);

/**
 * @brief Get the writable array a function stores its parameter symbols in.
 *
 * The array has a fixed capacity; the first `anParamSymCount` elements are live. The call also
 * builds the result slot on the way.
 *
 * @param f  The target function.
 * @return   The array's first address; NULL when `f` is NULL or the slot cannot be built.
 */
void **anParamSyms(FuncDef *f);

/* ---- operator / comparison facts ---------------------------------------------------
 *
 * `planConvCheck` / `planNeedOp` are read by code generation (to know that there are still
 * deferred operators to re-resolve, and that a narrowing conversion needs a runtime check); the
 * remaining `anXxx` fields are the checker's own state. */

/**
 * @brief Get the shallowest level a value must live to.
 *
 * `-1` means no constraint has landed on this node yet; the smaller the number, the longer the
 * required lifetime.
 *
 * @param node  The node to look up.
 * @return      The recorded level; 0 when this field has never been written.
 */
int  anMinAt(const void *node);

/**
 * @brief Record the shallowest level a value must live to.
 *
 * @param node  The target node.
 * @param v     The level; `-1` means no constraint yet.
 */
void anSetMinAt(void *node, int v);

/**
 * @brief Tell whether a value's storage is reused round after round (an `@overwrite` initialiser).
 *
 * @param node  The node to look up.
 * @return      `true` when it is; `false` when there is no record.
 */
bool anReuse(const void *node);

/**
 * @brief Mark a value's storage as reused round after round.
 *
 * @param node  The target node.
 * @param v     `true` pins its level to layer 1 of the current frame.
 */
void anSetReuse(void *node, bool v);

/**
 * @brief Tell whether a value is a reference borrowed from somewhere else.
 *
 * @param node  The node to look up.
 * @return      `true` when it is a borrowed reference; `false` when there is no record.
 */
bool anBorrowed(const void *node);

/**
 * @brief Record whether a value is a reference borrowed from somewhere else.
 *
 * @param node  The target node.
 * @param v     `true` for a borrowed reference.
 */
void anSetBorrowed(void *node, bool v);

/**
 * @brief Tell whether a name has been resolved as qualified (the source wrote `a::b`, or the
 * qualified-name pass already fixed it).
 *
 * @param node  The node to look up.
 * @return      `true` when it is qualified; `false` when there is no record.
 */
bool anQualified(const void *node);

/**
 * @brief Mark a name as resolved by qualified name, so later passes do not rewrite it through this
 * module's name table.
 *
 * @param node  The target node.
 * @param v     `true` for qualified.
 */
void anSetQualified(void *node, bool v);

/**
 * @brief Tell whether an explicit conversion needs a range check at run time.
 *
 * An integer widening, a float-to-float conversion, and a reference to a 64-bit integer need no
 * check; every other narrowing, sign change, and float-to-integer conversion does.
 *
 * @param e  The conversion expression.
 * @return   `true` when a runtime check has to be emitted; `false` when there is no record.
 */
bool planConvCheck(const Expr *e);

/**
 * @brief Record whether an explicit conversion needs a range check at run time.
 *
 * @param e  The conversion expression.
 * @param v  `true` when a check is needed.
 */
void planSetConvCheck(Expr *e, bool v);

/**
 * @brief Tell whether a binary operation is deferred to instantiation because an operand is a type
 * parameter.
 *
 * @param e  The binary expression.
 * @return   `true` when it has to be re-resolved; `false` when there is no record.
 */
bool planNeedOp(const Expr *e);

/**
 * @brief Record that a binary operation has to be deferred to instantiation for re-resolution.
 *
 * @param e  The binary expression.
 * @param v  `true` when re-resolution is needed.
 */
void planSetNeedOp(Expr *e, bool v);

/* ---- call / receiver facts ---------------------------------------------------------
 *
 * All read by code generation: whom a task is handed to (`EX_EXT`), which field an `EX_FIELD`
 * resolved to, which instance type an `EX_ASSOC` resolved to, and how a `dyn` call reaches its
 * receiver. */

/**
 * @brief The pad primitive an f-string format specifier selects.
 *
 * The specifier itself carries no type; which primitive is selected follows from the **static**
 * type of the value in the slot: integers go through `padInt`, floats through `padF`, and text
 * (`slice<u8>`) through `padText`. The checker records the choice on the node and code generation
 * emits the call accordingly.
 */
typedef enum {
    PLAN_PAD_NONE = 0,
    PLAN_PAD_TEXT,
    PLAN_PAD_INT,
    PLAN_PAD_FLOAT
} PlanPadKind;

/**
 * @brief Get the domain expression an `ext` task is handed to.
 *
 * @param e  The `ext` call node.
 * @return   The domain expression; NULL when there is no record.
 */
Expr     *planExtDom(const Expr *e);

/**
 * @brief Record the domain expression an `ext` task is handed to.
 *
 * @param e  The `ext` call node.
 * @param v  The domain expression.
 */
void planSetExtDom(Expr *e, Expr *v);

/**
 * @brief Get the field definition a field-access node resolves to.
 *
 * An `fn` value stored in a table field goes through this same slot, so a call site can obtain the
 * slot's effect clause from it.
 *
 * @param e  The field-access node.
 * @return   The matching field definition; NULL when there is no record.
 */
FieldDef *planField(const Expr *e);

/**
 * @brief Record the field definition a field-access node resolves to.
 *
 * @param e   The field-access node.
 * @param fd  The matching field definition.
 */
void planSetField(Expr *e, FieldDef *fd);

/**
 * @brief Get the concrete type an associated-item call or an enum-value node resolves to.
 *
 * Code generation uses it to obtain the type name after instantiation.
 *
 * @param e  The associated-item call or enum-value node.
 * @return   The resolved type; NULL when there is no record.
 */
Type     *planAssocOwner(const Expr *e);

/**
 * @brief Record the concrete type an associated-item call or an enum-value node resolves to.
 *
 * @param e  The associated-item call or enum-value node.
 * @param t  The resolved type.
 */
void planSetAssocOwner(Expr *e, Type *t);

/**
 * @brief Tell whether a `dyn` method call's receiver is reached through a reference.
 *
 * @param e  The method-call node.
 * @return   `true` when the receiver arrives through a reference; `false` when there is no record.
 */
bool     planDynRecvViaRef(const Expr *e);

/**
 * @brief Record whether a `dyn` method call's receiver is reached through a reference.
 *
 * @param e  The method-call node.
 * @param v  `true` when it arrives through a reference.
 */
void planSetDynRecvViaRef(Expr *e, bool v);

/**
 * @brief Tell whether a call is made through a function value rather than a named declaration.
 *
 * @param e  The call node.
 * @return   `true` when it goes through a function value; `false` when there is no record.
 */
bool     planCallViaFn(const Expr *e);

/**
 * @brief Record that a call is made through a function value.
 *
 * @param e  The call node.
 * @param v  `true` when it goes through a function value.
 */
void planSetCallViaFn(Expr *e, bool v);

/**
 * @brief Record that a lambda literal is used as a bare `fn(...)` function pointer.
 *
 * A lambda that captures nothing has an empty environment, so its value is the code pointer and
 * nothing else -- exactly what a `fn(...)` type is. The checker accepts that conversion where a
 * `fn` value is expected and records the decision here; code generation then emits the function
 * itself instead of the (empty) environment literal.
 *
 * @param e  The `EX_LAMBDA` node.
 * @param v  `true` when the literal's value is the code pointer.
 */
void planSetLamFnPtr(Expr *e, bool v);

/**
 * @brief Get whether a lambda literal is used as a bare `fn(...)` function pointer.
 *
 * @param e  The `EX_LAMBDA` node.
 * @return   `true` when the checker accepted the conversion for this literal.
 */
bool planLamFnPtr(const Expr *e);

/**
 * @brief Get the pad primitive an f-string format-specifier slot selects.
 *
 * @param e  The method-call node (`__extc_pad`).
 * @return   The selected primitive; `PLAN_PAD_NONE` when there is no record.
 */
PlanPadKind planPadKind(const Expr *e);

/**
 * @brief Record the pad primitive an f-string format-specifier slot selects.
 *
 * @param e  The method-call node (`__extc_pad`).
 * @param v  The selected primitive.
 */
void planSetPadKind(Expr *e, PlanPadKind v);

/* ---- depth and origin facts --------------------------------------------------------
 *
 * The parameter is `void *`: `refDepth` lives both on an `Expr` and on the checker's `Sym`, and one
 * accessor answers for both. */

/**
 * @brief Get the reference/escape depth recorded on a node.
 *
 * A larger number means a deeper block and therefore a shorter lifetime; `0` means the layer
 * outside this frame, i.e. the lifetime the caller chose, and is also the default when there is no
 * record. The same accessor serves expressions and the checker's bindings, hence the `void *`
 * parameter.
 *
 * @param node  The node to look up.
 * @return      The recorded depth; 0 when there is no record.
 */
int  anRefDepth(const void *node);

/**
 * @brief Record a node's reference/escape depth.
 *
 * @param node  The target node.
 * @param v     The depth; 0 means the layer outside this frame.
 */
void anSetRefDepth(void *node, int v);

/**
 * @brief Get the layer a call's result stays in.
 *
 * `-1` means the result outlives this frame and the call has to hand out its own home arena;
 * `>= 1` means the result stays at that explicit block level; `0` means no `mut ref` argument gave
 * a reason and the final pass decides from the current block.
 *
 * @param node  The call node.
 * @return      The recorded depth; 0 when there is no record.
 */
int  anHomeDepth(const void *node);

/**
 * @brief Record the layer a call's result stays in.
 *
 * It must be updated together with the arena argument the call actually passes down; the two may
 * not disagree.
 *
 * @param node  The call node.
 * @param v     The layer the result is in; `-1` means longer than this frame.
 */
void anSetHomeDepth(void *node, int v);

/**
 * @brief Get the lexical block level an allocation point sits in.
 *
 * The arena level may be rewritten to the home sentinel, so the lexical level keeps a slot of its
 * own.
 *
 * @param node  The allocation-point node.
 * @return      The recorded level; 0 when there is no record.
 */
int  anLexicalLevel(const void *node);

/**
 * @brief Record the lexical block level an allocation point sits in.
 *
 * @param node  The allocation-point node.
 * @param v     The block level.
 */
void anSetLexicalLevel(void *node, int v);

/**
 * @brief Look up the level a value was last published to.
 *
 * `-1` means it was never seen being published; `0` is also a real answer, meaning it was
 * published outside this frame.
 *
 * @param node  The value node to look up.
 * @return      The publication level; -1 when there is no record.
 */
int  anStoredAt(const void *node);

/**
 * @brief Record the level a value is published to.
 *
 * @param node  The target value node.
 * @param v     The publication level.
 */
void anSetStoredAt(void *node, int v);

/* ---- call-site markers for builtin forms -------------------------------------------
 *
 * A builtin form has no callee pointer to recognise it by, so the checker marks the **call node**
 * and code generation recognises it by the mark. */

/**
 * @brief Get the worker function in a `parallel::run(worker, ...)` call.
 *
 * @param e  The call node.
 * @return   The worker function; NULL when it is not such a call.
 */
FuncDef *planParWorker(const Expr *e);

/**
 * @brief Record the worker function in a `parallel::run(worker, ...)` call.
 *
 * @param e   The call node.
 * @param wf  The worker function.
 */
void     planSetParWorker(Expr *e, FuncDef *wf);

/**
 * @brief Tell whether this call is the builtin form that builds a domain.
 *
 * Such a form has no callee pointer to rely on, so it is recognised by a mark on the call node.
 *
 * @param e  The call node.
 * @return   `true` when it is; `false` when there is no record.
 */
bool     planDomNew(const Expr *e);

/**
 * @brief Mark this call as the builtin form that builds a domain.
 *
 * @param e  The call node.
 * @param v  `true` when it is.
 */
void     planSetDomNew(Expr *e, bool v);

/**
 * @brief Tell whether this call is the `extc_viewOf` builtin form, which has no function body.
 *
 * The declaration has no body and therefore no callee pointer to rely on, so the call is recognised
 * by a mark on the call node.
 *
 * @param e  The call node.
 * @return   `true` when it is; `false` when there is no record.
 */
bool     planViewOf(const Expr *e);

/**
 * @brief Mark this call as the `extc_viewOf` builtin form.
 *
 * @param e  The call node.
 * @param v  `true` when it is.
 */
void     planSetViewOf(Expr *e, bool v);

/* ---- impl / trait / module facts ---------------------------------------------------
 *
 * All four are read by code generation. `trait` / `target` are resolved while the checker interns
 * type names; `usedDyn` / `usesDyn` are set at the moment a `dyn` form is seen and decide whether
 * the dyn runtime is emitted. */

/**
 * @brief Get the trait an `impl` block implements.
 *
 * @param im  The `impl` block definition.
 * @return    The implemented trait; NULL for an inherent `impl` or before resolution.
 */
TraitDef *planImplTrait(const ImplDef *im);

/**
 * @brief Record the trait an `impl` block implements.
 *
 * @param im  The `impl` block definition.
 * @param tr  The implemented trait.
 */
void      planSetImplTrait(ImplDef *im, TraitDef *tr);

/**
 * @brief Get the concrete type an `impl` block resolves to.
 *
 * @param im  The `impl` block definition.
 * @return    The target type; NULL when there is no record.
 */
Type     *planImplTarget(const ImplDef *im);

/**
 * @brief Record the concrete type an `impl` block resolves to.
 *
 * @param im  The `impl` block definition.
 * @param t   The target type.
 */
void      planSetImplTarget(ImplDef *im, Type *t);

/**
 * @brief Tell whether a trait is used by a `dyn` form.
 *
 * Only when it is is its vtable generated.
 *
 * @param tr  The target trait.
 * @return    `true` when it is used; `false` when there is no record.
 */
bool      planUsedDyn(const TraitDef *tr);

/**
 * @brief Record that a trait is used by a `dyn` form.
 *
 * @param tr  The target trait.
 * @param v   `true` when it is used.
 */
void      planSetUsedDyn(TraitDef *tr, bool v);

/**
 * @brief Tell whether a `dyn` form appears in a module.
 *
 * The dyn runtime is generated only when one does.
 *
 * @param m  The target module.
 * @return   `true` when one appears; `false` when there is no record.
 */
bool      planUsesDyn(const Module *m);

/**
 * @brief Record that a `dyn` form appears in a module.
 *
 * @param m  The target module.
 * @param v  `true` when one appears.
 */
void      planSetUsesDyn(Module *m, bool v);

/**
 * @brief Get the template a function instance was generated from.
 *
 * @param f  The target function.
 * @return   The template function; NULL when `f` is not an instance.
 */
FuncDef *planTemplate(const FuncDef *f);

/**
 * @brief Get an instance's C name.
 *
 * @param f  The target function.
 * @return   The C name; NULL when there is no record.
 */
const char *planInstName(const FuncDef *f);

/* ---- arena and zone plan ----------------------------------------------------------- */

/**
 * @brief Get the arena level an allocation point should use.
 *
 * The `ARENA_HOME` sentinel means "use the home arena the caller passed"; a larger number means a
 * shorter lifetime.
 *
 * @param e  The allocation-point expression.
 * @return   The recorded level; 0 when there is no record.
 */
int  planArenaLevel(const Expr *e);
/**
 * @brief Get the arena argument a call passes down.
 *
 * `-1` means the caller's own home arena, `>= 1` that block level, and `0` the current block.
 *
 * @param e  The call node.
 * @return   The recorded argument; 0 when there is no record.
 */
int  planArenaArg(const Expr *e);
/**
 * @brief Get the zone level recorded for a pool site.
 *
 * ZONE_HOME means "use the home zone the caller received" (library functions are transparent about
 * place and pass the choice to the caller); k >= 1 means the zone of the block at level k. When the
 * site has no record, 0 is returned.
 *
 * @param e  The expression to query (the call site that creates the pool).
 * @return   The recorded zone level; 0 when there is no record.
 */
int  planZoneLevel(const Expr *e);
/**
 * @brief Tell whether a function receives the caller's home arena as a hidden parameter.
 *
 * The answer rests on "the body really reads the home arena it was given, or passes the home arena
 * down to a callee", and its value is only final once the summary closure has run.
 *
 * @param f  The function to query.
 * @return   true when the hidden home arena parameter is needed; false when there is no record.
 */
bool planUsesHome(const FuncDef *f);

/**
 * @brief Tell whether a function body allocates storage itself.
 *
 * When false the body allocates nothing, so code generation declares neither an arena array for it
 * nor a release call.
 *
 * @param f  The function to query.
 * @return   true when it may allocate; false when there is no record.
 */
bool planMayUseArena(const FuncDef *f);

/**
 * @brief Tell whether a function creates a pool, either directly or through a callee.
 *
 * The value is only final once the call-graph closure has run.
 *
 * @param f  The function to query.
 * @return   true when it creates a pool; false when there is no record.
 */
bool planMakesPool(const FuncDef *f);
/**
 * @brief Tell whether a loop's condition allocates storage.
 *
 * When true, code generation wraps the loop in an arena reclaimed every round, so allocations in
 * the condition return at the end of the round.
 *
 * @param s  The loop statement to query.
 * @return   true when the condition allocates; false when there is no record.
 */
bool planCondAllocs(const Stmt *s);
/**
 * @brief Record whether a declaration's initialiser points at storage outside the frame.
 *
 * Storage outside the frame covers globals, `global` declarations, the results of
 * `effects Ret=0`, and string literals; this result is written while the scope is still alive so it
 * can be asked about after the function body has been checked.
 *
 * @param s  The variable declaration statement.
 * @param v  true when the initialiser points outside the frame.
 */
void planSetVarInitOutOfFrame(Stmt *s, bool v);

/**
 * @brief Read back whether a declaration's initialiser points at storage outside the frame.
 *
 * @param s  The variable declaration statement to query.
 * @return   true when the initialiser points outside the frame; false when there is no record.
 */
bool planVarInitOutOfFrame(const Stmt *s);
/**
 * @brief Get the step statement of a desugared `for`.
 *
 * The step statement is syntax and is stored on the loop body, because `continue` must reach it;
 * once the loop is rewritten into an iterator there is no step any more, and this returns NULL.
 *
 * @param s  The loop-body statement to query.
 * @return   The step statement; NULL when the step has been dropped, `s` is NULL, or there is no
 *           step.
 */
Stmt *planForStep(const Stmt *s);
/**
 * @brief Tell whether an expression must first evaluate some part into a temporary.
 *
 * The expression (or its arguments, or its element count) appears more than once in the generated
 * C, or has to be evaluated only once outside a loop, so it is recorded as true and code generation
 * emits a temporary declaration first.
 *
 * @param e  The expression to query.
 * @return   true when a temporary is needed; false when there is no record.
 */
bool planNeedTemp(const Expr *e);

/* ---- coroutine family -------------------------------------------------------------- */

/**
 * @brief Tell whether a function is a coroutine.
 *
 * A coroutine's value is a frame handle rather than an ordinary C function.
 *
 * @param f  The function to query.
 * @return   true when it is a coroutine; false when there is no record.
 */
bool   planIsCoro(const FuncDef *f);
/**
 * @brief Get the type of the value a coroutine `yield`s.
 *
 * For `coroutine<A, B>` this is the last type argument; for the single-argument `coroutine<T>` it is
 * that one argument.
 *
 * @param f  The function to query.
 * @return   The yielded value's type; NULL when it is not a coroutine or there is no record.
 */
Type  *planYieldType(const FuncDef *f);
/**
 * @brief Get the C struct type of a coroutine frame.
 *
 * The frame struct's field layout matches the fields code generation emits, and the protocol
 * methods driving the coroutine hang off it.
 *
 * @param f  The function to query.
 * @return   The frame struct type; NULL when it is not a coroutine or the frame is not set up yet.
 */
Type  *planCoroFrameType(const FuncDef *f);
/**
 * @brief Tell whether a coroutine needs a place (zone) of its own.
 *
 * A coroutine that creates a pool, or whose frame was ever boxed into a handle, always has a task,
 * and its place is held by the task; when it does not, the step function is driven directly.
 *
 * @param f  The function to query.
 * @return   true when it needs its own zone; false when there is no record.
 */
bool   planCoroNeedsZone(const FuncDef *f);
/**
 * @brief Get the protocol number of a synthesised protocol method.
 *
 * A protocol method has no body and is expanded inline at the call site by code generation; the
 * number says which method it is: next/value/send on the frame are 1/2/5, and on the handle they are
 * 3/4/6.
 *
 * @param f  The function to query.
 * @return   The protocol number; 0 for an ordinary function.
 */
int planCoroProto(const FuncDef *f);
/**
 * @brief Tell whether a coroutine's frame is ever converted into a handle at an assignment.
 *
 * A boxed coroutine always gets a task, so it counts as needing a zone of its own; this fact is
 * written wherever the conversion happens.
 *
 * @param f  The function to query.
 * @return   true when the frame was ever boxed into a handle; false when there is no record.
 */
bool   planCoroBoxed(const FuncDef *f);

/* ---- setters (written by the checker) ----------------------------------------------
 *
 * Every write goes through a setter, so the write sites are the single place where assertions can be
 * added or the storage swapped uniformly. A setter with a NULL node is a no-op: some plan questions
 * are asked about nodes that do not exist. */

/**
 * @brief Record the arena level an allocation site should use.
 *
 * @param e  The allocation-site expression (`new`, or a call that may allocate).
 * @param v  The arena level: ARENA_HOME means the home arena, k >= 1 the arena of the block at
 *           level k.
 */
void planSetArenaLevel(Expr *e, int v);

/**
 * @brief Record the zone level a pool site should use.
 *
 * Only ever lowered: one node may be checked more than once, and a smaller level means a longer
 * lifetime, so the smaller one wins.
 *
 * @param e  The pool-site expression.
 * @param v  The zone level: ZONE_HOME means "use the home zone the caller received", k >= 1 the
 *           zone of the block at level k.
 */
void planSetZoneLevel(Expr *e, int v);

/**
 * @brief Record the arena argument a call passes down.
 *
 * @param e  The call-site expression.
 * @param v  The arena argument: ARENA_HOME means the caller's own home arena, k >= 1 the arena of
 *           the block at level k.
 */
void planSetArenaArg(Expr *e, int v);

/**
 * @brief Record whether a call site's arena argument still waits for the final round.
 *
 * @param e  The call-site expression; NULL ignores this write.
 * @param v  true when it still waits for the final round.
 */
void planSetArenaArgPending(Expr *e, bool v);

/**
 * @brief Record that an expression needs a temporary.
 *
 * @param e  The expression.
 * @param v  true when a temporary is needed.
 */
void planSetNeedTemp(Expr *e, bool v);
/**
 * @brief Record whether a function receives the caller's home arena as a hidden parameter.
 *
 * @param f  The function.
 * @param v  true when the hidden home arena parameter is needed.
 */
void planSetUsesHome(FuncDef *f, bool v);

/**
 * @brief Record whether a function body allocates storage itself.
 *
 * @param f  The function.
 * @param v  true when it may allocate.
 */
void planSetMayUseArena(FuncDef *f, bool v);

/**
 * @brief Record whether a function creates a pool, either directly or through a callee.
 *
 * @param f  The function.
 * @param v  true when it creates a pool.
 */
void planSetMakesPool(FuncDef *f, bool v);
/**
 * @brief Record whether a loop's condition allocates storage.
 *
 * @param st  The loop statement.
 * @param v   true when the condition allocates.
 */
void planSetCondAllocs(Stmt *st, bool v);
/**
 * @brief Record that the step statement of a desugared `for` is dropped.
 *
 * Once the loop is rewritten into an iterator it is advanced by the iterator's `next()`, `continue`
 * does not have to jump to the step label, and the label is not emitted either.
 *
 * @param st  The loop-body statement that carried the step statement.
 */
void planSetForStepDropped(Stmt *st);

/**
 * @brief Record whether a function is a coroutine.
 *
 * @param f  The function.
 * @param v  true when it is a coroutine.
 */
void planSetIsCoro(FuncDef *f, bool v);

/**
 * @brief Record the type of the value a coroutine `yield`s.
 *
 * @param f  The coroutine function.
 * @param v  The yielded value's type.
 */
void planSetYieldType(FuncDef *f, Type *v);

/**
 * @brief Record the C struct type of a coroutine frame.
 *
 * @param f  The coroutine function.
 * @param v  The frame struct type.
 */
void planSetCoroFrameType(FuncDef *f, Type *v);

/**
 * @brief Record whether a coroutine needs a place (zone) of its own.
 *
 * @param f  The coroutine function.
 * @param v  true when it needs its own zone.
 */
void planSetCoroNeedsZone(FuncDef *f, bool v);

/**
 * @brief Record the protocol number of a synthesised protocol method.
 *
 * @param f  The synthesised protocol method.
 * @param v  The protocol number: next/value/send on the frame are 1/2/5, and on the handle 3/4/6.
 */
void planSetCoroProto(FuncDef *f, int v);
/**
 * @brief Record whether a coroutine was ever boxed into a handle.
 *
 * This bit is written where a frame value is forced into a `coroutine<T>` handle; once set, the
 * coroutine always gets a task, even without an explicit spawn, and therefore counts as needing a
 * zone.
 *
 * @param f  The coroutine function definition; NULL ignores this write.
 * @param v  true when it was ever boxed into a handle.
 */
void planSetCoroBoxed(FuncDef *f, bool v);
/**
 * @brief Record the C name used when emitting an instance.
 *
 * The name is built from the template name followed by each type argument's rewritten string, is
 * written once at instantiation, and is used both for the emitted definition and for the call sites.
 * Only the pointer is stored, so the string must stay valid until it is read later.
 *
 * @param f     The instance function definition; NULL ignores this write.
 * @param name  The C name.
 */
void planSetInstName(FuncDef *f, const char *name);

/* ---- writing the resolved callee (the checker) ------------------------------------- */

/**
 * @brief Record the callee a call site resolves to, as the default answer.
 *
 * A call site inside a generic body is shared by all instances of that body, and each instance's
 * answer is recorded per body by `planSetAltCallee`; this function writes the one read when the site
 * belongs to no instance, and `planCallee` returns it when there is no emission context.
 *
 * @param e       The call-site (or function-value) expression; NULL ignores this write.
 * @param callee  The resolved function definition; may be NULL when there is no resolution.
 */
void planSetCallee(Expr *e, FuncDef *callee);
/**
 * @brief Record the callee a call site resolves to, per enclosing function body or type instance.
 *
 * One call site inside a generic body is shared by all instances of that body, and each instance may
 * resolve to a different function (`wrap<i32>` calls `pick_i32`, `wrap<meter>` calls `pick_meter`),
 * so the answer is recorded per (call site, enclosing body) pair and at least one of `enclosing` and
 * `inst` has to be given. Repeating the write for the same pair only overwrites the callee, because
 * the deferred-call closure replays several rounds.
 *
 * @param e          The call-site expression; NULL ignores this write.
 * @param enclosing  The function instance containing the call site; NULL when resolving inside a
 *                   method body.
 * @param inst       The type instance containing the call site; NULL when resolving inside a free
 *                   function body.
 * @param callee     The function definition resolved inside that enclosing body.
 */
void planSetAltCallee(Expr *e, FuncDef *enclosing, const Type *inst, FuncDef *callee);

/* ---- the emission context (code generation) ----------------------------------------
 *
 * Code generation brackets "which function is being emitted" with this pair of functions, so that
 * `planCallee` can answer "inside this instance's body, what does this call site resolve to". Leaving
 * is not optional: without leaving, emitting the next function keeps seeing the previous instance's
 * answers. */

/**
 * @brief Record the function code generation is emitting.
 *
 * Once set, `planCallee` prefers the callee resolution recorded for that body; after emitting one
 * body `planLeaveFunc` has to be called to clear it, otherwise the next body reads the previous
 * instance's answers.
 *
 * @param f  The function definition being emitted.
 */
void planEnterFunc(const FuncDef *f);

/**
 * @brief Clear the function code generation is emitting.
 *
 * Used as a pair with `planEnterFunc`: clear it after emitting a body so that `planCallee` stops
 * answering for the previous instance.
 */
void planLeaveFunc(void);
/**
 * @brief Record the type instance whose method body code generation is emitting.
 *
 * A generic struct's method bodies are shared by every type instance of that struct, so call sites
 * inside a method body are recorded per type instance and `planCallee` answers from that. Use it as
 * a pair with `planLeaveInst`.
 *
 * @param t  The type instance whose method body is being emitted.
 */
void planEnterInst(const Type *t);

/**
 * @brief Clear the type instance whose method body code generation is emitting.
 *
 * Used as a pair with `planEnterInst`; after clearing, `planCallee` stops answering for that type
 * instance.
 */
void planLeaveInst(void);
/**
 * @brief Record which template function an instance was instantiated from.
 *
 * The template pointer is kept in a plan side table rather than in a node field: the same pointer
 * decides the instance's name, its type arguments, and which body it shares. Instantiation writes it
 * after the shallow copy of the instance, because the shallow copy cannot carry this pointer.
 *
 * @param f     The instance function definition; NULL ignores this write.
 * @param tmpl  The template function definition used for the instantiation.
 */
void planSetTemplate(FuncDef *f, FuncDef *tmpl);

#endif /* EXTC_PLAN_H */
