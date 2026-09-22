/* 模块装载（定案 70）—— **语义导入 + 单 TU**
 *
 * 主人拍板的形状（`MODULES.md` §4 方案 A）：**一个文件就是一个模块**，
 * `use std::io` 是**语义导入**（不是 C 的文本包含 ✗），路径用 `::`，
 * **默认公开**、要藏就写 `@private`，**禁止 import 环**，
 * 而且**仍然只吐一个 .c**（保住 PLAN #37 的全 static 向量化收益 ✓）
 *
 * 做法上刻意把**全部模块机制塞在装载器里**，检查器与 codegen 几乎不动：
 *
 *   ① 根文件的 `use` ⇒ 解析路径 ⇒ 找文件（引用者目录 / `-I` / `$EXTC_STD`）
 *   ② 递归装载，**状态机查环**（正在装载的又被 use ⇒ 报错 ✓）
 *   ③ **后序**（依赖先）把每个模块的声明合进主 Module —— 跟 prelude 同一条路 ✓
 *   ④ **在检查之前**把限定名解析掉：
 *        `io::readLine(...)`   ⇒ 平名字 + 改写成 EX_CALL ✓
 *        `io::STDIN`           ⇒ 平名字 + 改写成 EX_IDENT ✓
 *        `io::File`（类型位置）⇒ 平名字 ✓
 *      ⇒ 检查器看到的还是它熟悉的那张平表 ✓（它一行都不用改 ✓）
 *
 * ⚠️ **v1 的诚实限制**（写在这里，别让文档说谎 ✗）：
 *   · 顶层名字要求**全局唯一**：两个模块各有一个私有 `helper` 现在会被"重名"挡下
 *     （错误信息会说清是模块之间的重名）。真正的 per-module 命名空间（mangle）是下一步 ✓
 *   · **不带限定地引用别的模块的名字（函数/全局）暂时合法**（检查器那张平表就是那样）
 *     ⇒ `@private` 现在挡的是**限定引用**这一路 ✓ 下一步在解析处统一挡 ✓
 *   · 两个 `use` 的**短名撞车**（`a::util` 与 `b::util`）⇒ 报错要求改名 ✓
 */
#define _POSIX_C_SOURCE 200809L

#include "modules.h"
#include "lexer.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* readlink：找 <extc>/../stdlib ✓ */

/* ---------------------------------------------------------------- 小工具 */

/* ⚠️ 长度必须**带出来**（第一版忘了 ⇒ 模块被当成空文件，而且症状很隐蔽 ✗）*/
static char *readWhole(Arena *a, const char *path, size_t *outLen) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)arenaAlloc(a, (size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (outLen) *outLen = got;
    return buf;
}

static bool fileExists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* `dir/io.extc` ⇒ `io`（模块短名 ✓）*/
static const char *baseNameNoExt(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t n = strlen(base);
    if (n > 5 && strcmp(base + n - 5, ".extc") == 0) n -= 5;
    return arenaStrndup(a, base, n);
}

/* `std::io` ⇒ `std/io`（模块路径 ⇒ 相对路径 ✓）*/
static char *pathToRel(Arena *a, const char *modPath) {
    Buf b;
    bufInit(&b, a);
    for (const char *p = modPath; *p; p++) {
        if (p[0] == ':' && p[1] == ':') { bufPutc(&b, '/'); p++; }
        else bufPutc(&b, *p);
    }
    return bufCstr(&b);
}

/* `dir/io.extc` ⇒ `dir`；没有斜杠 ⇒ `.` ✓ */
static char *dirOf(Arena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return arenaStrndup(a, ".", 1);
    return arenaStrndup(a, path, (size_t)(slash - path));
}

/* ---------------------------------------------------------------- 装载状态 */

typedef struct {
    const char *file;       /* 解析到的路径（唯一键 ✓）*/
    const char *modName;    /* 短名 */
    Module      mod;
    Ctx        *ctx;        /* **它自己的 Ctx** ⇒ 报错走它，文件与源码行才是对的 ✓
                             * ⚠️ 根那个单元指向调用者给的 ctx（**不能是副本**，
                             *    副本会把诊断吞掉 ✗）*/
    int         state;      /* 0 = 没开始，1 = 正在装载（查环），2 = 好了 ✓ */
} ModUnit;

typedef struct {
    Arena     *a;
    Module    *out;         /* 主 Module（prelude 已经在里面 ✓）*/
    Vec        units;       /* ModUnit* —— 装载过的（查重用）*/
    Vec        order;       /* ModUnit* —— **后序**（= 拓扑序 ✓）*/
    Vec       *ctxs;        /* Ctx* —— 交给上层渲染报错 ✓（**指针**：len 要传出去 ✗ 别按值拷）*/
    Vec        searchDirs;  /* const char*（`-I` ✓）*/
    const char *rootDir;    /* 项目根 = 入口文件所在目录（`use a::b` 一律相对它 ✓）*/
    const char *stdDir;     /* 标准库目录（`std/io.extc` 住那儿 ✓）*/
    int         errors;
} Loader;

static ModUnit *findUnit(Loader *L, const char *file) {
    for (size_t i = 0; i < L->units.len; i++) {
        ModUnit *u = *(ModUnit **)vecAt(&L->units, i);
        if (u && strcmp(u->file, file) == 0) return u;
    }
    return NULL;
}

/* 找 `use` 指向的文件：**离引用它的文件最近的先找** ✓
 * 找不到就把找过的地方都印出来（不然用户只能猜 ✗）*/
static char *resolveModFile(Loader *L, const char *modPath, const char *importerFile) {
    char *rel  = pathToRel(L->a, modPath);
    char *cand = arenaPrintf(L->a, "%s.extc", rel);
    Buf   tried;
    bufInit(&tried, L->a);

    char *p = arenaPrintf(L->a, "%s/%s", L->rootDir, cand);
    if (fileExists(p)) return p;
    bufPrintf(&tried, "\n        %s", p);

    for (size_t i = 0; i < L->searchDirs.len; i++) {
        const char *sd = *(const char **)vecAt(&L->searchDirs, i);
        char *q = arenaPrintf(L->a, "%s/%s", sd, cand);
        if (fileExists(q)) return q;
        bufPrintf(&tried, "\n        %s", q);
    }
    const char *stdDir = L->stdDir;
    if (stdDir && *stdDir) {
        char *r = arenaPrintf(L->a, "%s/%s", stdDir, cand);
        if (fileExists(r)) return r;
        bufPrintf(&tried, "\n        %s", r);
    }
    fprintf(stderr,
            "error: cannot find module `%s` (imported by %s)\n"
            "note:  `use a::b` looks for a file `a/b.extc`. Searched:%s\n"
            "note:  add a search directory with `-I <dir>`, or point $EXTC_STD at the\n"
            "       standard library's directory.\n",
            modPath, importerFile, bufCstr(&tried));
    L->errors++;
    return NULL;
}

/* `a::b` 里的 `a` 是**本模块自己声明的类型**吗？（那它就是 `Type::assoc`，不是模块 ✓）*/
static bool isOwnType(ModUnit *self, const char *name) {
    for (size_t i = 0; i < self->mod.structs.len; i++)
        if (strcmp((*(StructDef **)vecAt(&self->mod.structs, i))->name, name) == 0) return true;
    for (size_t i = 0; i < self->mod.types.len; i++)
        if (strcmp((*(TypeDef **)vecAt(&self->mod.types, i))->name, name) == 0) return true;
    return false;
}

/* 静默探测：`a::b` 那个模块**存不存在**？（决定报"没 use"还是"不是模块" ✓）*/
static bool moduleExists(Loader *L, const char *modPath) {
    char *cand = arenaPrintf(L->a, "%s.extc", pathToRel(L->a, modPath));
    char *p = arenaPrintf(L->a, "%s/%s", L->rootDir, cand);
    if (fileExists(p)) return true;
    for (size_t i = 0; i < L->searchDirs.len; i++) {
        const char *sd = *(const char **)vecAt(&L->searchDirs, i);
        if (fileExists(arenaPrintf(L->a, "%s/%s", sd, cand))) return true;
    }
    const char *stdDir = L->stdDir;
    if (stdDir && *stdDir && fileExists(arenaPrintf(L->a, "%s/%s", stdDir, cand))) return true;
    return false;
}

/* ---------------------------------------------------------------- 声明查找 */

static FuncDef *unitFunc(ModUnit *u, const char *name) {
    for (size_t i = 0; i < u->mod.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&u->mod.funcs, i);
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}
static GlobalDef *unitGlobal(ModUnit *u, const char *name) {
    for (size_t i = 0; i < u->mod.globals.len; i++) {
        GlobalDef *g = *(GlobalDef **)vecAt(&u->mod.globals, i);
        if (strcmp(g->name, name) == 0) return g;
    }
    return NULL;
}
static StructDef *unitStruct(ModUnit *u, const char *name) {
    for (size_t i = 0; i < u->mod.structs.len; i++) {
        StructDef *s = *(StructDef **)vecAt(&u->mod.structs, i);
        if (strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}
static TypeDef *unitType(ModUnit *u, const char *name) {
    for (size_t i = 0; i < u->mod.types.len; i++) {
        TypeDef *t = *(TypeDef **)vecAt(&u->mod.types, i);
        if (strcmp(t->name, name) == 0) return t;
    }
    return NULL;
}

/* 这个模块 `use` 了短名叫 `shortName` 的吗？（**必须先 use** ✓）*/
static ModUnit *importedAs(Loader *L, ModUnit *self, const char *shortName) {
    for (size_t i = 0; i < self->mod.uses.len; i++) {
        UseDecl *u = *(UseDecl **)vecAt(&self->mod.uses, i);
        if (strcmp(u->shortName, shortName) == 0)
            return u->file ? findUnit(L, u->file) : NULL;
    }
    return NULL;
}

/* ---------------------------------------------------------------- 限定名解析 */

static void rwStmt(Loader *L, ModUnit *self, Stmt *s);
static void rwExpr(Loader *L, ModUnit *self, Expr *e);

/* 类型位置：`io::File` ⇒ `File`（顺便查可见性 ✓）*/
static void rwType(Loader *L, ModUnit *self, Type *t) {
    if (!t) return;
    if (t->kind == TY_REF) { rwType(L, self, t->inner); return; }
    for (size_t i = 0; i < t->targs.len; i++) rwType(L, self, *(Type **)vecAt(&t->targs, i));
    if (t->kind != TY_UNRESOLVED || !t->name) return;
    const char *sep = strstr(t->name, "::");
    if (!sep) return;
    const char *shortName = arenaStrndup(L->a, t->name, (size_t)(sep - t->name));
    const char *rest = sep + 2;
    ModUnit *target = importedAs(L, self, shortName);
    if (!target) {
        ctxError(self->ctx, 0, 1,
                 "Modules are imported explicitly (semantic import, not a textual include)."
                 " Write `use a::b` at the top of the file, then use `b::Name`.",
                 "`%s` is not imported here -- add `use %s::%s`", shortName, shortName, rest);
        L->errors++;
        t->name = rest;
        return;
    }
    StructDef *sd = unitStruct(target, rest);
    TypeDef   *td = unitType(target, rest);
    if ((sd && sd->isPrivate) || (td && td->isPrivate)) {
        ctxError(self->ctx, 0, 1,
                 "`@private` means other modules must not name it. Drop the annotation if it is"
                 " meant to be used from here.",
                 "`%s::%s` is private to module `%s`", shortName, rest, shortName);
        L->errors++;
    } else if (!sd && !td) {
        ctxError(self->ctx, 0, 1,
                 "A module exports its top-level declarations; `@private` ones are hidden.",
                 "module `%s` has no type `%s`", shortName, rest);
        L->errors++;
    }
    t->name = rest;              /* 平名字 ⇒ 检查器看到的世界跟以前一样 ✓ */
}

/* 表达式位置：parser 把 `a::b(...)` 造成 EX_ASSOC 了 ⇒ 这里按"a 是不是模块"分流 ✓ */
static void rwQualified(Loader *L, ModUnit *self, Expr *e) {
    const char *tn = e->u.assoc.typeName;
    if (!tn || e->u.assoc.targs.len != 0) return;
    ModUnit *target = importedAs(L, self, tn);
    if (!target) {
        /* 两种可能：① `tn` 是个类型名（`Type::assoc` ⇒ 原样留给检查器 ✓）
         *           ② `tn` 是**存在但没 use** 的模块 ⇒ 报清楚（不然用户看到的是
         *              "unknown type `greet`" 这种八竿子打不着的消息 ✗ 踩过）*/
        /* ⚠️ 判据要小心：`fenwick::new(N)` 里 `fenwick` 是本文件里的一个 **struct**，
         * 而本文件就叫 `fenwick.extc` ⇒ 光看"文件存在"会误报成"模块没导入" ✗（真踩过）
         * ⇒ 自己声明的类型名一律放过；指向**本文件**的那个模块名也放过 ✓ */
        bool isSelfFile = strcmp(baseNameNoExt(L->a, self->file), tn) == 0;
        if (!isOwnType(self, tn) && !isSelfFile && moduleExists(L, tn)) {
            ctxError(self->ctx, e->line, 1,
                     "Modules are imported explicitly (semantic import, not a textual include)."
                     " Add `use %s` at the top of the file.",
                     "module `%s` is not imported here -- add `use %s`", tn, tn);
            L->errors++;
        }
        return;
    }

    const char *nm = e->u.assoc.name;
    FuncDef   *f = unitFunc(target, nm);
    GlobalDef *g = f ? NULL : unitGlobal(target, nm);

    if ((f && f->isPrivate) || (g && g->isPrivate)) {
        ctxError(self->ctx, e->line, 1,
                 "`@private` hides it from other modules. Drop the annotation if it is meant to be"
                 " used from here.",
                 "`%s::%s` is private to module `%s`", tn, nm, tn);
        L->errors++;
        return;
    }
    if (f) {
        /* ⚠️ EX_CALL 装的是**被调用者表达式**（`callee`），不是名字 ⇒ 造一个 EX_IDENT ✓
         * ⚠️ 而且 union 会被改写 ⇒ args 先挪出来 ✓ */
        Vec  args = e->u.assoc.args;
        Expr *id  = exprNew(L->a, EX_IDENT, e->line);
        id->u.ident.name = f->name;            /* 平名字 ✓ */
        e->kind = EX_CALL;
        e->u.call.callee = id;
        e->u.call.args   = args;
        e->qualified     = true;               /* ⇒ 检查器别再说"要写限定名" ✓ */
        return;
    }
    if (g) {
        e->kind = EX_IDENT;
        e->u.ident.name = g->name;
        e->qualified    = true;                /* 定案 70 ✓ */
        return;
    }
    ctxError(self->ctx, e->line, 1,
             "A module exports its top-level `fn` / `struct` / `type` / `let`/`var`;"
             " `@private` ones are hidden.",
             "module `%s` has nothing named `%s`", tn, nm);
    L->errors++;
}

static void rwExpr(Loader *L, ModUnit *self, Expr *e) {
    if (!e) return;
    if (e->kind == EX_ASSOC) rwQualified(L, self, e);   /* 可能把 kind 改掉 ⇒ 之后再走子节点 ✓ */

    switch (e->kind) {
    case EX_BIN:    rwExpr(L, self, e->u.bin.left);  rwExpr(L, self, e->u.bin.right); break;
    case EX_UN:     rwExpr(L, self, e->u.un.operand); break;
    case EX_REF:    rwExpr(L, self, e->u.ref.operand); break;
    case EX_DEREF:  rwExpr(L, self, e->u.deref.operand); break;
    case EX_SIGN:   rwExpr(L, self, e->u.sign.operand); break;
    case EX_CONV:   rwExpr(L, self, e->u.conv.operand); break;
    case EX_TRY:    rwExpr(L, self, e->u.try_.operand); break;
    case EX_INDEX:  rwExpr(L, self, e->u.index.obj); rwExpr(L, self, e->u.index.index); break;
    case EX_SLICE:  rwExpr(L, self, e->u.slice.obj); break;
    case EX_FIELD:  rwExpr(L, self, e->u.field.obj); break;
    case EX_NEW:    rwExpr(L, self, e->u.new_.count); rwType(L, self, e->u.new_.type); break;
    case EX_COALESCE:
        rwExpr(L, self, e->u.coalesce.main); rwExpr(L, self, e->u.coalesce.fallback); break;
    case EX_CALL:
        for (size_t i = 0; i < e->u.call.args.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.call.args, i));
        break;
    case EX_METHOD:
        rwExpr(L, self, e->u.method.recv);
        for (size_t i = 0; i < e->u.method.args.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.method.args, i));
        break;
    case EX_ASSOC:
        for (size_t i = 0; i < e->u.assoc.args.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.assoc.args, i));
        break;
    case EX_GENCALL:
        for (size_t i = 0; i < e->u.gencall.args.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.gencall.args, i));
        break;
    case EX_ENUMVAL:
        for (size_t i = 0; i < e->u.enumval.args.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.enumval.args, i));
        break;
    case EX_STRUCTLIT:
        for (size_t i = 0; i < e->u.lit.inits.len; i++)
            rwExpr(L, self, (*(FieldInit **)vecAt(&e->u.lit.inits, i))->value);
        break;
    case EX_ARRAYLIT:
        for (size_t i = 0; i < e->u.arraylit.elems.len; i++)
            rwExpr(L, self, *(Expr **)vecAt(&e->u.arraylit.elems, i));
        break;
    default: break;               /* 字面量 / 绑定 / null / EX_ENUMVAL 之类没有子节点 ✓ */
    }
}

static void rwStmt(Loader *L, ModUnit *self, Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case ST_VAR:
        if (s->u.var.ann) rwType(L, self, s->u.var.ann);
        rwExpr(L, self, s->u.var.init);
        break;
    case ST_ASSIGN: rwExpr(L, self, s->u.assign.target); rwExpr(L, self, s->u.assign.value); break;
    case ST_IF:  rwExpr(L, self, s->u.ifs.cond);
                 rwStmt(L, self, s->u.ifs.thenBody); rwStmt(L, self, s->u.ifs.elseBody); break;
    case ST_WHILE: rwExpr(L, self, s->u.whiles.cond); rwStmt(L, self, s->u.whiles.body); break;
    case ST_RETURN: rwExpr(L, self, s->u.ret.value); break;
    case ST_EXPR:  rwExpr(L, self, s->u.expr.expr); break;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block.stmts.len; i++)
            rwStmt(L, self, *(Stmt **)vecAt(&s->u.block.stmts, i));
        break;
    case ST_MATCH:
        rwExpr(L, self, s->u.match.scrutinee);
        for (size_t i = 0; i < s->u.match.arms.len; i++)
            rwStmt(L, self, (*(MatchArm **)vecAt(&s->u.match.arms, i))->body);
        break;
    case ST_BREAK: case ST_CONTINUE: break;
    }
}

/* ---------------------------------------------------------------- 合并 */

static void mergeUnit(Loader *L, ModUnit *u) {
    Module *src = &u->mod;
    for (size_t i = 0; i < src->structs.len; i++) {
        StructDef *s = *(StructDef **)vecAt(&src->structs, i);
        s->modName = u->modName;
        for (size_t j = 0; j < s->fields.len; j++)
            rwType(L, u, (*(FieldDef **)vecAt(&s->fields, j))->type);
        for (size_t j = 0; j < s->methods.len; j++) {
            FuncDef *m = *(FuncDef **)vecAt(&s->methods, j);
            m->modName = u->modName;
            if (m->ret) rwType(L, u, m->ret);
            for (size_t k = 0; k < m->params.len; k++)
                rwType(L, u, (*(Param **)vecAt(&m->params, k))->type);
            rwStmt(L, u, m->body);
        }
        *(StructDef **)vecPush(&L->out->structs) = s;
    }
    for (size_t i = 0; i < src->types.len; i++) {
        TypeDef *t = *(TypeDef **)vecAt(&src->types, i);
        t->modName = u->modName;
        for (size_t k = 0; k < t->variants.len; k++) {
            Variant *v = *(Variant **)vecAt(&t->variants, k);
            for (size_t j = 0; j < v->types.len; j++)
                rwType(L, u, *(Type **)vecAt(&v->types, j));
        }
        *(TypeDef **)vecPush(&L->out->types) = t;
    }
    for (size_t i = 0; i < src->globals.len; i++) {
        GlobalDef *g = *(GlobalDef **)vecAt(&src->globals, i);
        g->modName = u->modName;
        if (g->ann) rwType(L, u, g->ann);
        rwExpr(L, u, g->init);
        *(GlobalDef **)vecPush(&L->out->globals) = g;
    }
    for (size_t i = 0; i < src->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&src->funcs, i);
        f->modName = u->modName;
        if (f->ret) rwType(L, u, f->ret);
        for (size_t j = 0; j < f->params.len; j++)
            rwType(L, u, (*(Param **)vecAt(&f->params, j))->type);
        rwStmt(L, u, f->body);
        *(FuncDef **)vecPush(&L->out->funcs) = f;
    }
}

/* ---------------------------------------------------------------- 递归装载 */

static ModUnit *loadUnit(Loader *L, const char *modPath, const char *importerFile, int line) {
    char *file = resolveModFile(L, modPath, importerFile);
    if (!file) return NULL;

    ModUnit *u = findUnit(L, file);
    if (u) {
        if (u->state == 1) {                       /* ⚠️ 正在装载 ⇒ **环** ✗ */
            fprintf(stderr,
                    "%s:%d: error: import cycle: `%s` is still being loaded\n"
                    "note:  two modules must not depend on each other (each one's type is\n"
                    "       needed to check the other). Put the shared part in a third module.\n",
                    importerFile, line, modPath);
            L->errors++;
        }
        return u;                                  /* 已经装过 ⇒ 直接复用 ✓（去重 ✓）*/
    }

    size_t len = 0;
    char *src = readWhole(L->a, file, &len);
    if (!src) {
        fprintf(stderr, "error: cannot read `%s`\n", file);
        L->errors++;
        return NULL;
    }

    u = (ModUnit *)arenaAllocZero(L->a, sizeof(ModUnit));
    u->file    = file;
    u->modName = baseNameNoExt(L->a, file);
    u->state   = 1;                                /* 正在装载 ✓ */
    moduleInit(&u->mod, L->a);
    u->ctx = (Ctx *)arenaAllocZero(L->a, sizeof(Ctx));
    ctxInit(u->ctx, L->a, file, src, len);
    *(ModUnit **)vecPush(&L->units) = u;
    *(Ctx **)vecPush(L->ctxs) = u->ctx;

    Vec toks;
    vecInit(&toks, L->a, sizeof(Token));
    lexAll(u->ctx, &toks);
    if (!u->ctx->hasError) parseModule(u->ctx, L->a, &toks, &u->mod);
    if (getenv("EXTC_DBG_MOD"))
        fprintf(stderr, "[mod] loaded %s: %zu tokens, %zu funcs, %zu globals\n",
                file, toks.len, u->mod.funcs.len, u->mod.globals.len);
    if (u->ctx->hasError) { L->errors++; u->state = 2; return u; }

    /* 模块不能定义 `main`（入口只有根文件 ✓）*/
    for (size_t i = 0; i < u->mod.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&u->mod.funcs, i);
        if (strcmp(f->name, "main") == 0) {
            ctxError(u->ctx, f->line, 1,
                     "A module is a library -- the program starts at the file you pass on the"
                     " command line.",
                     "`main` must live in the entry file, not in a module");
            L->errors++;
        }
    }
    /* 短名撞车：`use a::util` 与 `use b::util`（引用时写不出区别 ✗）*/
    for (size_t i = 0; i < u->mod.uses.len; i++) {
        UseDecl *a = *(UseDecl **)vecAt(&u->mod.uses, i);
        for (size_t j = i + 1; j < u->mod.uses.len; j++) {
            UseDecl *b = *(UseDecl **)vecAt(&u->mod.uses, j);
            if (strcmp(a->shortName, b->shortName) == 0 && strcmp(a->path, b->path) != 0) {
                ctxError(u->ctx, b->line, 1,
                         "Two modules with the same last name cannot be told apart in the source."
                         " Rename one of the files.",
                         "both `%s` and `%s` are used as `%s`", a->path, b->path, b->shortName);
                L->errors++;
            }
        }
    }

    /* 递归装载依赖（**后序入 order** ⇒ 合成时依赖在前 ✓）*/
    for (size_t i = 0; i < u->mod.uses.len; i++) {
        UseDecl *ud = *(UseDecl **)vecAt(&u->mod.uses, i);
        ModUnit *dep = loadUnit(L, ud->path, file, ud->line);
        ud->file = dep ? dep->file : NULL;
    }
    u->state = 2;
    *(ModUnit **)vecPush(&L->order) = u;           /* ⭐ 后序：依赖已经先入表 ✓ */
    return u;
}

/* ---------------------------------------------------------------- 入口 */

bool loadModules(Arena *a, Module *out, Module *rootm, Ctx *rootCtx,
                 const char *rootPath, Vec *searchDirs, Vec *outCtxs) {
    Loader L;
    memset(&L, 0, sizeof L);
    L.a = a;
    L.out = out;
    L.rootDir = dirOf(a, rootPath);          /* 项目根 ✓ */
    /* 标准库目录：`$EXTC_STD` 优先，否则 `<extc 可执行文件所在目录>/../stdlib` ✓
     * （prelude 是**内嵌**的，所以它不在这；`std::io` 那种**真模块**需要真文件 ✓）*/
    {
        const char *env = getenv("EXTC_STD");
        if (env && *env) L.stdDir = env;
        else {
            char buf[4096];
            ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
            if (n > 0) {
                buf[n] = '\0';
                char *slash = strrchr(buf, '/');
                if (slash) { *slash = '\0'; L.stdDir = arenaPrintf(a, "%s/../stdlib", buf); }
            }
        }
    }
    vecInit(&L.units, a, sizeof(void *));
    vecInit(&L.order, a, sizeof(void *));
    if (outCtxs) { L.ctxs = outCtxs; if (!outCtxs->arena) vecInit(outCtxs, a, sizeof(void *)); }
    else { L.ctxs = (Vec *)arenaAllocZero(a, sizeof(Vec)); vecInit(L.ctxs, a, sizeof(void *)); }
    if (searchDirs) L.searchDirs = *searchDirs;
    else vecInit(&L.searchDirs, a, sizeof(void *));

    /* 根文件的 `use` */
    for (size_t i = 0; i < rootm->uses.len; i++) {
        UseDecl *u = *(UseDecl **)vecAt(&rootm->uses, i);
        ModUnit *dep = loadUnit(&L, u->path, rootPath, u->line);
        u->file = dep ? dep->file : NULL;
    }
    /* 根文件的短名撞车也查一遍 ✓ */
    for (size_t i = 0; i < rootm->uses.len; i++) {
        UseDecl *x = *(UseDecl **)vecAt(&rootm->uses, i);
        for (size_t j = i + 1; j < rootm->uses.len; j++) {
            UseDecl *y = *(UseDecl **)vecAt(&rootm->uses, j);
            if (strcmp(x->shortName, y->shortName) == 0 && strcmp(x->path, y->path) != 0) {
                fprintf(stderr, "error: both `%s` and `%s` are used as `%s`\n"
                                "note:  two modules with the same last name cannot be told apart\n"
                                "       in the source -- rename one of the files.\n",
                        x->path, y->path, x->shortName);
                L.errors++;
            }
        }
    }
    if (getenv("EXTC_DBG_MOD")) fprintf(stderr, "[mod] 装载完：errors=%d units=%zu order=%zu\n",
                                         L.errors, L.units.len, L.order.len);
    if (L.errors) return false;

    /* 模块：**拓扑序**（依赖在前 ✓）*/
    for (size_t i = 0; i < L.order.len; i++)
        mergeUnit(&L, *(ModUnit **)vecAt(&L.order, i));

    /* 根文件：声明最后进（它用到的模块已经在了 ✓）；函数体也要解析限定名 ✓ */
    {
        ModUnit root;
        memset(&root, 0, sizeof root);
        root.file    = rootPath;
        root.mod     = *rootm;
        root.ctx     = rootCtx;   /* ⚠️ 真 ctx，不是副本（副本会吞掉诊断 ✗）*/
        root.state   = 2;
        root.modName = NULL;
        for (size_t i = 0; i < rootm->structs.len; i++) {
            StructDef *sd = *(StructDef **)vecAt(&rootm->structs, i);
            for (size_t j = 0; j < sd->fields.len; j++)
                rwType(&L, &root, (*(FieldDef **)vecAt(&sd->fields, j))->type);
            for (size_t j = 0; j < sd->methods.len; j++) {
                FuncDef *m = *(FuncDef **)vecAt(&sd->methods, j);
                if (m->ret) rwType(&L, &root, m->ret);
                for (size_t k = 0; k < m->params.len; k++)
                    rwType(&L, &root, (*(Param **)vecAt(&m->params, k))->type);
                rwStmt(&L, &root, m->body);
            }
            *(StructDef **)vecPush(&out->structs) = sd;
        }
        for (size_t i = 0; i < rootm->types.len; i++)
            *(TypeDef **)vecPush(&out->types) = *(TypeDef **)vecAt(&rootm->types, i);
        for (size_t i = 0; i < rootm->globals.len; i++) {
            GlobalDef *g = *(GlobalDef **)vecAt(&rootm->globals, i);
            if (g->ann) rwType(&L, &root, g->ann);
            rwExpr(&L, &root, g->init);
            *(GlobalDef **)vecPush(&out->globals) = g;
        }
        for (size_t i = 0; i < rootm->funcs.len; i++) {
            FuncDef *f = *(FuncDef **)vecAt(&rootm->funcs, i);
            if (f->ret) rwType(&L, &root, f->ret);
            for (size_t j = 0; j < f->params.len; j++)
                rwType(&L, &root, (*(Param **)vecAt(&f->params, j))->type);
            rwStmt(&L, &root, f->body);
            *(FuncDef **)vecPush(&out->funcs) = f;
        }
    }
    if (getenv("EXTC_DBG_MOD")) fprintf(stderr, "[mod] 合并完：errors=%d 声明 funcs=%zu globals=%zu\n",
                                         L.errors, out->funcs.len, out->globals.len);
    if (L.errors) return false;

    return true;
}
