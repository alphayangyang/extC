/* ---------------------------------------------------------------- state */

/* A definition that the program may never name: a top-level global, or a row of
 * the scalar descriptor table.
 *
 * The decision cannot be made where the definition is emitted, because what
 * refers to it may be generated much later (a body, or the descriptor table
 * itself). It is made once the whole translation unit is assembled, on the text
 * alone: the name is counted in the finished output, and a definition whose name
 * occurs exactly once - in that definition and nowhere else - is dropped. The
 * direction is conservative in the only way that matters: any other mention,
 * even from code that is itself dead, keeps the definition. */
/* A function the program may never call.
 *
 * Every emitted function is declared once and defined once, so a name with exactly
 * those two mentions has no caller anywhere - what gcc and clang report as
 * `-Wunused-function`. The decision is made on the finished text, like DeadDef, so a
 * mention from a descriptor row (the `eq` adapter of a struct, say) counts as a use
 * and keeps the function. `main` is never a candidate: it is called from outside the
 * unit.
 *
 * Both halves are kept as *text*, not as offsets, and they are removed together or
 * not at all. Keeping both halves as text rules out the two failure modes: removing
 * the declaration and the definition independently leaves definitions whose calls
 * become implicit declarations (28 new warnings), and an offset pair keyed by
 * `FuncDef*` frames something that is not one whole function (a cut body produces
 * `-Wreturn-type`). A name is unique per emitted function, so it is the key. */
typedef struct {
    const char *name;    /* C name */
    const char *proto;   /* the declaration, exactly as emitted */
    const char *body;    /* the definition; filled in once the body buffer is complete */
    size_t      off, len;   /* where it sits in `g.body`, until `body` is filled in */
    /* Named from **outside** its instance -- a `dyn` table's thunk calls it, and the table is spliced
     * in **after** the "definitions nothing names" pass has run, so that pass cannot see the mention
     * and would remove a method instance nothing else calls.
     *
     * Named `namedByDynTable`, not `dynTable`: `FuncDef` has a `dynTable` of its own (which lives in the plan
     * side table), and the two are different facts on different structs. The textual freeze checker
     * cannot tell them apart, so the name has to. */
    bool        namedByDynTable;
} DeadFunc;

/* A local declaration that may never be read.
 *
 * A local name that occurs exactly once in its own function body cannot be used: that one
 * occurrence is the declaration. The stage is therefore the body text - the same capture
 * the function pruning already keeps - so no byte offsets have to survive anything, and
 * the answer is sound by construction.
 *
 * What is deliberately *not* done here: guessing declarations from indentation and a
 * trailing `;`. That matches assignments too (`foo = f();` is a statement with a side
 * effect) and deleting one would drop the call. Only lines the generator itself emitted as
 * a declaration are registered. */
typedef struct {
    const char *name;      /* C name of the local */
    const char *text;      /* the declaration line, exactly as emitted */
    const char *funcName;  /* C name of the function it belongs to */
    size_t      own;       /* mentions the declaration itself contributes to that stage:
                            * 1 when it sits inside the body, 0 when it is in the prologue
                            * (`__extc_home` is declared before the body, and the body text
                            * is the stage, so nothing there mentions it) */
} DeadLocal;

typedef struct {
    const char *name;   /* the C name, as written into the output */
    const char *text;   /* the definition, exactly as emitted, newline included */
    /* A definition inside a function body is decided on a smaller stage: what
     * can name it is that piece of code and nothing else. `scopeB > scopeA`
     * marks such a definition, and the two offsets bound the code that could
     * mention it, inside the body buffer. `off` is where the definition sits in
     * that same buffer. */
    size_t      off, scopeA, scopeB;
    bool        scoped;  /* true when the two offsets above are the stage to count on */
} DeadDef;

