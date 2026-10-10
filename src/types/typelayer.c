/**
 * @file typelayer.c
 * @brief 类型层实现（the type layer's implementations）：只回答关于类型与节点的**只读问题**，
 *        不持有检查器状态。
 *
 * 依赖保持单向：代码生成可以依赖“类型是什么”，绝不依赖检查器的工作状态。
 */
#include "typelayer.h"

/**
 * @brief 是不是六个比较运算符之一：`==` `!=` `<` `<=` `>` `>=`。
 *
 * @param op  运算符写法。
 * @return    是则返回 `true`。
 */
bool isCmpOp(const char *op) {
    return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
           strcmp(op, "<")  == 0 || strcmp(op, "<=") == 0 ||
           strcmp(op, ">")  == 0 || strcmp(op, ">=") == 0;
}

/**
 * @brief 是不是逻辑运算符之一：`&&` `||`。
 *
 * @param op  运算符写法。
 * @return    是则返回 `true`。
 */
bool isLogicOp(const char *op) {
    return strcmp(op, "&&") == 0 || strcmp(op, "||") == 0;
}

/**
 * @brief 是不是相等运算符之一：`==` `!=`。
 *
 * The pair shares everything that is special about it: one native rule, the `!=` -> `==`
 * fallback, and the diagnostics that talk about comparability rather than ordering. 判据只有这
 * 一处，上述每处都问它。
 *
 * @param op  运算符写法。
 * @return    是则返回 `true`。
 */
bool isEqualityOp(const char *op) {
    return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0;
}

/**
 * @brief 是不是可重载的运算符名（一个类型可以定义多次的名字）。
 *
 * The overloadable set is the language's operator set, which is exactly the set the parser
 * allows as a method name. 判据只此一处，解析器、重复定义检查与查找因此不会对“哪些名字是
 * 运算符”产生分歧。
 *
 * @param name  运算符名。
 * @return      是比较运算符、算术运算符或流运算符则返回 `true`。
 */
bool isOverloadableOp(const char *name) {
    return isCmpOp(name) || isArithOp(name) ||
           strcmp(name, "<<") == 0 || strcmp(name, ">>") == 0;
}

/**
 * @brief 这个类型里有没有出现类型参数（`T`），无论直接还是嵌套。
 *
 * 检查器对这样的类型推迟工作：它们还不能判定，要等到实例已知时再做。
 *
 * @param t  类型。
 * @return   提到类型参数则返回 `true`。
 */
bool mentionsParam(Type *t) {
    return t && (t->kind == TY_PARAM || ttHasParam(t));
}

/**
 * @brief `t` 是不是 prelude 里那两个有真实语义的容器之一，按名字与实参个数识别。
 *
 * 它们是保留定义，用户不能重定义，所以按名字识别是安全的。`?` 需要它们的标签字段名，
 * 这与 `data` 加 `len` 的视图协议是同一分工：语言知道协议，库提供结构。
 *
 * @param t      类型。
 * @param name   保留的容器名（`option` 或 `result`）。
 * @param nargs  它必须携带的类型实参个数。
 * @return       `t` 正是该容器且实参个数正好为 `nargs` 时返回 `true`。
 */
bool isProtoType(Type *t, const char *name, size_t nargs) {
    if (!t || t->targs.len != nargs) return false;
    /* 泛型结构体（`slice<T>`、`varArray<T>`）带 `sdef`；`option` / `result` 是枚举，
     * 其实例是携带具体实参的 `TY_ENUM`，带 `edef`。 */
    if (t->kind == TY_GENERIC && t->sdef) return strcmp(t->sdef->name, name) == 0;
    if (t->kind == TY_ENUM && t->edef)    return strcmp(t->edef->name, name) == 0;
    return false;
}

/**
 * @brief `t` 能不能对 `rhs` 原生地或经由方法回答 `op`。
 *
 * 这是推迟到实例上的逐实例规则，且刻意与具体类型路径用同一个谓词：同一条规则写两份会漂移，
 * 漂移表现为泛型接受了它自己的实例所拒绝的东西。Each family answers "native" differently,
 * exactly as the concrete path does:
 *   - `==` / `!=`：数字、`bool` 与无载荷枚举原生支持；
 *   - 序运算符：只有数字原生支持；
 *   - 算术运算符：数字原生支持，`%` 只有整数。
 * A user method is accepted for any of them, and an array's `==` is derived by the compiler
 * as long as its elements have one.
 *
 * 用户方法的签名已在定义处校验（checkOperatorSig），这里按名找到即可。
 *
 * @param tt   类型表。
 * @param t    运算符作用的类型。
 * @param op   运算符名，取 `==` `!=` `<` `<=` `>` `>=` `+` `-` `*` `/` `%` 之一。
 * @param rhs  右操作数类型；判断数组是否派生 `==` 时要用到。
 * @return     可用则返回 `true`；`t` 为 NULL 时返回 `false`。
 */
bool typeSupportsOp(TypeTable *tt, Type *t, const char *op, Type *rhs) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    bool isEq = isEqualityOp(op);

    if (isEq ? cmpIsNative(t) : ttIsNumeric(t)) {
        if (strcmp(op, "%") != 0 || ttIsInteger(t)) return true;
    }
    /* 数组的 `==` 由编译器派生，前提是元素可比，且只能与**同一个**数组类型比：只递归问
     * 元素能否与 `rhs` 比较，会放过 `[1]i64 == i32`，而生成的描述符比较随后会从 4 字节
     * 的操作数里读出 8 字节。 */
    if (isEq && t->kind == TY_ARRAY)
        return rhs && ttEquals(ttBase(t), ttBase(rhs)) &&
               typeSupportsOp(tt, t->inner, op, t->inner);

    Type *b = ttBase(t);
    if (!structOf(b)) return false;
    return findOp(tt, b, op, rhs, isEq && strcmp(op, "!=") == 0 ? "==" : NULL) != NULL;
}

/**
 * @brief 类型上名为 `name` 的方法；只找类型自己函数体里的方法，不考虑自由函数。
 *
 * @param st    承载方法的类型。
 * @param name  方法名。
 * @return      方法；没有则 `NULL`。
 */
FuncDef *findMethod(Type *st, const char *name) {
    /* `Type.mholder` holds methods that hang off a **type without a body of its own**: a
     * builtin scalar (`impl i64 { ... }`) and a generic **instance** (`impl slice<u8> { ... }`).
     * It is consulted first because it is the more specific of the two -- `structOf` answers
     * with the generic declaration's body, whose methods every instance shares. */
    StructDef *sd = st ? st->mholder : NULL;
    if (!sd) sd = structOf(st);
    if (!sd) return NULL;
    for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
        if (strcmp(m->name, name) == 0) return m;
    }
    return NULL;
}

/**
 * @brief `st` 上右操作数为 `rhs` 的那个运算符方法。
 *
 * 运算符是唯一允许重名的位置：签名形状固定（`self: ref T` 加一个操作数），能区分的只剩
 * the operand's type, and keying on it exactly (no conversions, no candidate ranking) is what
 * makes an overload set decidable without overload resolution.
 *
 * @param tt    类型表。
 * @param st    左操作数类型。
 * @param name  运算符名，例如 `<<`。
 * @param rhs   右操作数类型；传 NULL 表示接受第一个同名方法（操作数类型不关心的场合）。
 * @return      方法；没有则 `NULL`。
 */
FuncDef *findOperator(TypeTable *tt, Type *st, const char *name, Type *rhs) {
    StructDef *sd = structOf(st);
    if (!sd) return NULL;
    /* A method of a generic type spells its parameters in terms of the *owner's* type
     * parameters (`slice<T>::==` takes `other: ref slice<T>`), so the receiver's type
     * arguments have to be substituted before the operand type can be compared. */
    Vec *sp = NULL, *sa = NULL;
    Type *b = ttBase(st);
    if (b && b->kind == TY_GENERIC && b->sdef == sd && sd->typeParams.len == b->targs.len) {
        sp = &sd->typeParams;
        sa = &b->targs;
    }
    for (size_t i = 0; i < sd->methods.len; i++) {
        FuncDef *m = *(FuncDef **)vecAt(&sd->methods, i);
        if (strcmp(m->name, name) != 0) continue;
        if (!rhs || m->params.len < 2) return m;
        Param *p1 = *(Param **)vecAt(&m->params, 1);
        Type *pt = sp ? ttSubstitute(tt, p1->type, sp, sa) : p1->type;
        if (ttEquals(ttBase(pt), ttBase(rhs))) return m;
    }
    return NULL;
}

/**
 * @brief 这个枚举有没有至少一个带载荷的变体，例如 `| circle(f64)`。
 *
 * 这样的类型在 C 里不是 `enum` 而是带标签的结构体：不能用 `==` 比，打印也只打印变体名。
 *
 * @param td  枚举定义。
 * @return    有带载荷的变体则返回 `true`；`td` 为 NULL 时返回 `false`。
 */
bool enumHasPayload(TypeDef *td) {
    if (!td) return false;
    for (size_t i = 0; i < td->variants.len; i++)
        if ((*(Variant **)vecAt(&td->variants, i))->types.len > 0) return true;
    return false;
}

/**
 * @brief 是不是那五个可以重载的算术运算符之一：`+` `-` `*` `/` `%`。
 *
 * `-` 只作为二元减法：extC 不能重载一元运算符，所以名为 `-` 的方法由 `a - b` 触及，
 * `-a` 永远不会。
 *
 * @param op  运算符写法。
 * @return    是则返回 `true`。
 */
bool isArithOp(const char *op) {
    return strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
           strcmp(op, "/") == 0 || strcmp(op, "%") == 0;
}

/**
 * @brief C 自己能不能比较这个类型：数字、`bool` 与无载荷枚举。
 *
 * `str` 刻意不在此列：它的 `==` 会退化成指针比较，所以要求提供 `eq` 方法。
 *
 * @param t  类型。
 * @return   C 的 `==` 比的是值而不是地址则返回 `true`；`t` 为 NULL 时返回 `false`。
 */
bool cmpIsNative(Type *t) {
    if (!t) return false;
    if (ttIsError(t)) return true;
    if (ttIsInteger(t) || ttIsFloat(t) || ttIs(t, "bool")) return true;
    /* 无载荷枚举在 C 里是整数，可以直接比。带载荷的不行：它是 `struct { tag; union }`，
     * 而 C 结构体不支持 `==`，要用 `match`；派生 `_eq` 得递归比较载荷，不派生。 */
    if (ttBase(t)->kind == TY_ENUM) return !enumHasPayload(ttBase(t)->edef);
    return false;
}

/**
 * @brief `TY_STRUCT` / `TY_GENERIC` 背后的结构体定义；别的种类没有。
 *
 * @param t  类型。
 * @return   结构体定义；`t` 为 NULL 或其他种类时返回 NULL。
 */
StructDef *structOf(Type *t) {
    if (!t) return NULL;
    if (t->kind == TY_STRUCT || t->kind == TY_GENERIC) return t->sdef;
    return NULL;
}

/**
 * @brief 找一个类型的运算符方法。
 *
 * `!=` 是特例：没有 `!=` 方法时回落到 `==` 并取反，取反由代码生成做。A generic receiver's
 * arguments are substituted before matching.
 *
 * @param tt        类型表。
 * @param b         承载方法的类型（`mholder`）。
 * @param sym       要找的运算符名（`==`、`<`、`<<` 等）。
 * @param rhs       右操作数类型；运算符按它精确匹配，重名才可判定。
 * @param fallback  兜底运算符名，可以传 `NULL`。
 * @return          找到的方法；没有则 `NULL`。
 */
FuncDef *findOp(TypeTable *tt, Type *b, const char *sym, Type *rhs, const char *fallback) {
    FuncDef *m = findOperator(tt, b, sym, rhs);
    if (!m && fallback) m = findOperator(tt, b, fallback, rhs);
    return m;
}
