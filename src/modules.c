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

typedef struct { const char *from; const char *to; } Ren;

typedef struct {
    const char *file;       /* 解析到的路径（唯一键 ✓）*/
    const char *modName;    /* 短名 */
    Module      mod;
    Ctx        *ctx;        /* **它自己的 Ctx** ⇒ 报错走它，文件与源码行才是对的 ✓
                             * ⚠️ 根那个单元指向调用者给的 ctx（**不能是副本**，
                             *    副本会把诊断吞掉 ✗）*/
    int         state;      /* 0 = 没开始，1 = 正在装载（查环），2 = 好了 ✓ */
    /* ⭐ 模块 mangle：`源码名 → mangle 名`（`pair` → `liba$pair`）✓ */
    Vec         ren;        /* Ren* */
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

/* ⭐ 模块 mangle：`name` 加模块前缀（`io$readLine`）。**根模块不加** ✓
 * 为什么用 `$`：C 里合法、extC 标识符里不允许 ⇒ 天然不撞用户名字 ✓ */
static const char *mangleName(Loader *L, ModUnit *u, const char *name) {
    if (!u->modName || !*u->modName) return name;
    return arenaPrintf(L->a, "%s$%s", u->modName, name);
}

/* ⚠️⚠️ **`extern!` 的名字是 ABI，绝不能加前缀** ✗
 * `extern!("libc") fn read(…)` 里的 `read` 是**链接器要去找的符号名** ——
 * 改成 `sys$read` 只会得到一个 `undefined reference to sys$read`，
 * 而报错来自 ld，跟"模块改名"八竿子打不着，极难反查 ✗（真踩过：
 * `tests/io` 整个跑不起来，`stdlib/std/sys.extc` 里每个原语都踩）
 * ⇒ 这类声明**一律保留原名**（回程票登记成自己 ⇒ 模块内引用也不用改 ✓）*/
static bool externKeepsName(Module *src, const char *name) {
    for (size_t i = 0; i < src->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&src->funcs, i);
        if (f->isExtern && strcmp(f->name, name) == 0) return true;
    }
    return false;
}
/* 按**目标单位**查它的 mangle 名。
 * ⚠️ 不能按"调用者"查：根文件的 modName 是 NULL ⇒ 会早退 ⇒ 引用一条都改不到 ✗（踩过）*/
static const char *renOfTarget(ModUnit *target, const char *name) {
    if (!target || !target->modName || !*target->modName) return name;
    for (size_t i = 0; i < target->ren.len; i++) {
        Ren *r = (Ren *)vecAt(&target->ren, i);
        if (strcmp(r->from, name) == 0) return r->to;
    }
    return name;
}
/* 调用者自己单元里的 mangle 名（四个 unit* 查表助手要用 ✓）*/
static const char *renLookup(ModUnit *u, const char *name) {
    if (!u || !u->modName || !*u->modName) return name;
    for (size_t i = 0; i < u->ren.len; i++) {
        Ren *r = (Ren *)vecAt(&u->ren, i);
        if (strcmp(r->from, name) == 0) return r->to;
    }
    return NULL;
}

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
    /* ⚠️ 调用者给**源码名**，而声明已被 mangle ⇒ 先查 ren 表 ✓ */
    const char *want = renLookup(u, name);
    if (!want) want = name;
    for (size_t i = 0; i < u->mod.funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&u->mod.funcs, i);
        if (strcmp(f->name, want) == 0) return f;
    }
    return NULL;
}
static GlobalDef *unitGlobal(ModUnit *u, const char *name) {
    /* ⚠️ 调用者给**源码名**，而声明已被 mangle ⇒ 先查 ren 表 ✓ */
    const char *want = renLookup(u, name);
    if (!want) want = name;
    for (size_t i = 0; i < u->mod.globals.len; i++) {
        GlobalDef *g = *(GlobalDef **)vecAt(&u->mod.globals, i);
        if (strcmp(g->name, want) == 0) return g;
    }
    return NULL;
}
/* ⚠️ **两个名字都要认**：`mergeUnit` 是**原地**改名（`d->name` 被直接改掉），而根文件的
 * 限定名解析发生在合并**之后** ⇒ 这时 `u->mod.structs` 里已经是 `e1$color` 了，
 * 只按源码名 `color` 找会**找不到** ⇒ `e1::color` 被误判成"模块里没这个东西"✗
 * （踩过：报的是 `undefined name \`color\``，指向的跟真因差着十万八千里）*/
static StructDef *unitStruct(ModUnit *u, const char *name) {
    /* ⚠️ 调用者给**源码名**，而声明可能已被 mangle ⇒ 两种名字都试 ✓ */
    const char *want = renLookup(u, name);
    if (!want) want = name;
    for (size_t i = 0; i < u->mod.structs.len; i++) {
        StructDef *s = *(StructDef **)vecAt(&u->mod.structs, i);
        if (strcmp(s->name, want) == 0 || strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}
static TypeDef *unitType(ModUnit *u, const char *name) {
    /* ⚠️ 同上 ✓ */
    const char *want = renLookup(u, name);
    if (!want) want = name;
    for (size_t i = 0; i < u->mod.types.len; i++) {
        TypeDef *t = *(TypeDef **)vecAt(&u->mod.types, i);
        if (strcmp(t->name, want) == 0 || strcmp(t->name, name) == 0) return t;
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
static void rwExprName(ModUnit *self, Expr *e);

/* 类型位置：`io::File` ⇒ mangle 名（顺便查可见性 ✓）*/
static void rwType(Loader *L, ModUnit *self, Type *t) {
    if (!t) return;
    if (t->kind == TY_REF) { rwType(L, self, t->inner); return; }
    for (size_t i = 0; i < t->targs.len; i++) rwType(L, self, *(Type **)vecAt(&t->targs, i));
    if (t->kind != TY_UNRESOLVED || !t->name) return;
    const char *sep = strstr(t->name, "::");
    if (!sep) {
        /* ⚠️⚠️ **裸名也要改名 —— 这是本模块自己声明的类型**。
         * 不改的话它就以源码名 `pair` 留在 `FuncDef.ret` 上，等到检查器 `ttResolve`
         * 时才发现平表里有**两个** `pair`（另一个模块的）⇒ 只能靠别名表挑一个 ⇒
         * **静默绑成先注册的那个模块的类型**，而 `var q: beta::pair = beta::make(7)`
         * 的标注是 `beta$pair` ⇒ 报的却是 `struct \`alpha$pair\` has no field \`s\``，
         * 指的名字在源码里根本没出现过 ✗✗（这个洞最阴：**编得过、类型是错的**）
         * 判据跟 `rwExprName` 一致：**只认本模块自己声明的名字** ✓ */
        const char *m = renLookup(self, t->name);
        if (m) t->name = m;
        return;
    }
    const char *shortName = arenaStrndup(L->a, t->name, (size_t)(sep - t->name));
    const char *rest = sep + 2;
    /* ⚠️ 诊断里 `use …` 只能写**模块名**：`rest` 里还可能带 `::`
     * （`alpha::pair` 出现在 `lib::box<alpha::pair>` 的实参里 ⇒ `rest` = `pair`，
     * 而 `beta::color` ⇒ `rest` = `color`；但 `a::b::c` 那种就会把 `b::c` 拼进去 ✗）
     * 踩过：消息是"add `use alpha::pair`"，用户照着写会得到一个不存在的模块 ✗ */
    const char *restSep = strstr(rest, "::");
    const char *restTop = restSep ? arenaStrndup(L->a, rest, (size_t)(restSep - rest)) : rest;
    ModUnit *target = importedAs(L, self, shortName);
    if (!target) {
        ctxError(self->ctx, 0, 1,
                 "Modules are imported explicitly (semantic import, not a textual include)."
                 " Write `use a::b` at the top of the file, then use `b::Name`.",
                 "`%s` is not imported here -- add `use %s`", shortName, shortName);
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
                 "`%s::%s` is private to module `%s`", shortName, restTop, shortName);
        L->errors++;
    } else if (!sd && !td) {
        ctxError(self->ctx, 0, 1,
                 "A module exports its top-level declarations; `@private` ones are hidden.",
                 "module `%s` has no type `%s`", shortName, restTop);
        L->errors++;
    }
    t->name = renOfTarget(target, rest);
}

/* ⭐ 表达式位置里的**类型名**：`mod::Type { … }` / `mod::Type.variant`。
 * 判据只有一条 —— "`short::rest` 里的 `rest` 在这个模块里是个**类型**吗"。
 * 为什么必须有这条：模块之间同名类型只靠裸名写不出来（裸名有歧义 ⇒
 * 装载器不登记回程票）⇒ 不认这里，那个类型的字面量和枚举变体就**无字可写** ✗
 * 踩过的两个症状：`expected \`{\``（parser 不认限定名字面量）、
 * `module \`e1\` has nothing named \`color\``（把类型名当成了函数/常量在找）✗ */
static bool rwQualifiedTypeName(Loader *L, ModUnit *self, const char *qname, const char **out) {
    if (!qname || !out) return false;
    const char *sep = strstr(qname, "::");
    if (!sep) return false;
    const char *shortName = arenaStrndup(L->a, qname, (size_t)(sep - qname));
    const char *rest = sep + 2;
    if (strstr(rest, "::")) return false;          /* `a::b::c` 不支持（够用就好 ✓）*/
    ModUnit *target = importedAs(L, self, shortName);
    if (!target) return false;
    /* ⚠️ **指向自己的限定名一律不动** —— prelude 里就写着 `pcg32::withStream(…)`
     * （`pcg32` 是同一文件里的 struct），而 prelude 自己是**一个模块** ⇒ 不拦的话
     * 会把它改写成 `prelude$pcg32`，可 prelude 的舞台快照是**装载前**拿的 ⇒
     * 类型表里根本没这个名字 ⇒ `call to undefined function` ✗（真踩过）
     * 这跟 `rwQualified` 里那条"本文件名叫 `fenwick` 就放过"是同一个坑 ✓ */
    if (target == self) return false;
    StructDef *sd = unitStruct(target, rest);
    TypeDef   *td = sd ? NULL : unitType(target, rest);
    if (!sd && !td) return false;                  /* 不是类型 ⇒ 走原来那条路 ✓ */
    if ((sd && sd->isPrivate) || (td && td->isPrivate)) {
        ctxError(self->ctx, 0, 1,
                 "`@private` means other modules must not name it. Drop the annotation if it is"
                 " meant to be used from here.",
                 "`%s::%s` is private to module `%s`", shortName, rest, shortName);
        L->errors++;
        return true;                               /* 报过了 ⇒ 别再报第二条 ✓ */
    }
    *out = renOfTarget(target, rest);              /* 裸名 ⇒ 检查器按源码名解析 ✓ */
    return true;
}

/* 表达式位置：parser 把 `a::b(...)` 造成 EX_ASSOC 了 ⇒ 这里按"a 是不是模块"分流 ✓ */
static void rwQualified(Loader *L, ModUnit *self, Expr *e) {
    const char *tn = e->u.assoc.typeName;
    if (!tn || e->u.assoc.targs.len != 0) return;
    ModUnit *target = importedAs(L, self, tn);
    if (!target) {
        /* ① `a::b` 里 `a` 不是模块，但 `b` 是某个模块 `a` 的**类型** ⇒ 认（见上）✓ */
        const char *mangled = NULL;
        if (rwQualifiedTypeName(L, self, tn, &mangled) && mangled) {
            e->u.ident.name = mangled;             /* union 会被改写 ⇒ 先取名字 ✓ */
            e->kind = EX_IDENT;
            e->qualified = true;
            return;
        }
        (void)0;
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
        id->u.ident.name = renOfTarget(target, nm);
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
    /* ⚠️⚠️ **表达式里也有名字，别只走子节点** —— 这里最阴的一个洞：
     * `EX_STRUCTLIT` 的 `inits` 被走了、**它自己的类型名却没有** ⇒
     * 两个模块各有 `pair` 时，`liba::pair { … }` 的名字**原样**留给检查器，
     * 而检查器按裸名 `ttFromName` ⇒ 命中**先注册的那个模块**的结构体 ⇒
     * **静默绑成另一个类型**（字段名凑巧一样就一声不吭地编过 ✗✗ 真踩过，
     * 症状是 `struct \`alpha$pair\` has no field \`s\``，指向的还不是源码里的名字）
     * ⇒ 类型名一律在这里改写 ✓  `EX_CONV` 同理（`mod::T(x)`）✓ */
    switch (e->kind) {
    case EX_STRUCTLIT: {
        const char *m = NULL;
        if (e->u.lit.name && rwQualifiedTypeName(L, self, e->u.lit.name, &m) && m)
            e->u.lit.name = m;
        break;
    }
    case EX_ENUMVAL: {
        const char *m = NULL;
        if (e->u.enumval.typeName && rwQualifiedTypeName(L, self, e->u.enumval.typeName, &m) && m)
            e->u.enumval.typeName = m;
        break;
    }
    case EX_CONV: {
        const char *m = NULL;
        if (e->u.conv.typeName && rwQualifiedTypeName(L, self, e->u.conv.typeName, &m) && m)
            e->u.conv.typeName = m;
        break;
    }
    /* ⭐ `mod::Type.variant` —— parser 把 `mod::Type` 造成**裸标识符**了
     * （它只拼名字，不查符号表）⇒ 这里认出"它其实是个类型"并换成裸类型名，
     * 剩下的交给检查器那条现成的枚举变体路 ✓ */
    case EX_IDENT: {
        const char *m = NULL;
        if (e->u.ident.name && rwQualifiedTypeName(L, self, e->u.ident.name, &m) && m) {
            e->u.ident.name = m;
            e->qualified = true;     /* 写的就是限定名 ⇒ 别再提示"要写限定名" ✓ */
        } else {
            rwExprName(self, e);     /* `color.green` 里的 `color` ⇒ `e1$color` ✓ */
        }
        break;
    }
    default: break;
    }
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
        /* ⚠️ **被调者也要走** —— 模块里 `fn a() { b() }` 调的是**自己这个模块**的 `b`，
         * 而声明会被 mangle ⇒ 不走这一步，`b` 就永远找不到 ✗
         * （踩过：`call to undefined function \`twice\``，可 `greet$twice` 明明在表里）*/
        rwExpr(L, self, e->u.call.callee);
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

static void mangleUnitDecls(Loader *L, ModUnit *u) {
    if (!u->modName || !*u->modName) return;              /* 根模块：一律不动 ✓ */
    Module *src = &u->mod;
    /* ⚠️⚠️ **这里有个 arena 陷阱，踩过**：`mangleName` 用 `arenaPrintf` ⇒ 返回值指向
     * **arena 里的缓冲**，而 arena 会**原地增长/复用** ⇒ 如果"算一个名字就马上存进表、
     * 接着再算下一个"，先前那个指针会被**后一次分配覆盖** ✗
     * 实测症状极隐蔽：别名表打出来是 `pair=>make`（`pair` 的目标被 `make` 的名字覆盖）
     * ⇒ 于是裸名 `pair` 查不到 ⇒ `unknown type pair`，而**根因在内存复用、不在查表逻辑** ✗
     * ⇒ 修法：**先把所有名字算完**（4 个循环全是分配），**再**填表（只存指针，不再分配）✓ */
    const char **names = (const char **)arenaAlloc(L->a, sizeof(char *) * 64);
    size_t n = 0;
    /* ⚠️ **`srcName` 必须在这里、按"源码名"填**：等到 `mergeUnit` 里再填时
     * `d->name` 已经变成 mangle 名了 ⇒ 会填成 `alpha::alpha$pair` ✗（真踩过）
     * ⇒ 顺手在这一趟把给用户看的名字定下来（`alpha::pair`）✓ */
    for (size_t i = 0; i < src->structs.len && n < 64; i++) {
        StructDef *d = *(StructDef **)vecAt(&src->structs, i);
        d->srcName = arenaPrintf(L->a, "%s::%s", u->modName, d->name);
        names[n++] = mangleName(L, u, d->name);
    }
    for (size_t i = 0; i < src->types.len && n < 64; i++) {
        TypeDef *d = *(TypeDef **)vecAt(&src->types, i);
        d->srcName = arenaPrintf(L->a, "%s::%s", u->modName, d->name);
        names[n++] = mangleName(L, u, d->name);
    }
    for (size_t i = 0; i < src->globals.len && n < 64; i++)
        names[n++] = mangleName(L, u, (*(GlobalDef **)vecAt(&src->globals, i))->name);
    for (size_t i = 0; i < src->funcs.len && n < 64; i++) {
        const char *fn = (*(FuncDef **)vecAt(&src->funcs, i))->name;
        /* `extern!` ⇒ 原名进出（见 `externKeepsName` 的说明 ✓）*/
        names[n++] = externKeepsName(src, fn) ? fn : mangleName(L, u, fn);
    }
    /* 名字都算完了 ⇒ 现在只填表（**不再有分配** ⇒ 指针稳定 ✓）*/
    size_t k = 0;
    for (size_t i = 0; i < src->structs.len; i++) {
        StructDef *d = *(StructDef **)vecAt(&src->structs, i);
        Ren *r = (Ren *)vecPush(&u->ren); r->from = d->name; r->to = names[k++]; }
    for (size_t i = 0; i < src->types.len; i++) {
        TypeDef *d = *(TypeDef **)vecAt(&src->types, i);
        Ren *r = (Ren *)vecPush(&u->ren); r->from = d->name; r->to = names[k++]; }
    for (size_t i = 0; i < src->globals.len; i++) {
        GlobalDef *d = *(GlobalDef **)vecAt(&src->globals, i);
        Ren *r = (Ren *)vecPush(&u->ren); r->from = d->name; r->to = names[k++]; }
    for (size_t i = 0; i < src->funcs.len; i++) {
        FuncDef *d = *(FuncDef **)vecAt(&src->funcs, i);
        Ren *r = (Ren *)vecPush(&u->ren);
        r->from = d->name; r->to = names[k++];
        /* `to == from` ⇒ 不改名；但回程票**照样登记**（登记成自己）✓
         * 这样 `renLookup` 依然返回值，模块内的裸写不会被跳过 ✓ */}
    for (size_t i = 0; i < u->ren.len; i++) {
        Ren *r = (Ren *)vecAt(&u->ren, i);
        for (size_t j = 0; j < src->structs.len; j++)
            if (strcmp((*(StructDef **)vecAt(&src->structs, j))->name, r->from) == 0)
                (*(StructDef **)vecAt(&src->structs, j))->name = r->to;
        for (size_t j = 0; j < src->types.len; j++)
            if (strcmp((*(TypeDef **)vecAt(&src->types, j))->name, r->from) == 0)
                (*(TypeDef **)vecAt(&src->types, j))->name = r->to;
        for (size_t j = 0; j < src->globals.len; j++)
            if (strcmp((*(GlobalDef **)vecAt(&src->globals, j))->name, r->from) == 0)
                (*(GlobalDef **)vecAt(&src->globals, j))->name = r->to;
        for (size_t j = 0; j < src->funcs.len; j++)
            if (strcmp((*(FuncDef **)vecAt(&src->funcs, j))->name, r->from) == 0)
                (*(FuncDef **)vecAt(&src->funcs, j))->name = r->to;
    }
    if (getenv("EXTC_DBG_M")) {
        fprintf(stderr, "[mangle] %s: %zu 个声明改名:", u->modName, u->ren.len);
        for (size_t i = 0; i < u->ren.len; i++) { Ren *r = (Ren *)vecAt(&u->ren, i); fprintf(stderr, " %s->%s", r->from, r->to); }
        fprintf(stderr, "\n");
    }
}

/* ⭐ **模块自己文件里写的裸名**（`color.green` 的 `color`、`shout` 里调的 `twice`）
 * 也要跟着改名 —— 声明被 mangle 之后，模块**内部**这些裸名会全部失效 ✗
 * （踩过：报的是 `call to undefined function \`twice\``，而 `greet$twice` 明明就在表里）
 * 为什么**只认本模块自己声明的名字**（不认"任何模块导出过的名字"）：
 * 认了的话，同一个名字在别的模块里也有时，本模块里那句裸写会被**悄悄改写过去**，
 * 于是既编得过、又变成别人的东西 —— 比报错坏得多 ✗
 * 自己声明的名字被裸写 = 就是自己那个（`@private` 也照样解析得到，
 * 可见性由 `requireQualified` 在后面单独查 ✓）*/
static void rwExprName(ModUnit *self, Expr *e) {
    if (!e || e->kind != EX_IDENT) return;
    const char *nm = e->u.ident.name;
    /* ⚠️ 被调者处名字已被改成 `lib$open`（见 `rwQualified`）⇒ 这里必须能回退到
     * **源码名** `open`，否则"要写限定名"那条诊断会变成
     * `lib$open 属于模块 lib -- 请写 lib::lib$open`，纯属胡说 ✗（真踩过）*/
    if (!nm) nm = e->u.ident.srcName;
    const char *m = nm ? renLookup(self, nm) : NULL;
    if (m) { e->u.ident.srcName = nm; e->u.ident.name = m; }
}

static void mergeUnit(Loader *L, ModUnit *u) {
    Module *src = &u->mod;
    mangleUnitDecls(L, u);
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
    vecInit(&u->ren, L->a, sizeof(Ren));
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

    /* ⭐ 裸名回程票（必须在 mergeUnit **之后** —— ren 表是那里建的 ✓）*/
    if (!out->aliases.arena) vecInit(&out->aliases, a, sizeof(Alias));
    for (size_t i = 0; i < L.order.len; i++) {
        ModUnit *dep = *(ModUnit **)vecAt(&L.order, i);
        for (size_t j = 0; j < dep->ren.len; j++) {
            Ren *r = (Ren *)vecAt(&dep->ren, j);
            /* ⚠️ **不去重**：两条同名别名**都要登记** ⇒ 类型表数得出"≥2 个匹配"
             * 才能报"歧义，请写限定名" ✗（去重成一条的话，裸名会被**静默**
             * 解析成先注册的那个模块的类型 —— 编得过、类型是错的 ✗✗ 真踩过）*/
            bool same = false;
            for (size_t k = 0; k < out->aliases.len && !same; k++) {
                Alias *a = (Alias *)vecAt(&out->aliases, k);
                if (strcmp(a->from, r->from) == 0 && strcmp(a->to, r->to) == 0) same = true;
            }
            if (same) continue;                /* 同一条别名（同一个模块被 use 两次）✓ */
            Alias *al = (Alias *)vecPush(&out->aliases);
            al->from = r->from;  al->to = r->to;
        }
    }

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
