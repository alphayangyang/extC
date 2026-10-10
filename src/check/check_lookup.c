/* Name lookup and non-null narrowing for `?ref T`.
 *
 * Resolving a name to a binding lives here, together with the facts a comparison against
 * null establishes and the small predicates the rest of the checker is built from.
 */

#include <string.h>
#include "dbg.h"
#include "check_internal.h"
#include "plan.h"          /* the plan side: "is this an instance" is a plan question */

/* One recursion cap for the type walkers below: an enum may hold **itself**
 * (`type list = | cons(i64, list) | nil`), and such a walk would not terminate. Past the
 * cap each walker answers conservatively. */
#define ZERO_VALUE_DEPTH_LIMIT 64

/* ----------------------------------------------- non-null narrowing for `?ref T` */
/**
 * @brief 判断某个生成名在当前位置已被证明非空。
 *
 * 非空证明跟着绑定而不是源码里写的名字走，所以查询用绑定的生成 C 名。
 *
 * @param c      检查器，证明表在它里面。
 * @param cname  绑定的生成 C 名。
 * @return       证明仍然有效时返回 true；cname 为 NULL 或从未被证明时返回 false。
 */
bool isNarrowed(Checker *c, const char *cname) {
    if (!cname) return false;
    for (size_t i = 0; i < c->narrow.len; i++)
        if (strcmp(*(const char **)vecAt(&c->narrow, i), cname) == 0) return true;
    return false;
}

/**
 * @brief 记下当前位置某个绑定已被证明非空。
 *
 * 记的是绑定的生成 C 名而不是源码里写的名字，因为收窄跟着绑定走。重复记同一个名字不改变任何东西。
 *
 * @param c      检查器。
 * @param cname  绑定的生成 C 名；NULL 忽略。
 */
void pushNarrow(Checker *c, const char *cname) {
    if (!cname || isNarrowed(c, cname)) return;
    *(const char **)vecPush(&c->narrow) = cname;
}

/**
 * @brief 丢掉一个绑定以及以它为前缀的路径的非空证明。
 *
 * 证明按栈保存，从这个绑定的条目起整段截掉：对根的写入或取地址都会让 `h.p` 这类路径事实失效。
 * 绑定从未被证明过时什么也不做。
 *
 * @param c      检查器。
 * @param cname  绑定的生成 C 名；NULL 忽略。
 */
void unNarrow(Checker *c, const char *cname) {
    if (!cname) return;
    for (size_t i = 0; i < c->narrow.len; i++) {
        const char *k = *(const char **)vecAt(&c->narrow, i);
        if (strcmp(k, cname) == 0) { c->narrow.len = i; return; }
        /* A fact about a path, such as `h.p`, is dropped as soon as the prefix matches:
         * any write to the root, or taking its address, can make it stop holding. */
        size_t l = strlen(cname);
        if (strncmp(k, cname, l) == 0 && k[l] == '.') { c->narrow.len = i; return; }
    }
}

/**
 * @brief 取条件所证明非空的绑定名。
 *
 * `p != null` 在真分支证明成立，`p == null` 在假分支成立，后者正是把
 * `if p == null { return }` 变成守卫的依据。字段路径 `h.p` 只在根是未被取址的局部时成立，
 * 否则别名可能改掉它。
 *
 * @param c        检查器。
 * @param cond     要检查的条件表达式。
 * @param whenTrue 出参：证明在真分支成立时置 true，在假分支成立时置 false。
 * @return         绑定的生成 C 名，或字段路径键 `root.field`；条件证明不了任何东西时 NULL。
 */
const char *narrowTarget(Checker *c, Expr *cond, bool *whenTrue) {
    if (!cond || cond->kind != EX_BIN) return NULL;
    const char *op = cond->u.bin.op;
    if (!isEqualityOp(op)) return NULL;
    Expr *var = NULL;
    if (cond->u.bin.right->kind == EX_NULL)      var = cond->u.bin.left;
    else if (cond->u.bin.left->kind == EX_NULL)  var = cond->u.bin.right;
    else return NULL;
    /* Path narrowing, `if h.p != null { ... }`, restricted to the sound case: the root
     * has to be a local whose address was never taken, because then no alias can change
     * its fields and the fact "h.p is not null" stays true.
     *
     * A parameter cannot be the root, since the caller may hold an alias or another
     * `mut ref` to the same object, and neither can a binding whose address was taken.
     * This is the same alias restriction that the depth bookkeeping relies on. */
    if (var && var->kind == EX_FIELD && var->u.field.obj->kind == EX_IDENT) {
        Sym *rs = lookup(c, var->u.field.obj->u.ident.name);
        if (!rs || !rs->type || rs->depth < 1 || rs->addressed) return NULL;
        if (!var->type || var->type->kind != TY_REF || !var->type->nullable) return NULL;
        size_t kn = strlen(rs->cname) + strlen(var->u.field.name) + 2;
        char *key = (char *)arenaAlloc(c->arena, kn);
        snprintf(key, kn, "%s.%s", rs->cname, var->u.field.name);
        *whenTrue = (strcmp(op, "!=") == 0);
        return key;
    }
    if (!var || var->kind != EX_IDENT) return NULL;
    Sym *sy = lookup(c, var->u.ident.name);
    if (!sy || !sy->type || sy->type->kind != TY_REF || !sy->type->nullable) return NULL;
    *whenTrue = (strcmp(op, "!=") == 0);
    return sy->cname;
}

/**
 * @brief 记下条件能证明的每一条非空事实。
 *
 * `&&` 的两边都走：能到达条件成立的这一支，说明两边都成立。只有在该支为"真"的证明才记。
 *
 * @param c     检查器。
 * @param cond  要从中取事实的条件；NULL 忽略。
 */
void narrowFactsOf(Checker *c, Expr *cond) {
    if (!cond) return;
    if (cond->kind == EX_BIN && strcmp(cond->u.bin.op, "&&") == 0) {
        narrowFactsOf(c, cond->u.bin.left);
        narrowFactsOf(c, cond->u.bin.right);
        return;
    }
    bool whenTrue = false;
    const char *tg = narrowTarget(c, cond, &whenTrue);
    if (tg && whenTrue) pushNarrow(c, tg);
}

/**
 * @brief 判断语句是否总会离开所在的块。
 *
 * 这是识别 `if p == null { return }` 这种守卫的依据：走到 `if` 之后的代码就说明条件不成立。
 * 块取最后一条语句递归判断，空块不算离开。
 *
 * @param s  语句，通常是 `if` 的 then 分支。
 * @return   语句本身是 return、break 或 continue，或块的最后一条如此时返回 true。
 */
bool blockExits(Stmt *s) {
    if (!s) return false;
    if (s->kind == ST_BLOCK) {
        if (s->u.block.stmts.len == 0) return false;
        return blockExits(*(Stmt **)vecAt(&s->u.block.stmts, s->u.block.stmts.len - 1));
    }
    return s->kind == ST_RETURN || s->kind == ST_BREAK || s->kind == ST_CONTINUE;
}

/**
 * @brief 在没有非空证明时拒绝解引用可空引用。
 *
 * 可空引用不能直接解引用，必须先与 null 比较，让证明落在语法里而不是靠推断。
 *
 * @param c     检查器。
 * @param t     被解引用的类型。
 * @param node  该表达式，用于取行号。
 * @param what  被检查的操作，出现在诊断里。
 * @return      t 是可空引用时已报错并返回 true；否则返回 false。
 */
bool rejectNullableDeref(Checker *c, Type *t, Expr *node, const char *what) {
    if (!t || t->kind != TY_REF || !t->nullable) return false;
    ckError(c, node->line,
            "write `if p != null { ... }` (or `if p == null { return }`) first: inside"
            " that branch the compiler knows it is not null and generates no runtime check",
            "`%s` is a nullable reference (`?ref T`), so %s needs a non-null one",
            typeStr(c, t), what);
    return true;
}

/**
 * @brief 在最内层作用域声明一个绑定并返回它。
 *
 * 引用类型的绑定以所在槽位的深度作为指向深度，随后由知道初始化式的一方收窄。调用者负责把
 * 生成的 C 名字写回 AST。
 *
 * @param c       检查器。
 * @param name    源码里的名字，生成的 C 名字由它派生。
 * @param t       声明的类型。
 * @param mut     `var` 为 true，`let` 为 false。
 * @param shadow  是否允许同名绑定遮蔽本作用域里已有的绑定。
 * @param line    源行号，用于诊断。
 * @param depth   新绑定的词法深度。
 * @return        新建的绑定；同名 `var` 被拒时返回已有的那个绑定。
 */
Sym *declare(Checker *c, const char *name, Type *t, bool mut,
                    bool shadow, int line, int depth) {
    Scope *top = *(Scope **)vecAt(&c->scopes, c->scopes.len - 1);

    if (!shadow) {
        for (size_t i = 0; i < top->syms.len; i++) {
            Sym *old = *(Sym **)vecAt(&top->syms, i);
            if (strcmp(old->name, name) == 0) {
                ckError(c, line,
                        "`var` declares storage, so two `var`s with the same name in one scope "
                        "is almost always a typo -- write `x = ...` to assign the existing one, "
                        "or `let x = ...` if you really mean a new name",
                        "`%s` is already declared in this scope", name);
                return old;
            }
        }
    }

    Sym *s = (Sym *)arenaAllocZero(c->arena, sizeof(Sym));
    s->name = name;
    s->cname = cNameFor(c, name);
    s->type = t;
    s->mut = mut;
    s->depth = depth;
    /* A reference-typed binding starts with the depth of its slot as the pointee depth,
     * the conservative answer. Whoever knows the initializer narrows this to the real
     * depth afterwards, so `var cur: ?ref node = head` ends up at 0, since the cursor
     * points at something on the caller's side. */
    anSetRefDepth(s, (t && typeContainsRef(c->tt, t)) ? depth : 0);
    s->line = line;
    *(Sym **)vecPush(&top->syms) = s;
    *(Sym **)vecPush(&c->allSyms) = s;      /* also recorded for the self-check mode */
    return s;
}

/**
 * @brief 按名字解析出一个绑定，先查最内层作用域，最后查模块级绑定。
 *
 * 同一作用域内后声明的 `let` 胜出，因此从后往前扫。模块级绑定单独存放而不占一个作用域，
 * 否则函数体的深度会因全局多出一层。
 *
 * @param c     检查器。
 * @param name  要解析的源码名字。
 * @return      最内层同名绑定；没有时返回 NULL。
 */
Sym *lookup(Checker *c, const char *name) {
            for (size_t i = c->scopes.len; i-- > 0; ) {
        Scope *s = *(Scope **)vecAt(&c->scopes, i);
        /* Scanned backwards so the newest `let` of the scope wins; the other order would
         * resolve the second `a` of `let a = 1  let a = a + 1` back to the first. */
        for (size_t j = s->syms.len; j-- > 0; ) {
            Sym *sym = *(Sym **)vecAt(&s->syms, j);
            if (strcmp(sym->name, name) == 0) return sym;
        }
    }
    /* Module-level bindings have depth 0 and are deliberately not kept in a scope: a
     * function body computes its depth from the number of open scopes, so a scope for
     * the globals would push every local one level deeper. */
    for (size_t i = 0; i < c->globals.len; i++) {
        Sym *sym = *(Sym **)vecAt(&c->globals, i);
        if (strcmp(sym->name, name) == 0) return sym;
    }
    return NULL;
}

/* ------------------------------------------------------------------- lookups */

/**
 * @brief 把改写过的函数名还原成用户书写的形式，用于诊断。
 *
 * 结果可能指向一个静态缓冲区，下一次调用会覆盖它。`name` 为 NULL 时返回 `"?"`；模块名为空
 * （根模块）或名字里没有 `$` 前缀时原样返回 `name`。
 *
 * `FN(f)` 是它的诊断别名：`FN(f)` 展开成 `checkFnDisplay(f->name, f->modName)`。
 *
 * @param name     改写后的名字，可为 NULL。
 * @param modName  模块前缀；根模块里为空或 NULL。
 * @return         形如 `mod::fn` 的显示名；缓冲区放不下时返回 `name`。
 */
const char *checkFnDisplay(const char *name, const char *modName) {
    if (!name) return "?";
    if (!modName || !*modName) return name;    /* root module: already the source name */
    const char *d = strchr(name, '$');
    if (!d) return name;                       /* no mangling prefix, as for `extern!` */
    static char buf[512];
    size_t nl = (size_t)(d - name);
    if (nl + 2 + strlen(d + 1) + 1 > sizeof buf) return name;
    memcpy(buf, name, nl);
    buf[nl] = ':'; buf[nl + 1] = ':';
    strcpy(buf + nl + 2, d + 1);
    return buf;
}

/**
 * @brief 按名字在模块里查一个模块级函数。
 *
 * 泛型函数的实例不是程序能写出的名字，因此不参与匹配，模板本身可以匹配。
 *
 * @param c     检查器。
 * @param name  已经取平的函数名。
 * @return      匹配到的模板或普通函数；没有时返回 NULL。
 */
FuncDef *findFunc(Checker *c, const char *name) {
    for (size_t i = 0; i < c->m->funcs.len; i++) {
        FuncDef *f = *(FuncDef **)vecAt(&c->m->funcs, i);
        if (planTemplate(f)) continue;       /* an instance is not a name; the template is */
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

/**
 * @brief 判断某个 import 是否已把一个名字带进当前模块的作用域。
 *
 * 通配条目覆盖该模块的所有公开名字，列名条目只覆盖它写出的那一个。比较按加载器记下的
 * 模块名对进行，所以一个文件里的 `use` 不会漏进另一个文件。线性扫描 `opens` 表。
 *
 * @param m         已合并的模块，它的 `opens` 表由加载器填好。
 * @param importer  做引用的模块；根文件为 NULL。
 * @param opened    拥有该名字的模块。
 * @param what      要查的名字。
 * @return          该模块用 `use opened::*` 或 `use opened::{what}` 带进了这个名字时返回
 *                  `true`；否则返回 `false`，`m` 为 NULL 时也是 `false`。
 */
bool nameBrought(Module *m, const char *importer, const char *opened, const char *what) {
    if (!m) return false;
    for (size_t i = 0; i < m->opens.len; i++) {
        Open *o = (Open *)vecAt(&m->opens, i);
        if (!o->opened || strcmp(o->opened, opened) != 0) continue;
        /* A wildcard entry (no name) covers every public name; a list entry covers the
         * one it names and nothing else. `use mod::{a}` must not make `b` legal, which
         * is the whole reason to list names instead of opening the module. */
        if (o->name && (!what || strcmp(o->name, what) != 0)) continue;
        if (o->importer == NULL && importer == NULL) return true;
        if (o->importer && importer && strcmp(o->importer, importer) == 0) return true;
    }
    return false;
}

/**
 * @brief 当使用了别的模块的名字而没有写模块限定时报错。
 *
 * 同一文件、本模块与自动导入的 prelude 豁免；`use mod::*` 或 `use mod::{name}` 已把名字
 * 带进当前模块作用域时也放行。`@private` 仍在声明处拦截，打开模块不是绕过它的办法。
 *
 * @param c          检查器。
 * @param what       被引用的名字。
 * @param whatMod    拥有这个名字的模块。
 * @param qualified  这次引用是否来自限定名；限定名在加载时被改写成扁平名，这个标志是
 *                   书写方式的唯一痕迹。
 * @param line       报错用的源码行。
 */
void requireQualified(Checker *c, const char *what, const char *whatMod, bool qualified, int line) {
    if (qualified) return;
    if (!whatMod) return;
    /* This has to be answered **before** the unconditional root-file rule below: the root
     * file has no module name of its own, so every bare reference from it would be rejected.
     * The pair carries the importer, so an `open` in one file never leaks into another. */
    if (c->curFunc && nameBrought(c->m, c->curFunc->modName, whatMod, what)) return;
    if (!c->curFunc || !c->curFunc->modName) {
        if (c->curFunc) {                        /* root file: qualified by default */
            ckError(c, line++, "Write `mod::name` (and `use mod` at the top of the file), or"
                               " put the module's names in scope with `use mod::*`."
                               " A module's `@private` names are not reachable here at all.",
                    "`%s` belongs to module `%s` -- write `%s::%s`", what, whatMod, whatMod, what);
        }
        return;
    }
    if (strcmp(c->curFunc->modName, whatMod) == 0) return;   /* the own module */
    ckError(c, line, "Write `mod::name` (and `use mod` at the top of the file), or put the"
                     " module's names in scope with `use mod::*`."
                     " A module's `@private` names are not reachable here at all.",
            "`%s` belongs to module `%s` -- write `%s::%s`", what, whatMod, whatMod, what);
}

/**
 * @brief 按名字在结构体定义里查一个字段。
 *
 * @param sd    要查的结构体定义。
 * @param name  字段名。
 * @return      命中的字段定义；没有该名字时返回 NULL。
 */
FieldDef *findField(StructDef *sd, const char *name) {
    for (size_t i = 0; i < sd->fields.len; i++) {
        FieldDef *fd = *(FieldDef **)vecAt(&sd->fields, i);
        if (strcmp(fd->name, name) == 0) return fd;
    }
    return NULL;
}







/**
 * @brief 按名字在枚举定义里查一个变体。
 *
 * @param td    要查的枚举定义。
 * @param name  变体名。
 * @return      命中的变体；td 为空或没有该名字时返回 NULL。
 */
Variant *findVariant(TypeDef *td, const char *name) {
    if (!td) return NULL;
    for (size_t i = 0; i < td->variants.len; i++) {
        Variant *v = *(Variant **)vecAt(&td->variants, i);
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

/**
 * @brief 取变体第 k 个载荷的类型。
 *
 * 枚举是泛型实例时，载荷类型里的类型参数会被替换成类型实参，例如 `maybe<i64>` 中 `just`
 * 的载荷是 `i64` 而不是 `T`。
 *
 * @param tt  类型表，用于做替换。
 * @param et  枚举实例类型；不是实例时载荷类型原样返回。
 * @param v   变体定义。
 * @param k   载荷下标，从 0 起。
 * @return    该位置载荷的类型。
 */
Type *payloadType(TypeTable *tt, Type *et, Variant *v, size_t k) {
    Type *pt = *(Type **)vecAt(&v->types, k);
    if (et && et->kind == TY_ENUM && et->edef &&
        et->edef->typeParams.len > 0 && et->targs.len == et->edef->typeParams.len)
        pt = ttSubstitute(tt, pt, &et->edef->typeParams, &et->targs);
    return pt;
}



static bool typeContainsProtoAt(TypeTable *tt, Type *t, const char *name, int depth);

/**
 * @brief 判断类型里任意深处是否含有指定的协议标记。
 *
 * 标记可以出现在类型本身、类型实参里，或 view/数组/引用之后；泛型自身与已实例化的实例
 * 两种拼法都算。为避免自引用类型让遍历无法终止，只沿这些位置下行，不展开结构体字段；
 * 递归深度超过上限时按保守答案返回 true。
 *
 * @param tt    类型表。
 * @param t     要检查的类型；NULL 视为不含该标记。
 * @param name  标记的名字，例如 `coroutine`。
 * @return      找到该标记时为 true。
 */
bool typeContainsProto(TypeTable *tt, Type *t, const char *name) {
    return typeContainsProtoAt(tt, t, name, 0);
}

/**
 * @brief 按给定深度递归查找协议标记，深度超过上限时保守地返回 true。
 *
 * @param tt     类型表。
 * @param t      要检查的类型；NULL 视为不含该标记。
 * @param name   标记的名字。
 * @param depth  当前递归深度。
 * @return       找到该标记时为 true；超过深度上限时也返回 true。
 */
static bool typeContainsProtoAt(TypeTable *tt, Type *t, const char *name, int depth) {
    if (depth > ZERO_VALUE_DEPTH_LIMIT) return true;   /* conservative */
    if (!t) return false;
    if (isProtoType(t, name, 1)) return true;
    /* Three spellings reach here: the generic itself (`TY_GENERIC` + sdef), a plain name that has not
     * been resolved into a struct yet, and the instance (whose `sdef` is the prelude's `coroutine`). */
    if (t->sdef && t->sdef->name && strcmp(t->sdef->name, name) == 0) return true;
    if (t->name && strcmp(t->name, name) == 0) return true;
    if (t->kind == TY_ARRAY || t->kind == TY_REF) return typeContainsProtoAt(tt, t->inner, name, depth + 1);
    for (size_t i = 0; i < t->targs.len; i++)
        if (typeContainsProtoAt(tt, *(Type **)vecAt(&t->targs, i), name, depth + 1)) return true;
    /* Struct fields are deliberately **not** walked: substituting a generic's own `T` hands
     * back a type that mentions itself, so the walk would not terminate. The shapes that
     * matter are covered above -- a concrete marker appears as the type itself, inside
     * `targs`, or behind a view/array/reference. A field *declared* with the marker is
     * caught where fields are resolved instead. */
    return false;
}

static bool typeContainsRefAt(TypeTable *tt, Type *t, int depth);

/**
 * @brief 判断类型能否携带引用。
 *
 * 直接是引用、数组元素、任一变体的载荷或结构体字段里含引用都算。递归有深度上限，超过时
 * 保守地答"是"。泛型实例先替换类型实参再走，否则 `T` 看起来无害而检查会漏过。
 *
 * @param tt  类型表，用于替换泛型实例的实参。
 * @param t   要检查的类型；NULL 不携带引用。
 * @return    true 表示类型的任何部分都能持有引用。
 */
bool typeContainsRef(TypeTable *tt, Type *t) {
    return typeContainsRefAt(tt, t, 0);
}

/**
 * @brief 按给定深度递归查找引用，深度超过上限时保守地返回 true。
 *
 * @param tt     类型表。
 * @param t      要检查的类型；NULL 不携带引用。
 * @param depth  当前递归深度。
 * @return       找到引用时为 true；超过深度上限时也返回 true。
 */
static bool typeContainsRefAt(TypeTable *tt, Type *t, int depth) {
    if (depth > ZERO_VALUE_DEPTH_LIMIT) return true;   /* conservative */
    if (!t) return false;
    if (t->kind == TY_REF) return true;
    if (t->kind == TY_ARRAY) return typeContainsRefAt(tt, t->inner, depth + 1);
    /* Any variant payload can carry a reference, so all of them are inspected. */
    if (t->kind == TY_ENUM && t->edef) {
        for (size_t v = 0; v < t->edef->variants.len; v++) {
            Variant *va = *(Variant **)vecAt(&t->edef->variants, v);
            for (size_t i = 0; i < va->types.len; i++)
                if (typeContainsRefAt(tt, payloadType(tt, t, va, i), depth + 1)) return true;
        }
        return false;
    }
    StructDef *sd = structOf(t);
    if (!sd) return false;
    Vec *sp = NULL, *sa = NULL;
    if (t->kind == TY_GENERIC && t->targs.len == sd->typeParams.len) {
        sp = &sd->typeParams;
        sa = &t->targs;
    }
    for (size_t i = 0; i < sd->fields.len; i++) {
        Type *ft = (*(FieldDef **)vecAt(&sd->fields, i))->type;
        if (typeContainsRefAt(tt, ttSubstitute(tt, ft, sp, sa), depth + 1)) return true;
    }
    return false;
}

static bool typeContainsFnAt(TypeTable *tt, Type *t, int depth);

/**
 * @brief 判断类型是否就是函数类型或内部含有函数类型。
 *
 * 与 `typeContainsRef` 是同一族的走法，存在只为一处诊断的措辞：`fn(i64) -> i64` 没有零值，
 * 但原因不是引用。递归同样有深度上限，超过时保守地答"是"。
 *
 * @param tt  类型表，用于替换泛型实例的实参。
 * @param t   要检查的类型；NULL 不含函数类型。
 * @return    true 表示类型本身是 TY_FN，或数组、任一变体载荷、结构体字段里有函数类型。
 */
bool typeContainsFn(TypeTable *tt, Type *t) {
    return typeContainsFnAt(tt, t, 0);
}

/**
 * @brief 按给定深度递归查找函数类型，深度超过上限时保守地返回 true。
 *
 * @param tt     类型表。
 * @param t      要检查的类型；NULL 不含函数类型。
 * @param depth  当前递归深度。
 * @return       找到函数类型时为 true；超过深度上限时也返回 true。
 */
static bool typeContainsFnAt(TypeTable *tt, Type *t, int depth) {
    if (depth > ZERO_VALUE_DEPTH_LIMIT) return true;   /* conservative */
    if (!t) return false;
    if (t->kind == TY_FN) return true;
    if (t->kind == TY_ARRAY) return typeContainsFnAt(tt, t->inner, depth + 1);
    if (t->kind == TY_ENUM && t->edef) {
        for (size_t v = 0; v < t->edef->variants.len; v++) {
            Variant *va = *(Variant **)vecAt(&t->edef->variants, v);
            for (size_t i = 0; i < va->types.len; i++)
                if (typeContainsFnAt(tt, payloadType(tt, t, va, i), depth + 1)) return true;
        }
        return false;
    }
    StructDef *sd = structOf(t);
    if (!sd) return false;
    Vec *sp = NULL, *sa = NULL;
    if (t->kind == TY_GENERIC && t->targs.len == sd->typeParams.len) {
        sp = &sd->typeParams;
        sa = &t->targs;
    }
    for (size_t i = 0; i < sd->fields.len; i++) {
        Type *ft = (*(FieldDef **)vecAt(&sd->fields, i))->type;
        if (typeContainsFnAt(tt, ttSubstitute(tt, ft, sp, sa), depth + 1)) return true;
    }
    return false;
}

static bool typeLacksZeroValueAt(TypeTable *tt, Type *t, int depth);

/**
 * @brief 判断类型是否没有零值。
 *
 * 非空引用与函数类型都没有零值（后者清零会得到空函数指针），含它们的聚合也没有。带载荷
 * 枚举只看第一个变体，因为它的零值是 tag 0 加载荷清零，所以变体顺序有意义：
 * `| nothing | holding(slice<u8>)` 能清零，`| holding(slice<u8>) | nothing` 不能。递归有
 * 深度上限，超过时保守地答"是"。
 *
 * @param tt  类型表，用于替换泛型实例的实参。
 * @param t   要检查的类型。
 * @return    true 表示把该类型清零会产生空引用或空函数指针。
 */
bool typeLacksZeroValue(TypeTable *tt, Type *t) {
    return typeLacksZeroValueAt(tt, t, 0);
}

/**
 * @brief 按给定深度递归判断零值，深度超过上限时保守地返回 true。
 *
 * @param tt     类型表。
 * @param t      要检查的类型。
 * @param depth  当前递归深度。
 * @return       该类型没有零值时为 true；超过深度上限时也返回 true。
 */
static bool typeLacksZeroValueAt(TypeTable *tt, Type *t, int depth) {
    if (depth > ZERO_VALUE_DEPTH_LIMIT) return true;   /* conservative */
    if (!t) return false;
    /* The zero value of `?ref T` is null, which is the reason it exists: the `next` of a
     * list or a tree node finally has one. */
    if (t->kind == TY_REF) return !t->nullable;
    /* A function type has none either: the zero value of a code pointer is a **null function
     * pointer**, so `var f: fn(i64) -> i64` with no initializer would jump to address zero when
     * called. `option<fn …>` is how "may be absent" is written. */
    if (t->kind == TY_FN) return true;
    if (t->kind == TY_ARRAY) return typeLacksZeroValueAt(tt, t->inner, depth + 1);
    if (t->kind == TY_ENUM && t->edef) {
        if (t->edef->variants.len == 0) return false;
        Variant *v0 = *(Variant **)vecAt(&t->edef->variants, 0);
        for (size_t i = 0; i < v0->types.len; i++)
            if (typeLacksZeroValueAt(tt, payloadType(tt, t, v0, i), depth + 1)) return true;
        return false;
    }
    StructDef *sd = structOf(t);
    if (!sd) return false;
    Vec *sp = NULL, *sa = NULL;
    if (t->kind == TY_GENERIC && t->targs.len == sd->typeParams.len) {
        sp = &sd->typeParams;
        sa = &t->targs;
    }
    for (size_t i = 0; i < sd->fields.len; i++) {
        Type *ft = (*(FieldDef **)vecAt(&sd->fields, i))->type;
        if (typeLacksZeroValueAt(tt, ttSubstitute(tt, ft, sp, sa), depth + 1)) return true;
    }
    return false;
}

/**
 * @brief 判断重复求值这个表达式是否观察不到区别。
 *
 * `??` 生成的条件表达式里主体出现两次，有副作用的主体就会跑两遍。判据是保守的语法判断：
 * 含有调用、`?` 或 `??` 的表达式一律拒绝，而不是去推断纯度。
 *
 * @param e  要检查的表达式。
 * @return   表达式只由字面量、绑定、字段、下标、切片与一元、二元运算组成时返回 true。
 */
bool repeatablePure(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_STR: case EX_NULL:
    case EX_IDENT:
    case EX_ENUMVAL:
        return true;
    case EX_FIELD: return repeatablePure(e->u.field.obj);
    case EX_SIGN:  return repeatablePure(e->u.sign.operand);
    case EX_DEREF: return repeatablePure(e->u.deref.operand);
    case EX_INDEX: return repeatablePure(e->u.index.obj) &&
                          repeatablePure(e->u.index.index);
    case EX_UN:    return repeatablePure(e->u.un.operand);
    case EX_BIN:   return repeatablePure(e->u.bin.left) &&
                          repeatablePure(e->u.bin.right);
    default:       return false;   /* calls, methods, `?`, `??`, constructions: all refused */
    }
}

/**
 * @brief 判断能否对这个表达式取地址。
 *
 * @param e  表达式。
 * @return    表达式是绑定、字段或 `*p`（即 `p` 指向的地方）时返回 true。
 */
bool isLvalue(Expr *e) {
    return e->kind == EX_IDENT || e->kind == EX_FIELD || e->kind == EX_DEREF;
}

/**
 * @brief 用 prelude 里的声明构造 `slice<elem>` 类型。
 *
 * @param c     检查器，持有 prelude 里 slice 的定义。
 * @param elem  元素类型。
 * @return      构造出的视图类型；prelude 里找不到 slice 定义时返回错误类型。
 */
Type *sliceOf(Checker *c, Type *elem) {
    if (!c->sliceDef) return ttError(c->tt);
    Vec args;
    vecInit(&args, c->arena, sizeof(void *));
    *(Type **)vecPush(&args) = elem;
    return ttGeneric(c->tt, c->sliceDef, &args);
}

/**
 * @brief 造一个整数常量表达式节点。
 *
 * 类型固定为 `i32`；编译器用它补上用户省略的切片边界，好让 codegen 看到两个字面边界，
 * 从而知道该范围不需要运行时检查。
 *
 * @param c     检查器，节点从它的 arena 分配。
 * @param v     常量的值。
 * @param line  源行号。
 * @return      新建的 `EX_INT` 节点。
 */
Expr *intLit(Checker *c, long long v, int line) {
    Expr *e = exprNew(c->arena, EX_INT, line);
    e->u.ival = v;
    e->type = c->tI32;
    return e;
}

/**
 * @brief 把表达式读成整数字面量。
 *
 * 一元负号包裹的整数字面量也算，负的切片边界因此能在编译期被拒绝，而不是留到运行时。
 *
 * @param e    要读取的表达式。
 * @param out  读到时写出字面量的值。
 * @return     e 是整数字面量（可以带一元负号）时返回 true 并写出 out；否则返回 false。
 */
bool asIntLit(Expr *e, long long *out) {
    if (!e) return false;
    if (e->kind == EX_INT) { *out = e->u.ival; return true; }
    if (e->kind == EX_UN && e->u.un.op && strcmp(e->u.un.op, "-") == 0 &&
        e->u.un.operand && e->u.un.operand->kind == EX_INT) {
        *out = -e->u.un.operand->u.ival;
        return true;
    }
    return false;
}

/**
 * @brief 判断表达式是不是一个地方。
 *
 * 地方是绑定、字段、下标与切片组成的链。切定长数组要求操作数是地方，因为要取元素地址，
 * 否则会切到返回值的临时存储；切字符串字面量例外，它的字节是静态存储期的。
 *
 * @param e  表达式。
 * @return   链的根是绑定、每一步都是字段、下标或切片时返回 true，否则返回 false。
 */
bool isPlace(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EX_IDENT: return true;
    case EX_FIELD: return isPlace(e->u.field.obj);
    case EX_INDEX: return isPlace(e->u.index.obj);
    case EX_SLICE: return isPlace(e->u.slice.obj);
    default:       return false;
    }
}

/**
 * @brief 沿掩码允许的步进走到根的标识符节点。
 *
 * 只沿语法链走，不解析名字也不查作用域，返回的节点保证是 EX_IDENT。空掩码或含未知位
 * 在调试构建中被断言拒绝；走不到根时由调用者按"这不是一个地方"保守处理。
 *
 * @param e        起点表达式。
 * @param steps    允许的步进掩码，EXTC_ROOT_* 位的或。
 * @param maxHops  跳数上限；<= 0 表示不限跳数。
 * @return         根部的标识符节点；走不到根或超过跳数上限时返回 NULL。
 */
Expr *extcRootLeaf(Expr *e, unsigned steps, int maxHops) {
    /* 掩码本身是不变量：空掩码或未知位都是调用点的笔误，而"走不到根"会被上层当成"这不是
     * 一个地方"（保守），于是笔误**不会**报错、只会让分析悄悄变保守 —— 正是要断言的地方。*/
    EXTC_DBG_ASSERT(steps != 0 &&
                    (steps & ~(EXTC_ROOT_FIELD | EXTC_ROOT_INDEX | EXTC_ROOT_SLICE |
                               EXTC_ROOT_DEREF | EXTC_ROOT_SIGN)) == 0);
    for (int hops = 0; e; hops++) {
        if (maxHops > 0 && hops >= maxHops) return NULL;
        if (e->kind == EX_IDENT) return e;
        if (e->kind == EX_FIELD && (steps & EXTC_ROOT_FIELD)) { e = e->u.field.obj;     continue; }
        if (e->kind == EX_INDEX && (steps & EXTC_ROOT_INDEX)) { e = e->u.index.obj;     continue; }
        if (e->kind == EX_SLICE && (steps & EXTC_ROOT_SLICE)) { e = e->u.slice.obj;     continue; }
        if (e->kind == EX_DEREF && (steps & EXTC_ROOT_DEREF)) { e = e->u.deref.operand; continue; }
        if (e->kind == EX_SIGN  && (steps & EXTC_ROOT_SIGN))  { e = e->u.sign.operand;  continue; }
        return NULL;
    }
    return NULL;
}

/**
 * @brief 解析一个地方表达式根部的绑定。
 *
 * 沿字段、下标与切片走到根标识符，再用作用域查找解析成绑定；掩码为 FIELD|INDEX|SLICE 且
 * 不限跳数。解析名字需要作用域，因此要求至少有一个打开的作用域（调试构建中有断言）；
 * 所有作用域关闭后调用只能命中模块级绑定。
 *
 * @param c  检查器，提供作用域栈。
 * @param e  地方表达式。
 * @return   根标识符对应的绑定；走不到标识符或名字解析不到时返回 NULL。
 */
Sym *placeRoot(Checker *c, Expr *e) {
    EXTC_DBG_ASSERT_MSGF(c->scopes.len > 0,
                         "placeRoot with no open scope (phase=%s): needs a scope",
                        phaseName(c->phase));
    Expr *leaf = extcRootLeaf(e, EXTC_ROOT_FIELD | EXTC_ROOT_INDEX | EXTC_ROOT_SLICE, 0);
    if (!leaf) return NULL;
    Sym *sy = lookup(c, leaf->u.ident.name);
    /* 阶段契约：所有作用域关闭后，这里只能解析到模块级绑定。对全局这是对的、也是必需的；
     * 但如果调用方以为自己在解析一个**局部**，它就会静默拿到 `NULL`、或者更糟：拿到一个**同名全局**。
     * 所以这里留一个**计数哨兵**：POST 阶段的无作用域解析成功数变了，就说明有新的调用点依赖这条兜底。*/
    if (sy && c->scopes.len == 0)
        EXTC_DBG_NOTEF("placeRoot(`%s`) in phase=%s with no scope → module-level binding",
                       leaf->u.ident.name, phaseName(c->phase));
    return sy;
}


/**
 * @brief 判断能否通过这个表达式写入。
 *
 * 写权限有三个来源：绑定看 `var` 还是 `let`，参数看类型是 `mut ref` 还是 `ref`，字段与
 * 元素看根。`*p` 单独判断，权限来自引用类型本身；只读视图也挡写。
 *
 * @param c  检查器。
 * @param e  地方表达式；NULL 不可写。
 * @return   允许写入时返回 true；路径跨过只读引用、视图只读或根是 `let` 时返回 false。
 */
bool isWritablePlace(Checker *c, Expr *e) {
    if (!e) return false;
    /* `*p` is writable exactly when the reference itself is a `mut ref`: the permission
     * comes from the type of the reference. */
    if (e->kind == EX_DEREF) {
        Type *ot = e->u.deref.operand->type;
        return ot && ot->kind == TY_REF && ot->mut;
    }
    /* A read-only reference on the way blocks the write. */
    if (pathHasReadonlyRef(e)) return false;
    /* The expression is itself a reference, so only `mut ref` is writable. */
    if (e->type && e->type->kind == TY_REF) return e->type->mut;
    /* A view needs a writable type of its own, `mut slice<T>`. A string literal takes
     * this path too: its type is a read-only view, so it is not writable. */
    if (e->type && e->type->kind == TY_GENERIC && ttIsViewType(e->type) && !e->type->mut)
        return false;
    /* A binding, a field or an element: the root has to be `var`. */
    Sym *root = placeRoot(c, e);
    return root && root->mut;
}

/* Before a write into a place, its root has to be `var`; `requireMutable` performs that
 * check and returns true once it has reported the error, so the caller gives up. */
