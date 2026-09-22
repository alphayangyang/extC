/* 语句检查
 *
 * 从 check.c 拆出来的 —— **纯移动**：注释与逻辑一个字节没动 ✓
 */

#include "check_internal.h"

/* ---------------------------------------------------------------- 语句 */

/* **裸写构造器**：`return success(v)` / `failure(e)` / `some(v)` / `none()`。
 *
 * 类型从**函数签名里已经写好的返回类型**来 —— 这不是"推导"，是**没让你重复一遍**：
 * 编译器没有猜任何东西，它读的是你自己写的那一行 `-> result<i64, gameError>`。
 *
 * 为什么值得：`result<i64, gameError>::failure(...)` 那串类型长到把重点盖住了，
 * 而重点从来是「失败」本身。`?` 负责收，`failure` 负责发，两头都不该啰嗦 ✓
 *
 * 落点是**原地改写成 EX_ASSOC** —— 后面检查/生成的路一条都不变（零新机制）。 */
void desugarBareCtor(Checker *c, Expr *e, Type *want) {
    if (!e) return;

    const char *nm = NULL;
    const char *proto = NULL;
    size_t nargs = 0;
    bool noParens = false;

    if (e->kind == EX_CALL && e->u.call.callee->kind == EX_IDENT) {
        nm = e->u.call.callee->u.ident.name;
    } else if (e->kind == EX_IDENT) {
        /* 没载荷的构造器连括号都能省 —— `return none`（跟枚举变体一个待遇）*/
        nm = e->u.ident.name;
        noParens = true;
    } else return;

    if      (strcmp(nm, "success") == 0 || strcmp(nm, "failure") == 0) { proto = "result"; nargs = 2; }
    else if (strcmp(nm, "some")    == 0 || strcmp(nm, "none")    == 0) { proto = "option"; nargs = 1; }
    else return;
    if (noParens && strcmp(nm, "none") != 0) return;   /* 只有 `none` 是零参数的 */

    /* 用户自己写的同名函数优先 —— 裸写只是**在没歧义时**的省事 */
    if (findFunc(c, nm)) return;
    /* 返回类型不是对应容器 ⇒ 不认，照原样走（报「未定义的名字」，那是实话）*/
    if (!isProtoType(want, proto, nargs)) return;

    /* ⚠️ union：先把 args 拿**出来**再改 kind，否则会被自己的新字段覆盖 */
    Vec args;
    if (noParens) vecInit(&args, c->arena, sizeof(void *));
    else          args = e->u.call.args;
    /* `option` / `result` 现在是**普通枚举** ⇒ 裸构造器就是一个变体构造 ✓
     * （`typeName` 要写**实例名**：codegen 拿它拼 C 的 tag 常量 `option_i64_some`）*/
    e->kind = EX_ENUMVAL;
    e->u.enumval.typeName = want->name;
    e->u.enumval.variant  = nm;
    e->u.enumval.args     = args;
    e->assocOwner = want;
}

void checkStmt(Checker *c, Stmt *s);

static void checkBlockBody(Checker *c, Stmt *block) {
    pushScope(c);
    for (size_t i = 0; i < block->u.block.stmts.len; i++)
        checkStmt(c, *(Stmt **)vecAt(&block->u.block.stmts, i));
    popScope(c);
}

void checkStmt(Checker *c, Stmt *s) {
    c->stmtFx = 0;      /* ⭐ PLAN #22：每语句重新数"前面有没有副作用" ✓ */
    switch (s->kind) {
        case ST_VAR: {
            /* 局部变量声明的类型标注也要解析（parser 只造「类型名」） */
            if (s->u.var.ann)
                s->u.var.ann = ttResolve(c->tt, c->ctx, s->u.var.ann, s->line, c->curParams);
            if (ttIsError(s->u.var.ann)) s->u.var.ann = NULL;

            /* 没有初始化式 ⇒ 零初始化（定案 8）。parser 保证此时必有类型标注。 */
            if (!s->u.var.init) {
                /* 标注里提到 `T` ⇒ 现在看不出有没有零值，推到实例化再查 ✓ */
                if (s->u.var.ann && mentionsParam(s->u.var.ann))
                    recordZeroCheck(c, s->u.var.ann, s->line, s->u.var.name);
                else if (s->u.var.ann && typeLacksZeroValue(c->tt, s->u.var.ann)) {
                    ckError(c, s->line,
                            "`ref` is a non-nullable reference, so it has no zero value -- "
                            "and neither does any struct that contains one",
                            "cannot zero-initialize `%s`: it contains a reference",
                            s->u.var.name);
                }
                s->type = s->u.var.ann ? s->u.var.ann : ttError(c->tt);
                Sym *sym = declare(c, s->u.var.name, s->type, s->u.var.mut,
                                   !s->u.var.mut, s->line, c->scopes.len);
                /* 零初始化的含引用值：**里面只可能是 `null`**（非空引用没零值 ⇒ 早被拒了）
                 * ⇒ "里面的引用指哪" = 深度 0 ✓（不是槽位深度！否则
                 *   `var a: [2]?ref node  a[0] = ref *p  return a` 会被误报 ✗）*/
                if (s->type && typeContainsRef(c->tt, s->type)) sym->refDepth = 0;
                s->u.var.cname = sym->cname;
                return;
            }

            /* ⚠️ **`let`（将来叫 `const`）是只读绑定** ⇒ 类型里不许出现 `mut` ✗
             * （"又 let 又 mut"很诡异 —— 主人 2026-09-20 拍板）*/
            if (!s->u.var.mut && s->u.var.ann) {
                Type *at = s->u.var.ann;
                if (at && ((at->kind == TY_REF && at->mut) ||
                           (at->kind == TY_GENERIC && at->mut))) {
                    ckError(c, s->line,
                            "a read-only binding cannot carry a writable reference or view; "
                            "write `var` if you need to modify through it",
                            "`let %s` cannot be declared with a `mut` type", s->u.var.name);
                }
            }

            /* ⭐ 定案 65（`@overwrite`）：它修饰的是**分配** —— 三条检查 + 一个标记 ✓
             *   · 必须 `var`（每次执行都要重新绑定那块存储，而且通常还要往里写）✓
             *   · 右边**必须是 `new`**（值 / 局部 / 函数结果都没有"复用一块存储"这个意思 ✗）
             *   · 标记在**查它之前**打：层数要按"函数体"算，不是按语句所在块 ✓ */
            if (s->u.var.overwrite) {
                if (!s->u.var.mut) {
                    ckError(c, s->line,
                            "Reusing one piece of storage means the binding is re-pointed at it"
                            " on every execution, so it must be writable. Write `var`.",
                            "`@overwrite` needs `var`: it re-binds the name to the same storage"
                            " on every execution, and you will usually write into it too");
                    return;
                }
                if (!s->u.var.init || s->u.var.init->kind != EX_NEW) {
                    ckError(c, s->line,
                            "`@overwrite` is about an **allocation**: it says \"this piece of"
                            " storage is reused; the previous round's contents are gone\". A value,"
                            " a local, or a call result has no such piece of storage to reuse."
                            " Write `@overwrite var n = new T` (or drop the annotation).",
                            "`@overwrite` only applies to `new`: the initializer must be an"
                            " allocation");
                    return;
                }
                s->u.var.init->reuse = true;
            }

            /* ⭐ 2026-09-22（主人：「let new 真是难绷完了」）：
             * `let x = new T` ⇒ **这块存储永远是零** ✗ —— 可判定、零误报：
             *   · 分配出来只有一条引用（就是这个名字），而 `let` 给的是**只读**引用
             *   · 只读引用**既写不了、也提权不回来**（链式只读 ✓：`ref *n` / 传进 `mut ref`
             *     参数 / `.f = v` 全部被挡 ✓ 实测过）
             * ⇒ 没有任何路径能写它 ⇒ 全零 ✓
             * ⚠️ 只**警告**不拦：有一个合法小角落 —— 拿一块全零块喂给只读消费者
             *    （`let buf = new [64]u8  hash(buf[..])`）✓
             * ⚠️ 校准：`var w = new T  let r = w` **不警告**（对象有可写的源头 ✓
             *    —— `let` 的主场就是"把已有别名降级成只读视图"）✓ */
            if (!s->u.var.mut && s->u.var.init &&
                (s->u.var.init->kind == EX_NEW || s->u.var.init->kind == EX_GENCALL)) {
                ckWarn(c, s->line,
                       "`let` is about the **name**, not the object: it promises you will not"
                       " write through this name. A fresh allocation has no other name, so"
                       " nothing can ever write into it and the storage stays zero. Use `var`"
                       " to write into it -- or, for a read-only view, bind one of something"
                       " writable: `var w = new T  let r = w`.",
                       "this allocation can never be written: `let` bound a fresh allocation,"
                       " and a read-only reference cannot be turned back into `mut ref`");
            }

            if (s->u.var.ann) adoptContextType(s->u.var.init, s->u.var.ann);

            /* `let x = e?` —— `?` 的合法位置之一。
             * 有类型标注时按**标注**决定要不要解引用（标的是引用 ⇒ 别解）；
             * 没标注时按值位置（形状 3）。 */
            Type *it;
            if (s->u.var.init->kind == EX_TRY) it = checkTryInner(c, s->u.var.init);
            else if (s->u.var.ann)             it = checkInto(c, s->u.var.ann, s->u.var.init);
            /* **无标注 ⇒ 自然类型**（引用就还是引用）✓
             * 想要值的拷贝就写 `let v = *p` —— 显式 ✓
             * （否则 `let r = pickFirst(ref a, ref b)` 这种"绑定一个引用"写不出来 ✗）*/
            else                               it = checkExpr(c, s->u.var.init);

            /* #24：**查完之后**再定（`checkExpr` 里会把 homeDepth 设成"按实参选"）
             * ⇒ 这一句按"我把它存到多深"覆盖掉 ✓ */
            markCallHomeIfEscaping(c, s->u.var.init, (int)c->scopes.len);

            /* ⭐ **`let` 推断出来的东西自动降级成只读**（主人 2026-09-20）：
             * `let p = ref x` ⇒ `p: ref T`（不是 `mut ref T`）✓
             * `let s = a[..]` ⇒ `s: slice<T>` ✓（跟视图那边原本的行为**统一**了 ✓）
             * ⇒ 于是"看见 let ⇒ 整条链只读"成立 ✓
             * ⇒ 权限**写在类型里** ⇒ 拷到哪儿都跟着（`var q = p` 也洗不掉 ✓✓）*/
            if (!s->u.var.mut && !s->u.var.ann) {
                if (it && it->kind == TY_REF && it->mut) {
                    Type *ro = ttRef(c->tt, it->inner);
                    it = ro;
                } else if (it && it->kind == TY_GENERIC && it->mut && ttIsViewType(it)) {
                    it = ttViewReadonly(c->tt, it);
                }
            }
            Type *declT = s->u.var.ann ? s->u.var.ann : it;

            if (s->u.var.ann)
                checkAssignable(c, s->u.var.ann, it, s->u.var.init, "initializer");

            /* 逃逸②：初始化的引用不能指向比自己更深的局部 */
            checkEscape(c, s->u.var.init, c->scopes.len, s->line, "this initializer");
            s->type = ttIsError(declT) ? ttError(c->tt) : declT;
            Sym *sym = declare(c, s->u.var.name, s->type, s->u.var.mut,
                               !s->u.var.mut, s->line, c->scopes.len);
            s->u.var.cname = sym->cname;
            /* ⭐ 定案 63（PLAN #38）：记下这个绑定的**来路**（初始值表达式）——
             * 提升要顺着它往回走：`var n = new node` 之后别处 `head = n`
             * ⇒ 顺着 n 找回那个 `new`，把它提到 head 那一层 ✓（没初始化式 ⇒ NULL）*/
            noteOrigin(c, sym, s->u.var.init);
            /* 引用型绑定：它指的东西有多深，**从初始值数出来** ✓
             * （`var cur: ?ref node = head` ⇒ head 是参数 ⇒ 0 ⇒ 这个游标可以返回 ✓）*/
            /* ⚠️⚠️ **`let t = make()` 这种"初始化式是调用"的情形必须单独记** ✗
             * 为什么：调用结果里那些引用住哪，唯一知道的人是**调用点**（它选了
             * arenaArg）；而下面那张**字段表**只在初始化式是 `EX_STRUCTLIT` 时才填 ✗
             * ⇒ 于是 `t.p` 没有任何格子 ⇒ `exprRefDepth(t)` 从 `sym->refDepth`
             * 兜底得到 **0**（"里面没有指向深处的引用"）⇒ 之后 `b = t`（b 更浅）
             * 被判成安全 ⇒ 块一退就悬垂 ✗✗
             * （真踩过：ASan `heap-use-after-free`，而单跑一次还不崩 ——
             *   只有"在块里造、往块外存"才现形 ✓）
             * 判据：被调者**有家** ⇒ 它分配进"我传的那只 arena"，而那只就是
             * `markCallHomeIfEscaping` 上面刚按 `at` 定下来的 ⇒ 结果的深度 = `at` ✓
             * （`at` 取的是 `c->scopes.len` = 绑定所在那一层 ✓）*/
            if (s->type && typeContainsRef(c->tt, s->type)) {
                int d = exprRefDepth(c, s->u.var.init);
                Expr *ini = s->u.var.init;
                if ((ini->kind == EX_CALL || ini->kind == EX_METHOD)
                    && ini->func && ini->func->needsHome && (int)c->scopes.len > d)
                    d = (int)c->scopes.len;      /* 有家被调者 ⇒ 住在我传的那只 arena ✓ */
                sym->refDepth = d;
            }
            /* ⭐ 档2.3：结构体字面量 ⇒ 把**每个字段**的深度记进字段表 ✓
             * （没写出来的字段 = 零初始化 ⇒ 里面只可能是 null ⇒ 深度 0；
             *   我们不给它建格，读它时退回保守算法 —— 代价是可能误拒，不是洞 ✓）*/
            if (s->u.var.init && s->u.var.init->kind == EX_STRUCTLIT && sym) {
                for (size_t fi = 0; fi < s->u.var.init->u.lit.inits.len; fi++) {
                    FieldInit *fip = *(FieldInit **)vecAt(&s->u.var.init->u.lit.inits, fi);
                    noteFieldDepthWrite(c, sym, fip->name, exprRefDepth(c, fip->value));
                }
            }
            return;
        }

        case ST_ASSIGN: {
            Type *tt_ = checkExpr(c, s->u.assign.target);

            /* 目标是**已经收窄过**的绑定 ⇒ 这里比的是槽位的**声明类型**。
             * 收窄只影响"读出来的是什么"，不影响"这个槽位能装什么" ✓
             * （所以 `while cur != null { cur = cur.next }` 合法 —— 循环体里
             *   收窄被这次赋值作废，下一轮循环条件重新证明 ✓）*/
            if (s->u.assign.target->kind == EX_IDENT) {
                Sym *slot = lookup(c, s->u.assign.target->u.ident.name);
                if (slot && slot->type && slot->type->kind == TY_REF && slot->type->nullable &&
                    tt_->kind == TY_REF && !tt_->nullable) tt_ = slot->type;
            }

            /* **目标是引用 ⇒ 写进去**（形状 3：`p = v` 写 p 指向的那个地方）。
             *
             * 「换指向」已经取消（见 DECISIONS 引用语义定案），所以给引用赋值
             * 必须对得上**被指的类型**；想改指向哪儿，只能重新声明一个绑定。 */
            if (tt_->kind == TY_REF) {
                /* ⚠️ 这里**不能**用 `requireMutable` —— 它看的是"引用类型是不是 mut" ✗
                 * 而"换指向"写的是**那个槽位**（变量/字段本身）⇒
                 * 只要求**绑定/字段可写**（`var`）✓
                 * ⇒ 于是 `var p: ref T = ref x;  p = ref y` 合法 ✓（主人 2026-09-20）*/
                /* 换指向写的是**槽位本身** ⇒ 只要求"这个槽位可写"：
                 *   · 变量 / 字段 / 元素 ⇒ 根是 `var` ✓
                 *   · **`*p`（`mut ref`）⇒ 可写性由 p 的类型给** ✓
                 *     ⚠️ 这里以前只走 `placeRoot`，而它对 `*p` 返回 NULL ⇒
                 *        `fn push(head: mut ref ?ref node) { *head = cell }` 被误报成
                 *        "the binding is read-only" ✗（真 bug，2026-09-20 修）*/
                /* ⭐ PLAN #40：**先把路走通，再看落地那个槽位**
                 *   · 路上穿过的引用都得是 `mut ref`（`*p` / `cur.f` / `(*p).f`）✓
                 *   · 穿过了引用 ⇒ 存储在被指对象里（没有本帧的根）⇒ 到此为止 ✓
                 *   · 没穿过 ⇒ 存储是本帧某个绑定的槽位 ⇒ 那只绑定得可写（`var`/参数）✓
                 * ⚠️ 老写法只认"目标自己是 `*p`" + `placeRoot` ⇒ `(*cell).next = v`
                 *    既被误拒（`placeRoot` 返回 NULL）又漏掉写穿只读引用 ✗ 见 #40/#41 ✓ */
                bool crossed  = false;
                bool refsOk   = pathRefsAllMut(s->u.assign.target, &crossed);
                bool slotOk   = refsOk;
                if (slotOk && !crossed) {
                    Sym *slotRoot = placeRoot(c, s->u.assign.target);
                    slotOk = slotRoot && slotRoot->mut;
                }
                if (!slotOk) {
                    /* 两种原因**报两种话**（不然用户会以为是自己的绑定写错了 ✗）✓ */
                    if (!refsOk)
                        ckError(c, s->line,
                                "`ref T` is a **read-only** borrow; writing the object it "
                                "points to needs `mut ref T` in the declaration. Read-only "
                                "is the default so that a signature says what it does.",
                                "cannot write through a read-only reference");
                    else
                        ckError(c, s->line,
                                "rebinding a reference writes the binding itself, so the binding"
                                " must be writable (`var`, or a `mut ref`)",
                                "cannot rebind `%s`: the binding is read-only",
                                s->u.assign.target->kind == EX_DEREF
                                  ? typeStr(c, s->u.assign.target->u.deref.operand->type)
                                  : "this");
                    return;
                }

                Expr *v = s->u.assign.value;

                /* **看右边是什么，就知道是哪件事 —— 歧义靠类型消掉，不靠禁用。**
                 *   右边是**值** `T`      ⇒ 写进它指向的地方   `*p = v`
                 *   右边是**引用** `ref T` ⇒ 换指向             `p = q`
                 *
                 * 这两件事类型不同、而且右边就在源码里看得见（P′）——
                 * 所以不需要像之前那样把换指向整个禁掉。 */
                /* `null` 要的上下文是**引用本身**（`?ref T`），不是它指的东西 ✓
                 * （`p.next = null` 会走到这条"换指向"的路上 —— 右边是引用）*/
                adoptContextType(v, v->kind == EX_NULL ? tt_ : tt_->inner);
                Type *vt0 = checkExpr(c, v);          /* 自然类型：引用保留 */

                /* ⚠️ 作废非空证明要**等右边查完**：`cur = cur.next` 里右边的 `cur`
                 * 用的还是循环条件证明过的那份非空信息 ✓（踩过：放在前面会误报）*/
                if (s->u.assign.target->kind == EX_IDENT)
                    unNarrow(c, s->u.assign.target->u.ident.cname);

                if (vt0->kind == TY_REF) {
                    /* 换指向：类型要对得上（`mut ref` → `ref` 降级照旧允许），
                     * 而且新指向的东西不能活得比这个引用短。
                     * ⚠️ 深度比较用的是**旧的** refDepth（这个槽位原来指多深）——
                     * 所以更新被指深度必须等这些都查完，否则等于拿新值跟新值比，
                     * `p = ref <更深的局部>` 会整条漏过去 ✗（真踩过，攻击测试 h1 打出来）*/
                    /* ⚠️ `storeLayer`：这里问的是「往哪一层存」——
                     * 引用型绑定的坑见 `storeLayer` 的注释（PLAN #38 的根因）✓ */
                    int at0 = storeLayer(c, s->u.assign.target);
                    /* ⭐ 定案 63（PLAN #38）：先试着**提升** —— 这个值里的 `new`
                     * 能不能住到 at0 这一层？（提不动 ⇒ 下面那句照旧报错）✓
                     * ⚠️ 必须在 `checkEscape` **之前**：它读的是值里那份深度 ✓ */
                    promoteInto(c, v, at0);
                    checkAssignable(c, tt_, vt0, v, "assignment");
                    checkEscape(c, v, at0, s->line, "this reference");
                    /* 换指向 ⇒ 被指对象的深度跟着换 ✓（`cur = cur.next`）*/
                    if (s->u.assign.target->kind == EX_IDENT) {
                        Sym *slot0 = lookup(c, s->u.assign.target->u.ident.name);
                        if (slot0 && slot0->type && slot0->type->kind == TY_REF)
                            slot0->refDepth = exprRefDepth(c, v);
                        /* ⭐ 定案 63：**来路也跟着换**（`mid = n` ⇒ mid 的来路就是 n）——
                         * 提升要顺着来路往回走，中间经手的变量也得追得到 ✓
                         * ⚠️ 只在这一支（引用型目标的强更新）里记；槽位取过地址
                         *    ⇒ 别名可能改它 ⇒ 不记（保守：退回老行为报错）✓
                         * ⚠️ 环（`a = b  b = a`）由 `promoteInto` 的步数上限兜住 ✓ */
                        if (slot0) noteOrigin(c, slot0, v);
                    }
                    /* ⚠️ **元素/字段**是引用型时的换指向（`a[0] = ref local`）也要记！
                     * 不记的话：`var a: [2]?ref node`（零初始化 ⇒ 里面全 null ⇒ 深度 0）
                     * 之后 `a[0] = ref local`（深度 1）⇒ 上界还是 0 ⇒ `return a` 被放行 ✗✗
                     * （canary 当场抓出来：`canary_array_nullref_local` 从"挡住"变成"编过"）
                     * 取 max：refDepth 是"里面那些引用指哪"的**上界** ✓ */
                    {
                        Sym *rootA = placeRoot(c, s->u.assign.target);
                        if (rootA && rootA->type && typeContainsRef(c->tt, rootA->type)) {
                            int dA = exprRefDepth(c, v);
                            /* ⭐ 档2.3：按**字段**记；没取过地址就强更新（覆盖）⇒ `h.p = null`
                             * 之后根的有效深度真的会降下来 ✓（以前一律取 max ⇒ 误拒 ✗）*/
                            const char *fn_ = (s->u.assign.target->kind == EX_FIELD)
                                              ? s->u.assign.target->u.field.name : NULL;
                            noteFieldDepthWrite(c, rootA, fn_, dA);
                        }
                    }
                    /* ⚠️ **换指向也要查"借来的值"**（2026-09-20 攻击测试打出来）：
                     *     struct slot { r: mut ref i32 }
                     *     fn stash(b: mut ref slot, p: mut ref i32) { b.r = ref *p }
                     * 权限够、深度也够（p 是参数 = 0），只有"它其实是调用者的东西"这条能拦 ✗
                     * ⇒ 少了这一句就是一个**真悬垂**（实测打印出过期数据）✓ */
                    checkStoreEscape(c, v, s->u.assign.target, s->line);
                    return;
                }

                /* ⚠️ **引用上不再有隐式写穿**（2026-09-20 加了 `*p` 之后）：
                 * `p = v` 只有"换指向"一个含义；想写穿就写 `*p = v` ✓
                 * ⇒ **每个写法只有一个含义，不用看右边分辨**（显式优于推导）✓ */
                ckError(c, s->line,
                        "write through the reference instead: `*p = v` -- `=` on a reference"
                        " only ever retargets it",
                        "cannot assign a value to `%s`: it is a reference, not a place",
                        typeStr(c, tt_));
                return;
            }

            desugarBareCtor(c, s->u.assign.value, tt_);
            adoptContextType(s->u.assign.value, tt_);
            Type *vt = checkMaybeTry(c, s->u.assign.value);

            /* ⭐ 定案 63（PLAN #38）：**先把目的地的深度回填给值里的 `new`**。
             * 顺序要紧：下面紧跟着那两处会拿 `exprRefDepth(值)` 记进目标的深度表，
             * 晚提一步就会把**老那个数**记下来 ⇒ 之后全是误拒 ✗（真踩过）✓ */
            int atDst = storeLayer(c, s->u.assign.target);
            promoteInto(c, s->u.assign.value, atDst);
            markCallHomeIfEscaping(c, s->u.assign.value, atDst);
            /* 含引用的值绑定被写（整块赋值 或 写它的字段/元素）⇒
             * "里面那些引用指哪"要跟着**放宽**（取 max：refDepth 是上界 ✓）*/
            {
                Sym *vs = placeRoot(c, s->u.assign.target);
                if (vs && vs->type && typeContainsRef(c->tt, vs->type)) {
                    int d2 = exprRefDepth(c, s->u.assign.value);
                    /* ⭐ 档2（ARENA-FORMAL §7.4）：路径目标（`h.p = …`）——
                     * **没取过地址就强更新**（直接覆盖）✓
                     * 取过地址 ⇒ 只能弱更新（别名可能指别处，覆盖会漏掉旧深度 ✗）✓ */
                    if (s->u.assign.target->kind == EX_IDENT) vs->refDepth = d2;
                    else if (d2 > vs->refDepth) vs->refDepth = d2;
                }
            }
            if (requireMutable(c, s->u.assign.target, s->line, "write")) return;
            /* 逃逸③：给字段/元素赋值时，被指对象不能比目标更深 */
            checkStoreEscape(c, s->u.assign.value, s->u.assign.target, s->line);
            if (0) checkEscape(c, s->u.assign.value, placeDepth(c, s->u.assign.target),
                        s->line, "this assignment");
            if (ttIsError(tt_)) return;
            checkAssignable(c, tt_, vt, s->u.assign.value, "assignment");
            return;
        }

        case ST_IF: {
            expectBool(c, checkValue(c, s->u.ifs.cond), s->u.ifs.cond);

            /* **`?ref T` 的收窄**：`if p != null` / `if p == null ... else` /
             * 护栏形态 `if p == null { return }` —— 三种写法都能让编译器知道
             * "这里是那个分支，p 一定不是 null"，于是里面**一行运行时检查都不用加** ✓ */
            bool whenTrue = false;
            const char *tg = narrowTarget(c, s->u.ifs.cond, &whenTrue);
            const size_t mark = c->narrow.len;

            if (tg && whenTrue) narrowFactsOf(c, s->u.ifs.cond);
            checkBlockBody(c, s->u.ifs.thenBody);
            c->narrow.len = mark;              /* 出了 then 分支，证明就不算数了 ✓ */

            if (s->u.ifs.elseBody) {
                if (tg && !whenTrue) pushNarrow(c, tg);
                if (s->u.ifs.elseBody->kind == ST_BLOCK) checkBlockBody(c, s->u.ifs.elseBody);
                else                                      checkStmt(c, s->u.ifs.elseBody);
                c->narrow.len = mark;
            } else if (tg && !whenTrue && blockExits(s->u.ifs.thenBody)) {
                /* 护栏：条件成立就离开函数/循环 ⇒ 走到后面 ⟺ p 非空。
                 * 证明**留在当前作用域**（到本层块结束为止）✓ */
                pushNarrow(c, tg);
            }
            return;
        }

        case ST_WHILE: {
            /* ⚠️ `while` 的条件**不许**提前求值（吐出来只算一次，不是每轮）*/
            c->noHoist++;
            expectBool(c, checkValue(c, s->u.whiles.cond), s->u.whiles.cond);
            c->noHoist--;

            /* `while cur != null { cur = cur.next }` —— 链表**整个语言的存在理由**，
             * 所以循环条件也是收窄点：循环体里那个绑定一定非空 ✓
             * （循环体里给它赋了新值 ⇒ unNarrow 会作废，所以下一轮要重新比 —— 
             *   这跟 C 里 `while (p) { p = p->next; }` 的写法完全一致 ✓）*/
            bool whenTrue = false;
            const char *tg = narrowTarget(c, s->u.whiles.cond, &whenTrue);
            const size_t mark = c->narrow.len;
            if (tg && whenTrue) narrowFactsOf(c, s->u.whiles.cond);
            checkBlockBody(c, s->u.whiles.body);
            c->narrow.len = mark;
            return;
        }

        case ST_EXPR:
            /* `f()?` 单独成句 —— `?` 的第四个合法位置（最有用的那个：
             * 「这一步必须成功，否则整串失败」）*/
            checkMaybeTry(c, s->u.expr.expr);
            return;

        case ST_RETURN: {
            Type *want = c->curFunc ? c->curFunc->ret : NULL;
            if (!s->u.ret.value) {
                if (want && !ttIs(want, "void")) {
                    ckError(c, s->line, NULL, "`%s` must return a value of type `%s`",
                            c->curFunc->name, typeStr(c, want));
                }
                return;
            }
            if (!want) {
                checkExpr(c, s->u.ret.value);
                ckError(c, s->line, NULL, "`%s` does not return a value",
                        c->curFunc ? c->curFunc->name : "this function");
                return;
            }
            /* 裸写构造器（`return failure(e)`）—— 类型从上面这个 `want` 来 ✓ */
            desugarBareCtor(c, s->u.ret.value, want);

            /* `return e?` —— 表达式本身的值是**载荷**，而函数要交出的是外层类型，
             * 所以这里比的是「载荷能不能放进外层的载荷」（装回去由 codegen 做）。
             * 不能走下面那条 adoptContextType + checkAssignable：那会拿
             * option<i64> 去要求一个 i64。 */
            if (s->u.ret.value->kind == EX_TRY) {
                Type *vt = checkTryInner(c, s->u.ret.value);
                Type *wb = ttBase(want);
                if (wb && wb->kind == TY_GENERIC && wb->targs.len >= 1)
                    checkAssignable(c, *(Type **)vecAt(&wb->targs, 0), vt,
                                    s->u.ret.value, "return value");
                return;
            }

            adoptContextType(s->u.ret.value, want);
            Type *vt = checkInto(c, want, s->u.ret.value);   /* 期望是引用就别解 */
            markCallHomeIfEscaping(c, s->u.ret.value, 0);    /* 要交出去 ⇒ 用家 ✓ */
            checkAssignable(c, want, vt, s->u.ret.value, "return value");
            /* 逃逸①：返回的引用，被指对象必须在参数或静态数据里（深度 0）*/
            checkEscape(c, s->u.ret.value, 0, s->line, "this return value");
            return;
        }

        case ST_BREAK:
        case ST_CONTINUE:
            return;

        case ST_MATCH: {
            /* `match e { 变体 => ... }`
             *
             * **这个特性的全部价值就在穷尽检查这一件事上** ——
             * 今天写 `if e == gameError.outOfRange { } else { }` 得手写 else，
             * 而且**以后加了新变体，编译器不会提醒你漏了**。match 会 ✓ */
            Type *st = checkValue(c, s->u.match.scrutinee);
            Type *sb = ttBase(st);
            if (ttIsError(st)) return;

            if (!sb || sb->kind != TY_ENUM || !sb->edef) {
                ckError(c, s->line,
                        "`match` works on enums (`type color = | red | green`); "
                        "everything else is compared with `==`",
                        "cannot `match` on a value of type `%s`", typeStr(c, st));
                return;
            }
            TypeDef *td = sb->edef;

            /* 每条分支：名字要是这个枚举的变体，而且不能重复 */
            for (size_t i = 0; i < s->u.match.arms.len; i++) {
                MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
                if (!findVariant(td, arm->variant)) {
                    Buf note;
                    bufInit(&note, c->arena);
                    bufPuts(&note, "variants of ");
                    bufPuts(&note, sb->name);
                    bufPuts(&note, ":");
                    for (size_t j = 0; j < td->variants.len; j++)
                        bufPrintf(&note, " %s", (*(Variant **)vecAt(&td->variants, j))->name);
                    ckError(c, arm->line, bufCstr(&note),
                            "`%s` is not a variant of `%s`", arm->variant, sb->name);
                    return;
                }
                for (size_t j = 0; j < i; j++) {
                    MatchArm *prev = *(MatchArm **)vecAt(&s->u.match.arms, j);
                    if (strcmp(prev->variant, arm->variant) == 0) {
                        ckError(c, arm->line, NULL,
                                "`%s` is matched twice", arm->variant);
                        return;
                    }
                }
            }

            /* **穷尽**：一个都不能漏 */
            for (size_t j = 0; j < td->variants.len; j++) {
                const char *vn = (*(Variant **)vecAt(&td->variants, j))->name;
                bool covered = false;
                for (size_t i = 0; i < s->u.match.arms.len && !covered; i++)
                    covered = strcmp((*(MatchArm **)vecAt(&s->u.match.arms, i))->variant, vn) == 0;
                if (covered) continue;

                Buf note;
                bufInit(&note, c->arena);
                bufPuts(&note, "add a `");
                bufPuts(&note, vn);
                bufPuts(&note, " => { ... }` arm");
                ckError(c, s->line, bufCstr(&note),
                        "`match` does not handle `%s` -- every variant must be listed",
                        vn);
                return;
            }

            /* 分支体各自开一层作用域；**绑定的载荷**（`circle(r) => ...`）
             * 就住在这里 —— 第 i 个名字拿第 i 个载荷字段 ✓ */
            for (size_t i = 0; i < s->u.match.arms.len; i++) {
                MatchArm *arm = *(MatchArm **)vecAt(&s->u.match.arms, i);
                Variant  *v   = findVariant(td, arm->variant);

                if (arm->binds.len != v->types.len) {
                    ckError(c, arm->line, NULL,
                            "`%s` carries %zu value(s), so this arm binds %zu name(s), not %zu",
                            v->name, v->types.len, v->types.len, arm->binds.len);
                    return;
                }
                pushScope(c);
                for (size_t k = 0; k < arm->binds.len; k++) {
                    const char *bn = *(const char **)vecAt(&arm->binds, k);
                    Type *bt = payloadType(c->tt, sb, v, k);
                    /* 绑定的载荷是**这个值的副本**（值语义），名字只读 ——
                     * 想改就自己 `var` 一份 */
                    Sym *bs = declare(c, bn, bt, false, false, arm->line, c->scopes.len);
                    /* ⚠️ 把**解析后的 C 名字**写回绑定表（跟 `Param.cname` / `Stmt.u.var.cname`
                     * 同一个套路）。不写回的话，同一层里两个 `match` 绑同名时，
                     * 声明用原名、而分支体里查到的却是 `s__2` ⇒ 生成的 C 编不过
                     * （`'s__2' undeclared`）✗ —— 这就是 PLAN §0.4 #2 ✓ */
                    *(const char **)vecAt(&arm->binds, k) = bs->cname;
                }
                for (size_t k = 0; k < arm->body->u.block.stmts.len; k++)
                    checkStmt(c, *(Stmt **)vecAt(&arm->body->u.block.stmts, k));
                popScope(c);
            }
            return;
        }

        case ST_BLOCK:            checkBlockBody(c, s);
            return;
    }
}

