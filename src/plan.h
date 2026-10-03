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
#include "results.h"

/* ---- identity and instances ------------------------------------------------------- */

/* The instance a call site resolves to (NULL for a builtin, an operator, or one the
 * checker never resolved).
 * Writer: the checker, at the call site, and again per instance by the deferred-call
 * fixpoint (`planSetCallee` / `planSetAltCallee`).
 * Valid: after that fixpoint has run in the POST phase; before it, the pointer can still
 * be the template or provisional.
 * Stale: generated C calls a C function that was never emitted.
 *
 * A call site inside a generic body is shared by every instance of that body, so the
 * answer depends on which instance is being emitted. Code generation brackets each
 * function with `planEnterFunc` / `planLeaveFunc`, and this accessor then returns the
 * resolution recorded for that instance. */
FuncDef *planCallee(const Expr *e);

/* Was this function called? Code generation emits a function only when something calls it
 * (plus the roots: `main`, `@export`, a `dyn` table entry). Writers: the checker at every
 * call site, including the deferred-call fixpoint, which marks each instance it produced. */
bool planUsed(const FuncDef *f);
void planSetUsed(FuncDef *f, bool v);

/* The C name a binding (`Expr.u.ident.cname`) or a variable statement
 * (`Stmt.u.var.cname`) is emitted under. `Param` is **not** here on purpose: a parameter is
 * stored by value, so it keeps its own field. */
const char *planCName(const void *node);
void planSetCName(void *node, const char *name, ResultKind kind);

/* ---- struct-definition facts (T4) ---------------------------------------------------
 *
 * `builtinHolder` and `coroOf` are read by code generation, so they are plan facts. The
 * two `anXxx` entries are the checker's own working state (no later phase reads them);
 * they share the slot but not the audience -- see `ANALYSIS_FIELDS` in plan.c. */
bool     planBuiltinHolder(const StructDef *sd);
void     planSetBuiltinHolder(StructDef *sd, bool v);
FuncDef *planCoroOf(const StructDef *sd);
void     planSetCoroOf(StructDef *sd, FuncDef *f);
const char *anLamSig(const StructDef *sd);
void        anSetLamSig(StructDef *sd, const char *sig);
bool        anMakesPoolAny(const StructDef *sd);
void        anSetMakesPoolAny(StructDef *sd, bool v);

/* ---- per-function analysis facts (T4) ----------------------------------------------
 *
 * Staged: the accessors prefer the slot and fall back to the field; the setters write both.
 * `planInheritFuncFacts` must run right after `*in = *tmpl` in `funcInstance` -- a plain
 * field travels with the shallow copy, a slot keyed by node pointer does not. */
void planInheritFuncFacts(FuncDef *in, const FuncDef *tmpl);
bool anIsAssoc(const FuncDef *f);         void anSetIsAssoc(FuncDef *f, bool v);
bool anLamChecked(const FuncDef *f);      void anSetLamChecked(FuncDef *f, bool v);
bool anLamInferRet(const FuncDef *f);     void anSetLamInferRet(FuncDef *f, bool v);
bool anNeedsHome(const FuncDef *f);       void anSetNeedsHome(FuncDef *f, bool v);
int  anAllocState(const FuncDef *f);      void anSetAllocState(FuncDef *f, int v);
int  anMayPrintState(const FuncDef *f);   void anSetMayPrintState(FuncDef *f, int v);
int  anFreshCount(const FuncDef *f);      void anSetFreshCount(FuncDef *f, int v);
Vec  anArenaSites(const FuncDef *f);      void anSetArenaSites(FuncDef *f, Vec v);
int  anParamSymCount(const FuncDef *f);   void anSetParamSymCount(FuncDef *f, int v);
void **anParamSyms(FuncDef *f);

/* ---- operator / comparison facts (T4) -----------------------------------------------
 *
 * `planConvCheck` / `planNeedOp` are read by code generation; the `anXxx` entries are the
 * checker's own state (see `planAnalysisFields` in plan.c). */
int  anMinAt(const void *node);        void anSetMinAt(void *node, int v);
bool anReuse(const void *node);        void anSetReuse(void *node, bool v);
bool anBorrowed(const void *node);     void anSetBorrowed(void *node, bool v);
bool anQualified(const void *node);    void anSetQualified(void *node, bool v);
bool planConvCheck(const Expr *e);     void planSetConvCheck(Expr *e, bool v);
bool planNeedOp(const Expr *e);        void planSetNeedOp(Expr *e, bool v);

/* ---- call / receiver facts (T4) -----------------------------------------------------
 *
 * All read by code generation: where a task is handed (`EX_EXT`), which field an `EX_FIELD`
 * resolved to, the instance type an `EX_ASSOC` resolved to, and how a `dyn` call reaches its
 * receiver. */
Expr     *planExtDom(const Expr *e);        void planSetExtDom(Expr *e, Expr *v);
FieldDef *planField(const Expr *e);         void planSetField(Expr *e, FieldDef *fd);
Type     *planAssocOwner(const Expr *e);    void planSetAssocOwner(Expr *e, Type *t);
bool     planDynRecvViaRef(const Expr *e);  void planSetDynRecvViaRef(Expr *e, bool v);
bool     planCallViaFn(const Expr *e);      void planSetCallViaFn(Expr *e, bool v);

/* ---- depth and origin facts (T4) ----------------------------------------------------
 *
 * `void *` on purpose: `refDepth` lives on an `Expr` **and** on the checker's `Sym`, and one
 * accessor answers both (two spellings of one fact is what this batch removes). */
int  anRefDepth(const void *node);      void anSetRefDepth(void *node, int v);
int  anHomeDepth(const void *node);     void anSetHomeDepth(void *node, int v);
int  anLexicalLevel(const void *node);  void anSetLexicalLevel(void *node, int v);
int  anStoredAt(const void *node);      void anSetStoredAt(void *node, int v);

/* ---- call-site markers for builtin forms (T4) --------------------------------------
 *
 * A builtin form has no callee to recognise it by, so the checker marks the **call node**
 * and code generation keys on the marker. */
FuncDef *planParWorker(const Expr *e);
void     planSetParWorker(Expr *e, FuncDef *wf);
bool     planDomNew(const Expr *e);
void     planSetDomNew(Expr *e, bool v);
bool     planViewOf(const Expr *e);
void     planSetViewOf(Expr *e, bool v);

/* ---- impl / trait / module facts (T4) ----------------------------------------------
 *
 * All four are read by code generation, so they are plan facts. `trait`/`target` are
 * resolved by the checker while it interns type names; `usedDyn`/`usesDyn` are set the
 * moment a `dyn` form is seen, and decide whether the dyn runtime is emitted. */
TraitDef *planImplTrait(const ImplDef *im);
void      planSetImplTrait(ImplDef *im, TraitDef *tr);
Type     *planImplTarget(const ImplDef *im);
void      planSetImplTarget(ImplDef *im, Type *t);
bool      planUsedDyn(const TraitDef *tr);
void      planSetUsedDyn(TraitDef *tr, bool v);
bool      planUsesDyn(const Module *m);
void      planSetUsesDyn(Module *m, bool v);

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
 * has to reach it. **Syntax**: the parser writes it, and the checker only ever decides
 * that the step is gone (recorded as a result, see the setter below). */
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
 * parser owns it; what the checker records about it is only "the step is gone". */
void planSetCondAllocs(Stmt *st, bool v);
/* The loop was retargeted onto an iterator, so its step statement is no longer part of the
 * body and `continue` must not jump to a label nobody emits. */
void planSetForStepDropped(Stmt *st);

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

/* ---- writing the resolved callee (the checker) ------------------------------------- */

/* Record the callee of a call site: the fallback answer, used when the site is not
 * shared by several instances. */
void planSetCallee(Expr *e, FuncDef *callee);
/* Record the callee of a call site **for one enclosing body**: a site inside a generic
 * body belongs to every instance of that body, and each one may resolve to a different
 * instance (`wrap<i32>` calls `pick_i32`, `wrap<meter>` calls `pick_meter`).
 *   enclosing - the function instance, for a free-function body (else NULL)
 *   inst      - the type instance, for a method body of a generic struct (else NULL)
 * Idempotent, because the deferred-call fixpoint replays its rounds. */
void planSetAltCallee(Expr *e, FuncDef *enclosing, const Type *inst, FuncDef *callee);

/* ---- the emission context (code generation) ----------------------------------------
 *
 * Code generation brackets the emission of one function with these two, so that
 * `planCallee` can answer "inside this instance's body, what does this call site resolve
 * to". Leaving is not optional: without it the next function's emission would still see
 * the previous instance's answers. */
void planEnterFunc(const FuncDef *f);
void planLeaveFunc(void);
/* The same, for the methods of one generic type instance. A method body is shared by
 * every type instance of its struct, so this is the other half of the same question. */
void planEnterInst(const Type *t);
void planLeaveInst(void);
/* The instance -> template back pointer, written once per instance by instance
 * materialization. `FuncDef` must not carry this as a field: the same pointer decides
 * names, type arguments and which body is shared, and it has to be written through the
 * plan so that "who writes it, and when" stays one place. An instance is a shallow copy
 * of its template, so the copy no longer brings the pointer along -- this write is what
 * establishes it, and it must stay after the copy. */
void planSetTemplate(FuncDef *f, FuncDef *tmpl);

#endif /* EXTC_PLAN_H */
