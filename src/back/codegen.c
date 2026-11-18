/**
 * @file codegen.c
 * @brief Translation of a checked module into C source text.
 *
 * This pass does no type inference of its own: the type checker has already
 * written its results back into the AST (Expr.type, Expr.func, Expr.field,
 * Stmt.type), so all that is left here is translation.
 *
 * The generated C is deliberately dull C: no macro tricks, no optimization
 * stunts. `#line` maps gcc diagnostics back to line numbers in the .extc file.
 */

#include "codegen.h"
#include "dbg.h"         /* EXTC_DBG_ASSERT: contract violations are compiler bugs */
#include "plan.h"          /* the one read port for the plan: the side table the analysis passes record into */
#include "places.h"
#include "coroutine.h"
#include "domain.h"      /* the pool registry runtime */
#include "memfind.h"        /* the byte-search runtime (std::sys::mem), emitted on demand */
#include "plate.h"          /* the plate runtime (std::sys::heap), emitted on demand */
#include "bufrt.h"          /* the buffer substrate (std::data::buf), emitted on demand */
#include "reclaim.h"        /* the reclamation runtime (slab + holder counts + region sweep) */
/* The checker owns the rules this pass has to agree with, so it includes the checker's
 * header rather than restating them: `typeSupportsOp` decides whether an operator applied
 * to an instantiated type is native, and `isEqualityOp` answers the `==` / `!=` pair.
 * `isProtoType` and `enumHasPayload` were duplicated here as static copies of the
 * checker's - the same hazard in a smaller form - and are gone. */
#include "typelayer.h"   /* the read-only type questions (no checker header here) */
#include "extctime.h"

#include <assert.h>     /* the entry-point contracts below */
#include <stdarg.h>
#include <stdlib.h>   /* exit: exceeding the closure limit must fail loudly */
#include <stdio.h>
#include <string.h>

/* Everything from types.h arrives through codegen.h. Including it a second time here
 * would redeclare each of its prototypes: a repeated textual include is processed
 * before the header's own guard can take effect. */

/* ---- cg-split:begin
 * Sections. The 16k lines below are one translation unit split by topic: each file under
 * src/back/cg/ is a section of this pass in the order the compiler reads them, and the
 * #include lines below are that order. Nothing about the emitted C depends on the split:
 * the preprocessor sees the same tokens in the same sequence as before, and
 * tools/emitted_identity.py proves it byte for byte (identical generated C for every
 * source in its set).
 *
 * Why a textual split and not several translation units: a second TU would need hundreds
 * of statics exported across a new header, which is where a refactor like this usually
 * breaks behaviour; the split buys navigation and review without touching the ABI of the
 * pass. tools/source_split.py renders the file back into one stream, so the source-text
 * gates and the doc anchors read exactly what the compiler reads.
 * cg-split:end */
#include "cg/00-type-mapping.inc"
#include "cg/10-state.inc"
#include "cg/20-primitive-block.inc"
#include "cg/30-expressions.inc"
#include "cg/40-type-descriptors.inc"
#include "cg/50-descriptors-on-demand.inc"
#include "cg/60-structural-eq.inc"
#include "cg/70-trailing-args.inc"
#include "cg/80-roots-and-triggers.inc"
#include "cg/90-lambda-fnptr.inc"
#include "cg/100-debug-printing.inc"
#include "cg/110-statements.inc"
#include "cg/120-question-expansion.inc"
#include "cg/130-local-host-region.inc"
#include "cg/140-view-subscript.inc"
#include "cg/150-top-level.inc"
#include "cg/160-generic-instances.inc"
#include "cg/170-struct-order.inc"
#include "cg/180-unreferenced-defs.inc"
#include "cg/190-lambda-envs.inc"
#include "cg/200-region-reclaim.inc"
#include "cg/210-criterion-d.inc"
