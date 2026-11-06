/**
 * @file parser.c
 * @brief extC syntax, part two of the pipeline: the recursive-descent parser.
 *
 * Turns the token vector produced by the lexer into AST nodes. It resolves no names:
 * identifiers, module paths, and generic arguments are kept exactly as written, so the
 * loader and the type checker can decide what they mean. `{` after a name is a struct
 * literal by position, not by capitalization, and `p->inCond` records where a `{` must be
 * read as the start of a block instead.
 */
#include "parser.h"

#include <stdio.h>      /* fprintf: the EXTC_DBG_QN trace */
#include <stdlib.h>     /* getenv: the EXTC_DBG_QN switch */
#include <string.h>

#include "lexer.h"

typedef struct {
    Ctx   *ctx;
    Arena *arena;
    Vec   *toks;
    size_t pos;
    /* True while parsing the condition of `if` / `while` / `match`. Inside a condition a
     * `{` starts the body block, so a struct literal must be parenthesised -- the rule is
     * positional, not based on the capitalization of the type name. */
    bool   inCond;
    bool   noBody;       /* true while parsing an `extern!` signature (no body) */
    bool   sawBuiltin;   /* the annotation loop saw `@builtin` for this declaration */
    /* Nesting depth of the recursive descent. Two counters, because the two families have
     * different safe budgets and different answers to "who reports it first":
     *   depth     -- blocks and expressions (see `parseTooDeep`);
     *   typeDepth -- types, where the checker has its own 64-level limit and should get to
     *                report first (`TYPE_DEPTH_LIMIT`).
     * The counters are what turn "too deep to parse" into a diagnostic instead of a crash. */
    int    depth;
    int    typeDepth;
    /* Set by the `global` soft keyword just before a declaration and consumed by `parseVarDecl`
     * (`ST_VAR.isGlobal`). Soft, not a lexer keyword: `global` stays a legal identifier. */
    bool   sawGlobal;
    /* Extra statements produced by the desugaring. One user statement may become several
     * adjacent statements (see the desugaring section later in this file): the parser
     * returns the first and queues the rest here for the caller that holds the statement
     * vector to splice in. */
    Vec    pending;
    /* The remaining globals a multi-name top-level declaration expands into; the caller
     * splices them in the same way. */
    Vec    pendingGlobals;
    /* Counter for synthesized `__extc_t<N>` names: one block may hold several parallel
     * assignments, whose names must not collide. */
    int    sugarSeq;
    /* The `const` parameters of the function whose signature is being parsed, so an array
     * length can name one (`fn dot<const N: i64>(a: [N]f64)`). Empty outside a signature. */
    Vec    constParams;
    /* The module being parsed. When the desugaring expands into a library call (a value
     * f-string goes through `stl::string`, multiple subscripts through `std::nd`), the
     * module it needs is registered **here** as a hidden `use`: the sugar carries its own
     * module, so the user does not have to remember which file defines `from` for an
     * `f"..."`. */
    Module *mod;
} Parser;

/* The compiler's nesting budget for blocks and expressions.
 *
 * The parser, the checker, the dataflow helper and the emitter all recurse once per nesting level,
 * and C's stack is finite: 16000 nested parentheses and 1000 nested blocks take the whole
 * process down with SIGSEGV -- no diagnostic, and the driver cannot tell "too
 * deep" from "crashed". Crash thresholds for **block** nesting, where the frames are fattest:
 *
 *     release build (`-O1`)          crashes at ~250 levels
 *     sanitizer build (ASan+UBSan)   crashes at ~96 levels
 *
 * 64 is chosen so every build keeps a wide margin, and it is far above anything a human writes
 * (hand-written code stays under ten; the corpus peak is in the low tens). Deeply nested syntax
 * only ever comes from a machine, so the honest answer is a diagnostic that names the limit --
 * not a stack overflow. */
#define PARSE_DEPTH_LIMIT 64

/* The same budget for **types**, but larger on purpose: the checker has its own documented
 * `TYPE_DEPTH_LIMIT` (64) and produces a better message about generic instances, so the parser
 * must not fire first. Types recurse through `parseType` alone, whose frames are small, and the
 * checker stops them at 64 well before any pass could overflow. */
#define PARSE_TYPE_DEPTH_LIMIT 96



/* ---------------------------------------------------------------- lookahead */

/**
 * @brief Look `k` tokens ahead of the cursor without moving it.
 *
 * Past the end the result is clamped to the final TK_EOF, so a caller can always read a
 * fixed number of tokens ahead without bounds checks.
 *
 * @param p  Parser.
 * @param k  Lookahead distance in tokens; 0 is the token under the cursor.
 * @return   That token; the final TK_EOF when the distance runs past the end.
 */
static Token *pk(Parser *p, size_t k) {
    size_t i = p->pos + k;
    if (i >= p->toks->len) i = p->toks->len - 1;
    return (Token *)vecAt(p->toks, i);
}

/**
 * @brief Return the token under the cursor without consuming it.
 *
 * @param p  Parser.
 * @return   The token under the cursor.
 */
static Token *cur(Parser *p) { return pk(p, 0); }

/**
 * @brief Raise a nesting counter by one; past `limit` report "nesting too deep" and give
 *        the level back.
 *
 * The counter is the parser's recursion budget: C's stack is finite, and deeply nested
 * syntax overflows the stack in every recursive pass at once, so a diagnostic is issued
 * here instead of letting the compile process die with SIGSEGV.
 *
 * @param counter  Nesting counter to raise (`Parser.depth` or `Parser.typeDepth`).
 * @param limit    Limit of that counter.
 * @param p        Parser, for locating and reporting the diagnostic.
 * @param what     Syntax kind named in the diagnostic, e.g. `a type`.
 * @return         `true` when the limit is exceeded (the diagnostic has been reported and
 *                 this level has been given back); `false` otherwise.
 */
static bool parseTooDeepIn(int *counter, int limit, Parser *p, const char *what) {
    if (++*counter <= limit) return false;
    (*counter)--;
    ctxError(p->ctx, cur(p)->line, cur(p)->col,
             "Deeply nested syntax comes from a machine, and the compiler stops at a fixed depth"
             " rather than letting its own recursion overflow the C stack.",
             "nesting too deep in %s (the compiler's limit is %d levels)", what, limit);
    return true;
}

/**
 * @brief Spend one level of the nesting budget for statements and expressions.
 *
 * @param p     Parser.
 * @param what  Syntax kind named in the diagnostic.
 * @return      `true` when `PARSE_DEPTH_LIMIT` is exceeded.
 */
static bool parseTooDeep(Parser *p, const char *what) {
    return parseTooDeepIn(&p->depth, PARSE_DEPTH_LIMIT, p, what);
}

/**
 * @brief Spend one level of the nesting budget for types.
 *
 * The type budget is larger than the statement budget on purpose: types recurse through
 * `parseType` alone, whose frames are small, and the checker has its own type depth limit
 * and should report the more accurate diagnostic first.
 *
 * @param p  Parser.
 * @return   `true` when `PARSE_TYPE_DEPTH_LIMIT` is exceeded.
 */
static bool parseTypeTooDeep(Parser *p) {
    return parseTooDeepIn(&p->typeDepth, PARSE_TYPE_DEPTH_LIMIT, p, "a type");
}

/**
 * @brief Whether the token under the cursor is spelled exactly `value`.
 *
 * A punctuation spelling additionally requires the token to be punctuation itself.
 * `")"` and `"match"` are both compared as spellings, but the text of a string literal
 * carries no quotes: the source `")"` has the text `)`, so comparing text alone would
 * mistake the string in `println(")")` or `println("x)")` for punctuation and report
 * "expected an expression, found `)`".
 *
 * @param p      Parser.
 * @param value  Spelling to compare against.
 * @return       `true` when they are equal.
 */
static bool at(Parser *p, const char *value) {
    Token *t = cur(p);
    if (t->kind == TK_STRING && lexIsPunct(value)) return false;
    return strcmp(t->text, value) == 0;
}

/**
 * @brief Whether the kind of the token under the cursor is `k`, ignoring its text.
 *
 * @param p  Parser.
 * @param k  Expected token kind.
 * @return   `true` when the kinds match.
 */
static bool atKind(Parser *p, TokenKind k) { return cur(p)->kind == k; }

/**
 * @brief Consume and return the token under the cursor.
 *
 * The cursor never moves past the final TK_EOF, so a caller that keeps taking tokens on
 * malformed input cannot run off the token vector.
 *
 * @param p  Parser.
 * @return   The consumed token.
 */
static Token *take(Parser *p) {
    Token *t = cur(p);
    if (p->pos + 1 < p->toks->len) p->pos++;
    return t;
}

/**
 * @brief Consume the token under the cursor when it is spelled `value`.
 *
 * @param p      Parser.
 * @param value  Expected spelling.
 * @return       `true` when it matched and was consumed; `false` otherwise, with the
 *               cursor unmoved.
 */
static bool accept(Parser *p, const char *value) {
    if (!at(p, value)) return false;
    take(p);
    return true;
}

/**
 * @brief Printable spelling of `t`, for error messages.
 *
 * Tokens whose text is empty (TK_EOF, TK_NEWLINE) fall back to the kind name, so a
 * message never reads "found ``".
 *
 * @param t  Token to show.
 * @return   The token's text; its kind name when the text is empty.
 */
static const char *shown(const Token *t) {
    return t->text[0] ? t->text : tokenKindName(t->kind);
}

/**
 * @brief Consume the token under the cursor when it is spelled `value`; report it
 *        otherwise.
 *
 * @param p      Parser.
 * @param value  Required spelling, e.g. `"{"`.
 * @param note   Note stored with the diagnostic, usually the grammar rule the caller is
 *               following; `NULL` when there is nothing to add.
 * @return       `true` when consumed; `false` after reporting through `ctxError`, in which
 *               case the caller must return at once.
 */
static bool expect(Parser *p, const char *value, const char *note) {
    if (at(p, value)) { take(p); return true; }
    Token *t = cur(p);
    ctxError(p->ctx, t->line, t->col, note,
             "expected `%s`, found `%s`", value, shown(t));
    return false;
}

/**
 * @brief Consume the identifier under the cursor, or report what is missing.
 *
 * @param p     Parser.
 * @param what  Description of the missing item, appended after "expected ", e.g.
 *              `"a field name"`.
 * @return      The consumed token; `NULL` after reporting through `ctxError`.
 */
static Token *expectIdent(Parser *p, const char *what) {
    if (atKind(p, TK_IDENT)) return take(p);
    Token *t = cur(p);
    ctxError(p->ctx, t->line, t->col, NULL,
             "expected %s, found `%s`", what, shown(t));
    return NULL;
}

/**
 * @brief Consume any run of newline tokens and nothing else.
 *
 * @param p  Parser.
 */
static void skipNl(Parser *p) {
    while (atKind(p, TK_NEWLINE)) take(p);
}

/**
 * @brief Consume a run of empty-statement separators: newlines and an optional `;`.
 *
 * @param p  Parser.
 */
static void skipJunk(Parser *p) {
    for (;;) {
        if (atKind(p, TK_NEWLINE) || at(p, ";")) { take(p); continue; }
        return;
    }
}

/* ---------------------------------------------------------------- forward declarations */

static Type    *parseType(Parser *p);
static Type    *parseFnType(Parser *p);
static Stmt    *parseBlock(Parser *p);
static Stmt    *parseStmt(Parser *p);
static Stmt    *parseVarDecl(Parser *p);
static GlobalDef *parseGlobalDecl(Parser *p);
static Stmt    *parseIf(Parser *p);
static Stmt    *parseWhile(Parser *p);
static Stmt    *parseFor(Parser *p);
static Stmt    *parsePar(Parser *p);
static StructDef *parseStruct(Parser *p);
static FuncDef   *parseFunc(Parser *p);
static bool       parseFuncAnnotations(Parser *p, bool *outInline, bool *outPrivate,
                                       bool *outUnchecked, bool *outDrop);
static TypeDef   *parseTypeDecl(Parser *p);
static ImplDef   *parseImpl(Parser *p);
static TraitDef  *parseTrait(Parser *p);
static bool       parseDottedName(Parser *p, Buf *out, const char *what);

static bool   startsUpper(const char *s);
static Token *expectTypeName(Parser *p, const char *what);

static Expr *parseExpr(Parser *p);
/* The bodies behind the three nesting-budget wrappers (see `parseTooDeep`). */
static Type    *parseTypeInner(Parser *p);
static Stmt    *parseBlockInner(Parser *p);
static Expr    *parseUnaryInner(Parser *p);
static Expr *parseOr(Parser *p);
static Expr *parseAnd(Parser *p);
static Expr *parseBitOr(Parser *p);
static Expr *parseBitXor(Parser *p);
static Expr *parseBitAnd(Parser *p);
static Expr *parseEquality(Parser *p);
static Expr *parseComparison(Parser *p);
static Expr *parseShift(Parser *p);
static Expr *parseTerm(Parser *p);
static Expr *parseFactor(Parser *p);
static Expr *parseUnary(Parser *p);
static Expr *parsePostfix(Parser *p);
static Expr *parsePrimary(Parser *p);
static Expr *parseLambda(Parser *p);   /* `fn(x: i64) -> i64 { ... }`, */

/**
 * @brief Whether `n` is one of the ten scalar built-in type names.
 *
 * `bool` and `void` do not count: an explicit conversion requires scalar numeric types on
 * both sides, and extC has no implicit truth-value conversion.
 *
 * @param n  Type name.
 * @return   `true` when it is one of the ten scalar type names.
 */
static bool isScalarTypeName(const char *n) {
    static const char *N[] = { "i8","i16","i32","i64","u8","u16","u32","u64","f32","f64", NULL };
    for (size_t i = 0; N[i]; i++) if (strcmp(N[i], n) == 0) return true;
    return false;
}
static Expr *parseStructLit(Parser *p, const char *name);
static int   looksLikeAssoc(Parser *p);
static Expr *parseAssoc(Parser *p, const char *name, int line, size_t startPos,
                        const char **modPrefixOut);
static bool  parseArgs(Parser *p, Vec *out);

/**
 * @brief Build a binary-operator Expr node.
 *
 * @param p     Parser; the node is allocated in its arena.
 * @param op    Operator spelling, e.g. `"||"`; stored as written for code generation.
 * @param l     Left operand (already parsed).
 * @param r     Right operand (already parsed).
 * @param line  Source line attached to the node, for diagnostics.
 * @return      The new EX_BIN node.
 */
static Expr *mkBin(Parser *p, const char *op, Expr *l, Expr *r, int line) {
    Expr *e = exprNew(p->arena, EX_BIN, line);
    e->u.bin.op = op;
    e->u.bin.left = l;
    e->u.bin.right = r;
    return e;
}

/**
 * @brief Build a call node for a module function (e.g. `std::nd::at2(...)`).
 *
 * Same trick as `fstrFrom`: write the **full module path**, so the parser does not have
 * to know the caller's import aliases -- the loader resolves it as long as the module was
 * imported.
 *
 * @param p          Parser.
 * @param modPrefix  Module path (`"std::nd"`).
 * @param name       Function name.
 * @param args       Argument list (already filled in; ownership moves to the node).
 * @param line       Source line attached to the node.
 * @return           The new EX_ASSOC node (`isCall = true`).
 */
static Expr *moduleCall(Parser *p, const char *modPrefix, const char *name, Vec *args, int line) {
    Expr *f = exprNew(p->arena, EX_ASSOC, line);
    f->u.assoc.typeName = "stl";
    f->u.assoc.modPrefix = modPrefix;
    f->u.assoc.name = name;
    vecInit(&f->u.assoc.targs, p->arena, sizeof(void *));
    f->u.assoc.args = *args;
    f->u.assoc.isCall = true;
    return f;
}


/* ================================================================ top level */

/**
 * @brief Parse one `use a::b::c` import declaration.
 *
 * A file is a module, so `use std::io` refers to the file std/io.extc; search paths and
 * cycle checks belong to the loader. The short name is the last segment of the path, and
 * it is how this file names that module. Only the path is recorded here; nothing is
 * resolved.
 *
 * @param p  Parser.
 * @return   The new UseDecl; `NULL` on a syntax error.
 */
static UseDecl *parseUse(Parser *p) {
    Token *kw = take(p);
    Token *first = expectTypeName(p, "a module path (e.g. `use std::io`)");
    if (!first) return NULL;
    Buf  path;
    bufInit(&path, p->arena);
    bufPuts(&path, first->text);
    const char *shortName = first->text;
    bool wildcard = false;
    Vec  names;
    vecInit(&names, p->arena, sizeof(const char *));
    while (at(p, "::")) {
        take(p);
        /* `use std::io::*` -- the star is a whole segment and has to be last, so the
         * path stops here. `shortName` keeps the module's own name (`io`), which is
         * what diagnostics call it. */
        if (at(p, "*")) {
            take(p);
            wildcard = true;
            break;
        }
    /* `use std::io::{cin, cout}`: the braces hold a **name list**, never a module path. */
        if (at(p, "{")) {
            take(p);
            while (!at(p, "}")) {
                Token *nm = expectIdent(p, "a name to bring into scope (e.g. `{cin, cout}`)");
                if (!nm) return NULL;
                *(const char **)vecPush(&names) = nm->text;
                if (at(p, ",")) { take(p); continue; }
                if (!at(p, "}")) {
                    ctxError(p->ctx, cur(p)->line, cur(p)->col, NULL,
                             "expected `,` or `}` in the import list");
                    return NULL;
                }
            }
            take(p);
            if (at(p, "as")) {
                ctxError(p->ctx, cur(p)->line, cur(p)->col,
                         "An import list brings the names in under their own names.",
                         "`as` goes on a whole module: `use std::io as io`");
                return NULL;
            }
            break;
        }
        Token *seg = expectTypeName(p, "a module path segment");
        if (!seg) return NULL;
        bufPuts(&path, "::");
        bufPuts(&path, seg->text);
        shortName = seg->text;
    }
    /* `use std::sys::io as sysio`: here an alias is not a convenience but a requirement --
     * the short name of both std::io and std::sys::io is `io`, and reading stdin and
     * opening a file need both modules at hand.
     *
     * `path` keeps the full name as written (the loader matches fully qualified names
     * against fully qualified names); only `shortName` is replaced by the alias. */
    const char *alias = NULL;
    if (at(p, "as")) {
        take(p);
        Token *a = expectIdent(p, "an alias name (e.g. `use std::sys::io as sysio`)");
        if (!a) return NULL;
        alias = a->text;
    }
    UseDecl *u = (UseDecl *)arenaAllocZero(p->arena, sizeof(UseDecl));
    u->path      = bufCstr(&path);
    u->shortName = alias ? alias : shortName;
    u->line      = kw->line;
    u->wildcard  = wildcard;
    u->names     = names;
    return u;
}

/**
 * @brief Consume and return the next token that is not a newline.
 *
 * Soft keywords (`pub`, `global`, `par`) are ordinary identifiers, so recognising them
 * takes a look at the next word: `pub use` is a re-export, a lone `pub` is something
 * else. A newline counts as whitespace here, not as a token boundary.
 *
 * @param p  Parser.
 * @return   That token; `NULL` at the end of the token vector.
 */
static Token *takeAfterNewlines(Parser *p) {
    while (p->pos < p->toks->len) {
        Token *t = (Token *)vecAt(p->toks, p->pos);
        if (t->kind != TK_NEWLINE) { p->pos++; return t; }
        p->pos++;
    }
    return NULL;
}

/**
 * @brief The `skip`-th **non-newline** token from the current position, without consuming
 *        anything.
 *
 * Soft keywords (`pub`, `global`, `par`) are ordinary identifiers, so recognising them
 * takes a look at the next word: `pub use` is a re-export, a lone `pub` is something
 * else. A newline counts as whitespace here, not as a token boundary.
 *
 * @param p     Parser.
 * @param skip  Number of non-newline tokens to skip (1 = the next one).
 * @return      That token; `NULL` when the distance runs past the end.
 */
static Token *tokAfterNewlines(Parser *p, size_t skip) {
    size_t i = p->pos;
    while (i < p->toks->len) {
        Token *t = (Token *)vecAt(p->toks, i);
        if (t->kind != TK_NEWLINE) {
            if (skip == 0) return t;
            skip--;
        }
        i++;
    }
    return NULL;
}

/**
 * @brief The desugaring carries its own library module: register a hidden `use` unless the
 *        module has already been imported.
 *
 * The desugaring expands into **library calls** (the start of a value f-string, the kernel
 * of multiple-subscript sugar), and those names live in the standard library. Making the
 * user remember, for one `f"..."`, that the start lives in `stl::string` and the specifier
 * in `std::text::string`, would write implementation detail into the syntax; the module is
 * registered by the sugar itself and the user only sees the sugar. An already imported
 * module (including an aliased import) is not registered twice: a module should be `use`d
 * once.
 *
 * @param p     Parser; `p->mod` is the **module being parsed** (set up by `parseModule`).
 * @param path  Full module path, e.g. `stl::string`.
 * @param line  Line of the construct that triggered the sugar, so the hidden `use` carries
 *              the line the sugar was written on.
 */
static void sugarNeedModule(Parser *p, const char *path, int line) {
    if (!p->mod) return;
    for (size_t i = 0; i < p->mod->uses.len; i++) {
        UseDecl *u = *(UseDecl **)vecAt(&p->mod->uses, i);
        if (strcmp(u->path, path) == 0) return;
    }
    UseDecl *u = (UseDecl *)arenaAllocZero(p->arena, sizeof(UseDecl));
    u->path = arenaStrndup(p->arena, path, strlen(path));
    const char *colon = strrchr(path, ':');
    u->shortName = arenaStrndup(p->arena, colon ? colon + 1 : path,
                                strlen(colon ? colon + 1 : path));
    u->line = line;
    vecInit(&u->names, p->arena, sizeof(const char *));
    *(UseDecl **)vecPush(&p->mod->uses) = u;
}

/**
 * @brief Parse the `effects Addr=... Cont=... Ret=... Thread=...` clause.
 *
 * Written on **any declaration without a function body**: `extern!("libc")` and
 * `@builtin` both count, because the compiler cannot see through either of them
 * (`"libc"` is only a library name for diagnostics). The clause is the author's assertion
 * about argument addresses, about the return value (`Ret=`), and about state shared across
 * threads. Each round skips
 * newlines before reading the keyword, otherwise `at()` would see TK_NEWLINE and miss it.
 *
 * @param p             Parser.
 * @param f             Function definition that receives the effect bits.
 * @param outSawThread  When not `NULL`, set to `true` if `Thread=` is seen.
 * @return              `true` when parsing succeeded; `false` on error (the diagnostic has
 *                      been reported).
 */
static bool parseEffectsClause(Parser *p, FuncDef *f, bool *outSawThread) {
            take(p);
            f->hasEffects = true;
            f->extThreadMask = 1u;   /* conservative default: assume it touches state shared across threads */
            for (;;) {
                Token *nm = expectIdent(p, "`Addr`, `Cont` or `Thread`");
                if (!nm) return NULL;
                if (!expect(p, "=", NULL)) return NULL;
                Token *val = cur(p);
                if (val->kind != TK_INT) {
                    ctxError(p->ctx, val->line, val->col, NULL,
                             "`effects` takes small integers, e.g. `effects Addr=0 Cont=0`");
                    return NULL;
                }
                take(p);
                unsigned bits = (unsigned)val->ival;
                if (strcmp(nm->text, "Addr") == 0)        f->extAddrMask = bits;
                else if (strcmp(nm->text, "Cont") == 0)   f->extContMask = bits;
                else if (strcmp(nm->text, "Ret") == 0) {
                    /* `Ret=0`: "the reference/view I return does not point into the
                     * caller's frames" -- the return-side `Addr`; `Ret=1` is the
                     * conservative default, which asserts nothing, so only 0 is
                     * informative. */
                    if (bits > 1) {
                        ctxError(p->ctx, nm->line, nm->col,
                                 "`Ret` is one fact about the return value: 0 = it does not point"
                                 " into the caller's frames, 1 = say nothing (the default).",
                                 "`Ret=%u` is out of range (use 0 or 1)", bits);
                        return NULL;
                    }
                    f->extRetFresh = (bits == 0);
                }
                else if (strcmp(nm->text, "Thread") == 0) {
                    if (outSawThread) *outSawThread = true;
                    if (bits > 1u) {
                        ctxError(p->ctx, nm->line, nm->col,
                                 "`Thread` is a single fact about the call, not a per-argument mask:"
                                 " 0 = it touches no cross-thread shared state, 1 = it does.",
                                 "`Thread=%u` is out of range (use 0 or 1)", bits);
                        return NULL;
                    }
                    f->extThreadMask = bits;
                }
                else {
                    ctxError(p->ctx, nm->line, nm->col,
                             "`Addr` = it stores `&argument`; `Cont` = it stores a pointer it"
                             " read out of an argument. Bit i = the i-th argument.",
                             "unknown effect `%s` (only `Addr`, `Cont`, `Ret` and `Thread` exist)",
                             nm->text);
                    return NULL;
                }
                if (at(p, "Addr") || at(p, "Cont") || at(p, "Ret") || at(p, "Thread")) continue;
                break;
            }
            return true;
    return true;
}

/**
 * @brief Parse a C function declared by `extern!`: the library name is a string literal,
 *        and only the signature is on the C side.
 *
 * The shape is `extern!` + the library-name string literal + a `fn` signature, with an
 * optional `effects` clause after it (documented on `parseEffectsClause`). A parameter or
 * return type can only be a scalar or a single pointer (`ref T`, `?ref T`), because that is
 * all C has: `slice<T>` corresponds to C's "pointer plus length" pair of parameters and
 * cannot be expressed across the boundary, so a stdlib wrapper has to spell the two out.
 * The effect declarations are the author's assertions about the call; with none written,
 * the worst case is assumed (every parameter may be stored), which is safe but limits what
 * call sites may do. The resulting C function is only declared, not defined: `p->noBody`
 * makes `parseFunc` stop after the signature, and the implementation is the C library
 * itself.
 *
 * @param p  Parser.
 * @return   The new FuncDef; `NULL` on a syntax error.
 */
static FuncDef *parseExtern(Parser *p) {
    Token *kw = take(p);
    if (!expect(p, "!", NULL)) return NULL;
    if (!expect(p, "(", NULL)) return NULL;
    Token *lib = cur(p);
    if (lib->kind != TK_STRING) {
        ctxError(p->ctx, lib->line, lib->col, NULL,
                 "`extern!(\"lib\")` needs the C library's name as a string (for diagnostics)");
        return NULL;
    }
    take(p);
    if (!expect(p, ")", NULL)) return NULL;
    if (!at(p, "fn")) {
        ctxError(p->ctx, kw->line, kw->col, NULL, "`extern!` must be followed by a `fn` declaration");
        return NULL;
    }
    p->noBody = true;
    bool extInl = false;
    if (at(p, "@")) {
        if (!parseFuncAnnotations(p, &extInl, NULL, NULL, NULL)) return NULL;
        if (extInl) {
            ctxError(p->ctx, kw->line, kw->col,
                     "There is no body to inline: this declaration only names a function that"
                     " lives in another library. Mark the extC function that wraps it.",
                     "`@inline` needs a body, and an `extern!` declaration has none");
            return NULL;
        }
    }
    FuncDef *f = parseFunc(p);
    p->noBody = false;
    if (!f) return NULL;
    f->isExtern  = true;
    f->externLib = lib->text;
    f->body = NULL;

    /* The author may spread the effects clause over any number of lines (it is the
     * author's assertion), so the loop takes each round. */
    while (true) {
        skipJunk(p);
        if (at(p, "effects")) {
            if (!parseEffectsClause(p, f, NULL)) return NULL;
            continue;
        }
        if (at(p, "owned")) {
            Token *ow = take(p);
            ctxError(p->ctx, ow->line, ow->col,
                     "Memory returned by C has to be released, and extC has no `free`."
                     " The planned answer is a resource value owned by the frame that created"
                     " it, which is not implemented yet. Until then, declare only C functions"
                     " that write into memory the caller already owns.",
                     "`owned` is not implemented yet");
            return NULL;
        }
        break;
    }
    return f;
}

/**
 * @brief Parse a whole token vector into `out`.
 *
 * `out` is a Module that is **appended to**: the caller initializes it once, so the prelude
 * and the user file end up in the same module. The grammar accepted is the one listed in
 * `parser.h`; the token vector must end with TK_EOF (`lexAll` always appends one).
 *
 * @param ctx    Reports the first syntax error; parsing stops once it is set.
 * @param arena  Owns every AST node and every copied name.
 * @param toks   Token vector produced by `lexAll`.
 * @param out    Module appended to.
 * @return       `true` when the whole token vector was consumed; `false` when
 *               `ctx->hasError` is set, in which case `out` holds the declarations parsed
 *               so far.
 */
bool parseModule(Ctx *ctx, Arena *arena, Vec *toks, Module *out) {
    /* Field by field: the parser keeps growing fields, and a positional initializer would
     * silently shift out of place when it does. */
    Parser p = {0};
    p.ctx   = ctx;
    p.arena = arena;
    p.toks  = toks;
    p.mod   = out;
    vecInit(&p.pending, arena, sizeof(Stmt *));
    vecInit(&p.constParams, arena, sizeof(void *));
    vecInit(&p.pendingGlobals, arena, sizeof(GlobalDef *));
    skipJunk(&p);

    /* `module a::b`: optional declaration at the top of a file that decides what the
     * module is **called**. Without it the "path = module name" rule applies, so this step
     * is gradual -- a file that is fine today does not change, and only a library that
     * wants "one concept per module, files placed wherever" writes it. It has to be the
     * first declaration in the file (`out->declName` is set only once). */
    if (tokAfterNewlines(&p, 0) && strcmp(tokAfterNewlines(&p, 0)->text, "module") == 0 &&
        tokAfterNewlines(&p, 1) && tokAfterNewlines(&p, 1)->kind == TK_IDENT) {
        Token *kw = takeAfterNewlines(&p);
        Buf path;
        bufInit(&path, p.arena);
        Token *seg = take(&p);
        bufPuts(&path, seg->text);
        while (at(&p, "::")) {
            take(&p);
            Token *nseg = expectTypeName(&p, "a module path segment");
            if (!nseg) return false;
            bufPuts(&path, "::");
            bufPuts(&path, nseg->text);
        }
        out->declName = bufCstr(&path);
        out->declLine = kw->line;
        skipJunk(&p);
        if (at(&p, "module")) {
            ctxError(p.ctx, kw->line, 1, "A file declares its module name once, on its first line.",
                     "second `module` declaration");
            return false;
        }
    }
    while (!atKind(&p, TK_EOF) && !ctx->hasError) {
        /* `pub use a::b`: export the names **again** to whoever imports this module (a
         * facade module uses this to split the implementation up). `pub` is meaningful only
         * in this position; a `pub` anywhere else is reserved for later (reported as an
         * error for now, rather than read as a modifier nobody handles). */
        if (at(&p, "pub") && tokAfterNewlines(&p, 1) && strcmp(tokAfterNewlines(&p, 1)->text, "use") == 0) {
            take(&p);
            skipJunk(&p);
            UseDecl *u = parseUse(&p);
            if (!u) return false;
            u->isPub = true;
            *(UseDecl **)vecPush(&out->uses) = u;
            skipJunk(&p);
            continue;
        }
        /* `use a::b` is a semantic import: finding the file, checking the import cycle, and
         * resolving the qualified names it brings in all belong to the loader. */
        if (at(&p, "use")) {
            UseDecl *u = parseUse(&p);
            if (!u) return false;
            *(UseDecl **)vecPush(&out->uses) = u;
            skipJunk(&p);
            continue;
        }
        /* `@private`: declarations are public by default, so hiding one has to be written
         * out. */
        bool isPrivate = false;
        bool fnInline  = false;
        bool builtinDecl = false;  /* @builtin: implemented by the compiler (no function body) */
        bool fnUnchecked = false;  /* @unchecked: subscripts in this function body get no bounds checks */
        bool noCopy    = false;
        bool poolObject = false;   /* @poolObject: this struct owns a pool (per the author) */
        bool buffer     = false;   /* @buffer: this struct is the owning form of `new T[n]` */
        bool sharesStorage = false; /* @sharesStorage: copying by value makes both copies share storage (containers) */
        bool valueCopy = false;     /* @value: copying from a place must call `clone` */
        bool frozen     = false;   /* @frozen: the author signs off that the layout is C's layout (ticket to cross a C       boundary by value) */
        bool exported   = false;   /* @export: this function's C symbol is for outside use (the other side of extern!) */
        bool moduleEntry = false;  /* @module: this free function is this module's entry point (dispatcher calls it by name) */
        bool implExt    = false;  /* @ext: this impl block extends a type declared by another module */
        const char *moduleName = NULL;  /* display name from `@module("name")`; the module name when not written */
        const char *moduleDesc = NULL;  /* second string of `@module("name", "what it does")` */
        /* `@private` hides a declaration, `@inline` requests inlining: both may appear
         * before the same declaration in either order, so they are read together. Every
         * other annotation is an error here -- an instruction to the compiler that is
         * ignored would leave the reader believing the requested thing happened when it did
         * not. */
        while (at(&p, "@")) {
            Token *a = take(&p);
            Token *nm = expectIdent(&p, "an annotation name");
            if (!nm) return false;
            if (strcmp(nm->text, "private") == 0) {
                if (isPrivate) {
                    ctxError(ctx, a->line, a->col, NULL, "`@private` appears twice");
                    return false;
                }
                isPrivate = true;
                skipJunk(&p);
                continue;
            }
            if (strcmp(nm->text, "inline") == 0) {
                fnInline = true;
                skipJunk(&p);
                continue;
            }
            /* `@export`: hand the C symbol out (the other side of `extern!`). It only makes
             * sense on a free function, so every other declaration rejects it (the same
             * shape as `@frozen`). */
            if (strcmp(nm->text, "export") == 0) {
                if (exported) {
                    ctxError(ctx, a->line, a->col, NULL, "`@export` appears twice");
                    return false;
                }
                exported = true;
                skipJunk(&p);
                continue;
            }
            /* `@unchecked`: subscripts in this function body get **no** bounds checks. Same
             * family as `@inline` (both are instructions about the body), so they are read
             * together here; attached to the wrong declaration, each is rejected by its own
             * branch below. Saying it twice is an error: the same rule as `@private` and the
             * one inside a struct body. */
            if (strcmp(nm->text, "unchecked") == 0) {
                if (fnUnchecked) {
                    ctxError(ctx, a->line, a->col, NULL,
                             "`@unchecked` appears twice on the same function");
                    return false;
                }
                fnUnchecked = true;
                skipJunk(&p);
                continue;
            }
            if (strcmp(nm->text, "noCopy") == 0) {
                noCopy = true;
                skipJunk(&p);
                continue;
            }
            /* `@place` (formerly `@poolObject`): this struct **owns a place** -- its storage lives
             * on the place's own slabs and its lifetime follows the scope it sits in. A declaration beats
             * an inference: the compiler does not have to guess (both "does it have a `pid`
             * field" and "do its methods build pools" were tried: the first is a magic
             * number, the second has to wait for closures), and a **user-defined container
             * uses the same modifier** and takes exactly the same path. */
            /* `@place`: this struct's storage lives on **a place** that outlives the value (today:
             * a pool of its own), so it can grow in place and its lifetime follows the scope it
             * sits in. `@poolObject` is the earlier spelling of the same label and stays accepted;
             * both set one flag, and nothing else in the compiler knows either word. */
            if (strcmp(nm->text, "place") == 0 || strcmp(nm->text, "poolObject") == 0) {
                poolObject = true;
                skipJunk(&p);
                continue;
            }
            /* `@buffer`: this struct is the **owning** form of a `new` buffer. It says two things:
             * the element type is the struct's single type parameter, and the struct declares exactly
             * one one-argument (`i64` count) constructor, which `new T[n]` in this struct's context
             * calls. Naming the attribute rather than the type keeps the compiler from knowing any
             * library type by name; two user-written buffer types take the same path. */
            if (strcmp(nm->text, "buffer") == 0) {
                buffer = true;
                skipJunk(&p);
                continue;
            }

            /* `@sharesStorage`: copying this by value makes **both copies share one piece
             * of storage** (the container family). Not forbidden, only warned about: the
             * user may know exactly what they are doing, but has to be told what this line
             * does. */
            /* `@value`: a value type that **owns** storage. A copy from a place goes through
             * `clone` instead of copying the fields; a temporary is still moved. */
            if (strcmp(nm->text, "value") == 0) {
                valueCopy = true;
                skipJunk(&p);
                continue;
            }
            if (strcmp(nm->text, "sharesStorage") == 0) {
                sharesStorage = true;
                skipJunk(&p);
                continue;
            }
            /* `@frozen`: this struct's layout **is C's layout** (field order, alignment, no
             * hidden fields). It is the ticket to cross a C boundary by value, and it is a
             * signature: the compiler does not inspect what is inside, the author carries the
             * cost -- the same class as `effects`. */
            if (strcmp(nm->text, "frozen") == 0) {
                if (frozen) {
                    ctxError(ctx, a->line, a->col, NULL, "`@frozen` appears twice");
                    return false;
                }
                frozen = true;
                skipJunk(&p);
                continue;
            }
            if (strcmp(nm->text, "builtin") == 0) {
                /* `@builtin`: the declaration is implemented by the compiler (no body).
                 * The real parsing happens in the annotation loop; this branch only lets it
                 * through so the whitelist does not reject it as unknown. */
                builtinDecl = true;
                skipJunk(&p);
                continue;
            }
            /* `@module`: this free function is **this module's entry point**. A single
             * binary may hold several entry points, and C `main` dispatches on
             * `./a -m <name>`; the name and description are optional and only feed
             * `--help`. Meaningless on other declarations, which reject it below (the same
             * shape as `@export`). */
            if (strcmp(nm->text, "module") == 0) {
                if (moduleEntry) {
                    ctxError(ctx, a->line, a->col, NULL, "`@module` appears twice");
                    return false;
                }
                moduleEntry = true;
                skipJunk(&p);
                if (accept(&p, "(")) {
                    skipJunk(&p);
                    Token *s = cur(&p);
                    if (s->kind != TK_STRING) {
                        ctxError(ctx, s->line, s->col,
                                 "Write the name the command line uses, as a string:"
                                 " `@module(\"demo\")`.",
                                 "`@module` takes an optional string name");
                        return false;
                    }
                    take(&p);
                    moduleName = s->text;
                    skipJunk(&p);
                    if (accept(&p, ",")) {
                        skipJunk(&p);
                        Token *d = cur(&p);
                        if (d->kind != TK_STRING) {
                            ctxError(ctx, d->line, d->col,
                                     "The description is one string literal; it is printed by"
                                     " `--help`.",
                                     "`@module` takes an optional string description");
                            return false;
                        }
                        take(&p);
                        moduleDesc = d->text;
                        skipJunk(&p);
                    }
                    if (!expect(&p, ")", "the list is `@module(\"name\", \"description\")`"))
                        return false;
                }
                skipJunk(&p);
                continue;
            }
            /* `@ext`: the type this `impl` block extends lives in **another module**.
             * Ownership has to be written out, because a type has exactly one set of methods
             * and the reader must be able to see in one line who provides them; when the
             * block sits in the type's own module, writing it is **not** allowed. */
            if (strcmp(nm->text, "ext") == 0) {
                if (implExt) {
                    ctxError(ctx, a->line, a->col, NULL, "`@ext` appears twice");
                    return false;
                }
                implExt = true;
                skipJunk(&p);
                continue;
            }
            if (strcmp(nm->text, "recursive") == 0 || strcmp(nm->text, "main") == 0) {
                ctxError(ctx, a->line, a->col,
                         "The annotation is designed but not implemented yet, and accepting"
                         " it without doing anything would say otherwise.",
                         "`@%s` is not implemented yet", nm->text);
                return false;
            }
            ctxError(ctx, a->line, a->col,
                     "The top-level annotations today are `@private` (hide a declaration from"
                     " other modules), `@inline` (on a function: the call must be inlined),"
                     " `@unchecked` (on a function: its index expressions lose the bounds check),"
                     " `@noCopy` (on a struct:"
                     " it may only be passed as `ref` / `mut ref`), `@poolObject` (on a struct: it owns"
                     " a pool), `@sharesStorage` (on a struct: a by-value copy shares storage) and"
                     " `@frozen` (on a struct: its layout is the C layout, so it may cross the C"
                     " boundary by value), `@export` (on a function: it becomes a C symbol that"
                     " C can call), `@module` (on a free function: it becomes this module's"
                     " entry point, run by `./a -m <name>`), and `@ext` (on an `impl` block"
                     " that extends a type of another module). `@overwrite` is for locals.",
                     "unknown top-level annotation `@%s`", nm->text);
            return false;
        }
        /* `@ext` only describes an `impl` block: on any other declaration it asserts
         * nothing, and ignoring it would let the reader believe "this type/function is owned
         * by another module". */
        if (implExt && !at(&p, "impl")) {
            Token *t = cur(&p);
            ctxError(p.ctx, t->line, t->col, NULL,
                     "Only an `impl` block extends a type owned by another module; a declaration"
                     " is owned by the file that writes it.");
            return false;
        }
        if (at(&p, "struct")) {
            if (moduleEntry) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "A type has no command line to be an entry for. Write `@module` on the"
                         " free function that the module should run.",
                         "`@module` goes on a free function");
                return false;
            }
            if (exported) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "A struct is storage, not code. Export the functions that operate on it.",
                         "`@export` goes on a function");
                return false;
            }
        /* `@unchecked` describes a function body, while a struct is a declaration: each
         * method chooses it separately on its own line. */
            if (fnUnchecked) {
                Token *t = cur(&p);
                ctxError(ctx, t->line, t->col,
                         "`@unchecked` describes a function **body**. Write it on each method"
                         " whose indexes are already known to be in range.",
                         "`@unchecked` applies to a function, not to a `struct`");
                return false;
            }
            StructDef *s = parseStruct(&p);
            if (!s) return false;
            s->isPrivate = isPrivate;
            s->noCopy    = noCopy;
            if (valueCopy && s->noCopy) {
                ctxError(ctx, s->line, 1,
                         "`@value` says a copy builds an independent value; `@noCopy` says there is"
                         " no second owner. Pick one.",
                         "`@value` and `@noCopy` cannot both describe one struct");
                return false;
            }
            if (s->dropHook && !s->noCopy) {
                ctxError(ctx, s->dropHook->line, 1,
                         "A type with two owners has no single moment to hand its state back."
                         " Mark the struct `@noCopy` first.",
                         "`@drop` needs a `@noCopy` struct");
                return false;
            }
            if (frozen) s->frozen = true;
            if (sharesStorage) s->sharesStorage = true;
            if (valueCopy) {
                s->valueCopy = true;
                /* The hook is a method of this struct, so it is resolved after the body: `clone`
                 * by name is the only signature the label can promise. */
                for (size_t i = 0; i < s->methods.len && !s->cloneHook; i++) {
                    FuncDef *m = *(FuncDef **)vecAt(&s->methods, i);
                    if (m && strcmp(m->name, "clone") == 0) s->cloneHook = m;
                }
                if (!s->cloneHook) {
                    ctxError(ctx, s->line, 1,
                             "A `@value` type owns storage, so a copy has to build an independent"
                             " value: write `fn clone(self: ref T) -> T`.",
                             "`@value` needs a `clone` method");
                    return false;
                }
            }
            if (poolObject) s->poolObject = true;
            if (buffer) {
                if (s->typeParams.len != 1) {
                    ctxError(ctx, s->line, 1,
                             "`new T[n]` has one element type, so the owning form has one type"
                             " parameter: write `@buffer struct buf<T>`.",
                             "`@buffer` needs a struct with exactly one type parameter");
                    return false;
                }
                s->buffer = true;
            }
            *(StructDef **)vecPush(&out->structs) = s;
        } else if (at(&p, "impl")) {
            /* An `impl` block declares nothing: `@private`/`@noCopy` describe declarations,
             * `@poolObject`/`@sharesStorage` describe storage, `@inline`/`@unchecked`
             * describe bodies. The only applicable one is `@ext` (already read above); for
             * the rest, saying so is better than silently ignoring them. */
            if (isPrivate || noCopy || sharesStorage || poolObject || fnInline || fnUnchecked || frozen
                || exported || moduleEntry) {
                Token *t = cur(&p);
                ctxError(ctx, t->line, t->col,
                         "An `impl` block adds methods to a type that is declared elsewhere, so it "
                         "declares no storage and no visibility of its own; `@ext` is the one"
                         " annotation it takes.",
                         "no annotation applies to an `impl` block");
                return false;
            }
            ImplDef *im = parseImpl(&p);
            if (!im) return false;
            im->ctx = p.ctx;
            im->isExt = implExt;
            *(ImplDef **)vecPush(&out->impls) = im;
        } else if (at(&p, "trait")) {
            /* Like an `impl` block, a trait only declares behaviour, so no annotation
             * applies. */
            if (isPrivate || fnInline || fnUnchecked || noCopy || sharesStorage || poolObject || frozen
                || exported || moduleEntry) {
                Token *t = cur(&p);
                ctxError(ctx, t->line, t->col,
                         "A `trait` declares method signatures only: it has no storage to hide"
                         " and nothing to copy.",
                         "no annotation applies to a `trait` declaration");
                return false;
            }
            TraitDef *tr = parseTrait(&p);
            if (!tr) return false;
            *(TraitDef **)vecPush(&out->traits) = tr;
        } else if (at(&p, "type")) {
            if (fnUnchecked) {
                Token *t = cur(&p);
                ctxError(ctx, t->line, t->col,
                         "`@unchecked` describes a function **body**, and an enum has none.",
                         "`@unchecked` applies to a function, not to a `type`");
                return false;
            }
            if (noCopy) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "Only a `struct` has a copy to forbid; an enum is copied as a value.",
                         "`@noCopy` goes on a `struct`");
                return false;
            }

            if (sharesStorage) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "Only a `struct` can share storage on copy; an enum has no storage.",
                         "`@sharesStorage` goes on a `struct`");
                return false;
            }
            if (poolObject) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "Only a `struct` owns a pool; an enum has no storage of its own.",
                         "`@poolObject` goes on a `struct`");
                return false;
            }
            if (frozen) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "`@frozen` promises a byte layout for a field list; an enum's C width is"
                         " implementation-defined, so there is nothing here to freeze. Use an i32"
                         " where the C side needs a number.",
                         "`@frozen` goes on a `struct`");
                return false;
            }
            if (exported) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "Only a function has a C symbol to hand out; an enum has no code.",
                         "`@export` goes on a function");
                return false;
            }
            if (moduleEntry) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "Only a function can be an entry point; an enum has no code.",
                         "`@module` goes on a free function");
                return false;
            }
            TypeDef *td = parseTypeDecl(&p);
            if (!td) return false;
            td->isPrivate = isPrivate;
            *(TypeDef **)vecPush(&out->types) = td;
        } else if (at(&p, "global") || at(&p, "let") || at(&p, "var")) {
        /* `global var` at the top level is redundant but legal: top level is already global
         * (the same thing stated twice is allowed for self-documentation). */
            (void)accept(&p, "global");
            if (exported) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "A global is a C object, and this language has no `@export` for objects yet"
                         " -- a shared symbol needs a name the two sides agree on, which is the"
                         " decision `@export` makes for functions.",
                         "`@export` goes on a function");
                return false;
            }
            if (moduleEntry) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "A global is a value, not something a command line can run.",
                         "`@module` goes on a free function");
                return false;
            }
            if (frozen) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "`@frozen` promises a byte layout for a field list; only a `struct` has one."
                         " A function **pointer** has a layout, and that type is `fn(A) -> R`.",
                         "`@frozen` goes on a `struct`");
                return false;
            }
            if (fnUnchecked) {
                Token *t = cur(&p);
                ctxError(ctx, t->line, t->col,
                         "`@unchecked` describes a function **body**, and a global is a value.",
                         "`@unchecked` applies to a function, not to a global");
                return false;
            }
            GlobalDef *g = parseGlobalDecl(&p);
            if (!g) return false;
            g->isPrivate = isPrivate;
            *(GlobalDef **)vecPush(&out->globals) = g;
        /* One declaration named several names: the remaining globals come from the same
         * declaration and share its visibility. */
            for (size_t i = 0; i < p.pendingGlobals.len; i++) {
                GlobalDef *extra = *(GlobalDef **)vecAt(&p.pendingGlobals, i);
                extra->isPrivate = isPrivate;
                *(GlobalDef **)vecPush(&out->globals) = extra;
            }
            p.pendingGlobals.len = 0;
        } else if (at(&p, "extern")) {
            if (exported) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "`extern!` already says this symbol comes **from** C; `@export` says the"
                         " opposite. An `extern!` declaration has no body to hand out.",
                         "`@export` and `extern!` contradict each other");
                return false;
            }
            if (moduleEntry) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "An `extern!` declaration has no body, so there is nothing to run.",
                         "`@module` needs a body");
                return false;
            }
            if (frozen) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "`@frozen` promises a byte layout for a field list; only a `struct` has one."
                         " A function **pointer** has a layout, and that type is `fn(A) -> R`.",
                         "`@frozen` goes on a `struct`");
                return false;
            }

            /* `extern!` has no body, so `@inline` and `@unchecked` are rejected rather than
             * ignored: accepting them would let the reader believe the requested
             * optimisation happened. */
            if (fnInline) {
                Token *t = cur(&p);
                ctxError(ctx, t->line, t->col,
                         "There is no body to inline: this declaration only names a function"
                         " that lives in another library. Mark the extC function that wraps"
                         " it.",
                         "`@inline` needs a body, and an `extern` declaration has none");
                return false;
            }
            if (fnUnchecked) {
                Token *t = cur(&p);
                ctxError(ctx, t->line, t->col,
                         "There is no body whose index checks could be skipped: this declaration"
                         " only names a function that lives in another library. Mark the extC"
                         " function that wraps it.",
                         "`@unchecked` needs a body, and an `extern` declaration has none");
                return false;
            }
            FuncDef *f = parseExtern(&p);
            if (!f) return false;
            f->isPrivate = isPrivate;
            *(FuncDef **)vecPush(&out->funcs) = f;
        } else if (at(&p, "fn") || at(&p, "@")) {
            if (frozen) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "`@frozen` promises a byte layout for a field list; only a `struct` has one."
                         " A function **pointer** has a layout, and that type is `fn(A) -> R`.",
                         "`@frozen` goes on a `struct`");
                return false;
            }
            bool inl = fnInline;
            bool unchk = fnUnchecked;
            if (!parseFuncAnnotations(&p, &inl, NULL, &unchk, NULL)) return false;
            if (!at(&p, "fn")) {
                Token *t = cur(&p);
                ctxError(ctx, t->line, t->col, NULL,
                         "an annotation on a function must be followed by `fn`, found `%s`",
                         shown(t));
                return false;
            }
            bool builtin = builtinDecl;
            builtinDecl = false;
            p.sawBuiltin = false;
            bool savedNoBody = p.noBody;
            if (builtin) p.noBody = true;      /* `@builtin`, like `extern!`: signature only */
            FuncDef *f = parseFunc(&p);
            p.noBody = savedNoBody;
            if (!f) return false;
            f->isInline  = inl;
            f->isUnchecked = unchk;
            f->isPrivate = isPrivate;
            f->isBuiltin = builtin;
            /* `@module`: the dispatcher looks the function up by name and calls it. Three
             * "no such entry point" shapes are rejected on the spot: generic (every instance
             * is a function), coroutine (the value is a frame handle), `@export` (that symbol
             * is for C, not for `-m`). The signature (returns `i32`, at most one
             * `slice<slice<u8>>` parameter) is verified in the checker. */
            if (moduleEntry) {
                if (exported) {
                    ctxError(ctx, f->line, 1,
                             "`@export` hands one symbol to C and `@module` hands one entry to the"
                             " command line; one function cannot be both.",
                             "`@export` and `@module` on the same function");
                    return false;
                }
                if (f->typeParams.len > 0) {
                    ctxError(ctx, f->line, 1,
                             "A generic function has one body per instantiation, so there is no one"
                             " function for the dispatcher to call.",
                             "`@module` on a generic function");
                    return false;
                }
                if (f->ret && f->ret->name && strcmp(f->ret->name, "coroutine") == 0) {
                    ctxError(ctx, f->line, 1,
                             "A coroutine's value is a frame handle driven by a task, not something"
                             " a command line can run.",
                             "`@module` on a coroutine");
                    return false;
                }
                f->isModuleEntry = true;
                f->moduleEntryName = moduleName;
                f->moduleDesc = moduleDesc;
            }
            /* `@export`: hand one symbol out. Three "there is no single C symbol" shapes are
             * rejected on the spot (a generic has one function per instantiation, a coroutine's
             * value is a frame handle, `extern!` is declared elsewhere) and methods are rejected
             * in their own member loop. */
            if (exported) {
                if (f->typeParams.len > 0) {
                    ctxError(ctx, f->line, 1,
                             "A generic function has one body per instantiation, so a bare name is"
                             " not one C symbol. Write a non-generic function for the signature you"
                             " want to export.",
                             "`@export` on a generic function");
                    return false;
                }
                if (f->ret && f->ret->name && strcmp(f->ret->name, "coroutine") == 0) {
                    ctxError(ctx, f->line, 1,
                             "A coroutine's value is a frame handle driven by a task, not a C"
                             " function C could call.",
                             "`@export` on a coroutine");
                    return false;
                }
                f->isExport = true;
            }
            *(FuncDef **)vecPush(&out->funcs) = f;
        } else {
            Token *t = cur(&p);
            ctxError(ctx, t->line, t->col,
                     "the top level allows `fn`, `struct`, `type`, and `let`/`var` (globals)",
                     "expected `fn`, `struct`, `type`, `let` or `var`, found `%s`", shown(t));
            return false;
        }
        skipJunk(&p);
    }
    return !ctx->hasError;
}

/**
 * @brief Parse a `struct` declaration: type parameters, fields, and methods.
 *
 * Methods and fields may alternate; the annotation loop reads `@inline`/`@private`/
 * `@unchecked` together, and the declaration that follows decides which of them receives
 * them.
 *
 * @param p  Parser.
 * @return   The new StructDef; `NULL` on error (the diagnostic has been reported).
 */
static StructDef *parseStruct(Parser *p) {
    Token *kw = take(p);
    Token *name = expectTypeName(p, "a struct name");
    if (!name) return NULL;

    StructDef *sd = (StructDef *)arenaAllocZero(p->arena, sizeof(StructDef));
    sd->name = name->text;
    sd->line = kw->line;
    vecInit(&sd->typeParams, p->arena, sizeof(void *));
    vecInit(&sd->fields, p->arena, sizeof(void *));
    vecInit(&sd->methods, p->arena, sizeof(void *));

        /* Type parameter list: `struct Pair<A, B> { ... }` */
    if (accept(p, "<")) {
        skipNl(p);
        for (;;) {
            Token *tp = expectIdent(p, "a type parameter name");
            if (!tp) return NULL;
            if (!startsUpper(tp->text)) {
                ctxError(p->ctx, tp->line, tp->col,
                         "Type parameters start with an uppercase letter, so they can never "
                         "collide with a type name (which is camelCase).",
                         "type parameter `%s` must start with an uppercase letter", tp->text);
                return NULL;
            }
            *(const char **)vecPush(&sd->typeParams) = tp->text;
            if (accept(p, ",")) { skipNl(p); continue; }
            break;
        }
        skipNl(p);
        if (!expect(p, ">", NULL)) return NULL;
    }

    if (!expect(p, "{", NULL)) return NULL;
    skipJunk(p);
    while (!at(p, "}")) {
        /* Annotations may precede a **method or a field**. `@private` on a field is what keeps
         * a container's storage to itself, so both kinds share one annotation loop and the
         * declaration that follows decides which one it was. */
        bool inl = false;
        bool priv = false;
        bool unchk = false;
        bool dropA = false;
        p->sawBuiltin = false;      /* reset per member: the flag is sticky otherwise */
        if (at(p, "@") && !parseFuncAnnotations(p, &inl, &priv, &unchk, &dropA)) return NULL;
        if (p->sawBuiltin) {
            Token *b = cur(p);
            ctxError(p->ctx, b->line, b->col,
                     "A method is a function with a body, so nothing has to be supplied by the"
                     " compiler. `@builtin` is for the primitives a library declares and the"
                     " compiler implements (`std::sys::heap::extc_viewOf`).",
                     "`@builtin` applies to a free function, not to a method");
            return NULL;
        }
        if (at(p, "fn")) {
            FuncDef *m = parseFunc(p);
            if (!m) return NULL;
            m->isInline  = inl;
            m->isPrivate = priv;
            m->isUnchecked = unchk;
            m->owner = sd;
            if (dropA) {
                /* The `@noCopy` requirement is checked by the caller: the struct's own annotations
                 * land after its body has been parsed, so the flag is not visible here yet. */
                if (sd->dropHook) {
                    ctxError(p->ctx, m->line, 1, NULL,
                             "`%s` already has an `@drop` method", sd->name);
                    return NULL;
                }
                sd->dropHook = m;
            }
            *(FuncDef **)vecPush(&sd->methods) = m;
            skipJunk(p);
            /* A member list that accepts `,` after a field has to accept it after a method too,
             * or `struct P { x: i64, fn f() {} , }` is a syntax error for no visible reason. */
            if (at(p, ",")) { take(p); skipJunk(p); }
            continue;
        }

        if (inl) {
            Token *t = cur(p);
            ctxError(p->ctx, t->line, t->col,
                     "`@inline` asks for a call to be expanded at the call site, and a field is"
                     " never called.",
                     "`@inline` applies to a function, not to a field");
            return NULL;
        }
        if (unchk) {
            Token *t = cur(p);
            ctxError(p->ctx, t->line, t->col,
                     "`@unchecked` says the index expressions **inside a body** are not"
                     " bounds-checked, and a field has no body.",
                     "`@unchecked` applies to a function, not to a field");
            return NULL;
        }

        Token *fname = expectIdent(p, "a field name");
        if (!fname) return NULL;
        if (!expect(p, ":", NULL)) return NULL;
        Type *ft = parseType(p);
        if (!ft) return NULL;

        FieldDef *fd = (FieldDef *)arenaAllocZero(p->arena, sizeof(FieldDef));
        fd->name = fname->text;
        fd->type = ft;
        fd->line = fname->line;
        fd->isPrivate = priv;

        /* `effects Addr=… Cont=…` on a field: the signature of the function pointer this slot will
         * hold ( -- the callee is not a declaration, the host fills the slot at run
         * time). The clause is read by the **same** routine a declaration uses, into a throw-away
         * `FuncDef`, and only the masks are kept: one parser for one syntax. It goes on the field's
         * own line, like every other part of a declaration here. */
        if (at(p, "effects")) {
            if (ft->kind != TY_FN) {
                Token *e = cur(p);
                ctxError(p->ctx, e->line, e->col,
                         "An `effects` clause describes what the callee does with the pointers it is"
                         " given, so there has to be a callee: declare the field as `fn(...) -> R`"
                         " first.",
                         "`effects` on field `%s`, which is not a function pointer", fname->text);
                return NULL;
            }
            FuncDef tmp;
            memset(&tmp, 0, sizeof tmp);
            bool sawThread = false;
            if (!parseEffectsClause(p, &tmp, &sawThread)) return NULL;
            if (sawThread) {
                ctxError(p->ctx, fname->line, fname->col,
                         "`Thread` says whether a call touches cross-thread shared state, and the"
                         " check that reads it (which calls may appear in a worker) is about"
                         " **resolved** calls. A slot filled at run time is not one, so the clause"
                         " would be recorded and never read -- write it when that check grows a case"
                         " for function pointers.",
                         "`Thread=` on a field is not used yet");
                return NULL;
            }
            fd->hasEffects  = tmp.hasEffects;
            fd->effAddrMask = tmp.extAddrMask;
            fd->effContMask = tmp.extContMask;
            fd->effRetFresh = tmp.extRetFresh;   /* the return side of `Addr`, same clause */
        }
        *(FieldDef **)vecPush(&sd->fields) = fd;
        skipJunk(p);
        /* The separator is optional: fields are usually one per line, but `struct P { x: i64,
         * y: i64 }` is what every other language in this family accepts (and what the struct
         * **literal** syntax right above already requires). Accepting both keeps the two ends
         * of the same concept from disagreeing. */
        if (at(p, ",")) { take(p); skipJunk(p); }
    }
    if (!expect(p, "}", NULL)) return NULL;
    return sd;
}

/**
 * @brief Read a possibly qualified name into `out`: `point`, `i64`, `io::istream`.
 *
 * The two names an `impl` block may carry (`impl Type`, `impl Trait for Type`) share this
 * helper: the rules are identical (the block's name, the unit's imports, and the type's own
 * arguments are all read the same way), qualified names are resolved through this unit's
 * imports, and what the name refers to is the checker's business -- it holds the type table.
 *
 * @param p     Parser.
 * @param out   Buffer the name is appended to.
 * @param what  How diagnostics call it, e.g. "a type name after `for`".
 * @return      `false` on error (the diagnostic has been reported).
 */
static bool parseDottedName(Parser *p, Buf *out, const char *what) {
    if (cur(p)->kind != TK_IDENT && cur(p)->kind != TK_TYPE) {
        Token *t = cur(p);
        ctxError(p->ctx, t->line, t->col, NULL, "expected %s, found `%s`", what, shown(t));
        return false;
    }
    bufPuts(out, take(p)->text);
    while (at(p, "::")) {
        take(p);
        if (cur(p)->kind != TK_IDENT && cur(p)->kind != TK_TYPE) {
            Token *t = cur(p);
            ctxError(p->ctx, t->line, t->col, NULL,
                     "expected a name after `::`, found `%s`", shown(t));
            return false;
        }
        bufPuts(out, "::");
        bufPuts(out, take(p)->text);
    }
    return true;
}

/**
 * @brief Parse a `trait Name { fn ... }` declaration.
 *
 * Methods have **signatures only**, and the parser already has the switch: setting
 * `p->noBody` around `parseFunc` is exactly how an `extern!` declaration is parsed without a
 * body, so trait methods need no second parsing path.
 *
 * No type is resolved here. `Self` in a signature is stored as written; it means something
 * once some `impl` supplies the implementing type (`TraitDef`).
 *
 * @param p  Parser.
 * @return   The new TraitDef; `NULL` on error (the diagnostic has been reported).
 */
static TraitDef *parseTrait(Parser *p) {
    Token *kw = take(p);
    /* `expectTypeName` is deliberately not used here: type names are camelCase, and a leading
     * uppercase letter is reserved for type parameters. A trait name is a **third** kind of
     * name, taken as written, so the three namespaces cannot collide. */
    if (cur(p)->kind != TK_IDENT) {
        Token *t = cur(p);
        ctxError(p->ctx, t->line, t->col, NULL,
                 "expected a trait name after `trait`, found `%s`", shown(t));
        return NULL;
    }
    Token *name = take(p);
    if (!startsUpper(name->text)) {
        ctxError(p->ctx, name->line, name->col,
                 "Trait names start with a capital letter, which is what keeps them from ever"
                 " colliding with a type name (camelCase) or a type parameter.",
                 "trait name `%s` must start with a capital letter", name->text);
        return NULL;
    }
    TraitDef *tr = (TraitDef *)arenaAllocZero(p->arena, sizeof(TraitDef));
    tr->name = name->text;
    tr->line = kw->line;
    vecInit(&tr->typeParams, p->arena, sizeof(const char *));
    vecInit(&tr->methods, p->arena, sizeof(void *));
    if (at(p, "<")) {
        take(p);
        skipJunk(p);
        for (;;) {
            Token *tp = cur(p);
            if (tp->kind != TK_IDENT) {
                ctxError(p->ctx, tp->line, tp->col, NULL,
                         "expected a type parameter name, found `%s`", shown(tp));
                return NULL;
            }
            take(p);
            *(const char **)vecPush(&tr->typeParams) = tp->text;
            skipJunk(p);
            if (at(p, ",")) { take(p); skipJunk(p); continue; }
            break;
        }
        if (!expect(p, ">", NULL)) return NULL;
    }
    if (!expect(p, "{", NULL)) return NULL;
    skipJunk(p);
    while (!at(p, "}")) {
        if (!at(p, "fn")) {
            Token *t = cur(p);
            ctxError(p->ctx, t->line, t->col,
                     "A trait declares behaviour, never storage or visibility: the implementing"
                     " type owns its fields, and `@private`/`@inline` belong where there is an"
                     " implementation.",
                     "expected `fn` in the `trait` body, found `%s`", shown(t));
            return NULL;
        }
        bool saved = p->noBody;
        p->noBody = true;                       /* the `extern!` switch: signature only */
        FuncDef *m = parseFunc(p);
        p->noBody = saved;
        if (!m) return NULL;
        /* `Self` **is** a type parameter of the trait (`TraitDef`): giving every
         * signature its own parameter list is what lets the existing `resolveSignature` resolve
         * `ref Self` with no knowledge of traits at all. */
        vecInit(&m->typeParams, p->arena, sizeof(void *));
        *(const char **)vecPush(&m->typeParams) = "Self";
        /* `trait Codec<T>`: the explicit parameters follow the implicit `Self`, so a signature that
         * mentions `T` resolves through the very same list. */
        for (size_t pi = 0; pi < tr->typeParams.len; pi++)
            *(const char **)vecPush(&m->typeParams) = *(const char **)vecAt(&tr->typeParams, pi);
        *(FuncDef **)vecPush(&tr->methods) = m;
        skipJunk(p);
    }
    if (!expect(p, "}", NULL)) return NULL;
    return tr;
}

/**
 * @brief Parse an `impl Type { fn ... }` block.
 *
 * A method written here joins **the type's single method set**, exactly as if it were written
 * inside the type body; only where it may be written differs (and a built-in scalar, which
 * has no body, gains methods this way). The name, type parameters, and `for` clause are
 * recorded as written; what the name refers to is left to the checker.
 *
 * @param p  Parser.
 * @return   The new ImplDef; `NULL` on error (the diagnostic has been reported).
 */
static ImplDef *parseImpl(Parser *p) {
    Token *kw = take(p);
    /* The name may be **qualified** (`impl io::istream { ... }`), because an impl block
     * usually does not live in the same module as the type it extends. Two details:
     *   - the ten scalar names are lexed as `TK_TYPE`, not `TK_IDENT` (`i64`, `u8`, ...), so
     *     the first segment accepts both kinds;
     *   - later segments are module or type names, and a type name may be lowercase here
     *     (`stl::string`), which `expectTypeName` would not accept.
     * Whether the name is in fact a type belongs to the checker, which owns the type table
     * (it resolves the qualified form as well). */
    ImplDef *im = (ImplDef *)arenaAllocZero(p->arena, sizeof(ImplDef));
    vecInit(&im->typeParams, p->arena, sizeof(const char *));
    /* `traitArgs` is filled **before** the `for` clause, so its arena has to be supplied
     * here: initializing `typeArgs` below comes too late (pushing into a Vec whose arena is
     * NULL dereferences the null arena in `arenaAlloc`). */
    vecInit(&im->traitArgs, p->arena, sizeof(void *));
    /* `impl<T> pair<T> { ... }`: the block's own type parameters come first. The methods land on
     * the generic declaration's body, so their signatures resolve against that body's parameters
     * -- hence the names have to agree with the declaration's (the checker says so at the attach
     * point, rather than leaving a puzzling "unknown type `U`" behind). */
    if (at(p, "<")) {
        take(p);
        skipJunk(p);
        for (;;) {
            Token *tp = cur(p);
            if (tp->kind != TK_IDENT) {
                ctxError(p->ctx, tp->line, tp->col, NULL,
                         "expected a type parameter name, found `%s`", shown(tp));
                return NULL;
            }
            take(p);
            *(const char **)vecPush(&im->typeParams) = tp->text;
            skipJunk(p);
            if (at(p, ",")) { take(p); skipJunk(p); continue; }
            break;
        }
        if (!expect(p, ">", NULL)) return NULL;
    }

    Buf target;
    bufInit(&target, p->arena);
    /* A fixed array is a **shape**, not a declaration: `[N]T` owns no method set, so an `impl`
     * block naming one has nowhere to attach. Say that here instead of letting the generic
     * dotted-name error print "found `[`", which describes the syntax and not the rule. */
    if (at(p, "[")) {
        Token *t = cur(p);
        ctxError(p->ctx, t->line, t->col,
                 "An `impl` block attaches methods to a type that can own them: a struct, a "
                 "builtin scalar, or an instance of a generic type such as `slice<u8>`. A fixed "
                 "array is a shape, so write the method on what the array holds, or on the view "
                 "`slice<T>` it becomes when you slice it.",
                 "`impl` cannot extend a fixed array `[N]T`");
        return NULL;
    }
    if (!parseDottedName(p, &target, "a type name after `impl`")) return NULL;

    im->typeName = bufCstr(&target);
    /* `impl Trait for Type`: the name after `impl` is the **trait**, and the type follows
     * `for`. `typeName` is ultimately always the type the methods attach to (the rest of the
     * pipeline needs only that name), while `traitName` is what the conformance check reads. */
    /* The `<...>` after the first name belongs to the **trait** when a `for` follows
     * (`impl Codec<i64> for box`) and to the **type** otherwise (`impl pair<T>`, `impl slice<u8>`).
     * Which one it is only shows up after parsing it, so it is collected first and filed second --
     * filing it as a trait argument unconditionally made `impl<T> pair<T>` lose the type's own
     * arguments and report "`pair` is generic and needs type arguments". */
    Vec leadArgs;
    vecInit(&leadArgs, p->arena, sizeof(void *));
    if (at(p, "<")) {
        take(p);
        skipJunk(p);
        for (;;) {
            Type *arg = parseType(p);
            if (!arg) return NULL;
            *(Type **)vecPush(&leadArgs) = arg;
            skipJunk(p);
            if (at(p, ",")) { take(p); skipJunk(p); continue; }
            break;
        }
        if (!expect(p, ">", NULL)) return NULL;
    }
    bool hadFor = false;
    if (at(p, "for")) {
        im->traitArgs = leadArgs;
        hadFor = true;
        take(p);
        Buf ty;
        bufInit(&ty, p->arena);
        if (!parseDottedName(p, &ty, "a type name after `for`")) return NULL;
        im->traitName = im->typeName;
        im->typeName  = bufCstr(&ty);
    }
    im->line = kw->line;
    vecInit(&im->methods, p->arena, sizeof(void *));
    vecInit(&im->typeArgs, p->arena, sizeof(void *));
    /* No `for`: the leading `<...>` is the **type's** own arguments. This statement has to
     * come **after** the two `vecInit` calls above; before them it would be silently dropped. */
    if (!hadFor && leadArgs.len) im->typeArgs = leadArgs;

    /* `impl slice<u8> { ... }`: the target's type arguments. They are parsed here and resolved by
     * the checker the way an annotation's are (`ttResolve` recurses into `targs`), because only
     * the checker owns the type table: `slice<u8>` names an instance, while `pair<T>` would name
     * a parameter that no block-level binding can supply. Deciding that here would mean guessing
     * at names the parser cannot resolve. */
    if (at(p, "<")) {
        take(p);
        skipJunk(p);
        for (;;) {
            Type *arg = parseType(p);
            if (!arg) return NULL;
            *(Type **)vecPush(&im->typeArgs) = arg;
            skipJunk(p);
            if (at(p, ",")) { take(p); skipJunk(p); continue; }
            break;
        }
        if (!expect(p, ">", NULL)) return NULL;
    }

    if (!expect(p, "{", NULL)) return NULL;
    skipJunk(p);
    while (!at(p, "}")) {
        if (at(p, "fn") || at(p, "@")) {
            bool inl = false;
            bool priv = false;
            bool unchk = false;
            p->sawBuiltin = false;  /* reset per member: the flag is sticky otherwise */
            if (!parseFuncAnnotations(p, &inl, &priv, &unchk, NULL)) return NULL;
            if (p->sawBuiltin) {
                Token *b = cur(p);
                ctxError(p->ctx, b->line, b->col,
                         "A method is a function with a body, so nothing has to be supplied by"
                         " the compiler. `@builtin` is for the primitives a library declares and"
                         " the compiler implements (`std::sys::heap::extc_viewOf`).",
                         "`@builtin` applies to a free function, not to a method");
                return NULL;
            }
            if (!at(p, "fn")) {
                Token *t = cur(p);
                ctxError(p->ctx, t->line, t->col, NULL,
                         "an annotation on a function must be followed by `fn`, found `%s`",
                         shown(t));
                return NULL;
            }
            FuncDef *m = parseFunc(p);
            if (!m) return NULL;
            m->isInline    = inl;
            m->isPrivate   = priv;
            m->isUnchecked = unchk;
            /* `owner` is filled in when the block is attached: the target type's name is not
             * resolved yet (an impl may even appear before the type it extends). */
            *(FuncDef **)vecPush(&im->methods) = m;
            skipJunk(p);
            continue;
        }
        /* Storage stays in the declaration: one place decides a type's layout, and this is not
         * that place. Saying so keeps a stray field from being silently dropped. */
        Token *t = cur(p);
        ctxError(p->ctx, t->line, t->col,
                 "An `impl` block adds behaviour, never storage: the type's fields are declared "
                 "in its own body.",
                 "expected `fn` in the `impl` block, found `%s`", shown(t));
        return NULL;
    }
    if (!expect(p, "}", NULL)) return NULL;
    return im;
}

/**
 * @brief Parse a `type Name = | v1 | v2(T)` variant type declaration.
 *
 * The `|` before the first variant may be omitted.
 *
 * @param p  Parser.
 * @return   The new TypeDef; `NULL` on error (the diagnostic has been reported).
 */
static TypeDef *parseTypeDecl(Parser *p) {
    Token *kw = take(p);                    /* type */
    Token *name = expectTypeName(p, "a type name");
    if (!name) return NULL;

    TypeDef *td = (TypeDef *)arenaAllocZero(p->arena, sizeof(TypeDef));
    td->name = name->text;
    td->line = kw->line;
    vecInit(&td->typeParams, p->arena, sizeof(void *));
    vecInit(&td->variants, p->arena, sizeof(void *));

        /* Type parameter list: `type option<T> = | none | some(T)`, the same shape as on a
         * struct. */
    if (accept(p, "<")) {
        skipNl(p);
        for (;;) {
            Token *tp = expectIdent(p, "a type parameter name");
            if (!tp) return NULL;
            if (!startsUpper(tp->text)) {
                ctxError(p->ctx, tp->line, tp->col,
                         "Type parameters start with an uppercase letter, so they can never "
                         "collide with a type name (which is camelCase).",
                         "type parameter `%s` must start with an uppercase letter", tp->text);
                return NULL;
            }
            *(const char **)vecPush(&td->typeParams) = tp->text;
            if (accept(p, ",")) { skipNl(p); continue; }
            break;
        }
        skipNl(p);
        if (!expect(p, ">", NULL)) return NULL;
    }

    if (!expect(p, "=", NULL)) return NULL;
    skipNl(p);
    accept(p, "|");
    skipNl(p);

    for (;;) {
        Token *v = expectIdent(p, "a variant name");
        if (!v) return NULL;

        Variant *va = (Variant *)arenaAllocZero(p->arena, sizeof(Variant));
        va->name = v->text;
        va->line = v->line;
        vecInit(&va->types, p->arena, sizeof(void *));

        /* Payload: `| circle(f64) | rect(f64, f64)`. The parentheses hold types, ordered by
         * position with no field names, and `match` arms bind in that same order. */
        skipNl(p);
        if (at(p, "(")) {
            take(p);
            skipNl(p);
            while (!at(p, ")")) {
                Type *pt = parseType(p);
                if (!pt) return NULL;
                *(Type **)vecPush(&va->types) = pt;
                skipNl(p);
                if (accept(p, ",")) { skipNl(p); continue; }
                break;
            }
            if (!expect(p, ")", NULL)) return NULL;
        }
        *(Variant **)vecPush(&td->variants) = va;

        skipNl(p);
        if (accept(p, "|")) { skipNl(p); continue; }
        break;
    }
    return td;
}

/**
 * @brief Whether `s` starts with an uppercase ASCII letter.
 *
 * @param s  String to check.
 * @return   `true` when the first character is an uppercase ASCII letter.
 */
static bool startsUpper(const char *s) { return s[0] >= 'A' && s[0] <= 'Z'; }

/**
 * @brief Consume a type name, which must start with a lowercase letter.
 *
 * Type names are camelCase and type parameters start uppercase, so the two sets are
 * disjoint: a type parameter can never collide with a type name. Enforcing the rule here is
 * what rules out `struct T` and `struct box<T>` side by side -- in the latter the inner `T`
 * would shadow the outer one, and code that compiles but cannot be read is a design defect.
 *
 * @param p     Parser.
 * @param what  How diagnostics call this name.
 * @return      The consumed token; `NULL` when it is missing or starts uppercase (the
 *              diagnostic has been reported).
 */
static Token *expectTypeName(Parser *p, const char *what) {
    Token *t = expectIdent(p, what);
    if (!t) return NULL;
    if (startsUpper(t->text)) {
        ctxError(p->ctx, t->line, t->col,
                 "Type names are camelCase (lowercase first letter). "
                 "A leading uppercase letter is reserved for type parameters, "
                 "so the two can never collide.",
                 "type name `%s` must start with a lowercase letter", t->text);
        return NULL;
    }
    return t;
}

static bool atShift(Parser *p, const char *ch, const char **op);

/**
 * @brief Consume a function or method name: an identifier, or an overloadable operator
 *        (e.g. `fn ==(self, other)`).
 *
 * An operator is written as itself, so the definition is visible at its declaration; an
 * implicit convention would force the reader to look elsewhere. Comparison, arithmetic,
 * streaming, and subscript operators are all accepted; `<<` / `>>` are two `<` / `>` tokens
 * and `[]` / `[]=` are three or four ordinary tokens (see `atShift`), all joined into one
 * name here -- lookup and name mangling key on this spelling.
 *
 * @param p  Parser.
 * @return   The consumed name token; `NULL` when it is not a legal name (the diagnostic has
 *           been reported).
 */
static Token *expectFuncName(Parser *p) {
    if (atKind(p, TK_IDENT)) return take(p);
    /* Operator overloading: the name of an overriding method **is the operator**.
     * `==` / `!=` came first; the ordering and arithmetic operators take the same road,
     * so a container or a generic body can sort and add what it holds. `<<` and `>>` are
     * here for streams: `cout << x` and `cin >> x` are the shape users of other languages
     * expect, and a stream is an ordinary type with ordinary methods. The set stays small
     * on purpose, but it grew by two: `[]` and `[]=` let a container be read and written with the
     * syntax everyone already knows (`m[k]`), and they are **independent** names -- `[]` is looked
     * up on a read, `[]=` on an assignment, so a type may define either one without the other. A
     * type that defines `[]` but not `[]=` simply cannot be assigned through. */
    /* `<<` and `>>` are two `<` / `>` tokens rather than one (see atShift: fusing them
     * would make `box<box<i32>>` unparsable), so they are recognized as a pair here too. */
    const char *shift = NULL;
    if (atShift(p, "<", &shift) || atShift(p, ">", &shift)) {
        Token *tok = cur(p);
        take(p);
        take(p);
        tok->text = shift;              /* `<<` / `>>` are fixed strings */
        return tok;
    }
    if (at(p, "==") || at(p, "!=") || at(p, "<") || at(p, "<=") ||
        at(p, ">")  || at(p, ">=") || at(p, "+") || at(p, "-") ||
        at(p, "*")  || at(p, "/")  || at(p, "%")) return take(p);
    /* The compound forms are statement-level hooks: `x += y` is `x = x + y` unless the type says
     * otherwise, and saying otherwise means defining `fn +=` on a `mut ref` receiver. They are
     * separate names from `+` on purpose -- `+` builds a value, `+=` may write in place. */
    if (at(p, "+=") || at(p, "-=") || at(p, "*=") || at(p, "/=") || at(p, "%=")) return take(p);
    /* `[]` and `[]=` arrive as three or four ordinary tokens, so they are fused here the same way
     * `<<` is: the operator spelling is what the lookup and the mangler key on. */
    if (at(p, "[")) {
        Token *tok = cur(p);
        take(p);
        if (!at(p, "]")) {
            ctxError(p->ctx, cur(p)->line, cur(p)->col,
                     "the subscript operator is spelled `[]` (and its writing form `[]=`)",
                     "expected `]`, found `%s`", shown(cur(p)));
            return NULL;
        }
        take(p);
        if (at(p, "=")) {
            take(p);
            tok->text = "[]=";
        } else {
            tok->text = "[]";
        }
        return tok;
    }
    Token *t = cur(p);
    ctxError(p->ctx, t->line, t->col,
             "only comparisons (`==` `!=` `<` `<=` `>` `>=`), arithmetic (`+` `-` `*` `/` `%`), the stream operators (`<<` `>>`) and the subscript operators (`[]` `[]=`) can be overloaded",
             "expected a function name, found `%s`", shown(t));
    return NULL;
}

/**
 * @brief Read the annotations that may precede a function declaration: `@inline`,
 *        `@builtin`, `@private`, `@unchecked`.
 *
 * A declaration is not a statement, so function annotations need their own position; this
 * function is called from the three places that parse declarations: the module body, a struct
 * body, and `extern!`.
 *
 * An unknown annotation is always an error rather than a warning: an annotation is an
 * instruction to the compiler written in the source, and an ignored typo would leave the
 * reader believing the requested thing happened. `@recursive` / `@main` are designed but not
 * implemented, and that too is said out loud rather than silently accepted. `@unchecked`
 * speaks about the **function body**, so it is read where a body can be declared and rejected
 * where it cannot (`extern!` declarations, fields): those call sites pass `NULL` as
 * `outUnchecked`, and reaching it there is an error for the same reason as the `@private`
 * branch below -- the reader must not believe the request was honoured.
 *
 * @param p             Parser.
 * @param outInline     When not `NULL`, receives whether `@inline` was seen.
 * @param outPrivate    When not `NULL`, receives whether `@private` was seen.
 * @param outUnchecked  When not `NULL`, receives whether `@unchecked` was seen.
 * @return              `true` on success; `false` on error (the diagnostic has been
 *                      reported).
 */
static bool parseFuncAnnotations(Parser *p, bool *outInline, bool *outPrivate,
                                 bool *outUnchecked, bool *outDrop) {
    /* The flags are not reset here: the caller may already have seen `@inline` in the
     * top-level annotation loop (which reads `@private` and `@inline` together before it knows
     * whether the declaration is a function), and clearing it would make `@inline fn f()` look
     * as if nothing had been written. */
    while (at(p, "@")) {
        Token *a = take(p);
        Token *nm = expectIdent(p, "an annotation name");
        if (!nm) return false;
        if (strcmp(nm->text, "inline") == 0) {
            if (*outInline) {
                ctxError(p->ctx, a->line, a->col, NULL,
                         "`@inline` appears twice on the same function");
                return false;
            }
            *outInline = true;
            skipNl(p);
            continue;
        }
        if (strcmp(nm->text, "builtin") == 0) {
            /* `@builtin`: the declaration is implemented by the compiler, so it has **no
             * function body** (`noBody`, the same path as `extern!`). It exists for the same
             * reason as `alloc<T>`: the code to be generated must have that function named by
             * the compiler (the parallel worker trampoline) and a library cannot supply it --
             * but the declaration still lives in the library, where it can be discovered and
             * documented. */
            if (p->sawBuiltin) {
                ctxError(p->ctx, a->line, a->col, NULL, "`@builtin` appears twice on the same function");
                return false;
            }
            p->sawBuiltin = true;
            skipNl(p);
            continue;
        }
        if (strcmp(nm->text, "drop") == 0) {
            if (!outDrop) {
                ctxError(p->ctx, a->line, a->col,
                         "It runs when a value of the type leaves its block, so it belongs to a"
                         " method of a `struct`.",
                         "`@drop` applies to a method of a `struct`");
                return false;
            }
            if (*outDrop) {
                ctxError(p->ctx, a->line, a->col, NULL,
                         "`@drop` appears twice on the same method");
                return false;
            }
            *outDrop = true;
            skipNl(p);
            continue;
        }
        if (strcmp(nm->text, "private") == 0) {
            /* `@private` on a **member**: a method the type keeps for itself, or a field.
             * Declarations are public by default, so hiding one has to be written out -- the
             * same rule as at the top level, one level down. */
            if (!outPrivate) {
                ctxError(p->ctx, a->line, a->col,
                         "Use it on a struct field or method, where there is something to hide.",
                         "`@private` does not apply to this declaration");
                return false;
            }
            if (*outPrivate) {
                ctxError(p->ctx, a->line, a->col, NULL, "`@private` appears twice");
                return false;
            }
            *outPrivate = true;
            skipNl(p);
            continue;
        }
        if (strcmp(nm->text, "unchecked") == 0) {
            /* `@unchecked` describes a **body**: it is the body's index expressions that lose
             * their bounds check. Where there is a body to describe, the flag is set; where
             * there is none (an `extern!` declaration) saying so beats accepting a request that
             * nothing can carry out. */
            if (!outUnchecked) {
                ctxError(p->ctx, a->line, a->col,
                         "This declaration only names a function that lives elsewhere, so there"
                         " is no body whose index checks could be skipped. Mark the extC function"
                         " that has one.",
                         "`@unchecked` needs a body");
                return false;
            }
            if (*outUnchecked) {
                ctxError(p->ctx, a->line, a->col, NULL,
                         "`@unchecked` appears twice on the same function");
                return false;
            }
            *outUnchecked = true;
            skipNl(p);
            continue;
        }
        if (strcmp(nm->text, "recursive") == 0 || strcmp(nm->text, "main") == 0) {
            ctxError(p->ctx, a->line, a->col,
                     "The annotation is designed but not implemented yet, and accepting it"
                     " without doing anything would say otherwise.",
                     "`@%s` is not implemented yet", nm->text);
            return false;
        }
        /* `@export` on a **member**: this function reads annotations for a function declaration
         * wherever one may appear, and a method is the one place where the C symbol is not the name
         * written here (its C name carries the owner: `point_dist`). Saying which symbol the author
         * would have to write beats "unknown annotation", which reads like a typo. */
        if (strcmp(nm->text, "export") == 0 && outPrivate) {
            ctxError(p->ctx, a->line, a->col,
                     "A method is reached through its receiver, so its C name carries the owner"
                     " (`point_dist`); there is no single symbol to hand out. Write a free function"
                     " that forwards to it and export that one.",
                     "`@export` goes on a free function, not on a method");
            return false;
        }
        /* `@module` on a **member** or an `extern!`: the dispatcher calls the entry with
         * `(argc, argv)` and no receiver. Saying which function to write beats "unknown
         * annotation", which reads like a typo. */
        if (strcmp(nm->text, "module") == 0) {
            ctxError(p->ctx, a->line, a->col,
                     "The dispatcher calls the entry with `(argc, argv)` and no receiver."
                     " Write a free function for the entry and forward to the method.",
                     "`@module` goes on a free function, not on a method");
            return false;
        }
        ctxError(p->ctx, a->line, a->col,
                 "Annotations are compile-time instructions written in the source, so a"
                 " typo must not be silently ignored. On a function they are `@inline`"
                 " (the call must be inlined), `@unchecked` (this body's indexes are not"
                 " bounds-checked) and `@module` (this free function is an entry point the"
                 " command line can run).",
                 "unknown annotation `@%s` on a function -- `@inline`, `@unchecked` and"
                 " `@module` exist today",
                 nm->text);
        return false;
    }
    return true;
}

/**
 * @brief Parse a `fn` declaration: name, type parameters, parameters, return type, and body.
 *
 * When `p->noBody` is set (`extern!` or `@builtin`) parsing stops after the return type and
 * `fd->body` stays `NULL`. Reusing this function instead of copying it keeps parameters,
 * return type, and generic table from drifting apart between two copies.
 *
 * @param p  Parser.
 * @return   The new FuncDef; `NULL` on error (the diagnostic has been reported).
 */
static FuncDef *parseFunc(Parser *p) {
    Token *kw = take(p);
    Token *name = expectFuncName(p);
    if (!name) return NULL;

    FuncDef *fd = (FuncDef *)arenaAllocZero(p->arena, sizeof(FuncDef));
    fd->name = name->text;
    fd->line = kw->line;
    fd->ret = NULL;
    fd->ctx  = p->ctx;                         /* so later passes report in this file */
    fd->srcPath = p->ctx ? p->ctx->path : NULL; /* the file a run-time check will name */
    vecInit(&fd->params, p->arena, sizeof(void *));
    vecInit(&fd->typeParams, p->arena, sizeof(void *));
    vecInit(&fd->constParams, p->arena, sizeof(void *));
    vecInit(&fd->targs, p->arena, sizeof(void *));
        /* `fn f<T, U>(...)`: a free function's type parameters, the same table shape a struct
         * carries. */
    if (accept(p, "<")) {
        skipNl(p);
        for (;;) {
            /* `const N: i64`: a parameter that names a value. The syntax is accepted here so the
             * checker can say "not supported yet" at the declaration instead of the parser saying
             * "expected `>`" at the colon -- the reservation is part of the language's surface. */
            if (at(p, "const")) {
                take(p);
                Token *cp = expectIdent(p, "a constant parameter name (uppercase, e.g. `N`)");
                if (!cp) return NULL;
                if (!startsUpper(cp->text)) {
                    ctxError(p->ctx, cp->line, cp->col, NULL,
                             "constant parameter `%s` must start with an uppercase letter", cp->text);
                    return NULL;
                }
                if (!expect(p, ":", NULL)) return NULL;
                Token *cty = take(p);
                if (!cty || strcmp(cty->text, "i64") != 0) {
                    ctxError(p->ctx, cty ? cty->line : cp->line, cty ? cty->col : cp->col, NULL,
                             "a `const` parameter must have type `i64` (found `%s`)",
                             cty ? cty->text : "nothing");
                    return NULL;
                }
                /* A `const` parameter is also a type parameter for every later stage: inference
                 * stores its binding in the same argument vector, and the instance name has to
                 * differ per length. `constParams` only marks *which* of those names are values. */
                *(const char **)vecPush(&fd->constParams) = cp->text;
                *(const char **)vecPush(&fd->typeParams) = cp->text;
                *(const char **)vecPush(&p->constParams) = cp->text;   /* visible to `[N]T` in this signature */
                if (accept(p, ",")) { skipNl(p); continue; }
                break;
            }
            /* Type parameters start uppercase (`T`), so `expectIdent` is required here:
             * `expectTypeName` rejects exactly this shape, and its purpose is to reserve the
             * uppercase letters for type parameters. */
            Token *tp = expectIdent(p, "a type parameter name (uppercase, e.g. `T`)");
            if (!tp) return NULL;
            *(const char **)vecPush(&fd->typeParams) = tp->text;
            if (accept(p, ",")) { skipNl(p); continue; }
            break;
        }
        skipNl(p);
        if (!expect(p, ">", NULL)) return NULL;
    }

    if (!expect(p, "(", NULL)) return NULL;
    skipNl(p);
    while (!at(p, ")")) {
        Token *pn = expectIdent(p, "a parameter name");
        if (!pn) return NULL;
        if (!expect(p, ":", "parameters must be typed: `name: Type`")) return NULL;
        Type *pt = parseType(p);
        if (!pt) return NULL;

        Param *pm = (Param *)arenaAllocZero(p->arena, sizeof(Param));
        pm->name = pn->text;
        pm->type = pt;
        pm->line = pn->line;
        /* `name: Type = expr`: the value a call uses when it leaves this argument out. Only a
         * body-carrying `fn` may have one: a declaration (`extern!`, `@builtin`) is a promise
         * about somebody else's C function, and that function has no notion of a default. */
        if (at(p, "=")) {
            Token *eq = take(p);
            if (p->noBody) {
                ctxError(p->ctx, eq->line, eq->col,
                         "a declaration without a body promises an existing C function, and that "
                         "function takes exactly the parameters written here",
                         "an `extern!` or `@builtin` declaration cannot have a default value");
                return NULL;
            }
            skipNl(p);
            pm->dflt = parseExpr(p);
            if (!pm->dflt) return NULL;
        }
        *(Param **)vecPush(&fd->params) = pm;

        if (accept(p, ",")) skipNl(p);
        else break;
    }
    if (!expect(p, ")", NULL)) return NULL;

    if (accept(p, "->")) {
        fd->ret = parseType(p);
        if (!fd->ret) return NULL;
    }
    /* The signature is over: its `const` parameters must not make `[N]T` look symbolic in the next
     * function's signature. */
    p->constParams.len = 0;

    /* A declaration **without a function body** (`extern!` or `@builtin`) stops after the
     * return type. Both have to be able to declare `effects`, because the compiler cannot see
     * through either of them. (`extern!` also reads the clause in `parseExtern`, so that it can
     * appear after clauses such as `owned`; here it has to follow the signature directly.) */
    if (p->noBody) {
        /* The helper starts at the `effects` keyword and takes it, so the guard has to be here:
         * an unconditional call eats whatever token comes next (`stdlib/std/sys/io.extc`
         * then comes apart at `expected '='`). */
        skipJunk(p);
        if (at(p, "effects") && !parseEffectsClause(p, fd, NULL)) return NULL;
        return fd;
    }
    /* A function **with a body** may state `effects` too, but only the keys the body cannot
     * answer for itself: `Ret` (what the returned reference points at) and `Thread` (a fact about
     * the call, not about a store). `Addr`/`Cont` are computed from the body (`collectEffects`), so
     * accepting them here would look like a signature and be silently ignored -- refuse instead.
     *
     * This is where the standard library signs "my views do not point into the caller's frames"
     * once, for every user: `std::heap`'s `alloc`/`view`/`allocPtr`/`store` are ordinary extC
     * methods with bodies. */
    {
        bool sawThread = false;
        skipJunk(p);                       /* `effects` after a comment/newline counts too (`skipNl` skips no comments) */
        if (at(p, "effects")) {
            if (!parseEffectsClause(p, fd, &sawThread)) return NULL;
            if (fd->extAddrMask || fd->extContMask) {
                ctxError(p->ctx, fd->line, 0,
                         "A body already says what it stores: `Addr` and `Cont` are computed from"
                         " it, so writing them here would change nothing. A function with a body"
                         " uses `effects` for the keys the body cannot answer for itself: `Ret=0`"
                         " (what my return value points at) and `Thread=`.",
                         "`effects Addr=`/`Cont=` is for declarations without a body"
                         " (`extern!`); on a function with a body only `Ret` and `Thread` apply");
                return NULL;
            }
        }
    }
    fd->body = parseBlock(p);
    if (!fd->body) return NULL;
    return fd;
}

/**
 * @brief Parse one type expression: `mut`, `?`, `ref`, `[N]T`, names, and generic arguments.
 *
 * A module-qualified name (`io::File`) is collected into a single string. No symbol table is
 * consulted here, so whether a path names a module or a type is decided by the loader alone.
 *
 * This function is the only entry into type recursion: the depth counter is decremented on
 * **every** exit, error paths included -- leaving a `depth` that was never given back would
 * make every later file look too deep.
 *
 * @param p  Parser.
 * @return   The new Type node; `NULL` on error (the diagnostic has been reported).
 */
static Type *parseType(Parser *p) {
    if (parseTypeTooDeep(p)) return NULL;
    Type *t = parseTypeInner(p);
    p->typeDepth--;
    return t;
}

/**
 * @brief The real implementation of a type expression; `parseType` wraps it in the depth
 *        budget.
 *
 * @param p  Parser.
 * @return   The new Type node; `NULL` on error (the diagnostic has been reported).
 */
static Type *parseTypeInner(Parser *p) {
    /* `dyn Trait` -- the type of a dynamically dispatched value. As with the expression form,
     * it is recognised by **shape** (`dyn` followed by an identifier), because `dyn` is not a
     * reserved word in the language. */
    if (at(p, "dyn") && pk(p, 1) && pk(p, 1)->kind == TK_IDENT) {
        take(p);
        Token *tn = take(p);
        if (!startsUpper(tn->text)) {
            ctxError(p->ctx, tn->line, tn->col,
                     "Trait names start with a capital letter, which is what keeps them from ever"
                     " colliding with a type name or a type parameter.",
                     "`dyn %s` names no trait: trait names start with a capital letter", tn->text);
            return NULL;
        }
        Type *dt = typeNamed(p->arena, tn->text);
        dt->kind = TY_DYN;
        return dt;
    }

    /* `mut` means "writing through this value is allowed".  It only means
     * something for something that contains a reference:
     *     `mut ref T`     a writable reference
     *     `mut slice<T>`  a view whose elements may be written
     * A plain value does not need it -- a `var` binding is already writable.
     * Whether `slice<T>` really is a view cannot be decided here, because the
     * name is not resolved yet, so this only records the modifier and the type
     * checker rules on it. */
    if (at(p, "mut")) {
        Token *m = take(p);
        Type *ty = parseType(p);
        if (!ty) return NULL;
        if (ty->kind != TY_REF && ty->kind != TY_UNRESOLVED) {
            ctxError(p->ctx, m->line, m->col,
                     "`mut` qualifies a reference (`mut ref T`) or a view (`mut slice<T>`); "
                     "a plain value does not need it -- put it in a `var`.",
                     "`mut` cannot qualify this type");
            return NULL;
        }
        ty->mut = true;
        return ty;
    }
    /* `?T` means "may be absent", the same idea as the postfix `?`:
     *   `?ref T` / `mut ?ref T`  -> a nullable reference; null is its zero value
     *   `?T` for anything else   -> sugar for `option<T>`
     * One is a type and the other an expression suffix, so they never clash. */
    if (at(p, "?")) {
        Token *q = take(p);
        Type *inner = parseType(p);
        if (!inner) return NULL;
        if (inner->kind == TY_REF) {          /* `?ref T` */
            inner->nullable = true;
            return inner;
        }
        /* Everything else: build an `option<inner>` and let the checker
         * resolve that name. */
        Type *o = typeNamed(p->arena, "option");
        (void)q;
        vecInit(&o->targs, p->arena, sizeof(void *));
        *(Type **)vecPush(&o->targs) = inner;
        return o;
    }
    if (at(p, "ref")) {
        take(p);
        Type *inner = parseType(p);
        if (!inner) return NULL;
        return typeRef(p->arena, inner);
    }
    /* Fixed-size array `[N]T`, and its multi-dimensional spelling `[I, J]T`.
     *
     * Dimensions nest recursively either way: `[15][15]i32` and `[15, 15]i32` are the same type,
     * fifteen `[15]i32`. The comma form is only a spelling -- the shape is already part of the
     * type, so `[2, 3]f64` and `[3, 2]f64` stay different types. A dimension may name a `const`
     * parameter of the enclosing signature (`[N]T`), in which case the node records the name and
     * the type table fills the number in once the call is seen. */
    if (at(p, "[")) {
        Token *br = take(p);
        int64_t dims[8];
        const char *symDims[8];
        size_t ndim = 0;
        for (;;) {
            if (ndim >= 8) {
                ctxError(p->ctx, br->line, br->col, NULL,
                         "a fixed array may have at most 8 dimensions");
                return NULL;
            }
            if (atKind(p, TK_IDENT)) {
                Token *cp = cur(p);
                bool isConst = false;
                for (size_t ci = 0; ci < p->constParams.len; ci++)
                    if (strcmp(*(const char **)vecAt(&p->constParams, ci), cp->text) == 0) isConst = true;
                if (!isConst) {
                    ctxError(p->ctx, cp->line, cp->col,
                             "Only a `const` parameter may be used as an array length.",
                             "array length must be an integer literal, found `%s`", shown(cp));
                    return NULL;
                }
                take(p);
                dims[ndim] = -1;
                symDims[ndim] = cp->text;
                ndim++;
            } else if (atKind(p, TK_INT)) {
                Token *n = take(p);
                if (n->ival <= 0) {
                    ctxError(p->ctx, br->line, br->col, NULL,
                             "array length must be positive, got %lld", (long long)n->ival);
                    return NULL;
                }
                dims[ndim] = n->ival;
                symDims[ndim] = NULL;
                ndim++;
            } else {
                Token *bad = cur(p);
                ctxError(p->ctx, bad->line, bad->col,
                         "For now a fixed array's length must be an integer literal. "
                         "(Compile-time constants come with `const`.)",
                         "array length must be an integer literal, found `%s`", shown(bad));
                return NULL;
            }
            if (accept(p, ",")) continue;
            break;
        }
        if (!expect(p, "]", NULL)) return NULL;
        Type *elem = parseType(p);
        if (!elem) return NULL;
        for (size_t k = ndim; k > 0; k--) {
            size_t d = k - 1;
            elem = symDims[d] ? typeArraySym(p->arena, symDims[d], elem)
                              : typeArray(p->arena, dims[d], elem);
        }
        return elem;
    }

    /* `fn(A, B) -> R` in **type** position: a bare code pointer.
     *
     * The spelling is deliberately the same as a lambda's signature, so the language needs no
     * new symbol; the difference is that the parameters here are **types**, not `name: Type`.
     * Only the shape is parsed here -- parsing and interning the signature is `ttResolve`'s
     * job, as for every other type. */
    if (at(p, "fn") && pk(p, 1) && pk(p, 1)->text && strcmp(pk(p, 1)->text, "(") == 0)
        return parseFnType(p);

    Token *t = cur(p);
    if (t->kind == TK_TYPE || t->kind == TK_IDENT) {
        take(p);
        /* A type name may be module-qualified, e.g. `io::File`. The parser has no symbol
         * table, so the path is only collected into a single string here; resolving it is the
         * loader's job. */
        const char *nm = t->text;
        if (at(p, "::")) {
            Buf b;
            bufInit(&b, p->arena);
            bufPuts(&b, t->text);
            while (at(p, "::")) {
                take(p);
                Token *seg = expectTypeName(p, "a type name");
                if (!seg) return NULL;
                bufPuts(&b, "::");
                bufPuts(&b, seg->text);
            }
            nm = bufCstr(&b);
        }
        Type *ty = typeNamed(p->arena, nm);

        /* Generic arguments: `Pair<i32, u8>` */
        if (at(p, "<")) {
            take(p);
            skipNl(p);
            vecInit(&ty->targs, p->arena, sizeof(void *));
            for (;;) {
                Type *a = parseType(p);
                if (!a) return NULL;
                *(Type **)vecPush(&ty->targs) = a;
                if (accept(p, ",")) { skipNl(p); continue; }
                break;
            }
            skipNl(p);
            if (!expect(p, ">", NULL)) return NULL;
        }
        return ty;
    }
    ctxError(p->ctx, t->line, t->col, NULL, "expected a type, found `%s`", shown(t));
    return NULL;
}

/**
 * @brief Parse `fn(A, B) -> R` in **type** position into a TY_FN node.
 *
 * The return type is **required**: a lambda may omit it because its body says what comes
 * back; a type has no body, so `var f: fn(i64)` cannot be inferred and is rejected here
 * rather than guessed. Parameters carry no names, unlike a lambda's: a function type's
 * parameter names are not part of the type, and the called value is not a declaration.
 *
 * @param p  Parser.
 * @return   The new Type node; `NULL` on error (the diagnostic has been reported).
 */
static Type *parseFnType(Parser *p) {
    Token *ft = take(p);
    if (!expect(p, "(", NULL)) return NULL;

    Vec params;
    vecInit(&params, p->arena, sizeof(void *));
    skipNl(p);
    while (!at(p, ")")) {
        Type *pt = parseType(p);
        if (!pt) return NULL;
        *(Type **)vecPush(&params) = pt;
        if (accept(p, ",")) skipNl(p);
        else break;
    }
    if (!expect(p, ")", NULL)) return NULL;

    if (!at(p, "->")) {
        Token *bad = cur(p);
        ctxError(p->ctx, bad->line, bad->col,
                 "A lambda's return type may be inferred from its body; a function type has no"
                 " body, so its return type is written out.",
                 "a function type needs its return type: `fn(...) -> R`, found `%s`", shown(bad));
        (void)ft;
        return NULL;
    }
    take(p);
    Type *ret = parseType(p);
    if (!ret) return NULL;
    return typeFn(p->arena, &params, ret);
}

/* ================================================================ statements */

/* -------------------------------------------------------------- Desugaring
 *
 * Two pieces of syntax sugar are rewritten here directly into adjacent ordinary statements,
 * **without adding an AST node** -- for the same reason as the four forms of `for` (see the
 * `for` section below): adding a statement kind forces every recursive pass in the compiler to
 * learn it (effect summary, escape analysis, arena placement, `@overwrite`, name marking, ...),
 * and missing one place is one class of defect.
 *
 *     var a, b: i32 = 1, 2;   ->  var a: i32 = 1
 *                                  var b: i32 = 2
 *     a, b = b, a;            ->  var __extc_t0 = b
 *                                  var __extc_t1 = a
 *                                  a = __extc_t0
 *                                  b = __extc_t1
 *
 * One user statement may therefore expand into several **adjacent** statements. The parser
 * returns the first and queues the rest in `p->pending`; the four places that hold a statement
 * vector (a block body, a match arm body, a `for` init and step) splice them in at the same
 * position in order with `pendingSplice`. The queue is meaningful only within one `parseStmt`:
 * an inner statement's own queue is expanded and taken at that level, so call sites do not
 * have to save and restore state.
 *
 * The temporaries use `var`, not `let`: the right-hand side may be a writable view or a
 * writable reference, and a read-only binding does not allow those types. The counter gives
 * each parallel assignment in the same block its own name; a user binding with the same name
 * shadows it, and `declare` renames it like any other.
 */

/**
 * @brief Clear the expansion queue, ready for the next user statement.
 * @param p  Parser.
 */
static void pendingReset(Parser *p) { p->pending.len = 0; }

/**
 * @brief Queue one expanded statement.
 * @param p  Parser.
 * @param s  Statement to queue.
 */
static void pendingPush(Parser *p, Stmt *s) {
    *(Stmt **)vecPush(&p->pending) = s;
}

/**
 * @brief Append the queued statements to `stmts` in order, then clear the queue.
 * @param p      Parser.
 * @param stmts  Vector that receives the statements (a block body or a synthesized statement
 *               array).
 */
static void pendingSplice(Parser *p, Vec *stmts) {
    for (size_t i = 0; i < p->pending.len; i++)
        *(Stmt **)vecPush(stmts) = *(Stmt **)vecAt(&p->pending, i);
    p->pending.len = 0;
}

/**
 * @brief Make a hidden name for a parallel assignment (`__extc_t0`, `__extc_t1`, ...).
 * @param p  Parser.
 * @return   The name; it lives in the parser's arena, with the same lifetime as the AST.
 */
static const char *sugarTempName(Parser *p) {
    return arenaPrintf(p->arena, "__extc_t%d", p->sugarSeq++);
}

/**
 * @brief Whether this expression is a pure literal (reads no location, has no side effects).
 *
 * A parallel assignment can skip the temporary for a pure literal: moving its evaluation to
 * the write-back stage does not change the program's result, and an ordinary assignment
 * carries the target's type context, so narrow types and `null` get a type there.
 *
 * @param e  Expression.
 * @return   true when it is a pure literal.
 */
static bool exprIsPureLiteral(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: case EX_NULL:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Parse a `{ ... }` block, producing an ST_BLOCK statement.
 *
 * This function is the only entry into statement blocks: the depth counter is decremented on
 * **every** exit, error paths included -- leaving a `depth` that was never given back would
 * make every later file look too deep.
 *
 * @param p  Parser.
 * @return   The block statement; `NULL` on error (the diagnostic has been reported).
 */
static Stmt *parseBlock(Parser *p) {
    if (parseTooDeep(p, "a block")) return NULL;
    Stmt *t = parseBlockInner(p);
    p->depth--;
    return t;
}

/**
 * @brief The real implementation of a block; `parseBlock` wraps it in the depth budget.
 *
 * Statements inside the block are separated by newlines and an optional `;` (`skipJunk` skips
 * the separators).
 *
 * @param p  Parser.
 * @return   The block statement; `NULL` on error (the diagnostic has been reported).
 */
static Stmt *parseBlockInner(Parser *p) {
    Token *open = cur(p);
    if (!expect(p, "{", NULL)) return NULL;

    Stmt *b = stmtNew(p->arena, ST_BLOCK, open->line);
    vecInit(&b->u.block.stmts, p->arena, sizeof(void *));

    skipJunk(p);
    while (!at(p, "}")) {
        pendingReset(p);
        Stmt *s = parseStmt(p);
        if (!s) return NULL;
        *(Stmt **)vecPush(&b->u.block.stmts) = s;
        pendingSplice(p, &b->u.block.stmts);
        skipJunk(p);
    }
    if (!expect(p, "}", NULL)) return NULL;
    return b;
}

/**
 * @brief Make an empty `{ }` block.
 *
 * `if let` without `else` uses it as its fallback `_` arm body; `while let`'s `_` arm body is
 * instead a block wrapping `break`.
 *
 * @param p     Parser (for its arena).
 * @param line  Line of the block, for diagnostic locations.
 * @return      The new empty ST_BLOCK.
 */
static Stmt *emptyBlock(Parser *p, int line) {
    Stmt *b = stmtNew(p->arena, ST_BLOCK, line);
    vecInit(&b->u.block.stmts, p->arena, sizeof(void *));
    return b;
}

/**
 * @brief Wrap `s` in a statement block; when `s` is already a block, return it unchanged.
 *
 * A `match` arm body is walked as a block by the checker (`arm->body->u.block.stmts`), so an
 * arm body produced by desugaring must be a block -- the `else` branch of `if let` may be an
 * `else if` (ST_IF), and that is what has to be wrapped at this layer.
 *
 * @param p  Parser (for its arena).
 * @param s  Statement to wrap.
 * @return   The block statement.
 */
static Stmt *wrapInBlock(Parser *p, Stmt *s) {
    if (s->kind == ST_BLOCK) return s;
    Stmt *b = emptyBlock(p, s->line);
    *(Stmt **)vecPush(&b->u.block.stmts) = s;
    return b;
}

/**
 * @brief Parse one `match` arm pattern head (also used by `if let` / `while let`): variant
 *        name plus payload bindings.
 *
 * `_` is the fallback arm: it matches every variant the arms above do not cover, and so has
 * **no** payload to bind -- every variant that can land in it has a different payload type,
 * and no single name could have one type. It also has to be written last.
 *
 * A `_` in a payload binding position means "I do not want this value": the name is replaced
 * by a generated one, no statement reads it, and code generation then deletes it as a dead
 * declaration. Two `_` bindings in the same arm still do not collide by name.
 *
 * @param p              Parser.
 * @param arm            Arm to fill (`variant` / `wildcard` / `binds` / `line`).
 * @param allowWildcard  Whether `_` is allowed as the whole pattern of an arm (only `match`
 *                       allows it).
 * @param what           How the diagnostic names the pattern position, e.g. `"if let"`.
 * @return               true on success; false after reporting a diagnostic.
 */
static bool parseArmPattern(Parser *p, MatchArm *arm, bool allowWildcard, const char *what) {
    Token *name = cur(p);
    if (name->kind != TK_IDENT && name->kind != TK_KEYWORD) {
        ctxError(p->ctx, name->line, name->col,
                 "a `match` arm names one of the enum's variants, e.g. `occupied => { ... }`",
                 "expected a variant name, found `%s`", name->text);
        return false;
    }
    take(p);
    arm->variant = name->text;
    arm->line = name->line;
    vecInit(&arm->binds, p->arena, sizeof(void *));

    if (strcmp(name->text, "_") == 0) {
        if (!allowWildcard) {
            ctxError(p->ctx, name->line, name->col,
                     "`if let` / `while let` take one variant's payload into the block, so"
                     " they need a variant to name; `_` matches everything and has no payload"
                     " to bring.",
                     "`_` cannot be the pattern of %s -- name the variant instead", what);
            return false;
        }
        arm->wildcard = true;
        skipNl(p);
        if (at(p, "(")) {
            ctxError(p->ctx, name->line, name->col,
                     "Every variant it might match carries its own payload type, so one name"
                     " cannot be bound to all of them.",
                     "`_` matches any variant, so it binds no payload");
            return false;
        }
        return true;
    }

    /* Payload bindings: `circle(r) => ...` / `rect(w, h) => ...`. They have to be parsed
     * before `=>`, because they come right after the variant name. */
    skipNl(p);
    if (at(p, "(")) {
        take(p);
        skipNl(p);
        while (!at(p, ")")) {
            Token *b = expectIdent(p, "a name to bind the payload to");
            if (!b) return false;
            const char *bn = b->text;
        /* A `_` binding means "discard": the name is replaced by a generated one, so writing
         * it twice in the same arm does not collide. */
            if (strcmp(bn, "_") == 0) bn = sugarTempName(p);
            *(const char **)vecPush(&arm->binds) = bn;
            skipNl(p);
            if (accept(p, ",")) { skipNl(p); continue; }
            break;
        }
        if (!expect(p, ")", NULL)) return false;
        skipNl(p);
    }
    return true;
}

/**
 * @brief Parse `match e { variant => statement ... }`, with `_` as the last, fallback arm.
 *
 * It is a statement rather than an expression for the same reason as `?`: C has no statement
 * expressions, so "match yields a value" cannot be expressed here without a new mechanism.
 * Each arm's body is one statement, usually a block; an arm names one of the enum's variants,
 * and whether it exists and whether the payload bindings fit are checked against the type in
 * the type checker. Listing every variant is the default requirement; a `_` fallback arm has
 * to be **written out** (it covers all remaining variants and has to come last) -- so "not all
 * listed" stays a decision the eye can see, not an omission that happens silently.
 *
 * @param p  Parser.
 * @return   The new ST_MATCH statement; `NULL` on error (the diagnostic has been reported).
 */
static Stmt *parseMatch(Parser *p) {
    Token *kw = take(p);
    /* Struct literals have to be off here, as in `if` / `while` conditions: otherwise the `{`
     * of `match e { ... }` would be read as the start of `e { field: value }`. */
    const bool saved = p->inCond;
    p->inCond = true;
    Expr *scrut = parseExpr(p);
    p->inCond = saved;
    if (!scrut) return NULL;

    if (!expect(p, "{", "a `match` arm is written `variant => { ... }`")) return NULL;

    Stmt *s = stmtNew(p->arena, ST_MATCH, kw->line);
    s->u.match.scrutinee = scrut;
    vecInit(&s->u.match.arms, p->arena, sizeof(void *));

    skipJunk(p);
    while (!at(p, "}")) {
        MatchArm *arm = (MatchArm *)arenaAllocZero(p->arena, sizeof(MatchArm));
        if (!parseArmPattern(p, arm, true, "a `match` arm")) return NULL;

        if (!expect(p, "=>", NULL)) return NULL;

        /* Arm body: a block, or a single statement -- `occupied => return 1` has to be
         * writable. */
        skipNl(p);
        if (at(p, "{")) {
            arm->body = parseBlock(p);
        } else {
            pendingReset(p);
            Stmt *inner = parseStmt(p);
            if (!inner) return NULL;
            arm->body = stmtNew(p->arena, ST_BLOCK, inner->line);
            vecInit(&arm->body->u.block.stmts, p->arena, sizeof(void *));
            *(Stmt **)vecPush(&arm->body->u.block.stmts) = inner;
            pendingSplice(p, &arm->body->u.block.stmts);
        }
        if (!arm->body) return NULL;
        *(MatchArm **)vecPush(&s->u.match.arms) = arm;

        skipJunk(p);
        /* `_` stands for "whatever the arms above did not match": any arm written after it
         * can never match, so it is rejected during parsing instead of being left for the
         * checker to notice later. */
        if (arm->wildcard && !at(p, "}")) {
            ctxError(p->ctx, cur(p)->line, cur(p)->col,
                     "`_` matches whatever the arms above it did not, so an arm after it can"
                     " never match.",
                     "an arm after `_` can never match -- `_` must be the last arm");
            return NULL;
        }
    }
    if (!expect(p, "}", NULL)) return NULL;
    return s;
}

/**
 * @brief Parse one statement: a declaration, a control statement, a block, an assignment,
 *        or an expression statement.
 *
 * @param p  Parser.
 * @return   The new Stmt; `NULL` on error (the diagnostic has been reported).
 */
static Stmt *parseStmt(Parser *p) {
    Token *t = cur(p);


    /* An annotation `@xxx` can only introduce a local declaration, and the only one that
     * exists is `@overwrite`. Function annotations such as `@main` are not implemented yet,
     * and the error says so rather than leaving the reader to guess. */
    if (at(p, "@")) {
        Token *a = take(p);
        Token *nm = expectIdent(p, "an annotation name (only `overwrite` for now)");
        if (!nm) return NULL;
        if (strcmp(nm->text, "overwrite") != 0) {
            ctxError(p->ctx, a->line, a->col,
                     "Annotations are compile-time instructions written in the source, so a"
                     " typo must not be silently ignored. Right now the only one is"
                     " `@overwrite` (reuse one piece of storage).",
                     "unknown annotation `@%s` -- only `@overwrite` exists today", nm->text);
            return NULL;
        }
        if (!(at(p, "let") || at(p, "var"))) {
            ctxError(p->ctx, a->line, a->col,
                     "`@overwrite` says \"this allocation is reused\", so it must sit on a"
                     " local declaration: `@overwrite var n = new node`.",
                     "`@overwrite` must be followed by a `var` declaration");
            return NULL;
        }
        Stmt *v = parseVarDecl(p);
        /* `@overwrite` says "this piece of storage is reused"; it only makes sense on one binding. */
        if (v && p->pending.len != 0) {
            p->pending.len = 0;
            ctxError(p->ctx, a->line, a->col,
                     "`@overwrite` reuses one piece of storage, so it needs exactly one name.",
                     "`@overwrite` cannot sit on a declaration that binds several names");
            return NULL;
        }
        if (v) v->u.var.overwrite = true;
        return v;
    }
    if (at(p, "yield")) {
        Token *yk = take(p);
        Stmt *s = stmtNew(p->arena, ST_YIELD, yk->line);
        if (atKind(p, TK_NEWLINE) || atKind(p, TK_EOF) || at(p, "}")) {
            ctxError(p->ctx, yk->line, yk->col, NULL,
                     "`yield` needs a value: `yield expr`",
                     "`yield` hands a value to whoever resumes the coroutine");
            return NULL;
        }
        s->u.yield_.value = parseExpr(p);
        return s->u.yield_.value ? s : NULL;
    }
    /* `global var x: T` / `global let x: T = c` -- a **soft keyword** (`global` is still a
     * legal identifier, and the lexer is left alone: making it reserved would break existing
     * code that uses it as a name). The semantics: the storage is global (depth 0), while the
     * name stays in this block. */
    if (at(p, "global")) {
        Token *g = cur(p);
        if (!accept(p, "global")) return NULL;
        /* `global` is a prefix on a declaration, not an annotation on a statement: without this
         * check `global if c { }` or `global f()` would leave the flag set and turn the next
         * `var` in the same block into static storage. */
        if (!at(p, "let") && !at(p, "var")) {
            ctxError(p->ctx, g->line, g->col,
                     "`global` gives a binding static storage; the declaration after it is what the"
                     " storage belongs to.",
                     "`global` must be followed by `let` or `var`");
            return NULL;
        }
        p->sawGlobal = true;
    }
    if (at(p, "let") || at(p, "var"))  return parseVarDecl(p);
    if (at(p, "if"))                   return parseIf(p);
    if (at(p, "while"))                return parseWhile(p);
    if (at(p, "for"))                  return parseFor(p);
    /* `par i in lo..hi { ... }`: the only verb for parallelism. Like `ext` / `trap` it is a
     * **soft keyword**, recognised by shape (`par`, then a name, then `in`), so a variable
     * named `par` still works. */
    if (at(p, "par") && pk(p, 1) &&
        (pk(p, 1)->kind == TK_IDENT || pk(p, 1)->kind == TK_TYPE) &&
        pk(p, 2) && strcmp(pk(p, 2)->text, "in") == 0)
        return parsePar(p);
    /* `trap(msg)`: the program cannot continue (`ST_TRAP`). Recognised by **shape** (the word
     * followed by `(`), the same trick as the `ext` operators: a variable, field, or parameter
     * named `trap` still works, and only a call in statement position is this builtin. */
    if (at(p, "trap") && pk(p, 1) && strcmp(pk(p, 1)->text, "(") == 0) {
        Token *tk = take(p);
        Stmt *s = stmtNew(p->arena, ST_TRAP, tk->line);
        take(p);                                     /* `(` */
        s->u.trap_.msg = parseExpr(p);
        if (!s->u.trap_.msg) return NULL;
        if (!at(p, ")")) {
            ctxError(p->ctx, cur(p)->line, cur(p)->col, NULL,
                     "`trap` takes exactly one message: `trap(\"why the program cannot go on\")`",
                     "expected `)` after the `trap` message");
            return NULL;
        }
        take(p);
        return s;
    }
    /* `drop x`: run `x`'s `@drop` hook now and mark the place dead (`ST_DROP`). Recognised by
     * **shape** (`drop` followed by a name), so a variable named `drop` still works -- and
     * `drop(x)` is still a call. */
    if (at(p, "drop") && pk(p, 1) &&
        (pk(p, 1)->kind == TK_IDENT || pk(p, 1)->kind == TK_TYPE)) {
        Token *tk = take(p);
        Stmt *s = stmtNew(p->arena, ST_DROP, tk->line);
        Token *target = take(p);
        s->u.drop_.target = exprIdent(p->arena, target->text, target->line);
        return s;
    }
    if (at(p, "match"))                return parseMatch(p);

    if (at(p, "return")) {
        take(p);
        Stmt *s = stmtNew(p->arena, ST_RETURN, t->line);
        if (atKind(p, TK_NEWLINE) || atKind(p, TK_EOF) || at(p, "}") || at(p, ";")) {
            s->u.ret.value = NULL;
        } else {
            s->u.ret.value = parseExpr(p);
            if (!s->u.ret.value) return NULL;
        }
        return s;
    }

    if (at(p, "break"))    { take(p); return stmtNew(p->arena, ST_BREAK, t->line); }
    if (at(p, "continue")) { take(p); return stmtNew(p->arena, ST_CONTINUE, t->line); }

    if (at(p, "struct") || at(p, "fn")) {
        ctxError(p->ctx, t->line, t->col, "extC does not allow nested declarations",
                 "`%s` cannot appear inside a function body", t->text);
        return NULL;
    }

    if (at(p, "{")) return parseBlock(p);

    Expr *e = parseExpr(p);
    if (!e) return NULL;

    /* `d.run { … }`: a **trailing block**, i.e. a domain's body.
     * It becomes a `ST_DOMAIN` statement: the block is not a value (so it must not be an expression)
     * and the shape is `receiver.field { … }`.
     *
     * `{` must be on the **same line** as the statement: that keeps "a name, then a plain block on
     * the next line" meaning exactly what it meant before, so this form cannot quietly change the
     * meaning of an existing program. */
    if (at(p, "{") && cur(p)->line == t->line && e->kind == EX_FIELD) {
        Stmt *body = parseBlock(p);
        if (!body) return NULL;
        Stmt *s = stmtNew(p->arena, ST_DOMAIN, t->line);
        s->u.domain_.callee = e;
        s->u.domain_.body = body;
        return s;
    }

    /* `a, b = b, a`: a parallel assignment. **All** right-hand sides are evaluated into
     * temporaries before writing back to the left-hand sides left to right, so a swap needs no
     * temporary from the user. The temporary and write-back statements are queued in
     * `p->pending` and spliced in at the position of the current statement. */
    if (at(p, ",")) {
        Vec targets, values;
        vecInit(&targets, p->arena, sizeof(Expr *));
        vecInit(&values,  p->arena, sizeof(Expr *));
        *(Expr **)vecPush(&targets) = e;
        while (accept(p, ",")) {
            skipNl(p);
            Expr *target = parseExpr(p);
            if (!target) return NULL;
            *(Expr **)vecPush(&targets) = target;
        }
        if (!at(p, "=")) {
            ctxError(p->ctx, cur(p)->line, cur(p)->col,
                     "`+=` and friends write one target back, so a parallel assignment takes"
                     " plain `=`.",
                     "expected `=` after the assignment targets");
            return NULL;
        }
        take(p);                                     /* `=` */
        skipNl(p);
        for (;;) {
            Expr *v = parseExpr(p);
            if (!v) return NULL;
            *(Expr **)vecPush(&values) = v;
            if (!accept(p, ",")) break;
            skipNl(p);
        }
        if (vecLen(&values) != vecLen(&targets)) {
            ctxError(p->ctx, t->line, t->col,
                     "A parallel assignment writes every target, so it needs one value per"
                     " target, in the same order.",
                     "`%zu` target(s) but `%zu` value(s)",
                     vecLen(&targets), vecLen(&values));
            return NULL;
        }
        /* Declare the temporaries first, then write back: the two loops are "evaluate
         * everything", then "store everything".
         *
         * A pure literal needs no temporary: it reads no location and has no side effects, so
         * moving its evaluation to the write-back stage does not change the result. It then
         * goes through an ordinary assignment (with the target's type context), so narrow types
         * and `null` get a type; storing into an unannotated temporary first would lose their
         * natural type (`var t = 1` is i32, and writing it back to an i8 target would be judged
         * a narrowing). */
        Vec temps, out;
        vecInit(&temps, p->arena, sizeof(const char *));
        vecInit(&out,   p->arena, sizeof(Stmt *));
        for (size_t i = 0; i < vecLen(&values); i++) {
            Expr *v = *(Expr **)vecAt(&values, i);
            if (exprIsPureLiteral(v)) {
                *(const char **)vecPush(&temps) = NULL;
                continue;
            }
            const char *tmp = sugarTempName(p);
            *(const char **)vecPush(&temps) = tmp;
            Stmt *d = stmtNew(p->arena, ST_VAR, t->line);
            d->u.var.name = tmp;
            d->u.var.ann  = NULL;
            d->u.var.init = v;
            d->u.var.mut  = true;                /* values of writable-view / writable-reference type need `var` */
            d->u.var.isGlobal = false;
            *(Stmt **)vecPush(&out) = d;
        }
        for (size_t i = 0; i < vecLen(&targets); i++) {
            const char *tmp = *(const char **)vecAt(&temps, i);
            Stmt *as = stmtNew(p->arena, ST_ASSIGN, t->line);
            as->u.assign.target = *(Expr **)vecAt(&targets, i);
            as->u.assign.value  = tmp ? exprIdent(p->arena, tmp, t->line)
                                      : *(Expr **)vecAt(&values, i);
            as->u.assign.op     = NULL;
            as->u.assign.opExpr = NULL;
            *(Stmt **)vecPush(&out) = as;
        }
        Stmt *first = *(Stmt **)vecAt(&out, 0);
        for (size_t i = 1; i < vecLen(&out); i++)
            pendingPush(p, *(Stmt **)vecAt(&out, i));
        return first;
    }

    /* `=` and the five compound forms. `+=` and friends *are* `x = x + y`, so they are one
     * statement with an extra field: the checker resolves the operator and code generation keeps
     * the compound form in the C output, so the target is **evaluated once**. `++` / `--` are
     * deliberately absent: an expression that both reads and writes one location is the classic
     * source of undefined behavior. */
    if (at(p, "=") || at(p, "+=") || at(p, "-=") || at(p, "*=") || at(p, "/=") || at(p, "%=")) {
        const char *aop = take(p)->text;
        skipNl(p);
        /* `got = yield e`: the same binding as `var got = yield e`, but the target already
         * exists -- resume writes the value into it. Where that write happens is decided by the
         * checker. */
        if (strcmp(aop, "=") == 0 && at(p, "yield") && e->kind == EX_IDENT) {
            Token *yk = take(p);
            Stmt *ys = stmtNew(p->arena, ST_YIELD, yk->line);
            ys->u.yield_.bind     = e->u.ident.name;
            ys->u.yield_.bindLine = t->line;
            ys->u.yield_.value    = parseExpr(p);
            return ys->u.yield_.value ? ys : NULL;
        }
        Expr *v = parseExpr(p);
        if (!v) return NULL;
        /* `v[i, j] = x`: the two-subscript sugar reads as a call to `std::nd::at2(...)`, and a
         * call can never be an assignment target, so it is rewritten here into
         * `std::nd::set2(v, i, j, x)` (an ordinary call statement). The compound form works the
         * same way: `v[i, j] += x` becomes `set2(v, i, j, at2(v, i, j) + x)` -- the element is
         * read once, matching the shape of `v[i] += x`. */
        bool ndIdxTarget = false;
        const char *setName = NULL;
        if (e->kind == EX_ASSOC && e->u.assoc.modPrefix
            && strcmp(e->u.assoc.modPrefix, "std::nd") == 0 && e->u.assoc.name) {
            if (strcmp(e->u.assoc.name, "at2") == 0 && e->u.assoc.args.len == 3) {
                ndIdxTarget = true;
                setName = "set2";
            } else if (strcmp(e->u.assoc.name, "at3") == 0 && e->u.assoc.args.len == 4) {
                ndIdxTarget = true;
                setName = "set3";
            }
        }
        if (ndIdxTarget) {
            if (strcmp(aop, "=") != 0) {
                /* The operator name must have static storage: `mkBin` only records the pointer,
                 * and pointing into a stack array would leave a dangling string (the operator
                 * would come out blank in the generated C). */
                const char *op = strcmp(aop, "+=") == 0 ? "+"
                               : strcmp(aop, "-=") == 0 ? "-"
                               : strcmp(aop, "*=") == 0 ? "*"
                               : strcmp(aop, "/=") == 0 ? "/"
                               : "%=";
                v = mkBin(p, op, e, v, t->line);
            }
            Vec args;
            vecInit(&args, p->arena, sizeof(void *));
            for (size_t i2 = 0; i2 < e->u.assoc.args.len; i2++)
                *(Expr **)vecPush(&args) = *(Expr **)vecAt(&e->u.assoc.args, i2);
            *(Expr **)vecPush(&args) = v;
            sugarNeedModule(p, "std::nd", t->line);
            Expr *call = moduleCall(p, "std::nd", setName, &args, t->line);
            Stmt *ss = stmtNew(p->arena, ST_EXPR, t->line);
            ss->u.expr.expr = call;
            return ss;
        }
        Stmt *s = stmtNew(p->arena, ST_ASSIGN, t->line);
        s->u.assign.target = e;
        s->u.assign.value = v;
        s->u.assign.op = (strcmp(aop, "=") == 0) ? NULL : aop;
        s->u.assign.opExpr = NULL;
        return s;
    }

    Stmt *s = stmtNew(p->arena, ST_EXPR, t->line);
    s->u.expr.expr = e;
    return s;
}

/**
 * @brief Parse the name list at the start of a declaration; each name may carry its own type
 *        annotation.
 *
 * The annotation written after the last name is also given to the preceding names that have
 * none: in `var a, b: i32 = 1, 2` both are `i32`. A declaration is one unit, its annotation
 * follows the last name, and it reads as "they are all this type". All three spellings are
 * therefore meaningful: none at all (each is inferred), one for every name (each differs), and
 * only the last one (all uniform).
 *
 * @param p      Parser.
 * @param names  Output: vector of `Token *`.
 * @param anns   Output: vector of `Type *` parallel to `names`; `NULL` where no annotation was
 *               written.
 * @return       `true` on success; `false` on error (the diagnostic has been reported).
 */
static bool declParseNames(Parser *p, Vec *names, Vec *anns) {
    vecInit(names, p->arena, sizeof(Token *));
    vecInit(anns,  p->arena, sizeof(Type *));

    Token *name = expectIdent(p, "a variable name");
    if (!name) return false;
    for (;;) {
        Type *ann = NULL;
        if (accept(p, ":")) {
            ann = parseType(p);
            if (!ann) return false;
        }
        *(Token **)vecPush(names) = name;
        *(Type **)vecPush(anns)   = ann;
        if (!accept(p, ",")) break;
        skipNl(p);
        name = expectIdent(p, "a variable name");
        if (!name) return false;
    }

    const size_t n = vecLen(names);
    Type *last = *(Type **)vecAt(anns, n - 1);
    if (last) {
        bool anyMissing = false;
        for (size_t i = 0; i + 1 < n; i++)
            if (!*(Type **)vecAt(anns, i)) anyMissing = true;
        if (anyMissing)
            for (size_t i = 0; i + 1 < n; i++)
                if (!*(Type **)vecAt(anns, i)) *(Type **)vecAt(anns, i) = last;
    }

    /* Two bindings of the same name in one declaration are just a typo: the second shadows the first. */
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            Token *a = *(Token **)vecAt(names, i);
            Token *b = *(Token **)vecAt(names, j);
            if (strcmp(a->text, b->text) == 0) {
                ctxError(p->ctx, b->line, b->col,
                         "Each name in one declaration is its own binding, so a repeat is a typo"
                         " -- and only the second one would be visible.",
                         "duplicate name `%s` in one declaration", b->text);
                return false;
            }
        }
    return true;
}

/**
 * @brief Check that the name list and the initializer list agree.
 *
 * Without initializers every name needs an annotation; with initializers there is one value per
 * name, in the same order.
 *
 * @param p      Parser.
 * @param kw     Declaration keyword (`let` / `var`), used in the error text only.
 * @param names  Name list.
 * @param anns   Annotation list.
 * @param inits  Initializer list; length 0 when there is no `=`.
 * @return       `true` on success; `false` on error (the diagnostic has been reported).
 */
static bool declCheckShape(Parser *p, Token *kw, Vec *names, Vec *anns, Vec *inits) {
    const size_t n = vecLen(names);
    if (vecLen(inits) == 0) {
        for (size_t i = 0; i < n; i++) {
            if (*(Type **)vecAt(anns, i)) continue;
            Token *nm = *(Token **)vecAt(names, i);
            ctxError(p->ctx, nm->line, nm->col,
                     "without an initializer you must write the type: `var x: T`",
                     "`%s %s` needs a type or an initializer", kw->text, nm->text);
            return false;
        }
        return true;
    }
    if (vecLen(inits) != n) {
        Token *t = cur(p);
        /* A leftover `=` means the author wrote the "every name has its own initializer" shape.
         * Point that out instead of letting the count check complain about "2 values" -- that
         * value is really one name. */
        if (at(p, "=")) {
            ctxError(p->ctx, t->line, t->col,
                     "A declaration writes its names first, then one `=` and one value per"
                     " name: `var a, b = 1, 2`.",
                     "expected one value per name, not `name = value` pairs");
            return false;
        }
        ctxError(p->ctx, t->line, t->col,
                 "A declaration takes one value per name, in the same order.",
                 "`%s` binds %zu name(s) but gets %zu initializer(s)",
                 kw->text, n, vecLen(inits));
        return false;
    }
    return true;
}

/**
 * @brief Parse a top-level `let` / `var`, i.e. a global variable or constant.
 *
 * The syntax matches a local declaration, including several names in one declaration: the type
 * may be omitted (an annotation written at the end applies to all names), and an omitted
 * initializer means zero initialization. The initializer must be a literal, because a C global
 * initializer can only be a constant expression.
 *
 * One user declaration may produce several `GlobalDef`s: this returns the first and queues the
 * rest in `p->pendingGlobals`, which the module-parsing loop picks up (`GlobalDef` is not a
 * `Stmt`, hence a separate queue).
 *
 * @param p  Parser.
 * @return   The first GlobalDef; `NULL` on error (the diagnostic has been reported).
 */
static GlobalDef *parseGlobalDecl(Parser *p) {
    Token *kw = take(p);
    Vec names, anns, inits;
    if (!declParseNames(p, &names, &anns)) return NULL;

    vecInit(&inits, p->arena, sizeof(Expr *));
    if (accept(p, "=")) {
        skipNl(p);
        for (;;) {
            Expr *v = parseExpr(p);
            if (!v) return NULL;
            *(Expr **)vecPush(&inits) = v;
            if (!accept(p, ",")) break;
            skipNl(p);
        }
    }
    if (!declCheckShape(p, kw, &names, &anns, &inits)) return NULL;

    GlobalDef *first = NULL;
    for (size_t i = 0; i < vecLen(&names); i++) {
        GlobalDef *g = (GlobalDef *)arenaAllocZero(p->arena, sizeof(GlobalDef));
        g->name = (*(Token **)vecAt(&names, i))->text;
        g->ann  = *(Type **)vecAt(&anns, i);
        g->init = vecLen(&inits) ? *(Expr **)vecAt(&inits, i) : NULL;
        g->mut  = (strcmp(kw->text, "var") == 0);
        g->line = kw->line;
        if (i == 0) first = g;
        else *(GlobalDef **)vecPush(&p->pendingGlobals) = g;
    }
    return first;
}
/**
 * @brief Parse a local `let` / `var` declaration, possibly with several names.
 *
 * Without initializers every name needs a type annotation; with none written every name is
 * inferred on its own (`var a, b = 1, 2.0` gives `i32` and `f64`). Each name may have its own
 * annotation, and an annotation written only after the last name applies to all names that have
 * none (see `declParseNames`).
 *
 * Several names expand into adjacent ST_VAR statements: the first is returned and the rest are
 * queued in `p->pending`, spliced in by the caller that owns a statement vector (a block body, a
 * match arm body, a `for` init or step).
 *
 * @param p  Parser.
 * @return   The first ST_VAR statement; `NULL` on error (the diagnostic has been reported).
 */
static Stmt *parseVarDecl(Parser *p) {
    Token *kw = take(p);
    Vec names, anns, inits;
    if (!declParseNames(p, &names, &anns)) return NULL;

    /* The initializer may be omitted, which means zero initialization. Reading uninitialized
     * memory is one of the largest sources of undefined behavior in C, and it can only be
     * discovered at run time; defaulting to zero makes it something the compiler guarantees. */
    vecInit(&inits, p->arena, sizeof(Expr *));
    if (accept(p, "=")) {
        skipNl(p);
        /* `var got: T = yield e` -- the declaration **is** the yield statement, and `got` is a
         * coroutine-frame local: the frame holds it, and a resume re-runs the assignment from the
         * frame's `in` slot. `yield` hands over one value, so it cannot be one of several names. */
        if (at(p, "yield")) {
            Token *nm = *(Token **)vecAt(&names, 0);
            if (vecLen(&names) > 1) {
                ctxError(p->ctx, nm->line, nm->col,
                         "`yield` hands one value to the resumer, so it binds one name: split"
                         " the declaration.",
                         "`yield` cannot initialize %zu names", vecLen(&names));
                return NULL;
            }
            Token *yk = take(p);
            Stmt *ys = stmtNew(p->arena, ST_YIELD, yk->line);
            ys->u.yield_.bind     = nm->text;
            ys->u.yield_.bindAnn  = *(Type **)vecAt(&anns, 0);
            ys->u.yield_.bindLine = kw->line;
            if (atKind(p, TK_NEWLINE) || atKind(p, TK_EOF) || at(p, "}")) {
                ctxError(p->ctx, yk->line, yk->col, NULL,
                         "`yield` needs a value: `yield expr`",
                         "`yield` hands a value to whoever resumes the coroutine");
                return NULL;
            }
            ys->u.yield_.value = parseExpr(p);
            return ys->u.yield_.value ? ys : NULL;
        }
        for (;;) {
            Expr *v = parseExpr(p);
            if (!v) return NULL;
            *(Expr **)vecPush(&inits) = v;
            if (!accept(p, ",")) break;
            skipNl(p);
        }
    }
    if (!declCheckShape(p, kw, &names, &anns, &inits)) return NULL;

    Stmt *first = NULL;
    for (size_t i = 0; i < vecLen(&names); i++) {
        Stmt *s = stmtNew(p->arena, ST_VAR, kw->line);
        s->u.var.name = (*(Token **)vecAt(&names, i))->text;
        s->u.var.ann  = *(Type **)vecAt(&anns, i);
        s->u.var.init = vecLen(&inits) ? *(Expr **)vecAt(&inits, i) : NULL;
        s->u.var.mut  = (strcmp(kw->text, "var") == 0);
        s->u.var.isGlobal = p->sawGlobal;   /* set by the `global` prefix, see parseStmt */
        if (i == 0) first = s;
        else pendingPush(p, s);
    }
    p->sawGlobal = false;
    return first;
}

/**
 * @brief Parse `if let PAT = E { A } else { B }`, desugared into a single-variant `match`.
 *
 * This is sugar, not a new statement: `if let some(v) = e { A } else { B }` and
 * `if let some(v) = e { A }` become, respectively,
 *
 *     match e { some(v) => { A }  _ => { B } }
 *     match e { some(v) => { A }  _ => { } }
 *
 * so the checker, code generation, escape analysis, and reclamation only need to know `match`
 * and `_` arms -- the same route as the `for` desugaring. An `else if let` chain puts the next
 * `if` in the `_` arm body (wrapped in a block), with unchanged semantics.
 *
 * @param p   Parser.
 * @param kw  The already consumed `if` token (used for the line number).
 * @return    The new ST_MATCH; `NULL` on error (the diagnostic has been reported).
 */
static Stmt *parseIfLet(Parser *p, Token *kw) {
    take(p);                 /* `let` */
    skipNl(p);

    MatchArm *arm = (MatchArm *)arenaAllocZero(p->arena, sizeof(MatchArm));
    if (!parseArmPattern(p, arm, false, "`if let`")) return NULL;

    skipNl(p);
    if (!expect(p, "=", "`if let` tests one variant of a value: `if let some(v) = e { ... }`"))
        return NULL;
    skipNl(p);

    /* Condition position: here `{` starts the block, not a struct literal (like `if` and `match`). */
    const bool saved = p->inCond;
    p->inCond = true;
    Expr *scrut = parseExpr(p);
    p->inCond = saved;
    if (!scrut) return NULL;

    arm->body = parseBlock(p);
    if (!arm->body) return NULL;

    skipJunk(p);
    Stmt *els = NULL;
    if (at(p, "else")) {
        take(p);
        skipJunk(p);
        els = at(p, "if") ? parseIf(p) : parseBlock(p);
        if (!els) return NULL;
    }

    /* Fallback arm: run `else` when another variant matched (nothing when `else` is omitted). */
    MatchArm *wild = (MatchArm *)arenaAllocZero(p->arena, sizeof(MatchArm));
    wild->variant = "_";
    wild->wildcard = true;
    wild->line = kw->line;
    vecInit(&wild->binds, p->arena, sizeof(void *));
    wild->body = els ? wrapInBlock(p, els) : emptyBlock(p, kw->line);

    Stmt *s = stmtNew(p->arena, ST_MATCH, kw->line);
    s->u.match.scrutinee = scrut;
    vecInit(&s->u.match.arms, p->arena, sizeof(void *));
    *(MatchArm **)vecPush(&s->u.match.arms) = arm;
    *(MatchArm **)vecPush(&s->u.match.arms) = wild;
    return s;
}

/**
 * @brief Parse `if <cond> <block>`, with an optional `else` block or `else if`.
 *
 * `p->inCond` is set while the condition is parsed, so a `{` there starts the body instead of a
 * struct literal.
 *
 * @param p  Parser.
 * @return   The new ST_IF statement; `NULL` on error (the diagnostic has been reported).
 */
static Stmt *parseIf(Parser *p) {
    Token *kw = take(p);
    if (at(p, "let")) return parseIfLet(p, kw);
    const bool saved = p->inCond;
    p->inCond = true;
    Expr *cond = parseExpr(p);
    p->inCond = saved;
    if (!cond) return NULL;

    Stmt *thenBody = parseBlock(p);
    if (!thenBody) return NULL;

    skipJunk(p);
    Stmt *els = NULL;
    if (at(p, "else")) {
        take(p);
        skipJunk(p);
        els = at(p, "if") ? parseIf(p) : parseBlock(p);
        if (!els) return NULL;
    }

    Stmt *s = stmtNew(p->arena, ST_IF, kw->line);
    s->u.ifs.cond = cond;
    s->u.ifs.thenBody = thenBody;
    s->u.ifs.elseBody = els;
    return s;
}

/* ------------------------------------------------------------------- `for`
 *
 * Four forms, one implementation: each becomes `var` + `while` here, so the checker and code
 * generation never see a loop they do not know -- the compiler's walkers only need to know
 * `var`/`while`. A new statement kind would force every recursive pass in the compiler to learn
 * it (effect summary, escape analysis, arena placement, `@overwrite`, name marking, ...), and
 * missing one place is one class of defect; desugaring is therefore cheaper than adding a node.
 *
 *     for i in lo..hi { B }        ->  { var i = lo
 *                                         while i < hi { { B }  i += 1 } }
 *     for d in c { B }             ->  { var __extc_s = c[..]
 *                                         var __extc_i = 0
 *                                         while __extc_i < __extc_s.len {
 *                                             var d = __extc_s[__extc_i]  { B }  __extc_i += 1 } }
 *     for (init; cond; step) { B } ->  { init  while cond { { B }  step } }
 *
 * `i += 1` is a compound assignment; that is exactly the shape the loop is described with:
 * "some `{i = 0; while(cond) { BLOCK; i += 1; }}`".
 * The body the user wrote is wrapped in its own block, so declarations inside it cannot shadow
 * the loop variable that the step still uses.
 * `c[..]` makes the container form cover fixed-size arrays and views alike: an array has no
 * `.len` of its own, a view over an array does. Anything else (a call, `varArray`, a struct) is
 * rejected, and a name must be bound first, because the desugared loop uses `c` on every
 * iteration. The bounds are `lo..hi`, half-open, exactly like a slice range.
 */

/**
 * @brief Make a synthetic name for the loop's own use.
 *
 * @param p     Parser.
 * @param name  Name.
 * @param line  Source line attached to the node.
 * @return      The new EX_IDENT node.
 */
static Expr *forIdent(Parser *p, const char *name, int line) {
    return exprIdent(p->arena, name, line);
}

/**
 * @brief Make an integer literal for the loop's own use (the `+ 1` and the starting 0).
 *
 * @param p     Parser.
 * @param v     Literal value.
 * @param line  Source line attached to the node.
 * @return      The new EX_INT node.
 */
static Expr *forInt(Parser *p, long long v, int line) {
    Expr *e = exprNew(p->arena, EX_INT, line);
    e->u.ival = v;
    return e;
}

/**
 * @brief Make `tgt op= value`: the shape of the loop's own step.
 *
 * @param p      Parser.
 * @param tgt    Name of the assignment target.
 * @param op     Compound assignment operator, e.g. `+=`.
 * @param value  Right-hand side expression.
 * @param line   Source line attached to the node.
 * @return       The new ST_ASSIGN statement.
 */
static Stmt *forStep(Parser *p, const char *tgt, const char *op, Expr *value, int line) {
    Stmt *s = stmtNew(p->arena, ST_ASSIGN, line);
    s->u.assign.target  = forIdent(p, tgt, line);
    s->u.assign.value   = value;
    s->u.assign.op      = op;
    s->u.assign.opExpr  = NULL;
    return s;
}

/**
 * @brief Pack the given statements into one block (`{ a  b  c }`).
 *
 * @param p      Parser.
 * @param stmts  Statement array.
 * @param n      Number of statements.
 * @param line   Source line attached to the block.
 * @return       The new ST_BLOCK statement.
 */
static Stmt *forBlock(Parser *p, Stmt **stmts, size_t n, int line) {
    Stmt *b = stmtNew(p->arena, ST_BLOCK, line);
    vecInit(&b->u.block.stmts, p->arena, sizeof(Stmt *));
    for (size_t i = 0; i < n; i++)
        *(Stmt **)vecPush(&b->u.block.stmts) = stmts[i];
    return b;
}


/**
 * @brief Desugar `i in lo..hi { B }` into
 *        `[var i = lo, var __extc_hi = hi, while i < __extc_hi { B; i += 1 }]`.
 *
 * `for` and `par` share this tail: `par` only adds a `parLoop` flag to the same block (the
 * actual restriction set is decided by the checker; the parser does not need to know types).
 *
 * @param p     Parser.
 * @param vn    Loop-variable token.
 * @param head  `lo` expression (already parsed).
 * @param hi    `hi` expression (already parsed).
 * @param body  Loop body block (already parsed).
 * @param line  Source line attached to the node.
 * @return      The three-statement outer block after desugaring.
 */
static Stmt *forRangeFrom(Parser *p, Token *vn, Expr *head, Expr *hi, Stmt *body, int line) {
    Stmt *decl = stmtNew(p->arena, ST_VAR, line);
    decl->u.var.name = vn->text;
    decl->u.var.ann  = NULL;
    decl->u.var.init = head;
    decl->u.var.mut  = true;                 /* the loop itself writes it */
    decl->u.var.overwrite = false;

    /* **The upper bound is evaluated once**, on entry to the loop. Writing it into the condition
     * would re-read it on every iteration, and `for i in 0..n { n = 0 ... }` would follow the `n`
     * the body changes. A range is a bounded range -- `for i in 0..xs.len` must mean the length
     * at entry -- and the hidden `let` gives it exactly the lifetime the range desugaring
     * promises. The name follows the `__extc_` convention of `__extc_s` / `__extc_i`; a user
     * binding of the same name shadows it, and `declare` renames it like any other name. */
    Stmt *hiDecl = stmtNew(p->arena, ST_VAR, line);
    hiDecl->u.var.name = "__extc_hi";
    hiDecl->u.var.ann  = NULL;
    hiDecl->u.var.init = hi;
    hiDecl->u.var.mut  = false;              /* a bound is not something the loop writes */
    hiDecl->u.var.overwrite = false;

    Expr *cond = exprNew(p->arena, EX_BIN, line);
    cond->u.bin.op    = "<";                 /* half open, like a slice range */
    cond->u.bin.left  = forIdent(p, vn->text, line);
    cond->u.bin.right = forIdent(p, "__extc_hi", line);

    Stmt *stepStmt = forStep(p, vn->text, "+=", forInt(p, 1, line), line);
    Stmt *inner[2] = { body, stepStmt };
    Stmt *loopBody = forBlock(p, inner, 2, line);
    loopBody->forStep = stepStmt;         /* label target for `continue` */
    Stmt *w = stmtNew(p->arena, ST_WHILE, line);
    w->u.whiles.cond = cond;
    w->u.whiles.body = loopBody;
    Stmt *outer[3] = { decl, hiDecl, w };
    return forBlock(p, outer, 3, line);
}

/**
 * @brief Parse `par i in lo..hi { B }` (a parallel loop), desugared into the same block as a
 *        range `for` and flagged.
 *
 * `par` is a **soft keyword**: it is this statement only when the shape is `par <name> in`, so a
 * variable named `par` still works. Types and the restriction set are checked in the checker
 * (`parLower`).
 *
 * @param p  Parser.
 * @return   The desugared block; `NULL` on error (the diagnostic has been reported).
 */
static Stmt *parsePar(Parser *p) {
    Token *kw = take(p);
    const int line = kw->line;
    if (cur(p)->kind != TK_IDENT && cur(p)->kind != TK_TYPE) {
        ctxError(p->ctx, cur(p)->line, cur(p)->col, NULL,
                 "`par` binds a name here: `par i in 0..n { … }`");
        return NULL;
    }
    Token *vn = take(p);
    if (!expect(p, "in", "`par` needs `in`: `par i in 0..n { … }`")) return NULL;
    const bool savedCond = p->inCond;
    p->inCond = true;
    Expr *head = parseExpr(p);
    p->inCond = savedCond;
    if (!head) return NULL;
    if (!at(p, "..")) {
        ctxError(p->ctx, cur(p)->line, cur(p)->col,
                 "A `par` loop is a range loop `<name> in lo..hi { … }`; other `for` forms do not"
                 " carry the index an output slot is decided by.",
                 "`par` needs a range loop: `par i in 0..n { … }`");
        return NULL;
    }
    take(p);
    p->inCond = true;
    Expr *hi = parseExpr(p);
    p->inCond = savedCond;
    if (!hi) return NULL;
    Stmt *body = parseBlock(p);
    if (!body) return NULL;
    Stmt *block = forRangeFrom(p, vn, head, hi, body, line);
    if (block) block->parLoop = true;
    return block;
}

/**
 * @brief Whether the subject of `for d in SUBJ` can be evaluated again directly.
 *
 * Subjects other than a name, a field, or `*p` (`split(s)`, `v.asSlice()`) are first collected
 * into a hidden binding: the container is evaluated once at the start of the loop, and the body
 * keeps using that binding.
 *
 * @param e  Subject expression.
 * @return   `true` when it is a place.
 */
static bool forRepeatable(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_IDENT: return true;
    case EX_FIELD: return forRepeatable(e->u.field.obj);
    case EX_DEREF: return forRepeatable(e->u.deref.operand);
    default:       return false;
    }
}

/**
 * @brief Parse the four forms of `for`, desugaring them into `var` + `while` here.
 *
 * @param p  Parser.
 * @return   The desugared block statement; `NULL` on error (the diagnostic has been reported).
 */
static Stmt *parseFor(Parser *p) {
    Token *kw = take(p);
    const int line = kw->line;

    /* ---- form four: `for (init; cond; step) { B }` ---- */
    if (at(p, "(")) {
        take(p);
        Vec outer, inner;
        vecInit(&outer, p->arena, sizeof(Stmt *));
        vecInit(&inner, p->arena, sizeof(Stmt *));

        pendingReset(p);
        Stmt *init = parseStmt(p);
        if (!init) return NULL;
        *(Stmt **)vecPush(&outer) = init;
        pendingSplice(p, &outer);        /* the second statement expanded from `var i = 0, j = 0` */
        if (!expect(p, ";", "`for (init; cond; step)` needs `;` after the initializer")) return NULL;
        skipNl(p);
        Expr *cond = parseExpr(p);
        if (!cond) return NULL;
        if (!expect(p, ";", "`for (init; cond; step)` needs `;` after the condition")) return NULL;
        skipNl(p);
        pendingReset(p);
        Stmt *step = parseStmt(p);
        if (!step) return NULL;
        if (!expect(p, ")", "`for (init; cond; step)` needs the closing `)`")) return NULL;

        /* The statements expanded from the step must be taken first: parsing the loop body calls
         * `parseBlockInner` again, and the `pendingReset` at the start of every statement there
         * clears the queue. */
        Vec stepExtra;
        vecInit(&stepExtra, p->arena, sizeof(Stmt *));
        pendingSplice(p, &stepExtra);    /* the statements from a step that is `a, b = b, a` */

        Stmt *body = parseBlock(p);
        if (!body) return NULL;

        /* The step runs after the body: both go in a block inside the `while`. The block also
         * records **which** statement is the step, so codegen can put a label in front of it and
         * `continue` can jump there (C's `for` semantics; ). */
        *(Stmt **)vecPush(&inner) = body;
        *(Stmt **)vecPush(&inner) = step;
        for (size_t i = 0; i < vecLen(&stepExtra); i++)
            *(Stmt **)vecPush(&inner) = *(Stmt **)vecAt(&stepExtra, i);
        Stmt *loopBody = forBlock(p, (Stmt **)inner.data, inner.len, line);
        loopBody->forStep = step;
        Stmt *w = stmtNew(p->arena, ST_WHILE, line);
        w->u.whiles.cond = cond;
        w->u.whiles.body = loopBody;
        *(Stmt **)vecPush(&outer) = w;
        return forBlock(p, (Stmt **)outer.data, outer.len, line);
    }

    /* ---- `for <name> in ...` ---- */
    if (cur(p)->kind != TK_IDENT && cur(p)->kind != TK_TYPE) {
        ctxError(p->ctx, cur(p)->line, cur(p)->col, NULL,
                 "`for` binds a name here: `for i in 0..n` or `for d in items`");
        return NULL;
    }
    Token *vn = take(p);
    if (!expect(p, "in", "`for` needs `in`: `for i in 0..n`")) return NULL;
    const bool savedCond = p->inCond;
    p->inCond = true;                    /* `{` starts the body, not a literal */
    Expr *head = parseExpr(p);
    p->inCond = savedCond;
    if (!head) return NULL;

    /* ---- forms two and three: `for i in lo..hi { B }` (`0..n`, `-2..3`, ...) ----
     * `..` is settled before the loop body: the `{` after it starts the body, and requiring a
     * block first would make `0..5` complain about a missing `{` at the `..`. */
    Expr *hi = NULL;
    if (at(p, "..")) {
        take(p);
        p->inCond = true;
        hi = parseExpr(p);
        p->inCond = savedCond;
        if (!hi) return NULL;
    }

    Stmt *body = parseBlock(p);
    if (!body) return NULL;

    if (hi) return forRangeFrom(p, vn, head, hi, body, line);

    /* ---- form one: `for d in c { B }` ----
     *
     * The subject need not be a place: `for d in split(s)` and `for d in v.asSlice()` are both
     * valid. The container is read once at the start of the loop, so a non-place subject is first
     * collected into a hidden binding and the loop refers back to it. To the checker that binding
     * is an ordinary `var`, so `forSubjectType` can read its type from the symbol table as usual.
     */
    Stmt *hoist = NULL;
    if (!forRepeatable(head)) {
        const char *nm = arenaPrintf(p->arena, "__extc_c%d", p->sugarSeq++);
        hoist = stmtNew(p->arena, ST_VAR, line);
        hoist->u.var.name = nm;
        hoist->u.var.ann  = NULL;
        hoist->u.var.init = head;
        /* Mutable: driving a subject is not just reading it. A coroutine is stepped with
         * `next()`, which assigns into the binding, so `for x in counter(4)` on a temporary
         * needs a `var` here -- the name is compiler-owned, nobody else can see the binding. */
        hoist->u.var.mut  = true;
        hoist->u.var.overwrite = false;
        head = forIdent(p, nm, line);
    }

    /* `var __extc_s = c[..]`: one view, which is what gives an array a `.len` too. */
    Expr *whole = exprNew(p->arena, EX_SLICE, line);
    whole->u.slice.obj = head;
    whole->u.slice.lo  = NULL;
    whole->u.slice.hi  = NULL;
    Stmt *viewDecl = stmtNew(p->arena, ST_VAR, line);
    viewDecl->u.var.name = "__extc_s";
    viewDecl->u.var.ann  = NULL;
    viewDecl->u.var.init = whole;
    viewDecl->u.var.mut  = false;               /* the view itself is never rebound */
    viewDecl->u.var.overwrite = false;

    /* `i64(0)`, not `0`: this index is compared with `.len` and subscripts a view, both of
     * which are `i64`. A bare literal would default to `i32` and *work* - comparisons widen
     * - but the generated C would then index an `i64` view with an `int32_t`, and the
     * conversions would be there for no reason. The user's own range form keeps the type of
     * its lower bound instead: `for i in 0..a.len` is legal (the comparison widens) and a
     * `for i in i64(0)..a.len` indexes without any conversion at all. */
    Expr *zero = exprNew(p->arena, EX_CONV, line);
    zero->u.conv.typeName = "i64";
    zero->u.conv.operand  = forInt(p, 0, line);
    Stmt *idxDecl = stmtNew(p->arena, ST_VAR, line);
    idxDecl->u.var.name = "__extc_i";
    idxDecl->u.var.ann  = NULL;
    idxDecl->u.var.init = zero;
    idxDecl->u.var.mut  = true;
    idxDecl->u.var.overwrite = false;

    Expr *cond = exprNew(p->arena, EX_BIN, line);
    cond->u.bin.op    = "<";
    cond->u.bin.left  = forIdent(p, "__extc_i", line);
    Expr *lenOf = exprNew(p->arena, EX_FIELD, line);
    lenOf->u.field.obj  = forIdent(p, "__extc_s", line);
    lenOf->u.field.name = "len";
    cond->u.bin.right = lenOf;

    Stmt *elemDecl = stmtNew(p->arena, ST_VAR, line);
    elemDecl->u.var.name = vn->text;
    elemDecl->u.var.ann  = NULL;
    Expr *indexed = exprNew(p->arena, EX_INDEX, line);
    indexed->u.index.obj   = forIdent(p, "__extc_s", line);
    indexed->u.index.index = forIdent(p, "__extc_i", line);
    elemDecl->u.var.init = indexed;
    elemDecl->u.var.mut  = false;               /* a copy: writing it does not write back */
    elemDecl->u.var.overwrite = false;

    Stmt *stepStmt = forStep(p, "__extc_i", "+=", forInt(p, 1, line), line);
    Stmt *inner[3] = { elemDecl, body, stepStmt };
    Stmt *loopBody = forBlock(p, inner, 3, line);
    loopBody->forStep = stepStmt;             /* label target for `continue` () */
    Stmt *w = stmtNew(p->arena, ST_WHILE, line);
    w->u.whiles.cond = cond;
    w->u.whiles.body = loopBody;
    Stmt *outer[4];
    size_t n = 0;
    if (hoist) outer[n++] = hoist;
    outer[n++] = viewDecl;
    outer[n++] = idxDecl;
    outer[n++] = w;
    Stmt *desugared = forBlock(p, outer, n, line);
    /* What is iterated over may not be sliceable: the parser does not consult the symbol table
     * and cannot tell, so this is left to the checker, which may turn the statement into an
     * iterator loop. */
    desugared->forDesugar = true;
    return desugared;
}

/**
 * @brief Parse `while let PAT = E { B }`, desugared into
 *        `while true { match E { PAT => { B }  _ => { break } } }`.
 *
 * `E` is evaluated afresh each round, so a "take the next one, then test it" loop does not have
 * to repeat the fetch at the end of the body; a `continue` in the body returns to the start of
 * the loop (and re-evaluates), and `break` leaves the loop directly -- the checker still sees an
 * ordinary `while` + `match`, so the scoping rules of `break`/`continue` need no change.
 *
 * @param p   Parser.
 * @param kw  The already consumed `while` token (used for the line number).
 * @return    The new ST_WHILE; `NULL` on error (the diagnostic has been reported).
 */
static Stmt *parseWhileLet(Parser *p, Token *kw) {
    take(p);                 /* `let` */
    skipNl(p);

    MatchArm *arm = (MatchArm *)arenaAllocZero(p->arena, sizeof(MatchArm));
    if (!parseArmPattern(p, arm, false, "`while let`")) return NULL;

    skipNl(p);
    if (!expect(p, "=", "`while let` tests one variant of a value each round: `while let some(v) = next(it) { ... }`"))
        return NULL;
    skipNl(p);

    const bool saved = p->inCond;
    p->inCond = true;
    Expr *scrut = parseExpr(p);
    p->inCond = saved;
    if (!scrut) return NULL;

    arm->body = parseBlock(p);
    if (!arm->body) return NULL;

    /* The only way out of a failed match is to leave the loop. */
    Stmt *brk = stmtNew(p->arena, ST_BREAK, kw->line);
    MatchArm *wild = (MatchArm *)arenaAllocZero(p->arena, sizeof(MatchArm));
    wild->variant = "_";
    wild->wildcard = true;
    wild->line = kw->line;
    vecInit(&wild->binds, p->arena, sizeof(void *));
    wild->body = emptyBlock(p, kw->line);
    *(Stmt **)vecPush(&wild->body->u.block.stmts) = brk;

    Stmt *m = stmtNew(p->arena, ST_MATCH, kw->line);
    m->u.match.scrutinee = scrut;
    vecInit(&m->u.match.arms, p->arena, sizeof(void *));
    *(MatchArm **)vecPush(&m->u.match.arms) = arm;
    *(MatchArm **)vecPush(&m->u.match.arms) = wild;

    /* `while true`: the condition holds on every round; the real exit is the `break` in the `_` arm. */
    Expr *always = exprNew(p->arena, EX_BOOL, kw->line);
    always->u.bval = true;
    Stmt *body = emptyBlock(p, kw->line);
    *(Stmt **)vecPush(&body->u.block.stmts) = m;

    Stmt *s = stmtNew(p->arena, ST_WHILE, kw->line);
    s->u.whiles.cond = always;
    s->u.whiles.body = body;
    return s;
}

/**
 * @brief Parse `while <cond> <block>`.
 *
 * As in `parseIf`, the condition is parsed with `p->inCond` set, so a `{` there starts the loop
 * body.
 *
 * @param p  Parser.
 * @return   The new ST_WHILE statement; `NULL` on error (the diagnostic has been reported).
 */
static Stmt *parseWhile(Parser *p) {
    Token *kw = take(p);
    if (at(p, "let")) return parseWhileLet(p, kw);
    const bool saved = p->inCond;
    p->inCond = true;
    Expr *cond = parseExpr(p);
    p->inCond = saved;
    if (!cond) return NULL;

    Stmt *body = parseBlock(p);
    if (!body) return NULL;

    Stmt *s = stmtNew(p->arena, ST_WHILE, kw->line);
    s->u.whiles.cond = cond;
    s->u.whiles.body = body;
    return s;
}

/* ================================================================ expressions */

/**
 * @brief Parse `a ?? b`, the binary operator with the lowest precedence.
 *
 * It is right-associative: `a ?? b ?? c` is `a ?? (b ?? c)`. Where it may appear is a separate
 * rule: the left-hand side must be an option, a result, or `?ref T`, enforced by the type
 * checker.
 *
 * @param p  Parser.
 * @return   The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseCoalesce(Parser *p) {
    Expr *l = parseOr(p);
    if (!l) return NULL;
    while (at(p, "??")) {
        Token *op = take(p);
        skipNl(p);
        /* `x ?? break` / `x ?? continue`: make "leave the loop when there is no value" one line.
         * The checker recognises these two reserved names and expands them at statement level into
         * "evaluate once -> test for none -> jump" (see the early part of check_stmt.c). The names
         * carry `__`, so the user cannot write them. */
        Expr *r = NULL;
        if (at(p, "break") || at(p, "continue")) {
            const bool isBreak = at(p, "break");
            Token *kw = take(p);
            r = exprIdent(p->arena, isBreak ? "__extc_break" : "__extc_continue", kw->line);
        } else {
            r = parseCoalesce(p);          /* right-associative */
        }
        if (!r) return NULL;
        Expr *e = exprNew(p->arena, EX_COALESCE, op->line);
        e->u.coalesce.main = l;
        e->u.coalesce.fallback = r;
        l = e;
    }
    return l;
}

/**
 * @brief Entry point for parsing a complete expression, used by every caller that wants one.
 *
 * @param p  Parser.
 * @return   The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseExpr(Parser *p) { return parseCoalesce(p); }

/**
 * @brief Parse the `||` (logical or) level.
 *
 * @param p  Parser.
 * @return   The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseOr(Parser *p) {
    Expr *e = parseAnd(p);
    if (!e) return NULL;
    while (at(p, "||")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseAnd(p);
        if (!r) return NULL;
        e = mkBin(p, "||", e, r, op->line);
    }
    return e;
}

/**
 * @brief Parse the `&&` (logical and) level.
 *
 * @param p  Parser.
 * @return   The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseAnd(Parser *p) {
    Expr *e = parseBitOr(p);
    if (!e) return NULL;
    while (at(p, "&&")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseBitOr(p);
        if (!r) return NULL;
        e = mkBin(p, "&&", e, r, op->line);
    }
    return e;
}

/**
 * @brief Parse the `|` (bitwise or) level.
 *
 * The three bitwise levels use C's and Go's precedence:
 *     `|`  <  `^`  <  `&`  <  `== !=`  <  `< <=` ...  <  `<< >>`  <  `+ -`
 * Agreeing with C is deliberate: everyone writing algorithms here has C's muscle memory, and
 * "designing it better" would only buy a daily mistake.
 *
 * @param p  Parser.
 * @return   The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseBitOr(Parser *p) {
    Expr *e = parseBitXor(p);
    if (!e) return NULL;
    while (at(p, "|")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseBitXor(p);
        if (!r) return NULL;
        e = mkBin(p, "|", e, r, op->line);
    }
    return e;
}

/**
 * @brief Parse the `^` (bitwise xor) level.
 *
 * @param p  Parser.
 * @return   The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseBitXor(Parser *p) {
    Expr *e = parseBitAnd(p);
    if (!e) return NULL;
    while (at(p, "^")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseBitAnd(p);
        if (!r) return NULL;
        e = mkBin(p, "^", e, r, op->line);
    }
    return e;
}

/**
 * @brief Parse the `&` (bitwise and) level.
 *
 * @param p  Parser.
 * @return   The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseBitAnd(Parser *p) {
    Expr *e = parseEquality(p);
    if (!e) return NULL;
    /* `&` stays distinct from `&&`: at(p, "&") is false on a `&&` token,
     * because the comparison covers the whole token text. */
    while (at(p, "&")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseEquality(p);
        if (!r) return NULL;
        e = mkBin(p, "&", e, r, op->line);
    }
    return e;
}

/**
 * @brief Parse `==` and `!=`.
 *
 * @param p  Parser.
 * @return   The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseEquality(Parser *p) {
    Expr *e = parseComparison(p);
    if (!e) return NULL;
    while (at(p, "==") || at(p, "!=")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseComparison(p);
        if (!r) return NULL;
        e = mkBin(p, op->text, e, r, op->line);
    }
    return e;
}

/**
 * @brief Parse `<`, `<=`, `>`, and `>=`.
 *
 * @param p  Parser.
 * @return   The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseComparison(Parser *p) {
    Expr *e = parseShift(p);
    if (!e) return NULL;
    while (at(p, "<") || at(p, "<=") || at(p, ">") || at(p, ">=")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseShift(p);
        if (!r) return NULL;
        e = mkBin(p, op->text, e, r, op->line);
    }
    return e;
}

/**
 * @brief Whether the cursor is on a shift operator: `"<"` or `">"` twice in a row, spelled
 *        `"<<"` or `">>"`.
 *
 * The second token must be punctuation, so the text of a string literal cannot pass for one.
 * `<<` and `>>` are deliberately not in the lexer's table: the `>>` in `box<box<i32>>` has to stay
 * two separate `>` tokens so that generic arguments pair up (see `parseType` and `looksLikeAssoc`);
 * fusing them in the lexer would make nested generic types unparsable, the trap C++ fell into.
 * Only expressions see a shift operator; the type grammar never does.
 *
 * @param p   Parser.
 * @param ch  Character looked for twice in a row.
 * @param op  Receives the operator spelling.
 * @return    `true` when two adjacent tokens are both spelled `ch`.
 */
static bool atShift(Parser *p, const char *ch, const char **op) {
    /* Both halves have to be punctuation: the text of a string literal can also be `<` or `>`,
     * and only the kind tells the two apart (`at` checks the same thing for one token). */
    if (pk(p, 0)->kind != TK_PUNCT || pk(p, 1)->kind != TK_PUNCT) return false;
    if (strcmp(pk(p, 0)->text, ch) != 0 || strcmp(pk(p, 1)->text, ch) != 0) return false;
    *op = strcmp(ch, "<") == 0 ? "<<" : ">>";
    return true;
}

/* ---------------------------------------------------------------- f"..." (format splicing)
 *
 * `lhs << f"t1{e1}t2"` is expanded **in the parser** into `((lhs << "t1") << e1) << "t2"` --
 * using the **same** `mkBin` as a hand-written chain, so the expanded AST is byte-identical to the
 * hand-written `<<` chain, i.e. checking and emission need no change, and "can this type be
 * printed" is exactly "is there an `<<` overload" (reusing existing overload resolution instead of
 * introducing a second definition).
 *
 * The lexer has already recognised `f"..."` as TK_FSTRING (in the lexing stage); this part splits
 * the segments, decodes escapes, and **sub-parses** the expressions in the slots. A slot expression
 * is parsed with the **real parser** running on a fragment (lexRange carries the real line and
 * column, so diagnostics point into the file).
 */

/**
 * @brief Make an EX_STR node, copying the first `len` bytes of `s` into the arena.
 *
 * @param p     Parser.
 * @param s     Source text (not necessarily NUL-terminated).
 * @param len   Number of bytes.
 * @param line  Source line attached to the node.
 * @return      The new EX_STR node.
 */
static Expr *fmtStr(Parser *p, const char *s, size_t len, int line) {
    Expr *e = exprNew(p->arena, EX_STR, line);
    char *cp = (char *)arenaAlloc(p->arena, len + 1);
    memcpy(cp, s, len);
    cp[len] = 0;
    e->u.str.text = cp;
    return e;
}

/**
 * @brief One parsed format specifier (the `>8` part of `{x:>8}`).
 *
 * The specifier itself carries no type information, so which padding primitive to use and which way
 * the default alignment goes can only be decided once the checker knows the static type of the
 * value in the slot; this records only what was **written**, as five integer arguments.
 */
typedef struct {
    long long width;   /* width to pad to, in bytes; 0 means no padding */
    long long fill;    /* fill byte (ASCII); space by default */
    long long align;   /* `<` left, `>` right, `^` center; -1 when not written */
    long long prec;    /* number of fractional digits for floats; -1 when not written */
    long long code;    /* type code: 0 `d`, 1 `f`, 2 `s`; -1 when not written */
} FmtSpec;

/**
 * @brief Turn an alignment character into a direction code, matching io::LEFT / io::RIGHT /
 *        io::CENTER.
 *
 * @param c  Alignment character.
 * @return   0 left, 1 right, 2 center; -1 when it is not an alignment character.
 */
static int fmtAlignCode(char c) {
    if (c == '<') return 0;
    if (c == '>') return 1;
    if (c == '^') return 2;
    return -1;
}

/**
 * @brief Parse an f-string format specifier: `[[fill]align][0][width][.prec][type]`.
 *
 * The syntax mostly matches Python's format spec, keeping only what the three padding primitives
 * need: width counts **bytes**, precision is meaningful only for floats, and the type code `d` /
 * `f` / `s` serves only to agree with the value's static type (the real choice is made in the
 * checker).
 *
 * @param p     Parser.
 * @param s     Specifier text (after the colon, before the `}`, not necessarily NUL-terminated).
 * @param n     Number of specifier bytes.
 * @param line  Source line the specifier is on.
 * @param col   Column of the specifier's first byte (for diagnostics).
 * @param out   Parse result; the caller fills in initial values only, and success overwrites it
 *              entirely.
 * @return      `true` on success; `false` on a syntax error (the diagnostic has been reported).
 */
static bool fmtParseSpec(Parser *p, const char *s, size_t n, int line, int col, FmtSpec *out) {
    out->width = 0; out->fill = 32; out->align = -1; out->prec = -1; out->code = -1;
    if (n == 0) {
        ctxError(p->ctx, line, col,
                 "a specifier says how to lay the value out; `{}` already prints it as `<<` would",
                 "an empty format specifier");
        return false;
    }
    size_t k = 0;
    /* [fill]align: two characters whose second is an alignment character means the first is the fill byte; otherwise a single alignment character */
    int a = n >= 2 ? fmtAlignCode(s[1]) : -1;
    if (a >= 0) {
        out->fill = (long long)(unsigned char)s[0];
        out->align = a;
        k = 2;
    } else if ((a = fmtAlignCode(s[0])) >= 0) {
        out->align = a;
        k = 1;
    }
    /* `0` flag: zero padding; without an alignment it counts as right alignment (the usual `{n:04}`) */
    bool zero = false;
    if (k < n && s[k] == '0') {
        zero = true;
        out->fill = 48;
        if (out->align < 0) out->align = 1;
        k += 1;
    }
    /* width */
    while (k < n && s[k] >= '0' && s[k] <= '9') {
        out->width = out->width * 10 + (s[k] - '0');
        if (out->width > 999999) {
            ctxError(p->ctx, line, col + (int)k,
                     "a wider field than this is a typo, not a layout",
                     "format width is too large");
            return false;
        }
        k += 1;
    }
    if (zero && out->width == 0) {
        ctxError(p->ctx, line, col + (int)k,
                 "`0` means zero padding and needs a width, as in `{n:04}`",
                 "`0` in a format specifier needs a width");
        return false;
    }
    /* .prec */
    if (k < n && s[k] == '.') {
        k += 1;
        if (k >= n || s[k] < '0' || s[k] > '9') {
            ctxError(p->ctx, line, col + (int)k,
                     "a precision is `.` followed by the number of decimal places, as in `{v:.2f}`",
                     "expected digits after `.` in a format specifier");
            return false;
        }
        out->prec = 0;
        while (k < n && s[k] >= '0' && s[k] <= '9') {
            out->prec = out->prec * 10 + (s[k] - '0');
            if (out->prec > 99) {
                ctxError(p->ctx, line, col + (int)k,
                         "a precision this large would only print noise",
                         "format precision is too large");
                return false;
            }
            k += 1;
        }
    }
    /* type */
    if (k < n) {
        if (s[k] == 'd') out->code = 0;
        else if (s[k] == 'f') { out->code = 1; if (out->prec < 0) out->prec = 6; }
        else if (s[k] == 's') out->code = 2;
        else {
            ctxError(p->ctx, line, col + (int)k,
                     "a specifier is `[[fill]align][0][width][.prec][type]`, and the type is"
                     " `d`, `f` or `s`",
                     "unexpected `%c` in a format specifier", s[k]);
            return false;
        }
        k += 1;
    }
    if (k != n) {
        ctxError(p->ctx, line, col + (int)k,
                 "a specifier is `[[fill]align][0][width][.prec][type]`",
                 "unexpected `%c` in a format specifier", s[k]);
        return false;
    }
    return true;
}

/**
 * @brief Splice the parts of an f-string into the `lhs << ...` chain.
 *
 * Each slot first runs the **real lexer and parser** on its fragment (keeping the real line and
 * column), then the chain is built with the same `mkBin` in the order `lhs << text segment <<
 * expression`. `{{` / `}}` are escaped literal braces; a top-level `:` introduces a v2 format
 * specifier, which is rejected explicitly rather than silently becoming part of the expression.
 *
 * @param p    Parser.
 * @param lhs  Expression being appended to (the left end of the `<<` chain).
 * @param ft   The TK_FSTRING token that triggered this expansion.
 * @return     `true` on success; `false` on a syntax error or an empty slot (the diagnostic has
 *             been reported).
 */
static bool fmtSplice(Parser *p, Expr **lhs, Token *ft) {
    const char *src = ft->text;
    const Expr *before = *lhs;
    const size_t n = ft->len;
    char *txt = (char *)arenaAlloc(p->arena, n + 1);   /* decode buffer: escapes only ever make the text shorter, so this is enough */
    size_t tl = 0;
    int col = ft->col + 2;                              /* skip `f"` */
    size_t i = 0;
    while (i < n) {
        char c = src[i];
        if (c == '{' && i + 1 < n && src[i + 1] == '{') { txt[tl++] = '{'; i += 2; col += 2; continue; }
        if (c == '}' && i + 1 < n && src[i + 1] == '}') { txt[tl++] = '}'; i += 2; col += 2; continue; }
        if (c == '}') {
            ctxError(p->ctx, ft->line, col,
                     "write `}}` for a literal brace",
                     "an f-string has a `}` with no matching `{`");
            return false;
        }
        if (c != '{') { txt[tl++] = c; i += 1; col += 1; continue; }
        /* slot: flush the accumulated text segment into `lhs << "..."` first */
        if (tl > 0) { *lhs = mkBin(p, "<<", *lhs, fmtStr(p, txt, tl, ft->line), ft->line); tl = 0; }
        size_t j = i + 1;
        int depth = 1;
        bool inStr = false;
        while (j < n) {
            char d = src[j];
            if (inStr) {
                if (d == '\\') j += 1;
                else if (d == '"') inStr = false;
            } else if (d == '"') inStr = true;
            else if (d == '{') depth += 1;
            else if (d == '}') { depth -= 1; if (depth == 0) break; }
            j += 1;
        }
        if (depth != 0) {
            ctxError(p->ctx, ft->line, col,
                     "close the slot with `}`",
                     "an f-string has a `{` with no matching `}`");
            return false;
        }
        size_t innerLen = j - (i + 1);
        if (innerLen == 0) {
            ctxError(p->ctx, ft->line, col + 1, NULL, "an f-string slot cannot be empty");
            return false;
        }
        /* a top-level `:` means a format specifier: parse `[[fill]align][0][width][.prec][type]` */
        size_t q = j;
        {
            int d2 = 0; bool st = false;
            for (size_t t = i + 1; t < j; t++) {
                char d = src[t];
                if (st) { if (d == '\\') t += 1; else if (d == '"') st = false; continue; }
                if (d == '"') { st = true; continue; }
                if (d == '(' || d == '[' || d == '{') d2 += 1;
                else if (d == ')' || d == ']' || d == '}') d2 -= 1;
                else if (d == ':' && d2 == 0) {
                    /* `::` is the module-path separator (`{nd::sumv(v)}`), not the start of a specifier. */
                    if (t + 1 < j && src[t + 1] == ':') { t += 1; continue; }
                    q = t;
                    break;
                }
            }
        }
        size_t exprLen = q - (i + 1);
        if (exprLen == 0) {
            ctxError(p->ctx, ft->line, col + 1,
                     "write the value before the `:`",
                     "an f-string slot has no value before its format specifier");
            return false;
        }
        FmtSpec spec;
        spec.width = 0; spec.fill = 32; spec.align = -1; spec.prec = -1; spec.code = -1;
        if (q < j && !fmtParseSpec(p, src + q + 1, j - q - 1, ft->line, col + (int)(q - i) + 1, &spec))
            return false;
        /* Expression in the slot: the **real parser** runs on the fragment (keeping real lines and columns) */
        Vec toks;
        vecInit(&toks, p->arena, sizeof(Token));   /* tokens are stored **by value** (see lxPushAt) */
        lexRange(p->ctx, src + i + 1, exprLen, ft->line, col + 1, &toks);
        if (p->ctx->hasError) return false;
        Parser sub = *p;
        sub.toks = &toks;
        sub.pos = 0;
        sub.inCond = false;          /* expression position: `{` is a struct literal, not a block */
        Expr *e = parseExpr(&sub);
        if (!e) return false;
        skipNl(&sub);                 /* fragment scanning appends a NEWLINE at the end (see lexRange) */
        if (cur(&sub)->kind != TK_EOF) {
            ctxError(p->ctx, cur(&sub)->line, cur(&sub)->col,
                     "one expression per slot", "unexpected `%s` inside an f-string slot",
                     cur(&sub)->text);
            return false;
        }
        if (q < j) {
            /* Specifier: desugared into a call to the reserved-name `__extc_pad` method. The
             * arguments are, in order, the slot value, width, fill byte, alignment (-1 = not
             * written), precision (-1 = not written), and type code (-1 = not written). Which padding
             * primitive to use can only be decided once the checker knows the slot value's static
             * type, so the specifier is recorded as written. */
            Vec args;
            vecInit(&args, p->arena, sizeof(void *));
            *(Expr **)vecPush(&args) = e;
            *(Expr **)vecPush(&args) = forInt(p, spec.width, ft->line);
            *(Expr **)vecPush(&args) = forInt(p, spec.fill, ft->line);
            *(Expr **)vecPush(&args) = forInt(p, spec.align, ft->line);
            *(Expr **)vecPush(&args) = forInt(p, spec.prec, ft->line);
            *(Expr **)vecPush(&args) = forInt(p, spec.code, ft->line);
            Expr *m = exprNew(p->arena, EX_METHOD, ft->line);
            m->u.method.recv = *lhs;
            m->u.method.name = "__extc_pad";
            m->u.method.args = args;
            *lhs = m;
        } else {
            *lhs = mkBin(p, "<<", *lhs, e, ft->line);
        }
        col += (int)(j - i + 1);
        i = j + 1;
    }
    if (tl > 0) *lhs = mkBin(p, "<<", *lhs, fmtStr(p, txt, tl, ft->line), ft->line);
    /* No part at all (`f""`): without an error the `<<` would be silently eaten, which must not happen */
    if (*lhs == before) {
        ctxError(p->ctx, ft->line, ft->col + 2, NULL,
                 "an f-string with nothing in it has nothing to splice");
        return false;
    }
    return true;
}

/**
 * @brief Make a `stl::string::from(bytes)` call node (the start of a value f-string).
 *
 * It uses the **full module path** rather than the alias the caller imported with: the parser
 * cannot see the import list, and `stl::string::from` resolves as soon as that module is imported
 * under any alias.
 *
 * @param p      Parser.
 * @param bytes  Byte-view expression (`slice<u8>`, usually a string literal).
 * @param line   Source line attached to the node.
 * @return       The new EX_ASSOC node (`isCall = true`).
 */
static Expr *fstrFrom(Parser *p, Expr *bytes, int line) {
    Vec args;
    vecInit(&args, p->arena, sizeof(void *));
    *(Expr **)vecPush(&args) = bytes;
    Expr *f = exprNew(p->arena, EX_ASSOC, line);
    f->u.assoc.typeName = "stl";
    f->u.assoc.modPrefix = "std::text::string";
    f->u.assoc.name = "from";
    vecInit(&f->u.assoc.targs, p->arena, sizeof(void *));   /* `from` is not generic, so an empty table */
    f->u.assoc.args = args;
    f->u.assoc.isCall = true;
    return f;
}

/**
 * @brief Expand a **value-position** f-string into `stl::string::from(text segment) + slot + ...`.
 *
 * It shares the same scan with `fmtSplice` (the real lexer/parser runs on each slot's fragment);
 * only the product differs: that one appends the parts to an existing `<<` chain (streamed output),
 * while this one collects the parts and folds them into a `+` chain, yielding a `string` value. The
 * first version **accepts no format specifier** (`padInt`/`padF`/`padText` all hang off the stream
 * object, and a string target has no corresponding primitive), so a slot with `:` is reported as an
 * error instead of being silently mis-parsed.
 *
 * @param p   Parser.
 * @param ft  The TK_FSTRING token that triggered this expansion.
 * @return    The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *fmtValue(Parser *p, Token *ft) {
    /* A value f-string starts at `stl::string::from`: the sugar registers the module itself. */
    sugarNeedModule(p, "std::text::string", ft->line);
    const char *src = ft->text;
    const size_t n = ft->len;
    const int line = ft->line;
    char *txt = (char *)arenaAlloc(p->arena, n + 1);
    size_t tl = 0;
    int col = ft->col + 2;
    size_t i = 0;
    Vec parts;
    vecInit(&parts, p->arena, sizeof(void *));
    while (i < n) {
        char c = src[i];
        if (c == '{' && i + 1 < n && src[i + 1] == '{') { txt[tl++] = '{'; i += 2; col += 2; continue; }
        if (c == '}' && i + 1 < n && src[i + 1] == '}') { txt[tl++] = '}'; i += 2; col += 2; continue; }
        if (c == '}') {
            ctxError(p->ctx, line, col, "write `}}` for a literal brace",
                     "an f-string has a `}` with no matching `{`");
            return NULL;
        }
        if (c != '{') { txt[tl++] = c; i += 1; col += 1; continue; }
        if (tl > 0) {
            *(Expr **)vecPush(&parts) = fmtStr(p, txt, tl, line);
            tl = 0;
        }
        size_t j = i + 1;
        int depth = 1;
        bool inStr = false;
        while (j < n) {
            char d = src[j];
            if (inStr) {
                if (d == '\\') j += 1;
                else if (d == '"') inStr = false;
            } else if (d == '"') inStr = true;
            else if (d == '{') depth += 1;
            else if (d == '}') { depth -= 1; if (depth == 0) break; }
            j += 1;
        }
        if (depth != 0) {
            ctxError(p->ctx, line, col, "close the slot with `}`",
                     "an f-string has a `{` with no matching `}`");
            return NULL;
        }
        size_t innerLen = j - (i + 1);
        if (innerLen == 0) {
            ctxError(p->ctx, line, col + 1, NULL, "an f-string slot cannot be empty");
            return NULL;
        }
        /* A top-level `:` (and not the `::` of a module path) means a specifier. Desugared into a
         * call to the reserved name `__extc_fstr_pad(value, width, fill byte, alignment, precision,
         * type code)`: which padding primitive to use depends on the value's **static type**, so
         * that step can only happen in the checker (the same specifier parsing as `__extc_pad` on
         * the right of `<<`). */
        size_t q = innerLen;
        {
            int d2 = 0; bool st = false;
            for (size_t t = i + 1; t < j; t++) {
                char d = src[t];
                if (st) { if (d == '\\') t += 1; else if (d == '"') st = false; continue; }
                if (d == '"') { st = true; continue; }
                if (d == '(' || d == '[' || d == '{') d2 += 1;
                else if (d == ')' || d == ']' || d == '}') d2 -= 1;
                else if (d == ':' && d2 == 0) {
                    if (t + 1 < j && src[t + 1] == ':') { t += 1; continue; }
                    q = t - (i + 1);
                    break;
                }
            }
        }
        FmtSpec spec;
        spec.width = 0; spec.fill = 32; spec.align = -1; spec.prec = -1; spec.code = -1;
        if (q < innerLen &&
            !fmtParseSpec(p, src + i + 1 + q + 1, innerLen - q - 1, line,
                          col + (int)q + 1, &spec))
            return NULL;
        Vec toks;
        vecInit(&toks, p->arena, sizeof(Token));
        lexRange(p->ctx, src + i + 1, q, line, col + 1, &toks);
        if (p->ctx->hasError) return NULL;
        Parser sub = *p;
        sub.toks = &toks;
        sub.pos = 0;
        sub.inCond = false;
        Expr *e = parseExpr(&sub);
        if (!e) return NULL;
        skipNl(&sub);
        if (cur(&sub)->kind != TK_EOF) {
            ctxError(p->ctx, cur(&sub)->line, cur(&sub)->col, "one expression per slot",
                     "unexpected `%s` inside an f-string slot", cur(&sub)->text);
            return NULL;
        }
        if (q < innerLen) {
            /* The specifier lands on a padding primitive in `std::text::string`: likewise registered by the sugar. */
            sugarNeedModule(p, "std::text::string", line);
            Vec args;
            vecInit(&args, p->arena, sizeof(void *));
            *(Expr **)vecPush(&args) = e;
            *(Expr **)vecPush(&args) = forInt(p, spec.width, line);
            *(Expr **)vecPush(&args) = forInt(p, spec.fill, line);
            *(Expr **)vecPush(&args) = forInt(p, spec.align, line);
            *(Expr **)vecPush(&args) = forInt(p, spec.prec, line);
            *(Expr **)vecPush(&args) = forInt(p, spec.code, line);
            Expr *c = exprNew(p->arena, EX_CALL, line);
            c->u.call.callee = forIdent(p, "__extc_fstr_pad", line);
            c->u.call.args = args;
            e = c;
        }
        *(Expr **)vecPush(&parts) = e;
        col += (int)(j - i + 1);
        i = j + 1;
    }
    if (tl > 0) *(Expr **)vecPush(&parts) = fmtStr(p, txt, tl, line);
    if (parts.len == 0) {
        ctxError(p->ctx, line, ft->col + 2, NULL,
                 "an f-string with nothing in it has nothing to build");
        return NULL;
    }
    /* Fold into a `+` chain: a text segment starts as `from(segment)`, a slot as `from("")` then `+ slot`. */
    Expr *acc = NULL;
    for (size_t k = 0; k < parts.len; k++) {
        Expr *x = *(Expr **)vecAt(&parts, k);
        if (!acc) {
            if (x->kind == EX_STR) acc = fstrFrom(p, x, line);
            else {
                acc = fstrFrom(p, fmtStr(p, "", 0, line), line);
                acc = mkBin(p, "+", acc, x, line);
            }
        } else {
            acc = mkBin(p, "+", acc, x, line);
        }
    }
    return acc;
}

/**
 * @brief Parse `<<` and `>>`: they are recognised as two adjacent `<` / `>` tokens.
 *
 * @param p  Parser.
 * @return   The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseShift(Parser *p) {
    Expr *e = parseTerm(p);
    if (!e) return NULL;
    for (;;) {
        const char *op = NULL;
        if (!atShift(p, "<", &op) && !atShift(p, ">", &op)) break;
        int line = cur(p)->line;
        take(p);
        take(p);
        skipNl(p);
        /* `x << f"..."`: expanded here into the same `<<` chain (via `mkBin`, exactly like a hand-written one) */
        if (cur(p)->kind == TK_FSTRING) {
            if (strcmp(op, "<<") != 0) {
                ctxError(p->ctx, cur(p)->line, cur(p)->col,
                         "an f-string is concatenated with `<<`",
                         "an f-string cannot be the right-hand side of `%s`", op);
                return NULL;
            }
            if (!fmtSplice(p, &e, take(p))) return NULL;
            continue;
        }
        Expr *r = parseTerm(p);
        if (!r) return NULL;
        e = mkBin(p, op, e, r, line);
    }
    return e;
}

/**
 * @brief Parse the `+` and `-` (addition/subtraction) level.
 *
 * @param p  Parser.
 * @return   The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseTerm(Parser *p) {
    Expr *e = parseFactor(p);
    if (!e) return NULL;
    while (at(p, "+") || at(p, "-")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseFactor(p);
        if (!r) return NULL;
        e = mkBin(p, op->text, e, r, op->line);
    }
    return e;
}

/**
 * @brief Parse the `*`, `/`, and `%` (multiplication/division/modulo) level.
 *
 * @param p  Parser.
 * @return   The new expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseFactor(Parser *p) {
    Expr *e = parseUnary(p);
    if (!e) return NULL;
    while (at(p, "*") || at(p, "/") || at(p, "%")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseUnary(p);
        if (!r) return NULL;
        e = mkBin(p, op->text, e, r, op->line);
    }
    return e;
}

/**
 * @brief Whether a token can start an expression.
 *
 * It serves exactly **one** decision: `ext` is a **prefix operator** (`ext f(x)`, a dispatch domain
 * feature) whose spelling is also a perfectly legal variable name. Recognising it by shape alone
 * would make `ref ext`, `f(ext)`, and `-ext` be treated as operators and then die on "expected an
 * expression, found `)`" -- a bad way to learn that a name is also an operator.
 *
 * So the operator is recognised only when something that can be an operand follows it; nothing after
 * `ext` is still an error, it just says "no variable named `ext`".
 *
 * @param t  Token to check.
 * @return   `true` when it can start an expression; `false` for `NULL` or when it cannot.
 */
static bool tokenStartsExpr(Token *t) {
    if (!t || !t->text) return false;
    switch (t->kind) {
    case TK_IDENT: case TK_INT: case TK_FLOAT: case TK_STRING: case TK_TYPE: return true;
    case TK_KEYWORD:
        return strcmp(t->text, "true") == 0 || strcmp(t->text, "false") == 0 ||
               strcmp(t->text, "ref") == 0  || strcmp(t->text, "fn") == 0 ||
               strcmp(t->text, "if") == 0   || strcmp(t->text, "match") == 0 ||
               strcmp(t->text, "new") == 0;
    default: break;
    }
    return strcmp(t->text, "(") == 0 || strcmp(t->text, "-") == 0 ||
           strcmp(t->text, "!") == 0 || strcmp(t->text, "~") == 0 ||
           strcmp(t->text, "[") == 0 || strcmp(t->text, "null") == 0;
}

/**
 * @brief Parse a prefix expression: `!`, `-`, `~`, dereference `*p`, and reference `ref x`.
 *
 * `*` is unambiguous in prefix position, because a multiplication always has a left operand.
 *
 * This function is the only entry into expression recursion: the depth counter is decremented on
 * **every** exit, error paths included -- leaving a `depth` that was never given back would make
 * every later file look too deep.
 *
 * @param p  Parser.
 * @return   The operand expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseUnary(Parser *p) {
    if (parseTooDeep(p, "an expression")) return NULL;
    Expr *t = parseUnaryInner(p);
    p->depth--;
    return t;
}

/**
 * @brief The real implementation of a prefix expression; `parseUnary` wraps it in the depth
 *        budget.
 *
 * @param p  Parser.
 * @return   The operand expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseUnaryInner(Parser *p) {
    /* `ext f(x)`: "start one concurrent task". Parsed as a **prefix** thing whose operand is an
     * ordinary expression (normally a call), so the node is an expression and `let h = ext f(x)`
     * reads naturally. Whether the operand really is a call, and whether there is a domain around
     * it, is the checker's business -- the parser only locks the shape in.
     *
     * `ext` is not reserved anywhere else: `ext` on its own is a legal identifier, and a
     * name starting with `ext` (such as `extern!`) is a different token. */
    if (at(p, "ext") && tokenStartsExpr(pk(p, 1))) {
        Token *kw = take(p);
        Expr *inner = parseUnary(p);
        if (!inner) return NULL;
        Expr *e = exprNew(p->arena, EX_EXT, kw->line);
        e->u.ext_.call = inner;
        return e;
    }
    if (at(p, "!") || at(p, "-") || at(p, "~")) {
        Token *op = take(p);
        Expr *operand = parseUnary(p);
        if (!operand) return NULL;
        Expr *e = exprNew(p->arena, EX_UN, op->line);
        e->u.un.op = op->text;
        e->u.un.operand = operand;
        return e;
    }
    /* `*p` is the explicit dereference, the dual of `ref x`.  Read it as "the
     * value p points at", write it as "the place p points at" (`*p = v`).  The
     * prefix position of `*` was free, since multiplication is binary. */
    if (at(p, "*")) {
        Token *op = take(p);
        Expr *operand = parseUnary(p);
        if (!operand) return NULL;
        Expr *e = exprNew(p->arena, EX_DEREF, op->line);
        e->u.deref.operand = operand;
        return e;
    }
    /* In expression position `ref` takes a reference (`f(ref x)`); in type
     * position it is a reference type. */
    if (at(p, "ref")) {
        Token *kw = take(p);
        Expr *operand = parseUnary(p);
        if (!operand) return NULL;
        Expr *e = exprNew(p->arena, EX_REF, kw->line);
        e->u.ref.operand = operand;
        return e;
    }
    return parsePostfix(p);
}

/**
 * @brief Parse the postfix chain: `e!`, `e?`, `.field`, `.method(...)`, `[i]`, `[lo..hi]`, and a
 *        `(...)` call.
 *
 * @param p  Parser.
 * @return   The outermost expression so far; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parsePostfix(Parser *p) {
    Expr *e = parsePrimary(p);
    if (!e) return NULL;

    for (;;) {
        /* `e!` is the programmer's assertion that the value is present: it
         * unwraps an option or a result, or drops the nullability of a `?ref T`.
         * The assertion is **checked at runtime** -- a wrong one traps with a source
         * position -- and only a function marked
         * `@unchecked` signs the check away. Position alone separates it from
         * the prefix `!` (`!x` negates, `x!` asserts), and `!=` is a single
         * token, so neither collides with it. */
        if (at(p, "!")) {
            Token *b = take(p);
            Expr *sg = exprNew(p->arena, EX_SIGN, b->line);
            sg->u.sign.operand = e;
            e = sg;
            continue;
        }
        /* `e?` propagates failure to the caller. Where it may appear is a separate rule, decided by the type checker rather than the grammar. */
        if (at(p, "?")) {
            Token *q = take(p);
            Expr *t = exprNew(p->arena, EX_TRY, q->line);
            t->u.try_.operand = e;
            e = t;
            continue;
        }
        if (at(p, ".")) {
            Token *dot = take(p);
            Token *name = expectIdent(p, "a field or method name");
            if (!name) return NULL;

            if (at(p, "(")) {
                Vec args;
                if (!parseArgs(p, &args)) return NULL;
                Expr *m = exprNew(p->arena, EX_METHOD, dot->line);
                m->u.method.recv = e;
                m->u.method.name = name->text;
                m->u.method.args = args;
                /* `dyn Trait(x).m(...)`: the receiver of the call is the **payload**, so existing
                 * method resolution, argument checking, and code generation apply unchanged; the
                 * trait only rides along so dispatch can look it up. This loop builds **new** nodes,
                 * and a flag not carried over would stay on the receiver. */
                if (e->kind == EX_DYN) {
                    m->u.method.recv = e->u.dynv.payload;
                    m->dynTrait = e->u.dynv.traitName;
                } else {
                    m->dynTrait = e->dynTrait;
                }
                e = m;
            } else {
                if (e->kind == EX_DYN || e->dynTrait) {
                    ctxError(p->ctx, dot->line, dot->col,
                             "A `dyn` value is constructed and called in one step; reading a field"
                             " of the payload is not part of that step.",
                             "`dyn %s(...)` must be followed by a method call", e->dynTrait);
                    return NULL;
                }
                Expr *f = exprNew(p->arena, EX_FIELD, dot->line);
                f->u.field.obj = e;
                f->u.field.name = name->text;
                e = f;
            }
        } else if (at(p, "[")) {
            /* `a[i]` indexes, `a[lo..hi]` slices, and `a[i, j]` / `a[r0..r1, c0..c1]`
             * are the two-subscript forms (lowered to `std::nd::at2` / `std::nd::slice2`,
             * so they are plain library calls: no new AST kind, no new runtime shape). */
            Token *br = take(p);
            skipNl(p);
            const bool savedC = p->inCond;
            p->inCond = false;

            typedef struct { Expr *lo, *hi; bool isRange; } SubItem;
            Vec items;
            vecInit(&items, p->arena, sizeof(SubItem));
            for (;;) {
                SubItem it;
                it.lo = NULL;
                it.hi = NULL;
                it.isRange = false;
                if (!at(p, "..")) {
                    it.lo = parseExpr(p);
                    if (!it.lo) { p->inCond = savedC; return NULL; }
                    skipNl(p);
                }
                if (at(p, "..")) {
                    it.isRange = true;
                    take(p);
                    skipNl(p);
                    if (!at(p, "]") && !at(p, ",")) {
                        it.hi = parseExpr(p);
                        if (!it.hi) { p->inCond = savedC; return NULL; }
                        skipNl(p);
                    }
                }
                *(SubItem *)vecPush(&items) = it;
                if (!at(p, ",")) break;
                take(p);
                skipNl(p);
            }
            p->inCond = savedC;
            if (!expect(p, "]", NULL)) return NULL;

            if (items.len == 1) {
                SubItem *it = (SubItem *)vecAt(&items, 0);
                if (it->isRange) {
                    Expr *sl = exprNew(p->arena, EX_SLICE, br->line);
                    sl->u.slice.obj = e;
                    sl->u.slice.lo  = it->lo;
                    sl->u.slice.hi  = it->hi;
                    e = sl;
                } else {
                    Expr *ix = exprNew(p->arena, EX_INDEX, br->line);
                    ix->u.index.obj = e;
                    ix->u.index.index = it->lo;
                    e = ix;
                }
            } else if (items.len >= 2) {
                SubItem *a0 = (SubItem *)vecAt(&items, 0);
                SubItem *a1 = (SubItem *)vecAt(&items, 1);
                Vec args;
                vecInit(&args, p->arena, sizeof(void *));
                *(Expr **)vecPush(&args) = e;
                bool allIdx = true;
                for (size_t k = 0; k < items.len; k++) {
                    SubItem *it = (SubItem *)vecAt(&items, k);
                    if (it->isRange) allIdx = false;
                }
                if (allIdx) {
                    for (size_t k = 0; k < items.len; k++)
                        *(Expr **)vecPush(&args) = ((SubItem *)vecAt(&items, k))->lo;
                    static const char *atNames[] = {"at2", "at3"};
                    if (items.len > 3) {
                        ctxError(p->ctx, br->line, br->col,
                                 "up to three indices are supported (`v[i, j]`, `v[i, j, k]`);"
                                 " for higher ranks take a slice first",
                                 "a subscript list holds at most three indices");
                        return NULL;
                    }
                    sugarNeedModule(p, "std::nd", br->line);
                    e = moduleCall(p, "std::nd", atNames[items.len - 2], &args, br->line);
                } else if (items.len != 2) {
                    ctxError(p->ctx, br->line, br->col,
                             "range subscripts come in pairs: `v[r0..r1, c0..c1]`",
                             "a range in a subscript list needs two items");
                    return NULL;
                } else if (a0->isRange && a1->isRange) {
                    if (!a0->lo || !a0->hi || !a1->lo || !a1->hi) {
                        ctxError(p->ctx, br->line, br->col,
                                 "write both ends of each range: `v[r0..r1, c0..c1]`",
                                 "an open-ended range is not supported in a two-subscript"
                                 " slice");
                        return NULL;
                    }
                    *(Expr **)vecPush(&args) = a0->lo;
                    *(Expr **)vecPush(&args) = a0->hi;
                    *(Expr **)vecPush(&args) = a1->lo;
                    *(Expr **)vecPush(&args) = a1->hi;
                    sugarNeedModule(p, "std::nd", br->line);
                    e = moduleCall(p, "std::nd", "slice2", &args, br->line);
                } else {
                    ctxError(p->ctx, br->line, br->col,
                             "use two indices (`v[i, j]`) or two ranges"
                             " (`v[r0..r1, c0..c1]`), not one of each",
                             "a two-subscript expression mixes an index and a range");
                    return NULL;
                }
            }
        } else if (at(p, "(")) {
            Vec args;
            if (!parseArgs(p, &args)) return NULL;
            Expr *c = exprNew(p->arena, EX_CALL, e->line);
            c->u.call.callee = e;
            c->u.call.args = args;
            e = c;
        } else {
            return e;
        }
    }
}

/**
 * @brief Parse a parenthesized, comma-separated argument list into `out`.
 *
 * The cursor must be on the opening parenthesis, which this function consumes. `p->inCond` is
 * cleared inside the parentheses, because there a `{` is again unambiguously a struct literal.
 *
 * @param p    Parser.
 * @param out  Vector that receives the Expr pointers; initialized here, so the caller may pass an
 *             uninitialized one.
 * @return     `true` when the closing `)` was consumed; `false` on error (the diagnostic has been
 *             reported).
 */
static bool parseArgs(Parser *p, Vec *out) {
    vecInit(out, p->arena, sizeof(void *));
    if (!expect(p, "(", NULL)) return false;
    skipNl(p);
    const bool savedCond = p->inCond;
    p->inCond = false;
    while (!at(p, ")")) {
        Expr *a = parseExpr(p);
        if (!a) return false;
        *(Expr **)vecPush(out) = a;
        if (accept(p, ",")) skipNl(p);
        else break;
    }
    skipNl(p);
    p->inCond = savedCond;
    return expect(p, ")", NULL);
}

/**
 * @brief Whether the `( ... )` after an expression-position `fn` is a **type list** rather than
 *        `name: Type`.
 *
 * An expression-position `fn` reuses the same keyword (syntax B): if the first parameter is not
 * followed by `:`, the list is a type list; an empty list returns `false` and is left to
 * `parseLambda` to decide after the return type.
 *
 * @param p  Parser, cursor on `fn`.
 * @return   `true` when the first parameter is not followed by `:`; `false` for an empty list or
 *           when a `:` follows.
 */
static bool fnParenIsTypeList(Parser *p) {
    Token *first = pk(p, 2);                       /* pk(1) is the `(` */
    if (!first || !first->text || strcmp(first->text, ")") == 0) return false;
    Token *after = pk(p, 3);
    return !(after && after->text && strcmp(after->text, ":") == 0);
}

/**
 * @brief Parse `fn(A, B) -> R(expr)` in expression position: convert a value to a function type.
 *
 * The type itself is parsed by `parseFnType` (the same routine as in type position), so a function
 * type has only one parser. What follows is an ordinary `(expr)`, consistent with other conversion
 * spellings (`i32(x)`).
 *
 * @param p  Parser.
 * @return   The new EX_CONV expression; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseFnConv(Parser *p) {
    Token *ft = cur(p);
    Type *t = parseFnType(p);
    if (!t) return NULL;
    if (!expect(p, "(", NULL)) return NULL;
    skipNl(p);
    Expr *in = parseExpr(p);
    if (!in) return NULL;
    skipNl(p);
    if (!expect(p, ")", NULL)) return NULL;
    Expr *cv = exprNew(p->arena, EX_CONV, ft->line);
    cv->u.conv.type    = t;
    cv->u.conv.operand = in;
    return cv;
}

/**
 * @brief Parse an expression-position `fn` closure literal: parameters, optional capture list,
 *        return type, and body.
 *
 * The return type may be omitted and is inferred from the body, as for a named function; the
 * capture list names only variables the **body will write** (reads are captured by value and need
 * no entry). When what follows is `(` instead of `{` or `[`, the shape is really a function-type
 * conversion, and this function builds that conversion node as well.
 *
 * @param p  Parser.
 * @return   The new EX_LAMBDA (or EX_CONV); `NULL` on error (the diagnostic has been reported).
 */
static Expr *parseLambda(Parser *p) {
    Token *ft = take(p);
    Expr *e = exprNew(p->arena, EX_LAMBDA, ft->line);
    vecInit(&e->u.lambda.params, p->arena, sizeof(void *));
    vecInit(&e->u.lambda.captures, p->arena, sizeof(void *));
    /* The parameter **types** as well, for the one case where this turns out to be a conversion
     * rather than a literal (`fn(i64) -> i64(p)`): a function type holds types, not `Param`s. */
    Vec paramTypes;
    vecInit(&paramTypes, p->arena, sizeof(void *));
    vecInit(&e->u.lambda.inits, p->arena, sizeof(void *));
    e->u.lambda.ret = NULL;
    e->u.lambda.body = NULL;
    e->u.lambda.sdef = NULL;
    e->u.lambda.tname = NULL;

    if (!expect(p, "(", NULL)) return NULL;
    skipNl(p);
    while (!at(p, ")")) {
        Token *pn = expectIdent(p, "a parameter name");
        if (!pn) return NULL;
        if (!expect(p, ":", "parameters must be typed: `name: Type`")) return NULL;
        Type *pt = parseType(p);
        if (!pt) return NULL;
        Param *pm = (Param *)arenaAllocZero(p->arena, sizeof(Param));
        pm->name = pn->text;
        pm->type = pt;
        pm->line = pn->line;
        *(Param **)vecPush(&e->u.lambda.params) = pm;
        *(Type **)vecPush(&paramTypes) = pt;
        if (accept(p, ",")) skipNl(p);
        else break;
    }
    if (!expect(p, ")", NULL)) return NULL;
    skipNl(p);
    if (at(p, "[")) {                          /* capture list: only the ones the body will **write** */
        take(p);
        skipNl(p);
        while (!at(p, "]")) {
            Token *mt = cur(p);
            if (!at(p, "mut") || !(pk(p, 1) && pk(p, 1)->text && strcmp(pk(p, 1)->text, "ref") == 0)) {
                ctxError(p->ctx, mt->line, mt->col,
                         "A captured read needs no entry: it is captured by value. Write an entry"
                         " only for a variable the body writes.",
                         "a capture list entry is `mut ref name`, found `%s`", shown(mt));
                return NULL;
            }
            take(p);                           /* mut */
            take(p);                           /* ref */
            Token *cn = expectIdent(p, "a captured variable name");
            if (!cn) return NULL;
            LamCap *cap = (LamCap *)arenaAllocZero(p->arena, sizeof(LamCap));
            cap->name = cn->text;
            cap->mutRef = true;
            cap->line = cn->line;
            *(LamCap **)vecPush(&e->u.lambda.captures) = cap;
            if (accept(p, ",")) skipNl(p);
            else break;
        }
        if (!expect(p, "]", NULL)) return NULL;
        skipNl(p);
    }
    if (accept(p, "->")) {
        e->u.lambda.ret = parseType(p);
        if (!e->u.lambda.ret) return NULL;
    }
    /* This is a **conversion**, not a literal: `fn(i64) -> i64(p)`. The two shapes can only be told
     * apart here: a literal's body is `{` (or the capture list `[` read above), while a conversion
     * continues with `(`. The spelling matches other conversions -- `type(expr)` -- and it is
     * unambiguous because a function type always writes its return type. */
    if (at(p, "(")) {
        take(p);                                   /* ( */
        skipNl(p);
        Expr *in = parseExpr(p);
        if (!in) return NULL;
        skipNl(p);
        if (!expect(p, ")", NULL)) return NULL;
        if (!e->u.lambda.ret) {
            ctxError(p->ctx, ft->line, ft->col,
                     "A conversion writes the type it converts **to**, and a function type always"
                     " writes its return type: `fn(i64) -> i64(p)`.",
                     "a function-type conversion needs a return type");
            return NULL;
        }
        Expr *cv = exprNew(p->arena, EX_CONV, ft->line);
        cv->u.conv.type    = typeFn(p->arena, &paramTypes, e->u.lambda.ret);
        cv->u.conv.operand = in;
        return cv;
    }
    skipNl(p);
    e->u.lambda.body = parseBlock(p);
    if (!e->u.lambda.body) return NULL;
    return e;
}

/**
 * @brief Parse a primary expression: a literal, `null`, `new`, a parenthesized expression, an array
 *        or struct literal, an explicit conversion, or a name (including qualified names and
 *        associated calls).
 *
 * @param p  Parser.
 * @return   The new Expr; `NULL` on error (the diagnostic has been reported).
 */
static Expr *parsePrimary(Parser *p) {
    Token *t = cur(p);

    /* `fn(x: i64) -> i64 { ... }`: a closure literal. `fn` reaches this point only in **expression**
     * position and only when followed by `(` -- that is exactly what distinguishes it from a
     * declaration; the top-level and struct-body loops handle their own `fn` branches. Reusing `fn`
     * adds zero symbols to the language, which is the point of syntax B. */
    /* `fn(i64) -> i64(p)`: a **conversion**, not a literal.
     *
     * The two shapes are told apart by the **parameter list**: a literal's parameters are
     * `name: Type` and a type's are bare types, so the token after the first parameter decides
     * (`,` / `)` / `->` means a type list, `:` means a literal). Nothing else in the language begins
     * with `fn(` in expression position, so that is the whole test. The empty list (`fn() -> R(...)`)
     * is decided later, in `parseLambda`, because only what follows the return type tells it apart
     * from `fn() -> R { ... }`. */
    if (at(p, "fn") && pk(p, 1) && pk(p, 1)->text && strcmp(pk(p, 1)->text, "(") == 0 &&
        fnParenIsTypeList(p))
        return parseFnConv(p);
    if (at(p, "fn") && pk(p, 1) && pk(p, 1)->text && strcmp(pk(p, 1)->text, "(") == 0)
        return parseLambda(p);

    /* `dyn Trait(expr)` -- the construction for dynamic dispatch.
     *
     * Stage 1 keeps construction and the call in **one expression**: the mark set below travels
     * to the `EX_METHOD` node the postfix loop builds, and anything that is not a method call on
     * it is refused here. Storing a `dyn` value needs a pool and arrives in stage 3, so there is
     * deliberately no way to write one yet. */
    /* `dyn` is **not** a reserved word: `let dyn = a[lo..hi]` is a legal binding (examples/slices.extc
     * writes exactly that, and the first version of this branch broke it). The form is entered only
     * for the shape `dyn Trait(`, which no expression can otherwise begin with. */
    if (at(p, "dyn") && pk(p, 1) && pk(p, 1)->kind == TK_IDENT &&
        pk(p, 2) && pk(p, 2)->text && strcmp(pk(p, 2)->text, "(") == 0) {
        take(p);                            /* dyn */
        Token *tn = cur(p);
        if (tn->kind != TK_IDENT) {
            ctxError(p->ctx, tn->line, tn->col, NULL,
                     "expected a trait name after `dyn`, found `%s`", shown(tn));
            return NULL;
        }
        take(p);
        if (!expect(p, "(", NULL)) return NULL;
        Expr *payload = parseExpr(p);
        if (!payload) return NULL;
        if (!expect(p, ")", NULL)) return NULL;
        /* `dyn Trait(x)` as a **value**. An immediately attached method call
         * `dyn Trait(x).m(...)` is built by the postfix loop, which takes the **payload** as the
         * call's receiver -- that path is unchanged, and the stage-1/2 criteria still run the same
         * code. */
        Expr *dv = exprNew(p->arena, EX_DYN, tn->line);
        dv->u.dynv.payload = payload;
        dv->u.dynv.traitName = tn->text;
        return dv;
    }

    if (t->kind == TK_INT) {
        take(p);
        Expr *e = exprNew(p->arena, EX_INT, t->line);
        e->u.ival = t->ival;
        return e;
    }
    if (t->kind == TK_FLOAT) {
        take(p);
        Expr *e = exprNew(p->arena, EX_FLOAT, t->line);
        e->u.fval = t->fval;
        return e;
    }
    if (t->kind == TK_STRING) {
        take(p);
        Expr *e = exprNew(p->arena, EX_STR, t->line);
        e->u.str.text = t->text;
        return e;
    }
    /* A value-position f-string: expanded into a `+` chain of `stl::string::from(text segment) +
     * slot ...` (the result is a `string` value). It needs that module to be imported -- the full
     * path makes any alias work. */
    if (t->kind == TK_FSTRING) return fmtValue(p, take(p));
    /* `null` is a contextual keyword, recognised in expression position only, like `ref` and `mut`.
     * Its type comes entirely from the context: `var p: ?ref node = null`, `if p != null`, or
     * `return null` when the return type is `?ref T`. */
    if (at(p, "null")) {
        take(p);
        return exprNew(p->arena, EX_NULL, t->line);
    }
    if (at(p, "true") || at(p, "false")) {
        take(p);
        Expr *e = exprNew(p->arena, EX_BOOL, t->line);
        e->u.bval = (t->text[0] == 't');
        return e;
    }
    /* `new T` / `new T[n]` / `new [N]T` takes a piece of zeroed memory from the current block's
     * arena. `new` is a contextual keyword, recognised in expression position only. */
    if (at(p, "new")) {
        Token *kw = take(p);
        skipNl(p);
        Expr *e = exprNew(p->arena, EX_NEW, kw->line);
        Type *ty = parseType(p);
        if (!ty) return NULL;
        e->u.new_.type = ty;
        /* No skipNl here.  Statements end at a newline, so consuming one would
         * swallow the next line into this expression: with `new [3]i32` on its
         * own line, the following line disappeared and the error was "expected
         * an expression, found `=`".  `new T[n]` therefore has to be written on
         * one line, as `a[..]` already is. */
        if (at(p, "[")) {
            take(p);
            skipNl(p);
            const bool savedC = p->inCond;
            p->inCond = false;
            Expr *n = parseExpr(p);
            p->inCond = savedC;
            if (!n) return NULL;
            e->u.new_.count = n;
            skipNl(p);
            if (!expect(p, "]", NULL)) return NULL;
        }
        return e;
    }
    if (at(p, "(")) {
        take(p);
        skipNl(p);
        const bool saved = p->inCond;
        p->inCond = false;          /* inside parentheses a `{` is a literal again */
        Expr *e = parseExpr(p);
        p->inCond = saved;
        if (!e) return NULL;
        skipNl(p);
        if (!expect(p, ")", NULL)) return NULL;
        return e;
    }
    if (at(p, "{")) return parseStructLit(p, NULL);

    /* Array literal `[1, 2, 3]`. A trailing `...` means the remaining elements are zero. */
    if (at(p, "[")) {
        Token *br = take(p);
        Expr *e = exprNew(p->arena, EX_ARRAYLIT, br->line);
        vecInit(&e->u.arraylit.elems, p->arena, sizeof(void *));
        e->u.arraylit.rest = false;

        skipNl(p);
        while (!at(p, "]")) {
            if (at(p, "...")) {
                take(p);
                e->u.arraylit.rest = true;
                skipNl(p);
                break;
            }
            const bool savedC = p->inCond;
            p->inCond = false;
            Expr *el = parseExpr(p);
            p->inCond = savedC;
            if (!el) return NULL;
            *(Expr **)vecPush(&e->u.arraylit.elems) = el;

            skipNl(p);
            if (accept(p, ",")) { skipNl(p); continue; }
            break;
        }
        skipNl(p);
        if (!expect(p, "]", NULL)) return NULL;
        return e;
    }

    /* The ten scalar type names lex as TK_TYPE -- neither TK_IDENT nor TK_KEYWORD -- so this branch
     * must be tested before the identifier branch below. */
    if (t->kind == TK_TYPE && isScalarTypeName(t->text)) {
        take(p);
        if (!at(p, "(")) {
            ctxError(p->ctx, t->line, t->col,
                     "a type name only appears in expression position as an explicit"
                     " conversion, e.g. `i32(x)`",
                     "`%s` is a type, not a value -- did you mean `%s(x)`?", t->text, t->text);
            return NULL;
        }
        take(p);                                   /* ( */
        skipNl(p);
        Expr *in = parseExpr(p);
        if (!in) return NULL;
        skipNl(p);
        if (!expect(p, ")", NULL)) return NULL;
        Expr *cv2 = exprNew(p->arena, EX_CONV, t->line);
        cv2->u.conv.typeName = t->text;
        cv2->u.conv.operand  = in;
        return cv2;
    }
    if (t->kind == TK_IDENT) {
        take(p);
        /* Debug switch `EXTC_DBG_QN=1`: print the start position of every identifier in expression
         * position plus the four tokens that follow it, so a qualified name can be watched segment by
         * segment. Printing both the cursor position and the token texts is deliberate: every line
         * must correspond to exactly one call, or interleaved call sites make the output look like the
         * call being investigated.
         *
         * stderr only; token dumps such as `--dump-tokens` are unaffected. */
        if (dbgOn("EXTC_DBG_QN")) {
            fprintf(stderr, "[qn ENT] pos=%d cur=`%s` n1=`%s` n2=`%s` n3=`%s` n4=`%s`\n",
                    (int)p->pos, t->text, pk(p,0)->text, pk(p,1)->text,
                    pk(p,2)->text, pk(p,3)->text);
        }
        /* `i32(x)` / `f64(y)`: an explicit conversion -- narrowing, changing signedness, or crossing
         * between integer and float. C writes `(T)x`, but that is ambiguous with a parenthesized
         * expression here because the parser has no symbol table; moving the parentheses onto `T(x)`
         * removes the ambiguity for the same meaning. */
        if (isScalarTypeName(t->text) && at(p, "(")) {
            take(p);                                  /* ( */
            skipNl(p);
            Expr *in = parseExpr(p);
            if (!in) return NULL;
            skipNl(p);
            if (!expect(p, ")", NULL)) return NULL;
            Expr *cv = exprNew(p->arena, EX_CONV, t->line);
            cv->u.conv.typeName = t->text;
            cv->u.conv.operand  = in;
            return cv;
        }
        /* `name { ... }` is a struct literal.
         *
         * The rule is positional, as in Go: inside an `if` / `while` condition a `{` starts the body
         * block, and a literal there needs parentheses: `if x == (point { a: 1 }) { }`. A type name
         * therefore need not be distinguished by case -- the rule lives in the grammar instead of
         * being hidden in the spelling. */
        if (at(p, "{") && !p->inCond) return parseStructLit(p, t->text);

        /* Qualified type name: `mod::Type { ... }` and `mod::Type.variant`.
         *
         * This syntax has to exist because two modules may each define a type of the same name, and a
         * bare name cannot say which: the loader deliberately registers no return path for an
         * ambiguous bare name, so it never resolves. Without this syntax a literal of such a type
         * could not be written at all.
         *
         * The path is only joined into one string, with no symbol table lookup, exactly as in type
         * position; whether it names a module or a type is decided by the loader. `mod::fn(...)` is
         * unaffected: when neither `.` nor `{` follows, the code falls through to `looksLikeAssoc`. */
        if (at(p, "::")) {
            /* Look only, do not consume. `mod::fn(args)` is the overwhelmingly common case, and
             * eating its tokens would break the `looksLikeAssoc` path that handles it: moving `pos`
             * turns the prelude's `pcg32::withStream(...)` into `pcg32` plus garbage. Take over only
             * when a `{` or `.` really follows. */
            int k = 0;
            bool takeIt = false;
            for (;;) {
                if (strcmp(pk(p, k)->text, "::") != 0) break;
                Token *seg = pk(p, k + 1);
                if (seg->kind != TK_IDENT && seg->kind != TK_TYPE) break;
                k += 2;
                /* A `{` is a struct literal of a qualified type: that is this path's job.
                 *
                 * A `.` is not taken over here: building `EX_IDENT("mod::value")` by hand yields
                 * a node the checker cannot resolve, because a qualified name only becomes an
                 * expression through the `EX_ASSOC` shape `looksLikeAssoc` builds. Every
                 * "qualified value followed by a dot" needs that shape: `io::cin.bad()` otherwise
                 * says "only direct function calls are supported" and `io::cin.fd` says "undefined
                 * name", while `cin.fd`, `io::cin >> x` and `io::cin` alone are all fine - two
                 * representations of one thing, and only one of them knows about values.
                 *
                 * The dot therefore goes to the postfix loop, and whether it is a field, a method or a
                 * variant of a qualified *type* is decided in the checker, where the symbol table
                 * is. */
                if (strcmp(pk(p, k)->text, "{") == 0 && !p->inCond) { takeIt = true; break; }
                if (strcmp(pk(p, k)->text, ".") == 0) break;
            }
            if (takeIt) {
                Buf b;
                bufInit(&b, p->arena);
                bufPuts(&b, t->text);
                for (int j = 0; j < k; j += 2) {
                    take(p);                                   /* `::` */
                    bufPuts(&b, "::");
                    bufPuts(&b, take(p)->text);                /* segment name */
                }
                if (at(p, "{")) return parseStructLit(p, bufCstr(&b));
                /* `mod::Type.variant` becomes a field access, and the loader
                 * rewrites the name; the checker's existing enum-variant path
                 * then handles it unchanged. */
                take(p);                                       /* `.` */
                Token *vn = expectIdent(p, "a variant name");
                if (!vn) return NULL;
                Expr *id = exprNew(p->arena, EX_IDENT, t->line);
                id->u.ident.name = bufCstr(&b);
                Expr *fa = exprNew(p->arena, EX_FIELD, t->line);
                fa->u.field.obj  = id;
                fa->u.field.name = vn->text;
                return fa;
            }
        }

        /* An associated call `option<i64>::some(x)` / `point::origin()`, or a qualified name of any
         * depth `std::sys::io::write(...)` / `std::sys::io::STDOUT`.
         *
         * `IDENT <` is ambiguous with less-than, so first look ahead without consuming to see where
         * the type arguments would end and whether `::` follows, and only then parse for real.
         * Deciding first keeps a speculative parse from spraying meaningless errors. The criterion is
         * `::`: it is not a legal binary operator, the two readings are mutually exclusive, so the
         * guess cannot be wrong. */
        {
            const size_t assocPos = p->pos;      /* cursor is on the first `::` */
            if (looksLikeAssoc(p)) {
                const char *modPrefix = NULL;
                Expr *ae = parseAssoc(p, t->text, t->line, assocPos, &modPrefix);
                /* `modPrefix` is a field of the **assoc** union member. `parseAssoc` returns an
                 * `EX_ASSOC` for a qualified type/value, but an `EX_CALL` when the path ends in a
                 * call (`sysio::write(...)`) -- and writing `u.assoc.modPrefix` on *that* node used
                 * to scribble over whatever the `call` member has at the same offset. It was
                 * harmless only because nothing read those bytes; adding one field to `call` was
                 * enough to turn it into "every `a::b(...)` call has a bogus trailing block".
                 * Guard on the kind: only the node that owns the field gets written. */
                if (ae && ae->kind == EX_ASSOC) ae->u.assoc.modPrefix = modPrefix;
                return ae;
            }
        }

        /* A `{` in a condition starts the body block -- but `{ ident :` is clearly a literal with
         * missing parentheses, so the error gives the line the reader should have written. */
        if (at(p, "{") && p->inCond &&
            pk(p, 1)->kind == TK_IDENT && strcmp(pk(p, 2)->text, ":") == 0) {
            Token *bt = cur(p);
            ctxError(p->ctx, bt->line, bt->col,
                     "Inside an `if` / `while` condition a `{` starts the body block. "
                     "To write a struct literal there, wrap it in parentheses.",
                     "struct literal in a condition needs parentheses: `(%s { ... })`",
                     t->text);
            return NULL;
        }

        Expr *e = exprNew(p->arena, EX_IDENT, t->line);
        e->u.ident.name = t->text;
        return e;
    }

    ctxError(p->ctx, t->line, t->col, NULL,
             "expected an expression, found `%s`", shown(t));
    return NULL;
}

/**
 * @brief Whether a name is followed by an optional type-argument list and `::name`, without moving
 *        the cursor.
 *
 * Look only; the cursor stays where it is. `IDENT <` is ambiguous with less-than, so the decision
 * has to be made before parsing starts, or the speculative parse would spray meaningless errors.
 * The criterion is `::`: it is not a legal binary operator, so an associated call and an `a < b`
 * comparison cannot both hold, and the guess can never be wrong.
 *
 * Returning the number of segments rather than a bool is exactly what callers need: in
 * `std::sys::io::STDOUT` the cursor is on the **first** `::`, and a caller that only asked "is this
 * qualified?" would consume `::` plus one identifier (`sys`) and leave the cursor on `::io::STDOUT`.
 * How many segments follow is known only to this scan.
 *
 * @param p  Parser, cursor on the first `::`.
 * @return   The number of segments after the first `::`; 0 when this is not a qualified name.
 */
static int looksLikeAssoc(Parser *p) {
    size_t i = 0;

    if (strcmp(pk(p, i)->text, "<") == 0) {
        int depth = 0;
        for (;; i++) {
            Token *t = pk(p, i);
            if (t->kind == TK_EOF) return 0;
            if (strcmp(t->text, "<") == 0) {
                depth++;
            } else if (strcmp(t->text, ">") == 0) {
                if (--depth == 0) { i++; break; }
            } else if (t->kind == TK_IDENT || t->kind == TK_TYPE ||
                       t->kind == TK_INT) {
                /* a type name, a builtin type, or an array length */
            } else if (t->kind == TK_KEYWORD) {
                /* the contextual keywords `ref` and `mut` */
            } else if (strcmp(t->text, ",") == 0 || strcmp(t->text, "[") == 0 ||
                       strcmp(t->text, "]") == 0) {
            } else {
                return 0;           /* something a type cannot contain: not type args */
            }
        }
    }
    /* `Name<targs>::fn(...)` is an associated call; `name<T>(...)` is a generic
     * call (only the builtin primitives use it today, e.g. `alloc<i32>(n)`).
     * Both share the scanning above and are told apart by `::` versus `(`.
     * A generic call is only possible once type arguments have actually been
     * seen; otherwise an ordinary `f(x)` would be misread as one. */
    int segs = 0;
    while (strcmp(pk(p, i)->text, "::") == 0) {
        Token *seg = pk(p, i + 1);
        if (seg->kind != TK_IDENT && seg->kind != TK_TYPE) break;
        i += 2;
        segs++;
    }
    if (segs > 0) return segs;
    return (strcmp(pk(p, i)->text, "(") == 0 && i > 0) ? 1 : 0;
}

/**
 * @brief Count how many complete `::name` segments follow the cursor.
 *
 * `looksLikeAssoc` cannot answer this: it stops at questions like "is a `(`, `{`, or `.` immediately
 * after it", while parsing a path needs "how long is the path". The cursor must be on a `::`.
 *
 * @param p  Parser, cursor on a `::`.
 * @return   Number of segments; 0 when the cursor is not on a `::`.
 */
static int countFollowingSegs(Parser *p) {
    int k = 0;
    while (strcmp(pk(p, k)->text, "::") == 0) {
        Token *seg = pk(p, k + 1);
        if (seg->kind != TK_IDENT && seg->kind != TK_TYPE) break;
        k += 2;
    }
    return k / 2;
}

/**
 * @brief Parse a qualified name: `Name<targs>::name(args)`, `Type::assoc(args)`, or an arbitrarily
 *        deep `mod::sub::name(args)` / `mod::sub::CONST`.
 *
 * Two-segment names and deep paths share this one path; what follows the last segment decides: `(`
 * means a call (`isCall = true`), anything else is a value such as a constant or a payload-free
 * variant (`isCall = false`).
 *
 * A value is an EX_ASSOC too, not an EX_IDENT: the loader walks a known set of node kinds when it
 * resolves qualified names, an EX_IDENT with `::` in its name is invisible to it, and the checker
 * would look up the bare name and report it undefined.
 *
 * `modPrefixOut` receives a provisional split, `std::sys::io` plus `STDOUT`; the final split belongs
 * to the loader, which alone knows which modules were imported -- what is handed out here is the
 * path cut at the last `::`.
 *
 * @param p             Parser.
 * @param name          Leading name, already consumed by the caller.
 * @param line          Line of that name, attached to the node.
 * @param startPos      Cursor position when the scan started (currently unused).
 * @param modPrefixOut  Receives the module prefix; pass `NULL` when the caller does not need it.
 * @return              An EX_ASSOC node, or an EX_GENCALL node for `name<T>(...)`; `NULL` on error
 *                      (the diagnostic has been reported).
 */
static Expr *parseAssoc(Parser *p, const char *name, int line, size_t startPos,
                        const char **modPrefixOut) {
    Vec targs;
    vecInit(&targs, p->arena, sizeof(void *));
    if (accept(p, "<")) {
        skipNl(p);
        for (;;) {
            Type *a = parseType(p);
            if (!a) return NULL;
            *(Type **)vecPush(&targs) = a;
            if (accept(p, ",")) { skipNl(p); continue; }
            break;
        }
        skipNl(p);
        if (!expect(p, ">", NULL)) return NULL;
    }
    /* No `::`, so this is a generic call `name<T>(args)`, as in
     * `alloc<i32>(n)`. */
    if (!at(p, "::")) {
        Vec gargs;
        if (!parseArgs(p, &gargs)) return NULL;
        Expr *g = exprNew(p->arena, EX_GENCALL, line);
        g->u.gencall.name  = name;
        g->u.gencall.targs = targs;
        g->u.gencall.args  = gargs;
        return g;
    }

    /* Path: consume `::name` segments but leave the last one, which is the
     * symbol name.
     *
     * The last segment stays because what follows it decides the meaning: `(`
     * is a call, `{` a literal, `.` a variant, anything else a value.  That
     * token has to remain under the cursor for the caller to see.  With two
     * segments the loop below never runs at all (`fn` in `mod::fn` is not
     * followed by `::`), so the two-segment path is untouched.
     *
     * The test is `> 1`, not `>= 2`, because the last segment must survive.
     * `std::sys::io::STDOUT` splits into the prefix `std::sys::io` and the
     * symbol `STDOUT`: `::sys` and `::io` are consumed, `::STDOUT` is left.
     * With `>= 2` one segment too many is consumed, so the prefix becomes
     * `std::sys` and the symbol `io`, and the loader then reports
     * "`sys` is not imported" -- two steps away from the real mistake.
     *
     * The prefix has to be built while those tokens are consumed; pulling text
     * out of the token range afterwards cannot work, because the range starts
     * after the first `::` and the leading name (`std`) is not in it.  Doing it
     * that way yielded `sys::io` instead of `std::sys::io`. */
    const char *prefix = NULL;
    if (countFollowingSegs(p) > 1) {
        Buf pb;
        bufInit(&pb, p->arena);
        bufPuts(&pb, name);                          /* first segment, taken by caller */
        while (countFollowingSegs(p) > 1) {
            take(p);                                 /* `::` */
            bufPuts(&pb, "::");
            bufPuts(&pb, take(p)->text);             /* a middle segment */
        }
        prefix = bufCstr(&pb);
    }
    take(p);                                         /* `::` */
    Token *sym = expectIdent(p, "a function, constant or variant name");
    if (!sym) return NULL;

    /* One test decides it: a `(` after the LAST segment makes this a call.
     * Without one it is a value, such as a module constant -- C has no function
     * pointers and extC has no function values, so a qualified name without
     * parentheses can only be a constant.  The split goes to the loader. */
    const bool isCall = !targs.len && at(p, "(");
    if (modPrefixOut) *modPrefixOut = prefix;

    Vec args;
    vecInit(&args, p->arena, sizeof(void *));
    /* The argument list may be omitted: `maybe<i64>::nothing` is how a variant
     * without a payload is written.  One with a payload needs its parentheses:
     * `option<i64>::some(3)`. */
    if (at(p, "(") && !parseArgs(p, &args)) return NULL;

    Expr *e = exprNew(p->arena, EX_ASSOC, line);
    e->u.assoc.typeName = name;
    e->u.assoc.targs = targs;
    e->u.assoc.name = sym->text;
    e->u.assoc.args = args;
    e->u.assoc.isCall = isCall;
    (void)startPos;
    return e;
}

/* Parse a struct literal `{ field: value, ... }`.
 *
 * Params:
 *   name - the struct's name, or NULL for an anonymous `{ ... }` literal
 *
 * Returns:
 *   The new EX_STRUCTLIT node, or NULL after reporting an error.
 */
static Expr *parseStructLit(Parser *p, const char *name) {    Token *open = cur(p);
    if (!expect(p, "{", NULL)) return NULL;

    Expr *e = exprNew(p->arena, EX_STRUCTLIT, open->line);
    e->u.lit.name = name;
    vecInit(&e->u.lit.inits, p->arena, sizeof(void *));

    skipNl(p);
    while (!at(p, "}")) {
        Token *fn = expectIdent(p, "a field name");
        if (!fn) return NULL;
        if (!expect(p, ":", NULL)) return NULL;
        skipNl(p);
        Expr *v = parseExpr(p);
        if (!v) return NULL;

        FieldInit *fi = (FieldInit *)arenaAllocZero(p->arena, sizeof(FieldInit));
        fi->name = fn->text;
        fi->value = v;
        *(FieldInit **)vecPush(&e->u.lit.inits) = fi;

        skipNl(p);
        accept(p, ",");
        skipNl(p);
    }
    if (!expect(p, "}", NULL)) return NULL;
    return e;
}
