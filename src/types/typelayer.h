/**
 * @file typelayer.h
 * @brief 类型层：关于一个类型的**只读问题**，检查器与代码生成共用。
 *
 * 这里只放"拿类型和节点、回答一个问题、什么都不改"的函数：没有 `Checker`、没有侧表、
 * 没有 memo 缓存。需要检查器状态的东西属于检查器自己的私有头文件，代码生成不该想要它。
 *
 * 这条边界让依赖保持单向：代码生成可以依赖**类型是什么**，绝不依赖检查器的工作状态。
 */
#ifndef EXTC_TYPELAYER_H
#define EXTC_TYPELAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "ast.h"
#include "types.h"

/* ---- 运算符 ------------------------------------------------------------------------- */

/**
 * @brief 是不是比较运算符：`==` `!=` `<` `<=` `>` `>=`。
 * @param op  运算符写法。
 * @return    是则 `true`。
 */
bool isCmpOp(const char *op);

/**
 * @brief 是不是逻辑运算符：`&&` `||`。
 * @param op  运算符写法。
 * @return    是则 `true`。
 */
bool isLogicOp(const char *op);

/**
 * @brief 是不是相等运算符：`==` `!=`。
 * @param op  运算符写法。
 * @return    是则 `true`。
 */
bool isEqualityOp(const char *op);

/**
 * @brief 用户函数能不能声明这个运算符名（可重载集合里的名字）。
 * @param name  运算符名。
 * @return      可以则 `true`。
 */
bool isOverloadableOp(const char *name);

/**
 * @brief 这个类型里有没有出现类型参数（`T`），直接或嵌套。
 *
 * 检查器对这样的类型推迟工作，等到实例化时再做。
 *
 * @param t  类型。
 * @return   提到类型参数则 `true`。
 */
bool mentionsParam(Type *t);

/**
 * @brief `t` 能不能对 `rhs` 原生地或经由方法回答 `op`。
 *
 * 只读：它会走一遍类型，遇到方法时再走该类型的方法表。
 *
 * @param tt   类型表。
 * @param t    左操作数类型。
 * @param op   运算符写法。
 * @param rhs  右操作数类型。
 * @return     支持则 `true`。
 */
bool typeSupportsOp(TypeTable *tt, Type *t, const char *op, Type *rhs);

/* ---- 运算符（续） ------------------------------------------------------------------- */

/**
 * @brief 是不是那五个算术运算符：`+` `-` `*` `/` `%`。
 *
 * `<<` / `>>` **不在**这里 —— 它们由调用方按流运算符单独判（`isOverloadableOp` 里那两个
 * `strcmp` 就是干这个的）。
 *
 * @param op  运算符写法。
 * @return    是则 `true`。
 */
bool isArithOp(const char *op);

/**
 * @brief C 直接用 `==` 就能比较这个类型吗？
 * @param t  类型。
 * @return   C 原生可比则 `true`。
 */
bool cmpIsNative(Type *t);

/**
 * @brief 找一个类型的运算符方法。
 *
 * `sym` 不存在时改用 `fallback`（`!=` 回落到 `==` 的规则）。
 *
 * @param tt        类型表。
 * @param b         承载方法的类型（`mholder`）。
 * @param sym       要找的运算符名。
 * @param rhs       右操作数类型（用来在重名方法里挑）。
 * @param fallback  兜底运算符名，可以传 `NULL`。
 * @return          找到的方法；没有则 `NULL`。
 */
FuncDef *findOp(TypeTable *tt, Type *b, const char *sym, Type *rhs, const char *fallback);

/* ---- 类型与它的成员 ----------------------------------------------------------------- */

/**
 * @brief `t` 是不是 prelude 类型 `name` 的、实参个数正好 `nargs` 的实例？
 *
 * 例：`isProtoType(x, "coroutine", 1)`。这是全编译器共识的形状判据。
 *
 * @param t      类型。
 * @param name   类型名。
 * @param nargs  类型实参个数。
 * @return       形状相符则 `true`。
 */
bool isProtoType(Type *t, const char *name, size_t nargs);

/**
 * @brief `TY_STRUCT` / `TY_GENERIC` 背后的结构体定义。
 * @param t  类型。
 * @return   结构体定义；不是则 `NULL`。
 */
StructDef *structOf(Type *t);

/**
 * @brief 类型上名为 `name` 的方法。
 * @param st    承载方法的类型。
 * @param name  方法名。
 * @return      方法；没有则 `NULL`。
 */
FuncDef *findMethod(Type *st, const char *name);

/**
 * @brief `st` 上右操作数为 `rhs` 的那个运算符方法。
 *
 * `rhs` 传 `NULL` 表示接受第一个同名的。比较前会替换泛型类型的参数，
 * 所以 `slice<u8>` 能找到 `slice<T>::==`。
 *
 * @param tt    类型表。
 * @param st    左操作数类型。
 * @param name  运算符名。
 * @param rhs   右操作数类型，可以传 `NULL`。
 * @return      方法；没有则 `NULL`。
 */
FuncDef *findOperator(TypeTable *tt, Type *st, const char *name, Type *rhs);

/**
 * @brief 这个枚举有没有带载荷的变体。
 * @param td  枚举定义。
 * @return    有则 `true`。
 */
bool enumHasPayload(TypeDef *td);

/**
 * @brief 取声明名的最后一段（模块改名会加 `$` 前缀）。
 *
 * `$` 不可能出现在 extC 标识符里，所以它只能来自加载器的模块改名。
 *
 * @param name  声明名，可以传 `NULL`。
 * @return      最后一段；`name` 为 `NULL` 时返回它自己。
 */
static inline const char *plateDeclTail(const char *name) {
    const char *d = name ? strrchr(name, '$') : NULL;
    return d ? d + 1 : name;
}

/**
 * @brief 这个声明是不是板层的那一个原语 `extc_viewOf`。
 * @param name  声明名。
 * @return      是则 `true`。
 */
static inline bool plateIsViewOfName(const char *name) {
    return name && strcmp(plateDeclTail(name), "extc_viewOf") == 0;
}

/**
 * @brief 这个声明是不是 `extc_memCopy`。
 * @param name  声明名。
 * @return      是则 `true`。
 */
static inline bool plateIsMemCopyName(const char *name) {
    return name && strcmp(plateDeclTail(name), "extc_memCopy") == 0;
}

/* ---- 池的问题 ----------------------------------------------------------------------- */

/**
 * @brief 这条语句能不能够到 `extc_pool_new`？
 *
 * `descendBlocks` 是两个问法的分界：检查器要走进嵌套块（每个块有自己的 zone），
 * 代码生成只问一个块里的直接语句。
 *
 * @param s              语句。
 * @param descendBlocks  要不要走进嵌套块。
 * @return               够得到则 `true`。
 */
bool stmtMakesPool(Stmt *s, bool descendBlocks);

/* ---- 池的名字（一份清单、一种拼写） -------------------------------------------------- */

/**
 * @brief 是不是池的构造器名（`extc_pool_new` / `extc_pool_new_at` / `extc_pool_new_table`）。
 * @param n  声明名。
 * @return   是则 `true`。
 */
static inline bool isPoolCtorName(const char *n) {
    return n && (strcmp(n, "extc_pool_new") == 0 || strcmp(n, "extc_pool_new_at") == 0 ||
                 strcmp(n, "extc_pool_new_table") == 0);
}

/**
 * @brief 这个池构造器要不要显式给 zone（`extc_pool_new` / `extc_pool_new_table` 要）。
 * @param n  声明名。
 * @return   要则 `true`。
 */
static inline bool poolCtorNeedsZone(const char *n) {
    return n && (strcmp(n, "extc_pool_new") == 0 || strcmp(n, "extc_pool_new_table") == 0);
}

/**
 * @brief 是不是那六个池原语之一（`poolSlice` / `poolSliceRaw` / `poolResize` /
 * `poolResizeRaw` / `poolGive` / `copyInto`）。
 *
 * 检查器校验它们的实参个数，代码生成手工把它们降级成 C。名字只写在这一处，
 * 两边的清单不会各自漂。
 *
 * @param n  声明名。
 * @return   是则 `true`。
 */
static inline bool isPoolPrimitiveName(const char *n) {
    return n && (strcmp(n, "poolSlice") == 0 || strcmp(n, "poolSliceRaw") == 0 ||
                 strcmp(n, "poolResize") == 0 || strcmp(n, "poolResizeRaw") == 0 ||
                 strcmp(n, "poolGive") == 0 || strcmp(n, "copyInto") == 0);
}

#endif
