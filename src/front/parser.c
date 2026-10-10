/* extC syntax, part two of the pipeline: the recursive-descent parser.
 *
 * Turns the token vector from lexAll into ast.h nodes (see parser.h for the
 * accepted grammar).  It resolves no names: identifiers, module paths, and
 * generic arguments are kept exactly as written, so the loader and the type
 * checker can decide what they mean.  `{` after a name is a struct literal by
 * position, not by capitalization, and p->inCond records where a `{` must be
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
    /* 解析 `if` / `while` / `match` 的条件期间为真。条件里的 `{` 开始体块，所以结构体字面量
     * 必须加括号 —— 这是位置规则，不靠类型名的大小写。 */
    bool   inCond;
    bool   noBody;
    bool   sawBuiltin;   /* the annotation loop saw @builtin for this declaration */      /* true while parsing an `extern!` signature (no body) */
    /* Nesting depth of the recursive descent. Two counters, because the two families have
     * different safe budgets and different "who reports it first" stories:
     *   depth     -- blocks and expressions (see `parseTooDeep`);
     *   typeDepth -- types, where the checker has its own 64-level limit and should get to
     *                report first (`TYPE_DEPTH_LIMIT`).
     * The counters are what turn "too deep to parse" into a diagnostic instead of a crash. */
    int    depth;
    int    typeDepth;
    /* Set by the `global` soft keyword just before a declaration and consumed by `parseVarDecl`
     * (`ST_VAR.isGlobal`). Soft, not a lexer keyword: `global` stays a legal identifier. */
    bool   sawGlobal;
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
 * @brief 看光标前方第 `k` 个 token，不移动光标。
 *
 * 越过末尾时钳到收尾的 TK_EOF：调用方因此不用做边界检查，就能固定向前读若干个 token。
 *
 * @param p  解析器。
 * @param k  前瞻距离（token 数）；0 就是光标处的 token。
 * @return   该 token；距离越过末尾时返回收尾的 TK_EOF。
 */
static Token *pk(Parser *p, size_t k) {
    size_t i = p->pos + k;
    if (i >= p->toks->len) i = p->toks->len - 1;
    return (Token *)vecAt(p->toks, i);
}

/**
 * @brief 返回光标处的 token，不消费它。
 *
 * @param p  解析器。
 * @return   光标处的 token。
 */
static Token *cur(Parser *p) { return pk(p, 0); }

/**
 * @brief 给一个嵌套计数器加一，越过 `limit` 时报告「嵌套太深」并把这一层减回去。
 *
 * 计数器就是解析器的递归预算：C 的栈有限，深的嵌套语法会让每个递归的 pass 一起爆栈，
 * 所以这里先给出诊断，而不是让编译进程带着 SIGSEGV 倒下。
 *
 * @param counter  要增加的嵌套计数器（`Parser.depth` 或 `Parser.typeDepth`）。
 * @param limit    该计数器的上限。
 * @param p        解析器，用于定位与报告诊断。
 * @param what     诊断里点名的语法种类，例如 `a type`。
 * @return         超过上限时为 `true`（已报告诊断并把本次计数减回去）；否则为 `false`。
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
 * @brief 为语句块与表达式增加一层嵌套深度预算。
 *
 * @param p     解析器。
 * @param what  诊断里点名的语法种类。
 * @return      超过 `PARSE_DEPTH_LIMIT` 时为 `true`。
 */
static bool parseTooDeep(Parser *p, const char *what) {
    return parseTooDeepIn(&p->depth, PARSE_DEPTH_LIMIT, p, what);
}

/**
 * @brief 为类型增加一层嵌套深度预算。
 *
 * 类型预算比语句块大：类型只经 `parseType` 递归、栈帧小，而且检查器有自己的类型深度
 * 上限，应当由它先报出更准确的诊断。
 *
 * @param p  解析器。
 * @return   超过 `PARSE_TYPE_DEPTH_LIMIT` 时为 `true`。
 */
static bool parseTypeTooDeep(Parser *p) {
    return parseTooDeepIn(&p->typeDepth, PARSE_TYPE_DEPTH_LIMIT, p, "a type");
}

/**
 * @brief 判断光标处的 token 是否正好拼作 `value`。
 *
 * 标点拼写还要求这个 token 本身就是标点。例如 `")"` 与 `"match"` 都要按标点比，而
 * 字符串字面量的 text（literal's text）不带引号，源码 `")"` 的 text 正好是 `)`；
 * 只比 text 会把 `println(")")` 与 `println("x)")` 里的字符串误当成标点，报出
 * "expected an expression, found `)`"。
 *
 * @param p      解析器。
 * @param value  要比对的拼写。
 * @return       相等时为 `true`。
 */
static bool at(Parser *p, const char *value) {
    Token *t = cur(p);
    if (t->kind == TK_STRING && lexIsPunct(value)) return false;
    return strcmp(t->text, value) == 0;
}

/**
 * @brief 判断光标处 token 的 kind 是否为 `k`，不看它的 text。
 *
 * @param p  解析器。
 * @param k  期望的 token 种类。
 * @return   相同则为 `true`。
 */
static bool atKind(Parser *p, TokenKind k) { return cur(p)->kind == k; }

/**
 * @brief 消费并返回光标处的 token。
 *
 * 光标不会越过收尾的 TK_EOF：在畸形输入上不停取 token 的调用方也走不出 token 向量。
 *
 * @param p  解析器。
 * @return   被消费的 token。
 */
static Token *take(Parser *p) {
    Token *t = cur(p);
    if (p->pos + 1 < p->toks->len) p->pos++;
    return t;
}

/**
 * @brief 光标处的 token 拼作 `value` 时消费它。
 *
 * @param p      解析器。
 * @param value  期望的拼写。
 * @return       匹配并已消费为 `true`；否则为 `false`，且光标不动。
 */
static bool accept(Parser *p, const char *value) {
    if (!at(p, value)) return false;
    take(p);
    return true;
}

/**
 * @brief 取 `t` 可打印的拼写，用于错误消息。
 *
 * text 为空的 token（TK_EOF、TK_NEWLINE）改用 kind 名，消息里不会出现 "found ``"。
 *
 * @param t  要展示的 token。
 * @return   该 token 的 text；text 为空时返回其 kind 名。
 */
static const char *shown(const Token *t) {
    return t->text[0] ? t->text : tokenKindName(t->kind);
}

/**
 * @brief 光标处的 token 拼作 `value` 时消费它，否则报告它。
 *
 * @param p      解析器。
 * @param value  要求的拼写，例如 `"{"`。
 * @param note    随诊断一起存的说明，通常是调用方在遵循的语法规则；没有可补的传 `NULL`。
 * @return        消费了为 `true`；否则经 `ctxError` 报告后返回 `false`，调用方必须立刻返回。
 */
static bool expect(Parser *p, const char *value, const char *note) {
    if (at(p, value)) { take(p); return true; }
    Token *t = cur(p);
    ctxError(p->ctx, t->line, t->col, note,
             "expected `%s`, found `%s`", value, shown(t));
    return false;
}

/**
 * @brief 消费光标处的标识符，否则报告缺了什么。
 *
 * @param p     解析器。
 * @param what  对缺失物的描述，接在 "expected " 之后，例如 `"a field name"`。
 * @return      被消费的 token；否则经 `ctxError` 报告后返回 `NULL`。
 */
static Token *expectIdent(Parser *p, const char *what) {
    if (atKind(p, TK_IDENT)) return take(p);
    Token *t = cur(p);
    ctxError(p->ctx, t->line, t->col, NULL,
             "expected %s, found `%s`", what, shown(t));
    return NULL;
}

/**
 * @brief 消费任意长的一串换行 token，别的什么都不动。
 *
 * @param p  解析器。
 */
static void skipNl(Parser *p) {
    while (atKind(p, TK_NEWLINE)) take(p);
}

/**
 * @brief 消费连续的空语句分隔符：换行与可选的 `;`。
 *
 * @param p  解析器。
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
static StructDef *parseStruct(Parser *p);
static FuncDef   *parseFunc(Parser *p);
static bool       parseFuncAnnotations(Parser *p, bool *outInline, bool *outPrivate,
                                       bool *outUnchecked);
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
static Expr *parseLambda(Parser *p);   /* `fn(x: i64) -> i64 { ... }`, LAMBDA.md section 2 */

/**
 * @brief 判断 `n` 是不是十种标量内建类型名之一。
 *
 * `bool` 与 `void` 不算：显式转换要求两侧都是标量数值类型，而 extC 没有隐式真值转换。
 *
 * @param n  类型名。
 * @return   是十种标量类型名之一时为 `true`。
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
 * @brief 建一个二元运算符 Expr 节点。
 *
 * @param p     解析器；节点分配到它的 arena。
 * @param op    运算符拼写，例如 `"||"`；按写出来的样子存给代码生成。
 * @param l     左操作数（已解析）。
 * @param r     右操作数（已解析）。
 * @param line  挂到节点上的源码行，用于诊断。
 * @return      新的 EX_BIN 节点。
 */
static Expr *mkBin(Parser *p, const char *op, Expr *l, Expr *r, int line) {
    Expr *e = exprNew(p->arena, EX_BIN, line);
    e->u.bin.op = op;
    e->u.bin.left = l;
    e->u.bin.right = r;
    return e;
}

/* ================================================================ top level */

/**
 * @brief 解析一条 `use a::b::c` 导入声明。
 *
 * 一个文件就是一个模块，`use std::io` 指的是文件 std/io.extc；搜索路径与循环检查归加载器。
 * 短名是路径的最后一段，也是本文件称呼该模块的名字。这里只记下路径，不做任何解析。
 *
 * @param p  解析器。
 * @return   新的 UseDecl；语法出错时返回 `NULL`。
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
        /* `use std::io::{cin, cout}`：花括号里是**名字列表**，永远不是模块路径。 */
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
    /* `use std::sys::io as sysio`：别名在这里不是便利而是必需 —— std::io 与
     * std::sys::io 的短名都是 `io`，而同时读 stdin 与开文件要两个模块都在手边。
     *
     * `path` 保留写出来的完整名字（加载器按完整名对完整名匹配深限定名），
     * 只有 `shortName` 换成别名。 */
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
 * @brief 解析 `effects Addr=… Cont=… Thread=…` 子句。
 *
 * 写在**任何没有函数体的声明**上：`extern!("libc")` 与 `@builtin` 都算，因为编译器
 * 看不穿它们中的任何一个（`"libc"` 只是诊断用的库名）。它是作者对参数地址
 * （arguments' addresses）与跨线程共享状态的断言。每一轮先跳过换行再读关键字，否则
 * `at()` 会看到 TK_NEWLINE 而错过它。
 *
 * @param p             解析器。
 * @param f             接收效应位的函数定义。
 * @param outSawThread  非 `NULL` 时，见到 `Thread=` 就置为 `true`。
 * @return              解析成功为 `true`；出错为 `false`（已报告诊断）。
 */
static bool parseEffectsClause(Parser *p, FuncDef *f, bool *outSawThread) {
            take(p);
            f->hasEffects = true;
            f->extThreadMask = 1u;   /* 缺省保守：假定它碰跨线程共享的状态 */
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
                /** @brief 记录 `Ret=` 这位效应。 */
                else if (strcmp(nm->text, "Ret") == 0) {
                    /* `Ret=0`: "the reference/view I return does not point into the caller's
                     * frames" —— 返回侧的 `Addr`；`Ret=1` 是保守缺省，等于什么都没多说，
                     * 所以只有 0 有意思。 */
                    if (bits > 1) {
                        ctxError(p->ctx, nm->line, nm->col,
                                 "`Ret` is one fact about the return value: 0 = it does not point"
                                 " into the caller's frames, 1 = say nothing (the default).",
                                 "`Ret=%u` is out of range (use 0 or 1)", bits);
                        return NULL;
                    }
                    f->extRetFresh = (bits == 0);
                }
                /** @brief 记录 `Thread=`：0 = 不碰跨线程共享状态，1 = 碰。 */
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
                if (at(p, "Addr") || at(p, "Cont") || at(p, "Thread")) continue;
                break;
            }
            return true;
    return true;
}

/**
 * @brief 解析 `extern!` 声明的 C 函数：库名以字符串字面量给出，C 侧只有签名。
 *
 * 形状是 `extern!` + 库名的字符串字面量 + `fn` 签名，可选的 `effects` 子句跟在后面
 * （具体拼写见 `parseEffectsClause` 的说明）。参数或返回类型只能是标量或单个指针
 * （`ref T`、`?ref T`），因为 C 只有这些：`slice<T>` 对应 C 的「指针加长度」两个参数，
 * 跨边界表达不了，stdlib 包装层只能自己拆开写。效应声明是作者对这次调用的断言；一条都
 * 不写就按最坏情况假定（每个参数都可能被存下），安全，但会限制调用点的自由度。返回的
 * C 函数只声明不定义：`p->noBody` 让 `parseFunc` 在签名之后停下，实现就是 C 库本身。
 *
 * @param p  解析器。
 * @return   新的 FuncDef；语法出错时返回 `NULL`。
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
        if (!parseFuncAnnotations(p, &extInl, NULL, NULL)) return NULL;
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

    /* 效应子句可以由调用方分成任意多行写（caller's assertion），循环把每一轮都接住。 */
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
 * @brief 把一整个 token 向量解析进 `out`。
 *
 * `out` 是**追加**到的 Module：调用方初始化一次，prelude 与用户文件因此落在同一个模块里。
 * 接受的语法就是 `parser.h` 里列的那些；token 向量必须以 TK_EOF 收尾（`lexAll` 总会补一个）。
 *
 * @param ctx    报告第一个语法错误；一旦置位就停止解析。
 * @param arena  拥有每个 AST 节点与每个拷贝的名字。
 * @param toks   `lexAll` 产出的 token 向量。
 * @param out    追加到的 Module。
 * @return       整个 token 向量被消费完则 `true`；`ctx->hasError` 置位则 `false`，此时
 *               `out` 里留的是已经解析出来的声明。
 */
bool parseModule(Ctx *ctx, Arena *arena, Vec *toks, Module *out) {
    Parser p = { ctx, arena, toks, 0, false, false, false, 0, 0 , false};   /* 末两位 = depth, typeDepth */
    p.sawBuiltin = false;                 /* 位置初始化列表不动，免得顺序一变就错位 */
    skipJunk(&p);

    while (!atKind(&p, TK_EOF) && !ctx->hasError) {
        /* `use a::b` 是语义导入：找文件、查导入环、解析它引入的限定名都归加载器。 */
        if (at(&p, "use")) {
            UseDecl *u = parseUse(&p);
            if (!u) return false;
            *(UseDecl **)vecPush(&out->uses) = u;
            skipJunk(&p);
            continue;
        }
        /* `@private`：声明默认公开，要藏起来必须写出来。 */
        bool isPrivate = false;
        bool fnInline  = false;
        bool builtinDecl = false;  /* @builtin：这个声明由编译器实现（没有函数体） */
        bool fnUnchecked = false;  /* @unchecked：这个函数体的下标不生成边界检查 */
        bool noCopy    = false;
        bool poolObject = false;   /* @poolObject：这个 struct 拥有一个池（作者口径） */
        bool sharesStorage = false; /* @sharesStorage：按值拷贝时两份共用存储（容器那一族） */
        bool frozen     = false;   /* @frozen：作者签字"布局就是 C 的布局"（按值过界的门票） */
        bool exported   = false;   /* @export：这个函数的 C 符号给外面用（extern! 的另一面） */
        /* `@private` 隐藏声明，`@inline` 要求内联：两者可以出现在同一声明前、顺序无意义，
         * 所以一起读。其余注解在这里一律报错 —— 给编译器的指令若被忽略，读者会以为要求的
         * 事情发生了，而它并没有。 */
        while (at(&p, "@")) {
            Token *a = take(&p);
            Token *nm = expectIdent(&p, "an annotation name (only `private` at the top level)");
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
            /* `@export`：把 C 符号交出去（`extern!` 的另一面）。它只对自由函数有意义，
             * 所以别的声明各自把它拒掉（与 `@frozen` 同一形状）。 */
            if (strcmp(nm->text, "export") == 0) {
                if (exported) {
                    ctxError(ctx, a->line, a->col, NULL, "`@export` appears twice");
                    return false;
                }
                exported = true;
                skipJunk(&p);
                continue;
            }
            /* `@unchecked`：这个函数体的下标**不生成**边界检查。和 `@inline` 同族（都是对
             * 函数体的指令），所以在这里一起读；跟错了声明会在下面各自的分支里被拒。
             * 说两遍要报错：与 `@private` / struct 体内那条路同一条规矩。 */
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
            /* `@poolObject`：这个 struct **拥有一个池** —— 存储住在池自己的板块上、
             * 寿命随它所在的地方。声明比推断好：编译器不必猜（试过"有没有 `pid` 字段"
             * 与"方法建不建池"：前者是魔数、后者要等闭包），而且**用户自定义容器用同一个
             * 修饰符**声明，走完全一样的路。 */
            if (strcmp(nm->text, "poolObject") == 0) {
                poolObject = true;
                skipJunk(&p);
                continue;
            }

            /* `@sharesStorage`：按值拷贝时**两份共用同一块存储**（容器就是这一族）。
             * 不禁止、只警告（作者口径：万一用户就是神人，但必须让他知道这一行在做什么）。 */
            if (strcmp(nm->text, "sharesStorage") == 0) {
                sharesStorage = true;
                skipJunk(&p);
                continue;
            }
            /* `@frozen`：这个 struct 的布局**就是 C 的布局**（字段顺序、对齐、没有隐藏字段）。
             * 它是"按值过 C 边界"的门票，也是一句签字：编译器不审里面放了什么，代价由作者担
             * —— 与 `effects` 同一类（C-ABI.md §9.8）。 */
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
                /* `@builtin`：声明由编译器实现（没有函数体）。真正的解析在注解循环里，
                 * 这里只放行，免得"白名单"把它当成未知注解拒掉。 */
                builtinDecl = true;
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
                     " boundary by value) and `@export` (on a function: it becomes a C symbol that"
                     " C can call). `@overwrite` is for"
                     " locals.",
                     "unknown top-level annotation `@%s`", nm->text);
            return false;
        }
        if (at(&p, "struct")) {
            if (exported) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "A struct is storage, not code. Export the functions that operate on it.",
                         "`@export` goes on a function");
                return false;
            }
            /* `@unchecked` 描述的是函数体，而 struct 是声明：每个方法在自己的行上单独选择。 */
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
            if (frozen) s->frozen = true;
            if (sharesStorage) s->sharesStorage = true;
            if (poolObject) s->poolObject = true;
            *(StructDef **)vecPush(&out->structs) = s;
        } else if (at(&p, "impl")) {
            /* `impl` 块不声明任何东西：`@private`/`@noCopy` 描述声明，`@poolObject`/
             * `@sharesStorage` 描述存储，`@inline`/`@unchecked` 描述函数体。与其静默忽略，
             * 不如直接说清楚。 */
            if (isPrivate || noCopy || sharesStorage || poolObject || fnInline || fnUnchecked || frozen
                || exported) {
                Token *t = cur(&p);
                ctxError(ctx, t->line, t->col,
                         "An `impl` block adds methods to a type that is declared elsewhere, so it "
                         "declares no storage and no visibility of its own.",
                         "no annotation applies to an `impl` block");
                return false;
            }
            ImplDef *im = parseImpl(&p);
            if (!im) return false;
            *(ImplDef **)vecPush(&out->impls) = im;
        } else if (at(&p, "trait")) {
            /* 与 `impl` 块一样，trait 只声明行为，任何注解都不适用。 */
            if (isPrivate || fnInline || fnUnchecked || noCopy || sharesStorage || poolObject || frozen
                || exported) {
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
            TypeDef *td = parseTypeDecl(&p);
            if (!td) return false;
            td->isPrivate = isPrivate;
            *(TypeDef **)vecPush(&out->types) = td;
        } else if (at(&p, "global") || at(&p, "let") || at(&p, "var")) {
            /* 顶层写 `global var` 冗余但合法：顶层本来就是全局（同一件事，允许自文档化）。 */
            (void)accept(&p, "global");
            if (exported) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "A global is a C object, and this language has no `@export` for objects yet"
                         " -- a shared symbol needs a name the two sides agree on, which is the"
                         " decision `@export` makes for functions.",
                         "`@export` goes on a function");
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
        } else if (at(&p, "extern")) {
            if (exported) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "`extern!` already says this symbol comes **from** C; `@export` says the"
                         " opposite. An `extern!` declaration has no body to hand out.",
                         "`@export` and `extern!` contradict each other");
                return false;
            }
            if (frozen) {
                ctxError(ctx, cur(&p)->line, cur(&p)->col,
                         "`@frozen` promises a byte layout for a field list; only a `struct` has one."
                         " A function **pointer** has a layout, and that type is `fn(A) -> R`.",
                         "`@frozen` goes on a `struct`");
                return false;
            }

            /* `extern!` 没有函数体，所以 `@inline` 与 `@unchecked` 都被拒而不是忽略：
             * 接受它们等于让读者以为要求的优化发生了。 */
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
            if (!parseFuncAnnotations(&p, &inl, NULL, &unchk)) return false;
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
            if (builtin) p.noBody = true;      /* `@builtin` 和 `extern!` 一样：只要签名 */
            FuncDef *f = parseFunc(&p);
            p.noBody = savedNoBody;
            if (!f) return false;
            f->isInline  = inl;
            f->isUnchecked = unchk;
            f->isPrivate = isPrivate;
            f->isBuiltin = builtin;
            /* `@export`：符号交出去。三个"没有那一个 C 符号"的形状当场拒掉（泛型的每个实例是
             * 一个函数、协程的值是帧句柄、`extern!` 已经在别处了），方法与它自己的成员循环里拒。 */
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
 * @brief 解析一条 `struct` 声明：类型参数、字段与方法。
 *
 * 方法与字段可以交替出现；注解循环把 `@inline`/`@private`/`@unchecked` 一起读下来，
 * 由紧随其后的声明决定它们落在谁身上。
 *
 * @param p  解析器。
 * @return   新的 StructDef；出错时返回 `NULL`（已报告诊断）。
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

    /* 类型参数表：`struct Pair<A, B> { ... }` */
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
        if (at(p, "@") && !parseFuncAnnotations(p, &inl, &priv, &unchk)) return NULL;
        if (at(p, "fn")) {
            FuncDef *m = parseFunc(p);
            if (!m) return NULL;
            m->isInline  = inl;
            m->isPrivate = priv;
            m->isUnchecked = unchk;
            m->owner = sd;
            *(FuncDef **)vecPush(&sd->methods) = m;
            skipJunk(p);
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
         * hold (HEAP.md section 2 -- the callee is not a declaration, the host fills the slot at run
         * time). The clause is read by the **same** routine a declaration uses, into a throw-away
         * `FuncDef`, and only the masks are kept: one parser for one syntax. It goes on the field's
         * own line, like every other part of a declaration here. */
        if (at(p, "effects")) {
            if (!ft || ft->kind != TY_FN) {
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
    }
    if (!expect(p, "}", NULL)) return NULL;
    return sd;
}

/**
 * @brief 把一个可能带限定的名字读进 `out`：`point`、`i64`、`io::istream`。
 *
 * `impl` 块可能带的两个名字（`impl Type`、`impl Trait for Type`）共用这一个 helper：
 * 规则完全相同（the block's name、the unit's imports、the type's own arguments 都按同一套
 * 读法），限定名经本单元的导入解析，而名字到底指什么归检查器管，类型表在它手里。
 *
 * @param p     解析器。
 * @param out   名字追加到的缓冲。
 * @param what  诊断里怎么称呼它，例如 "a type name after `for`"。
 * @return      出错时为 `false`（已报告诊断）。
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
 * @brief 解析一条 `trait Name { fn ... }` 声明。
 *
 * 方法**只有签名**，而解析器已经有现成开关：在 `parseFunc` 外围置 `p->noBody` 正是
 * `extern!` 声明无函数体的做法，所以 trait 方法不需要第二条解析路径。
 *
 * 这里什么都不解析类型。签名里的 `Self` 按写出来的样子存；等到某个 `impl` 给出实现类型，
 * 它才有意义（见 ast.h 里的 `TraitDef`）。
 *
 * @param p  解析器。
 * @return   新的 TraitDef；出错时返回 `NULL`（已报告诊断）。
 */
static TraitDef *parseTrait(Parser *p) {
    Token *kw = take(p);
    /* 这里故意不用 `expectTypeName`：类型名是 camelCase，首字母大写留给类型参数。trait 名
     * 是**第三种**名字，按写出来的样子取，三个名字空间才不会互相撞。 */
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
        /* `Self` **is** a type parameter of the trait (see TraitDef in ast.h): giving every
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
 * @brief 解析一条 `impl Type { fn ... }` 块。
 *
 * 写在这里的方法加入该类型**唯一的那套方法集**，与写在类型体内完全一样；差别只在允许写在
 * 哪里（以及没有函数体的内建标量也能这样获得方法）。名字、类型参数与 `for` 子句都只按写
 * 出来的样子记录，名字指什么交给检查器。
 *
 * @param p  解析器。
 * @return   新的 ImplDef；出错时返回 `NULL`（已报告诊断）。
 */
static ImplDef *parseImpl(Parser *p) {
    Token *kw = take(p);
    /* 名字可以是**限定名**（`impl io::istream { ... }`），因为 impl 块通常与它扩展的类型
     * 不在同一个模块里。两个细节：
     *   - 十个标量名按 `TK_TYPE` 而不是 `TK_IDENT` 出词（`i64`、`u8`……），所以第一段两种
     *     kind 都收；
     *   - 后续段是模块名或类型名，而这里类型名可以小写（`stl::string`），`expectTypeName`
     *     对它们不合适。
     * 名字究竟是不是类型，归拥有类型表的检查器管（限定形式也由它解析）。 */
    ImplDef *im = (ImplDef *)arenaAllocZero(p->arena, sizeof(ImplDef));
    vecInit(&im->typeParams, p->arena, sizeof(const char *));
    /* `traitArgs` 在 `for` 子句**之前**就会被填，所以它的 arena 要在这里给：下面
     * `typeArgs` 的初始化来得太晚（往 arena 为 NULL 的 Vec 里 push 会在 arenaAlloc 里
     * 解引用空 arena）。 */
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
    if (!parseDottedName(p, &target, "a type name after `impl`")) return NULL;

    im->typeName = bufCstr(&target);
    /* `impl Trait for Type`：`impl` 后的名字是 **trait**，类型跟在 `for` 后面。`typeName`
     * 最终总是方法要挂上去的那个类型（流水线其余部分只需要这个名字），而 `traitName` 是
     * 一致性检查读的。 */
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
    /* 没有 `for`：开头的 `<...>` 就是**type's** 自己的实参。这一句必须写在上面两个
     * `vecInit` **之后**，写在前面会静默丢掉。 */
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
            if (!parseFuncAnnotations(p, &inl, &priv, &unchk)) return NULL;
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
 * @brief 解析一条 `type Name = | v1 | v2(T)` 变体类型声明。
 *
 * 第一个变体前面的 `|` 可以省略。
 *
 * @param p  解析器。
 * @return   新的 TypeDef；出错时返回 `NULL`（已报告诊断）。
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

    /* 类型参数表：`type option<T> = | none | some(T)`，形状与 struct 上的一致。 */
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

        /* 载荷：`| circle(f64) | rect(f64, f64)`。括号里是类型，按位置排、没有字段名，
         * match 臂也照这个顺序绑定。 */
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
 * @brief 判断 `s` 是否以大写 ASCII 字母开头。
 *
 * @param s  要检查的字符串。
 * @return   首字母是大写 ASCII 字母时为 `true`。
 */
static bool startsUpper(const char *s) { return s[0] >= 'A' && s[0] <= 'Z'; }

/**
 * @brief 消费一个必须以小写字母开头的类型名。
 *
 * 类型名是 camelCase、类型参数以大写开头，两个集合因此不相交：类型参数永远不会与类型名
 * 相撞。在这里强制这条规则，正是否掉 `struct T` 与 `struct box<T>` 并存 —— 后者里内层的
 * T 会遮蔽外层的，能编译却读不懂是设计缺陷。
 *
 * @param p     解析器。
 * @param what  诊断里对这个名字的称呼。
 * @return      消费到的 token；缺失或首字母大写时返回 `NULL`（已报告诊断）。
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
 * @brief 消费一个函数或方法名：标识符，或可重载的运算符（如 `fn ==(self, other)`）。
 *
 * 运算符就按它本身写，定义因此在声明处就能看见；隐式约定一个名字就得另找地方查。
 * 比较、算术、流运算符与下标运算符都收；`<<` / `>>` 是两个 `<` / `>` token、`[]` / `[]=`
 * 是三四个普通 token（见 `atShift`），都在这里拼成一个名字 —— 查找与名字修饰以这个拼写作键。
 *
 * @param p  解析器。
 * @return   消费到的名字 token；不是合法名字时返回 `NULL`（已报告诊断）。
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
 * @brief 读函数声明前可以出现的注解：`@inline`、`@builtin`、`@private`、`@unchecked`。
 *
 * 声明不是语句，函数注解因此需要自己的位置；本函数从三个解析声明的地方调用：模块体、
 * struct 体与 `extern!`。
 *
 * 未知注解一律报错而不是警告：注解是写在源码里给编译器的指令，被忽略的笔误会让读者以为
 * 要求的事情发生了。`@recursive` / `@main` 设计了但没实现，也明确说出来而不是静默接受。
 * `@unchecked` 说的是**函数体**，所以在能声明函数体的地方读它、在不能的地方拒掉（`extern!`
 * 声明、字段）：那些位置传进来的 `outUnchecked` 是 `NULL`，走到它就是错误，理由与下面
 * `@private` 的分支相同 —— 不能让读者以为请求被照办了。
 *
 * @param p             解析器。
 * @param outInline     非 `NULL` 时接收是否见到 `@inline`。
 * @param outPrivate    非 `NULL` 时接收是否见到 `@private`。
 * @param outUnchecked  非 `NULL` 时接收是否见到 `@unchecked`。
 * @return              成功为 `true`；出错为 `false`（已报告诊断）。
 */
static bool parseFuncAnnotations(Parser *p, bool *outInline, bool *outPrivate,
                                 bool *outUnchecked) {
    /* 这里不重置标志：调用方可能已经在顶层注解循环里见过 `@inline`（那里在知道声明是不是
     * 函数之前就把 `@private` 与 `@inline` 一起读了），清掉它会让 `@inline fn f()` 像什么
     * 都没写一样。 */
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
            /* `@builtin`: 这个声明由编译器实现，所以**没有函数体**（与 `extern!` 同走 noBody）。
             * 它存在的理由和 `alloc<T>` 一样：要生成的代码必须由编译器命名那个函数（并行 worker 的
             * trampoline），库给不出 —— 但声明仍然放在库里，可发现、可文档化。 */
            if (p->sawBuiltin) {
                ctxError(p->ctx, a->line, a->col, NULL, "`@builtin` appears twice on the same function");
                return false;
            }
            p->sawBuiltin = true;
            skipNl(p);
            continue;
        }
        if (strcmp(nm->text, "private") == 0) {
            /* `@private` 用在**成员**上：类型留给自己用的方法，或字段。声明默认公开，
             * 要藏起来必须写出来 —— 与顶层同一条规则，只是低一层。 */
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
        ctxError(p->ctx, a->line, a->col,
                 "Annotations are compile-time instructions written in the source, so a"
                 " typo must not be silently ignored. On a function they are `@inline`"
                 " (the call must be inlined) and `@unchecked` (this body's indexes are not"
                 " bounds-checked).",
                 "unknown annotation `@%s` on a function -- `@inline` and `@unchecked` exist today",
                 nm->text);
        return false;
    }
    return true;
}

/**
 * @brief 解析一条 `fn` 声明：名字、类型参数、参数、返回类型与函数体。
 *
 * `p->noBody` 置位时（`extern!` 或 `@builtin`）解析在返回类型之后停下，`fd->body` 留
 * `NULL`。复用本函数而不是另抄一份，是为了让参数、返回类型与泛型表不会在两份拷贝之间
 * 走样。
 *
 * @param p  解析器。
 * @return   新的 FuncDef；出错时返回 `NULL`（已报告诊断）。
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
    vecInit(&fd->params, p->arena, sizeof(void *));
    vecInit(&fd->typeParams, p->arena, sizeof(void *));
    vecInit(&fd->targs, p->arena, sizeof(void *));
    /* `fn f<T, U>(...)`：自由函数的类型参数，形状与 struct 携带的那张表一致。 */
    if (accept(p, "<")) {
        skipNl(p);
        for (;;) {
            /* 类型参数以大写开头（`T`），所以这里必须用 `expectIdent`：`expectTypeName`
             * 拒的正是这个形状，它存在的意义就是把大写留给类型参数。 */
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
        *(Param **)vecPush(&fd->params) = pm;

        if (accept(p, ",")) skipNl(p);
        else break;
    }
    if (!expect(p, ")", NULL)) return NULL;

    if (accept(p, "->")) {
        fd->ret = parseType(p);
        if (!fd->ret) return NULL;
    }

    /* **没有函数体**的声明（`extern!` 或 `@builtin`）在返回类型之后停下。两种都要能声明
     * `effects`，因为编译器看不穿它们中的任何一个。（`extern!` 还会在 `parseExtern` 里读这个
     * 子句，好让它出现在 `owned` 之类的子句之后；这里则必须紧跟签名。） */
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
        skipJunk(p);                       /* 注释/换行之后写 `effects` 也算（skipNl 会漏注释） */
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
 * @brief 解析一个类型表达式：`mut`、`?`、`ref`、`[N]T`、名字与泛型实参。
 *
 * 带模块限定的名字（`io::File`）被收成一个字符串。这里不查符号表，所以路径指的是模块还是
 * 类型，只由加载器决定。
 *
 * 本函数是进入类型递归的唯一入口：深度计数器在**每一条**出口上递减，错误路径也一样 ——
 * 留下没降回去的 `depth` 会让后面每个文件都显得太深。
 *
 * @param p  解析器。
 * @return   新的 Type 节点；出错时返回 `NULL`（已报告诊断）。
 */
static Type *parseType(Parser *p) {
    if (parseTypeTooDeep(p)) return NULL;
    Type *t = parseTypeInner(p);
    p->typeDepth--;
    return t;
}

/**
 * @brief 类型表达式的真正实现，`parseType` 的深度预算外壳套在它外面。
 *
 * @param p  解析器。
 * @return   新的 Type 节点；出错时返回 `NULL`（已报告诊断）。
 */
static Type *parseTypeInner(Parser *p) {
    /* `dyn Trait` —— 动态分发值的类型（DYN.md 第 3 阶段）。与表达式形式一样，靠**形状**
     * 判断（`dyn` 后跟标识符），因为 `dyn` 在语言里不是保留字。 */
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
    /* 定长数组 `[N]T`。维度靠递归嵌套：`[15][15]i32` 是十五个 `[15]i32`。 */
    if (at(p, "[")) {
        Token *br = take(p);
        if (!atKind(p, TK_INT)) {
            Token *bad = cur(p);
            ctxError(p->ctx, bad->line, bad->col,
                     "For now a fixed array's length must be an integer literal. "
                     "(Compile-time constants will come with `const`.)",
                     "array length must be an integer literal, found `%s`", shown(bad));
            return NULL;
        }
        Token *n = take(p);
        if (!expect(p, "]", NULL)) return NULL;
        Type *elem = parseType(p);
        if (!elem) return NULL;
        if (n->ival <= 0) {
            ctxError(p->ctx, br->line, br->col, NULL,
                     "array length must be positive, got %lld", (long long)n->ival);
            return NULL;
        }
        return typeArray(p->arena, n->ival, elem);
    }

    /* **类型**位置上的 `fn(A, B) -> R`：裸代码指针（`docs/topics/C-ABI.md`）。
     *
     * 拼写故意与 lambda literal's signature 相同，语言因此不必新增符号；差别在于这里的参数是
     * **类型**而不是 `name: Type`。这里只解析形状 —— 解析并驻留签名是 `ttResolve`'s job，
     * 与其他所有类型一样。 */
    if (at(p, "fn") && pk(p, 1) && pk(p, 1)->text && strcmp(pk(p, 1)->text, "(") == 0)
        return parseFnType(p);

    Token *t = cur(p);
    if (t->kind == TK_TYPE || t->kind == TK_IDENT) {
        take(p);
        /* 类型名可以带模块限定，如 `io::File`。解析器没有符号表，所以这里只把路径收成一个
         * 字符串，解析归加载器。 */
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

        /* 泛型实参：`Pair<i32, u8>` */
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
 * @brief 把**类型**位置上的 `fn(A, B) -> R` 解析成 TY_FN 节点。
 *
 * 返回类型**必需**：lambda 可以省，因为它的体说明了返回什么；类型没有体，所以
 * `var f: fn(i64)` 无从推断，在这里直接拒掉，而不是猜一个。
 * 参数不带名字，与 lambda's 不同：函数类型的参数名不属于这个类型，被调用的值也不是声明。
 *
 * @param p  解析器。
 * @return   新的 Type 节点；出错时返回 `NULL`（已报告诊断）。
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

/**
 * @brief 解析一个 `{ ... }` 块，产出一个 ST_BLOCK 语句。
 *
 * 本函数是进入语句块的唯一入口：深度计数器在**每一条**出口上递减，错误路径也一样 ——
 * 留下没降回去的 `depth` 会让后面每个文件都显得太深。
 *
 * @param p  解析器。
 * @return   块语句；出错时返回 `NULL`（已报告诊断）。
 */
static Stmt *parseBlock(Parser *p) {
    if (parseTooDeep(p, "a block")) return NULL;
    Stmt *t = parseBlockInner(p);
    p->depth--;
    return t;
}

/**
 * @brief 语句块的真正实现，`parseBlock` 的深度预算外壳套在它外面。
 *
 * 块内语句由换行与可选的 `;` 分隔（`skipJunk` 负责跳过分隔符）。
 *
 * @param p  解析器。
 * @return   块语句；出错时返回 `NULL`（已报告诊断）。
 */
static Stmt *parseBlockInner(Parser *p) {
    Token *open = cur(p);
    if (!expect(p, "{", NULL)) return NULL;

    Stmt *b = stmtNew(p->arena, ST_BLOCK, open->line);
    vecInit(&b->u.block.stmts, p->arena, sizeof(void *));

    skipJunk(p);
    while (!at(p, "}")) {
        Stmt *s = parseStmt(p);
        if (!s) return NULL;
        *(Stmt **)vecPush(&b->u.block.stmts) = s;
        skipJunk(p);
    }
    if (!expect(p, "}", NULL)) return NULL;
    return b;
}

/**
 * @brief 解析 `match e { variant => statement ... }`。
 *
 * 这是语句而不是表达式，理由与 `?` 相同：C 没有语句表达式，所以 "match yields a value"
 * 在这里没有新机制就表达不出来。每条臂的体是一个语句，通常是一个块。
 *
 * 每条 arm's body 是一个语句，通常是一个块；臂写的是 enum's variants 之一，它是否存在、
 * 载荷绑定是否匹配，都对照类型在类型检查器里检查。这里故意没有 `_ =>` 兜底：列全每个
 * variant 正是 match 提供的价值。
 *
 * @param p  解析器。
 * @return   新的 ST_MATCH 语句；出错时返回 `NULL`（已报告诊断）。
 */
static Stmt *parseMatch(Parser *p) {
    Token *kw = take(p);
    /* 这里必须关掉结构体字面量，`if` / `while` 条件里也一样：否则 `match e { ... }` 的 `{`
     * 会被读成 `e { field: value }` 的开头。 */
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
        Token *name = cur(p);
        if (name->kind != TK_IDENT && name->kind != TK_KEYWORD) {
            ctxError(p->ctx, name->line, name->col,
                     "a `match` arm names one of the enum's variants, e.g. `occupied => { ... }`",
                     "expected a variant name, found `%s`", name->text);
            return NULL;
        }
        take(p);

        MatchArm *arm = (MatchArm *)arenaAllocZero(p->arena, sizeof(MatchArm));
        arm->variant = name->text;
        arm->line = name->line;
        vecInit(&arm->binds, p->arena, sizeof(void *));

        /* 载荷绑定：`circle(r) => ...` / `rect(w, h) => ...`。必须在 `=>` 之前解析，
         * 因为它紧跟 variant 名。 */
        skipNl(p);
        if (at(p, "(")) {
            take(p);
            skipNl(p);
            while (!at(p, ")")) {
                Token *b = expectIdent(p, "a name to bind the payload to");
                if (!b) return NULL;
                *(const char **)vecPush(&arm->binds) = b->text;
                skipNl(p);
                if (accept(p, ",")) { skipNl(p); continue; }
                break;
            }
            if (!expect(p, ")", NULL)) return NULL;
            skipNl(p);
        }

        if (!expect(p, "=>", NULL)) return NULL;

        /* 臂的体：一个块，或一个语句 —— `occupied => return 1` 要能写。 */
        skipNl(p);
        if (at(p, "{")) {
            arm->body = parseBlock(p);
        } else {
            Stmt *inner = parseStmt(p);
            if (!inner) return NULL;
            arm->body = stmtNew(p->arena, ST_BLOCK, inner->line);
            vecInit(&arm->body->u.block.stmts, p->arena, sizeof(void *));
            *(Stmt **)vecPush(&arm->body->u.block.stmts) = inner;
        }
        *(MatchArm **)vecPush(&s->u.match.arms) = arm;

        skipJunk(p);
    }
    if (!expect(p, "}", NULL)) return NULL;
    return s;
}

/**
 * @brief 解析一条语句：声明、控制语句、块、赋值或表达式语句。
 *
 * @param p  解析器。
 * @return   新的 Stmt；出错时返回 `NULL`（已报告诊断）。
 */
static Stmt *parseStmt(Parser *p) {
    Token *t = cur(p);

    /* 注解 `@xxx` 只能引出一条局部声明，而存在的只有 `@overwrite`。`@main` 这类函数注解
     * 还没实现，错误里直接这么说，而不是让读者去猜。 */
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
    /* `global var x: T` / `global let x: T = c` —— **软关键字**（`global` 仍是合法标识符，
     * 不动词法表：把它变成保留字会破坏现有代码里拿它当名字的地方）。语义见
     * docs/topics/GLOBAL.md：存储是全局的（深度 0），名字留在这个块里。 */
    if (at(p, "global")) {
        if (!accept(p, "global")) return NULL;
        p->sawGlobal = true;
    }
    if (at(p, "let") || at(p, "var"))  return parseVarDecl(p);
    if (at(p, "if"))                   return parseIf(p);
    if (at(p, "while"))                return parseWhile(p);
    if (at(p, "for"))                  return parseFor(p);
    /* `trap(msg)`：程序无法继续（见 ast.h 的 `ST_TRAP`）。靠**形状**识别（这个词后面跟
     * `(`），与 `ext` 运算符同一招：叫 `trap` 的变量、字段或参数照常可用，只有语句位置的
     * 调用才是这个内建。 */
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

    /* `d.run { … }`: a **trailing block**, i.e. a domain's body (CONCURRENCY.md「`ext` 与调度域」).
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

    /* `=` 与五种复合形式。`+=` 之类就是 `x = x + y`，所以是同一条语句多一个字段：检查器
     * 解析运算符，代码生成在 C 输出里保留复合形式，于是目标**只求值一次**。`++` / `--`
     * 故意不在这里：一个表达式同时读和写同一个位置，是未定义行为的经典来源。 */
    if (at(p, "=") || at(p, "+=") || at(p, "-=") || at(p, "*=") || at(p, "/=") || at(p, "%=")) {
        const char *aop = take(p)->text;
        skipNl(p);
        /* `got = yield e`：与 `var got = yield e` 同一个绑定，但目标已经存在 —— resume
         * 把值写进去。写在哪里由检查器决定。 */
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
 * @brief 解析顶层的 `let` / `var`，即全局变量或常量。
 *
 * 语法与局部声明一致：类型可以省略，省略初始化器表示零初始化。初始化器必须是字面量，
 * 因为 C 的全局初始化器只能是常量表达式。
 *
 * @param p  解析器。
 * @return   新的 GlobalDef；出错时返回 `NULL`（已报告诊断）。
 */
static GlobalDef *parseGlobalDecl(Parser *p) {
    Token *kw = take(p);
    Token *name = expectIdent(p, "a variable name");
    if (!name) return NULL;

    Type *ann = NULL;
    if (accept(p, ":")) {
        ann = parseType(p);
        if (!ann) return NULL;
    }

    Expr *init = NULL;
    if (accept(p, "=")) {
        skipNl(p);
        init = parseExpr(p);
        if (!init) return NULL;
    } else if (ann == NULL) {
        Token *t = cur(p);
        ctxError(p->ctx, t->line, t->col,
                 "without an initializer you must write the type: `var x: T`",
                 "`%s %s` needs a type or an initializer", kw->text, name->text);
        return NULL;
    }

    GlobalDef *g = (GlobalDef *)arenaAllocZero(p->arena, sizeof(GlobalDef));
    g->name = name->text;
    g->ann  = ann;
    g->init = init;
    g->mut  = (strcmp(kw->text, "var") == 0);
    g->line = kw->line;
    return g;
}
/**
 * @brief 解析局部的 `let` / `var` 声明。
 *
 * 没有初始化器时类型标注就是必需的；两者都没有，错误会点名读者忘了标类型的那个绑定。
 *
 * @param p  解析器。
 * @return   新的 ST_VAR 语句；出错时返回 `NULL`（已报告诊断）。
 */
static Stmt *parseVarDecl(Parser *p) {
    Token *kw = take(p);
    Token *name = expectIdent(p, "a variable name");
    if (!name) return NULL;

    Type *ann = NULL;
    if (accept(p, ":")) {
        ann = parseType(p);
        if (!ann) return NULL;
    }

    /* 初始化器可以省略，省略就表示零初始化。读未初始化的内存是 C 里最大的未定义行为
     * 来源之一，而且只能在运行时发现；缺省为零把它变成编译器保证的事情。 */
    Expr *init = NULL;
    if (accept(p, "=")) {
        skipNl(p);
        /* `var got: T = yield e` -- the declaration **is** the yield statement, and `got` is a
         * coroutine-frame local: the frame holds it, and a resume re-runs the assignment from the
         * frame's `in` slot (stage 2 of docs/topics/CONCURRENCY.md 4.4). */
        if (at(p, "yield")) {
            Token *yk = take(p);
            Stmt *ys = stmtNew(p->arena, ST_YIELD, yk->line);
            ys->u.yield_.bind     = name->text;
            ys->u.yield_.bindAnn  = ann;
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
        init = parseExpr(p);
        if (!init) return NULL;
    } else if (ann == NULL) {
        Token *t = cur(p);
        ctxError(p->ctx, t->line, t->col,
                 "without an initializer you must write the type: `var x: T`",
                 "`%s %s` needs a type or an initializer", kw->text, name->text);
        return NULL;
    }

    Stmt *s = stmtNew(p->arena, ST_VAR, kw->line);
    s->u.var.name = name->text;
    s->u.var.ann = ann;
    s->u.var.init = init;
    s->u.var.mut = (strcmp(kw->text, "var") == 0);
    s->u.var.isGlobal = p->sawGlobal;      /* 由 `global` 前缀置位，见 parseStmt */
    p->sawGlobal = false;
    return s;
}

/**
 * @brief 解析 `if <cond> <block>`，可带 `else` 块或 `else if`。
 *
 * 条件解析期间 `p->inCond` 置位，那里的 `{` 因此是体的开头而不是结构体字面量。
 *
 * @param p  解析器。
 * @return   新的 ST_IF 语句；出错时返回 `NULL`（已报告诊断）。
 */
static Stmt *parseIf(Parser *p) {
    Token *kw = take(p);
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
 * 四种形式，一份实现：每一种都在这里变成 `var` + `while`，检查器与代码生成因此永远看不到
 * 它们不认识的循环 —— compiler's walkers 全都只需要认识 `var`/`while`。新增一种语句则要
 * 让编译器里每个递归的 pass 都学会它（效果摘要、逃逸分析、arena 放置、`@overwrite`、名字
 * 标记……），漏掉一处就是一类缺陷；脱糖因此比加节点便宜。
 *
 *     for i in lo..hi { B }        ->  { var i = lo
 *                                         while i < hi { { B }  i += 1 } }
 *     for d in c { B }             ->  { var __extc_s = c[..]
 *                                         var __extc_i = 0
 *                                         while __extc_i < __extc_s.len {
 *                                             var d = __extc_s[__extc_i]  { B }  __extc_i += 1 } }
 *     for (init; cond; step) { B } ->  { init  while cond { { B }  step } }
 *
 * `i += 1` 是复合赋值；作者描述这个循环时用的正是这个形状："某种 `{i = 0;
 * while(cond) { BLOCK; i += 1; }}`"。
 * 用户写下的体（user's body）被包进自己的块里，里面的声明因此不会遮蔽 step 还要用到的
 * 循环变量。
 * `c[..]` 让容器形式同时覆盖定长数组与视图：数组自己没有 `.len`，数组上的视图有。别的东西
 * （调用、`varArray`、struct）都被拒，并要求先绑一个名字，因为脱糖后的循环每轮都要用 `c`。
 * 边界是 `lo..hi`，半开，与切片区间完全一样。
 */

/**
 * @brief 造一个循环自己用的合成名字（loop's own making）。
 *
 * @param p     解析器。
 * @param name  名字。
 * @param line  挂到节点上的源码行。
 * @return      新的 EX_IDENT 节点。
 */
static Expr *forIdent(Parser *p, const char *name, int line) {
    return exprIdent(p->arena, name, line);
}

/**
 * @brief 造一个循环自己用的整数字面量（loop's `+ 1` 与起点 0）。
 *
 * @param p     解析器。
 * @param v     字面量的值。
 * @param line  挂到节点上的源码行。
 * @return      新的 EX_INT 节点。
 */
static Expr *forInt(Parser *p, long long v, int line) {
    Expr *e = exprNew(p->arena, EX_INT, line);
    e->u.ival = v;
    return e;
}

/**
 * @brief 造 `tgt op= value`：循环自己那一步的形状（loop's own step）。
 *
 * @param p      解析器。
 * @param tgt    赋值目标名。
 * @param op     复合赋值运算符，例如 `+=`。
 * @param value  右值表达式。
 * @param line   挂到节点上的源码行。
 * @return       新的 ST_ASSIGN 语句。
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
 * @brief 把给定的若干语句装进一个块（`{ a  b  c }`）。
 *
 * @param p      解析器。
 * @param stmts  语句数组。
 * @param n      语句个数。
 * @param line   挂到块上的源码行。
 * @return       新的 ST_BLOCK 语句。
 */
static Stmt *forBlock(Parser *p, Stmt **stmts, size_t n, int line) {
    Stmt *b = stmtNew(p->arena, ST_BLOCK, line);
    vecInit(&b->u.block.stmts, p->arena, sizeof(Stmt *));
    for (size_t i = 0; i < n; i++)
        *(Stmt **)vecPush(&b->u.block.stmts) = stmts[i];
    return b;
}

/**
 * @brief 判断一个表达式能否每轮迭代都重新求值而又不做任何事情。
 *
 * 只有标识符、字段访问与解引用可以：它们读的是同一处存储，循环每轮重新读它即可。
 *
 * @param e  要判断的表达式。
 * @return   可以重复求值为 `true`；`NULL` 或别的形状为 `false`。
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
 * @brief 解析 `for` 的四种形式，并在这里脱糖成 `var` + `while`。
 *
 * @param p  解析器。
 * @return   脱糖后的块语句；出错时返回 `NULL`（已报告诊断）。
 */
static Stmt *parseFor(Parser *p) {
    Token *kw = take(p);
    const int line = kw->line;

    /* ---- form four: `for (init; cond; step) { B }` ---- */
    if (at(p, "(")) {
        take(p);
        Stmt *init = parseStmt(p);
        if (!init) return NULL;
        if (!expect(p, ";", "`for (init; cond; step)` needs `;` after the initializer")) return NULL;
        skipNl(p);
        Expr *cond = parseExpr(p);
        if (!cond) return NULL;
        if (!expect(p, ";", "`for (init; cond; step)` needs `;` after the condition")) return NULL;
        skipNl(p);
        Stmt *step = parseStmt(p);
        if (!step) return NULL;
        if (!expect(p, ")", "`for (init; cond; step)` needs the closing `)`")) return NULL;

        Stmt *body = parseBlock(p);
        if (!body) return NULL;

        /* The step runs after the body: both go in a block inside the `while`. The block also
         * records **which** statement is the step, so codegen can put a label in front of it and
         * `continue` can jump there (C's `for` semantics; ). */
        Stmt *inner[2] = { body, step };
        Stmt *loopBody = forBlock(p, inner, 2, line);
        loopBody->forStep = step;
        Stmt *w = stmtNew(p->arena, ST_WHILE, line);
        w->u.whiles.cond = cond;
        w->u.whiles.body = loopBody;
        Stmt *outer[2] = { init, w };
        return forBlock(p, outer, 2, line);
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

    /* ---- 形式二与三：`for i in lo..hi { B }`（`0..n`、`-2..3`……）----
     * `..` 在循环体之前定下来：它后面的 `{` 开始循环体；先要一个块，`0..5` 就会在 `..`
     * 处抱怨缺 `{`。 */
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

    if (hi) {
        Stmt *decl = stmtNew(p->arena, ST_VAR, line);
        decl->u.var.name = vn->text;
        decl->u.var.ann  = NULL;
        decl->u.var.init = head;
        decl->u.var.mut  = true;                 /* the loop itself writes it */
        decl->u.var.overwrite = false;

        /* **上界只求值一次**，在进入循环时。照写进条件里会让循环每轮重读它，
         * `for i in 0..n { n = 0 ... }` 就会跟着体里改动的 `n` 走。区间是有界区间 ——
         * `for i in 0..xs.len` 必须指进入时的长度 —— 隐藏的 `let` 正好给上界脱糖承诺的
         * 生命周期。名字沿用 `__extc_s` / `__extc_i` 的 `__extc_` 约定；用户绑了同名就是
         * 遮蔽，`declare` 会像对别的名字一样重命名它。 */
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
        loopBody->forStep = stepStmt;         /* label target for `continue` () */
        Stmt *w = stmtNew(p->arena, ST_WHILE, line);
        w->u.whiles.cond = cond;
        w->u.whiles.body = loopBody;
        Stmt *outer[3] = { decl, hiDecl, w };
        return forBlock(p, outer, 3, line);
    }

    /* ---- form one: `for d in c { B }` ---- */
    if (!forRepeatable(head)) {
        ctxError(p->ctx, head->line, 1,
                 "the loop reads the container once per iteration, so bind it first:"
                 " `var xs = ...` and then `for d in xs`",
                 "`for d in ...` needs a name, a field or `*p` on the right");
        return NULL;
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
    Stmt *outer[3] = { viewDecl, idxDecl, w };
    Stmt *desugared = forBlock(p, outer, 3, line);
    /* 被遍历的东西未必能切片：解析器不查符号表，判断不了，所以留给检查器把它改成迭代器
     * 循环的机会。 */
    desugared->forDesugar = true;
    return desugared;
}

/**
 * @brief 解析 `while <cond> <block>`。
 *
 * 条件与 `parseIf` 一样在 `p->inCond` 置位下解析，那里的 `{` 是循环体的开头。
 *
 * @param p  解析器。
 * @return   新的 ST_WHILE 语句；出错时返回 `NULL`（已报告诊断）。
 */
static Stmt *parseWhile(Parser *p) {
    Token *kw = take(p);
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
 * @brief 解析 `a ?? b`，优先级最低的二元运算符。
 *
 * 它是右结合的：`a ?? b ?? c` 就是 `a ?? (b ?? c)`。能出现在哪里是另一条规则：左侧必须是
 * option、result 或 `?ref T`，由类型检查器强制。
 *
 * @param p  解析器。
 * @return   新的表达式；出错时返回 `NULL`（已报告诊断）。
 */
static Expr *parseCoalesce(Parser *p) {
    Expr *l = parseOr(p);
    if (!l) return NULL;
    while (at(p, "??")) {
        Token *op = take(p);
        skipNl(p);
        Expr *r = parseCoalesce(p);        /* right-associative */
        if (!r) return NULL;
        Expr *e = exprNew(p->arena, EX_COALESCE, op->line);
        e->u.coalesce.main = l;
        e->u.coalesce.fallback = r;
        l = e;
    }
    return l;
}

/**
 * @brief 解析一个完整表达式的入口，供每个想要表达式的调用方使用。
 *
 * @param p  解析器。
 * @return   新的表达式；出错时返回 `NULL`（已报告诊断）。
 */
static Expr *parseExpr(Parser *p) { return parseCoalesce(p); }

/**
 * @brief 解析 `||`（逻辑或）这一层。
 *
 * @param p  解析器。
 * @return   新的表达式；出错时返回 `NULL`（已报告诊断）。
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
 * @brief 解析 `&&`（逻辑与）这一层。
 *
 * @param p  解析器。
 * @return   新的表达式；出错时返回 `NULL`（已报告诊断）。
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
 * @brief 解析 `|`（按位或）这一层。
 *
 * 三个按位层级用 C 与 Go 的优先级：
 *     `|`  <  `^`  <  `&`  <  `== !=`  <  `< <=` ...  <  `<< >>`  <  `+ -`
 * 与 C 一致是刻意的：在这里写算法的人都有 C 的肌肉记忆，而 "designing it better" 只会带来
 * 每天的失误。
 *
 * @param p  解析器。
 * @return   新的表达式；出错时返回 `NULL`（已报告诊断）。
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
 * @brief 解析 `^`（按位异或）这一层。
 *
 * @param p  解析器。
 * @return   新的表达式；出错时返回 `NULL`（已报告诊断）。
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
 * @brief 解析按位这一层的 `&`。
 *
 * @param p  解析器。
 * @return   新的表达式；出错时返回 `NULL`（已报告诊断）。
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
 * @brief 解析 `==` 与 `!=`。
 *
 * @param p  解析器。
 * @return   新的表达式；出错时返回 `NULL`（已报告诊断）。
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
 * @brief 解析 `<`、`<=`、`>` 与 `>=`。
 *
 * @param p  解析器。
 * @return   新的表达式；出错时返回 `NULL`（已报告诊断）。
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
 * @brief 看光标处是不是一个移位运算符：`"<"` 或 `">"` 连续出现两次，拼写作 `"<<"` 或 `">>"`。
 *
 * 第二个 token 必须是标点，字符串字面量的 literal's text 因此冒充不了。`<<` 与 `>>` 故意
 * 不进 lexer's table：`box<box<i32>>` 里的 `>>` 必须留成两个独立的 `>` token，泛型实参才
 * 配得上对（见 `parseType` 与 `looksLikeAssoc`）；在词法层融合它们会让嵌套泛型类型解析不
 * 了，正是 C++ 掉过的坑。只有表达式会看到移位运算符，类型文法永远看不到。
 *
 * @param p   解析器。
 * @param ch  要连续看两次的字符。
 * @param op  接收运算符拼写。
 * @return    两个相邻 token 都拼作 `ch` 时为 `true`。
 */
static bool atShift(Parser *p, const char *ch, const char **op) {
    if (strcmp(pk(p, 0)->text, ch) != 0 || strcmp(pk(p, 1)->text, ch) != 0) return false;
    if (pk(p, 1)->kind != TK_PUNCT) return false;
    *op = strcmp(ch, "<") == 0 ? "<<" : ">>";
    return true;
}

/* ---------------------------------------------------------------- f"…"（格式拼接）
 *
 * `lhs << f"t1{e1}t2"` **在解析器里就展开**成 `((lhs << "t1") << e1) << "t2"` —— 用的是与手写
 * 链**同一个** `mkBin`，所以展开出来的 AST 与手写 `<<` 链逐字节相同 ⇒ 检查、发射一行都不用改，
 * 而"这个类型能不能打印"就等于"有没有 `<<` 重载"（复用既有重载解析，不引入第二套定义）。
 *
 * 词法已经把 `f"…"` 认成 TK_FSTRING（词法阶段）；这里负责切段、解转义、**子解析**槽里的表达式。
 * 槽里的表达式是用**真解析器**在片段上跑出来的（lexRange 带真实行列 ⇒ 诊断指得到文件里）。
 */

/**
 * @brief 造一个 EX_STR 节点，把 `s` 的前 `len` 个字节拷进 arena。
 *
 * @param p     解析器。
 * @param s     源文本（未必以 NUL 结尾）。
 * @param len   字节数。
 * @param line  挂到节点上的源码行。
 * @return      新的 EX_STR 节点。
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
 * @brief 把 f-string 的部件展开进 `lhs << …` 链。
 *
 * 每个槽先在片段上跑**真正的词法与解析器**（行列保持真实），再按
 * `lhs << 文字段 << 表达式` 的顺序用同一个 `mkBin` 拼出来。`{{` / `}}` 是转义的字面括号；
 * 顶层出现的 `:` 是 v2 的格式说明符，明确拒绝而不是让它静默变成表达式的一部分。
 *
 * @param p    解析器。
 * @param lhs  被追加到的表达式（`<<` 链的左端）。
 * @param ft   触发这次展开的 TK_FSTRING token。
 * @return     成功为 `true`；语法错误或空槽为 `false`（已报告诊断）。
 */
static bool fmtSplice(Parser *p, Expr **lhs, Token *ft) {
    const char *src = ft->text;
    const Expr *before = *lhs;
    const size_t n = ft->len;
    char *txt = (char *)arenaAlloc(p->arena, n + 1);   /* 解码缓冲：转义只让文本变短 ⇒ 够 */
    size_t tl = 0;
    int col = ft->col + 2;                              /* 跳过 `f"` */
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
        /* 槽：先把累积的文字段冲成 `lhs << "…"` */
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
        /* 顶层的 `:` ⇒ 格式说明符：**v2**（先明确拒绝，别让它静默变成表达式的一部分） */
        {
            int d2 = 0; bool st = false;
            for (size_t q = i + 1; q < j; q++) {
                char d = src[q];
                if (st) { if (d == '\\') q += 1; else if (d == '"') st = false; continue; }
                if (d == '"') { st = true; continue; }
                if (d == '(' || d == '[' || d == '{') d2 += 1;
                else if (d == ')' || d == ']' || d == '}') d2 -= 1;
                /** @brief 顶层的 `:` 是 v2 的格式说明符，v1 在这里明确拒绝。 */
                else if (d == ':' && d2 == 0) {
                    ctxError(p->ctx, ft->line, col,
                             "format specs (`{x:>8}`) are v2; v1 prints the value as `<<` would",
                             "format specifiers are not implemented yet");
                    return false;
                }
            }
        }
        /* 槽里的表达式：**真解析器**跑在片段上（行列保持真实） */
        Vec toks;
        vecInit(&toks, p->arena, sizeof(Token));   /* token 是**按值**存的（见 lxPushAt）*/
        lexRange(p->ctx, src + i + 1, innerLen, ft->line, col + 1, &toks);
        if (p->ctx->hasError) return false;
        Parser sub = *p;
        sub.toks = &toks;
        sub.pos = 0;
        sub.inCond = false;          /* 表达式位置：`{` 是结构体字面量，不是块 */
        Expr *e = parseExpr(&sub);
        if (!e) return false;
        skipNl(&sub);                 /* 片段扫描会在末尾补一个 NEWLINE（见 lexRange）*/
        if (cur(&sub)->kind != TK_EOF) {
            ctxError(p->ctx, cur(&sub)->line, cur(&sub)->col,
                     "one expression per slot", "unexpected `%s` inside an f-string slot",
                     cur(&sub)->text);
            return false;
        }
        *lhs = mkBin(p, "<<", *lhs, e, ft->line);
        col += (int)(innerLen + 2);
        i = j + 1;
    }
    if (tl > 0) *lhs = mkBin(p, "<<", *lhs, fmtStr(p, txt, tl, ft->line), ft->line);
    /* 一个部件都没有（`f""`）：不报错的话 `<<` 会被静默吃掉 ✗ */
    if (*lhs == before) {
        ctxError(p->ctx, ft->line, ft->col + 2, NULL,
                 "an f-string with nothing in it has nothing to splice");
        return false;
    }
    return true;
}

/**
 * @brief 解析 `<<` 与 `>>`：它们被认成两个相邻的 `<` / `>` token。
 *
 * @param p  解析器。
 * @return   新的表达式；出错时返回 `NULL`（已报告诊断）。
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
        /* `x << f"…"`：在这里展开成同一条 `<<` 链（用 `mkBin`，与手写的一模一样） */
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
 * @brief 解析 `+` 与 `-`（加减）这一层。
 *
 * @param p  解析器。
 * @return   新的表达式；出错时返回 `NULL`（已报告诊断）。
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
 * @brief 解析 `*`、`/` 与 `%`（乘除取模）这一层。
 *
 * @param p  解析器。
 * @return   新的表达式；出错时返回 `NULL`（已报告诊断）。
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
 * @brief 判断一个 token 能不能作为一个表达式的开头。
 *
 * 只为**一个**决定服务：`ext` 是**前缀运算符**（`ext f(x)`，调度域特性），而它的拼写同时是
 * 一个完全合法的变量名。只按形状识别它，会让 `ref ext`、`f(ext)`、`-ext` 被当成运算符，然后
 * 死在 "expected an expression, found `)`" 上 —— 用这种方式得知一个名字也是运算符并不好。
 *
 * 所以只有当后面跟着**能当操作数**的东西时才认这个运算符；`ext` 后面什么都没有仍然是错误，
 * 只是会说 "no variable named `ext`"。
 *
 * @param t  要判断的 token。
 * @return   可以作为一个表达式的开头为 `true`；`NULL` 或不能时为 `false`。
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
 * @brief 解析前缀表达式：`!`、`-`、`~`、解引用 `*p` 与取引用 `ref x`。
 *
 * `*` 在前缀位置没有歧义，因为乘法总有一个左操作数。
 *
 * 本函数是进入表达式递归的唯一入口：深度计数器在**每一条**出口上递减，错误路径也一样 ——
 * 留下没降回去的 `depth` 会让后面每个文件都显得太深。
 *
 * @param p  解析器。
 * @return   操作数表达式；出错时返回 `NULL`（已报告诊断）。
 */
static Expr *parseUnary(Parser *p) {
    if (parseTooDeep(p, "an expression")) return NULL;
    Expr *t = parseUnaryInner(p);
    p->depth--;
    return t;
}

/**
 * @brief 前缀表达式的真正实现，`parseUnary` 的深度预算外壳套在它外面。
 *
 * @param p  解析器。
 * @return   操作数表达式；出错时返回 `NULL`（已报告诊断）。
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
 * @brief 解析后缀链：`e!`、`e?`、`.field`、`.method(...)`、`[i]`、`[lo..hi]` 与 `(...)` 调用。
 *
 * @param p  解析器。
 * @return   到目前为止最外层的表达式；出错时返回 `NULL`（已报告诊断）。
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
        /* `e?` 把失败传给调用方。它能出现在哪里是另一条规则，由类型检查器而非文法决定。 */
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
                /* `dyn Trait(x).m(...)`：调用的接收者是 **payload**，既有的方法解析、实参
                 * 检查与代码生成因此原样适用；trait 只是跟着走，好让分发查表。这个循环会建
                 * **新**节点，标记不带上就会留在接收者上。 */
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
            /* `a[i]` indexes and `a[lo..hi]` slices.  The brackets hold an
             * ordinary expression, so struct literals are allowed inside. */
            Token *br = take(p);
            skipNl(p);
            const bool savedC = p->inCond;
            p->inCond = false;

            Expr *lo = NULL, *hi = NULL;
            bool isRange = false;
            if (!at(p, "..")) {
                lo = parseExpr(p);
                if (!lo) { p->inCond = savedC; return NULL; }
                skipNl(p);
            }
            if (at(p, "..")) {
                isRange = true;
                take(p);
                skipNl(p);
                if (!at(p, "]")) {
                    hi = parseExpr(p);
                    if (!hi) { p->inCond = savedC; return NULL; }
                }
                skipNl(p);
            }
            p->inCond = savedC;
            if (!expect(p, "]", NULL)) return NULL;

            if (isRange) {
                Expr *sl = exprNew(p->arena, EX_SLICE, br->line);
                sl->u.slice.obj = e;
                sl->u.slice.lo  = lo;
                sl->u.slice.hi  = hi;
                e = sl;
            } else {
                Expr *ix = exprNew(p->arena, EX_INDEX, br->line);
                ix->u.index.obj = e;
                ix->u.index.index = lo;
                e = ix;
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
 * @brief 把一个括号包围、逗号分隔的实参表解析进 `out`。
 *
 * 光标必须在开括号上，本函数会消费它。圆括号内部 `p->inCond` 被清掉，因为那里的 `{` 又
 * 明确是结构体字面量。
 *
 * @param p    解析器。
 * @param out  接收 Expr 指针的向量；在这里初始化，调用方传未初始化的即可。
 * @return     消费了收尾的 `)` 为 `true`；出错为 `false`（已报告诊断）。
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
 * @brief 判断表达式位置 `fn` 后面的 `( ... )` 是**类型表**而不是 `name: Type`。
 *
 * 表达式位置的 `fn` 复用同一个关键字（author's syntax B）：第一个参数后面不是 `:` 就说明
 * 它是类型表；空表返回 `false`，交给 `parseLambda` 在返回类型之后再定。
 *
 * @param p  解析器，光标在 `fn` 上。
 * @return   第一个参数后面不是 `:` 时为 `true`；空表或后面跟 `:` 时为 `false`。
 */
static bool fnParenIsTypeList(Parser *p) {
    Token *first = pk(p, 2);                       /* pk(1) is the `(` */
    if (!first || !first->text || strcmp(first->text, ")") == 0) return false;
    Token *after = pk(p, 3);
    return !(after && after->text && strcmp(after->text, ":") == 0);
}

/**
 * @brief 解析表达式位置的 `fn(A, B) -> R(expr)`：把一个值转换成函数类型。
 *
 * 类型本身由 `parseFnType` 解析（与类型位置用的是同一个例程），函数类型因此只有一份解析器。
 * 它后面跟的是普通 `(expr)`，与别的转换写法一致（`i32(x)`）。
 *
 * @param p  解析器。
 * @return   新的 EX_CONV 表达式；出错时返回 `NULL`（已报告诊断）。
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
 * @brief 解析表达式位置的 `fn` 闭包字面量：参数、可选的捕获表、返回类型与函数体。
 *
 * 返回类型可以省略，由函数体推断，与具名函数一样；捕获表只写**函数体会写**的变量（读是
 * 按值捕获，不需要条目）。当后面跟的是 `(` 而不是 `{` 或 `[` 时，这个形状其实是函数类型
 * 转换，本函数顺带把转换节点建出来。
 *
 * @param p  解析器。
 * @return   新的 EX_LAMBDA（或 EX_CONV）；出错时返回 `NULL`（已报告诊断）。
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
    if (at(p, "[")) {                          /* 捕获列表：只写 body 会**写**的那些 */
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
    /* 这是**转换**而不是字面量：`fn(i64) -> i64(p)`（`C-ABI.md` 9.9 节 ③）。两种形状只有在
     * 这里才能判定：a literal's body 是 `{`（或上面读过的捕获表 `[`），而转换继续跟 `(`。
     * 拼写与别的转换一致 —— `type(expr)` —— 它无歧义，是因为函数类型总会写出返回类型。 */
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
 * @brief 解析一个基本表达式：字面量、`null`、`new`、括号表达式、数组或结构体字面量、
 *        显式转换，或一个名字（含限定名与关联调用）。
 *
 * @param p  解析器。
 * @return   新的 Expr；出错时返回 `NULL`（已报告诊断）。
 */
static Expr *parsePrimary(Parser *p) {
    Token *t = cur(p);

    /* `fn(x: i64) -> i64 { ... }`：闭包字面量（docs/topics/LAMBDA.md 的语法 B）。`fn` 只在
     * **表达式**位置、且后面跟着 `(` 时进到这里，这正是它与声明的区别；顶层与 struct 体内
     * 各自的循环早就处理过它们的 `fn` 分支。复用 `fn` 让语言新增符号数为零，这正是语法 B
     * 的要点。 */
    /* `fn(i64) -> i64(p)`: a **conversion**, not a literal (`C-ABI.md` section 9.9 piece ③).
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

    /* `dyn Trait(expr)` -- the construction for dynamic dispatch (DYN.md stage 1).
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
        /* `dyn Trait(x)` 作为**值**（DYN.md 第 3 阶段）。紧贴的方法调用
         * `dyn Trait(x).m(...)` 由后缀循环建出来，它把 **payload** 当作 call's receiver
         * —— 那条路径没有改动，stage-1/2 的判据跑的仍是同一段代码。 */
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
    if (t->kind == TK_FSTRING) {
        ctxError(p->ctx, t->line, t->col,
                 "an f-string is expanded at compile time into a `<<` chain, so it has to be the"
                 " right-hand side of `<<`: `io::cout << f\"…\"`",
                 "an f-string is only valid as the right-hand side of `<<`");
        return NULL;
    }
    /* `null` 是上下文关键字，只在表达式位置识别，与 `ref`、`mut` 一样。它的类型完全来自
     * 上下文：`var p: ?ref node = null`、`if p != null`，或返回类型是 `?ref T` 时的
     * `return null`。 */
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
    /* `new T` / `new T[n]` / `new [N]T` 从当前块的 arena 拿一块清零内存。`new` 是上下文
     * 关键字，只在表达式位置识别。 */
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

    /* 数组字面量 `[1, 2, 3]`。末尾的 `...` 表示剩下的元素为零。 */
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

    /* 十个标量类型名按 TK_TYPE 出词，不是 TK_IDENT 也不是 TK_KEYWORD，所以这一支必须在
     * 下面的标识符分支之前判断。 */
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
        /* 调试开关 `EXTC_DBG_QN=1`：打印表达式位置每个标识符的起始位置，以及紧随其后的四个
         * token，好把限定名一段段看住。光标位置与 token 文本都打印出来是刻意的：每一行都必须
         * 唯一对应一次调用，否则调用点混在一起时，输出会被误读成正在查的那一次。
         *
         * 只写 stderr；`--dump-tokens` 之类的 token 输出不受影响。 */
        if (dbgOn("EXTC_DBG_QN")) {
            fprintf(stderr, "[qn ENT] pos=%d cur=`%s` n1=`%s` n2=`%s` n3=`%s` n4=`%s`\n",
                    (int)p->pos, t->text, pk(p,0)->text, pk(p,1)->text,
                    pk(p,2)->text, pk(p,3)->text);
        }
        /* `i32(x)` / `f64(y)`：显式转换，收窄、改符号，或整数与浮点之间跨越。C 写作
         * `(T)x`，但这里与括号表达式有歧义，因为解析器没有符号表；把括号搬到 `T(x)` 上，
         * 同一件事就没有歧义了。 */
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
        /* `name { ... }` 是结构体字面量。
         *
         * 规则是位置性的，与 Go 一样：`if` / `while` 条件里 `{` 开始体块，那里的字面量要加
         * 括号：`if x == (point { a: 1 }) { }`。类型名因此不必靠大小写区分 —— 规则住在文法
         * 里，而不是藏在拼写里。 */
        if (at(p, "{") && !p->inCond) return parseStructLit(p, t->text);

        /* 限定类型名：`mod::Type { ... }` 与 `mod::Type.variant`。
         *
         * 这条语法必须存在，因为两个模块可以各定义一个同名类型，而裸名说不出是哪一个：
         * 加载器故意不为有歧义的裸名登记返回路径，于是它永远解析不出来。没有这条语法，
         * 这种类型的字面量根本写不出来。
         *
         * 路径只拼成一个字符串，不查符号表，与类型位置完全一样；它指的是模块还是类型由
         * 加载器决定。`mod::fn(...)` 不受影响：`.` 与 `{` 都不跟随时，代码落到
         * `looksLikeAssoc`。 */
        if (at(p, "::")) {
            /* 只看，不消费。`mod::fn(args)` 是压倒性的常见情形，吃掉它的 token 会破坏本来
             * 处理它的 `looksLikeAssoc` 路径：`pos` 一动，prelude's `pcg32::withStream(...)`
             * 就会被拆成 `pcg32` 加垃圾。只有后面真的跟着 `{` 或 `.` 时才接管。 */
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

        /* 关联调用 `option<i64>::some(x)` / `point::origin()`，或任意深的限定名
         * `std::sys::io::write(...)` / `std::sys::io::STDOUT`。
         *
         * `IDENT <` 与小于号有歧义，所以先不消费地看一眼类型实参会在哪里结束、后面是否跟着
         * `::`，然后才真正解析。先决定可以不让试探性解析喷出无意义的错误。判据就是 `::`：
         * 它不是合法的二元运算符，两种读法互斥，猜测不会错。 */
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

        /* 条件里的 `{` 是体块的开头 —— 但 `{ ident :` 显然是漏了括号的字面量，错误里直接
         * 给出读者该写的那一行。 */
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
 * @brief 判断一个名字后面是否跟着可选的类型实参表和 `::name`，不移动光标。
 *
 * 只看看而已，光标留在原处。`IDENT <` 与小于号有歧义，所以决定必须在下手解析之前做出来，
 * 否则试探性的解析会喷出无意义的错误。判据是 `::`：它不是合法的二元运算符，关联调用与
 * `a < b` 比较不可能同时成立，于是这个猜测永远不会错。
 *
 * 返回段数而不是 bool，正是调用方需要的：`std::sys::io::STDOUT` 里光标停在**第一个** `::`
 * 上，只问 "is this qualified?" 的调用方会消费掉 `::` 加一个标识符（`sys`），把光标丢在
 * `::io::STDOUT` 上。后面还有几段，只有这次扫描知道。
 *
 * @param p  解析器，光标在第一个 `::` 上。
 * @return   第一个 `::` 之后的段数；不是限定名时为 0。
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
 * @brief 数光标后面有多少段完整的 `::name`。
 *
 * `looksLikeAssoc` 回答不了这个问题：它停在 "is a `(`、`{` 还是 `.` 紧跟其后" 这类问题上，
 * 而解析一条路径要问的是 "how long is the path"。光标必须停在 `::` 上。
 *
 * @param p  解析器，光标在 `::` 上。
 * @return   段数；光标不在 `::` 上时为 0。
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
 * @brief 解析一个限定名：`Name<targs>::name(args)`、`Type::assoc(args)`，或任意深的
 *        `mod::sub::name(args)` / `mod::sub::CONST`。
 *
 * 两段名与深路径共用这一条路，由最后一段后面是什么决定：`(` 就是调用（`isCall = true`），
 * 别的都是值，比如常量或无载荷变体（`isCall = false`）。
 *
 * 值也是 EX_ASSOC 而不是 EX_IDENT：加载器解析限定名时只走一组已知的节点种类，名字里带
 * `::` 的 EX_IDENT 它看不见，检查器会去查裸名并报未定义。
 *
 * `modPrefixOut` 拿到的是临时切分，`std::sys::io` 加 `STDOUT`；最终切分归加载器，只有它
 * 知道导入了哪些模块 —— 这里交出去的就是在最后一个 `::` 处切开的路径。
 *
 * @param p            解析器。
 * @param name         开头的名字，调用方已消费。
 * @param line         该名字的行号，挂到节点上。
 * @param startPos     扫描开始时的光标位置（当前未使用）。
 * @param modPrefixOut 接收模块前缀；调用方不需要时可传 `NULL`。
 * @return             EX_ASSOC 节点，或 `name<T>(...)` 的 EX_GENCALL 节点；出错时返回
 *                     `NULL`（已报告诊断）。
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
