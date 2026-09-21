/* 类型表。
 *
 * 核心设计：**类型是驻留（interned）的** —— 一个类型永远只有一个实例，
 * 所以「类型相等」就是「指针相等」。这让后面所有的类型检查都变成指针比较。
 *
 * 三层职责分清楚（T1 的核心）：
 *   ast.[ch]    只描述数据的形状
 *   types.[ch]  类型表：驻留、相等、渲染、内建类型
 *   check.[ch]  类型推导与检查（唯一做类型推理的地方）
 *   codegen.[ch] 只读 check 写回 AST 的结果，不做任何推理
 */
#ifndef EXTC_TYPES_H
#define EXTC_TYPES_H

#include "ast.h"
#include "base.h"

typedef struct {
    Arena *arena;
    Vec    builtins;    /* Type* —— 内建类型，驻留 */
    Vec    structs;     /* StructDef* */
    Vec    enums;       /* TypeDef*  */
    Vec    instances;   /* Type* —— 泛型实例，驻留 */
    /* 泛型**枚举**的实例（`option<i64>`）—— 单独一张表：
     * 它们的 owner 是 `edef`（TypeDef）而 `sdef` 是空的，混进 `instances`
     * 会让那些按 `sdef` 走的循环出岔子 ✓ */
    Vec    enumInstances; /* Type* */
    /* 可写视图的「影子」实例（`mut slice<T>`）。
     * 它跟只读版**共用同一个 C 结构体名**，所以**不进 instances** ——
     * 进去的话 codegen 会生成两份一模一样的结构体。
     * 「一个视图类型、一套方法集」靠这个成立（Rust 是给切片造两个类型）。 */
    Vec    viewShadows; /* Type* */
    Type  *tVoid;
    Type  *tError;
} TypeTable;

/* parser 不碰类型表（它只造 TY_UNRESOLVED 的「类型名」），
 * 所以类型表在 parse 之后造。m 可以为 NULL，之后用 ttRegister 补登记。 */
TypeTable *ttNew(Arena *a, Module *m);

/* 把 Module 里的 struct / type 登记进表（**按名字去重，可重复调用**）。
 * prelude 和用户文件共用**同一张表** —— 类型是驻留的、相等是指针比较，
 * 用两张表会让同一个 `bool` 变成两个指针，然后到处报「bool 不是 bool」。 */
void ttRegister(TypeTable *tt, Module *m);

/* 内建类型名（i8/i32/f64/bool/str/void…）*/
bool  ttIsBuiltinName(const char *name);

/* 按名字取类型：内建 → struct → type 枚举。找不到返回 NULL。 */
Type *ttFromName(TypeTable *tt, const char *name);

Type *ttVoid(TypeTable *tt);
Type *ttError(TypeTable *tt);

/* ref T —— 引用类型不做驻留（很少见），靠 ttEquals 结构化比较 */
Type *ttRef(TypeTable *tt, Type *inner);

/* 把 parser 造出来的 TY_UNRESOLVED / TY_REF 解析成驻留类型。
 * `params` 是当前可见的泛型参数名（NULL 或空 = 不在泛型上下文里）。
 * 解析不出来时报告错误并返回哑类型（抑制级联报错）。*/
Type *ttResolve(TypeTable *tt, Ctx *ctx, Type *t, int line, Vec *params);

/* 泛型实例：驻留（`Pair<i32,u8>` 全局只有一份） */
Type *ttGeneric(TypeTable *tt, StructDef *sd, Vec *args);
Type *ttEnumGeneric(TypeTable *tt, TypeDef *td, Vec *args);
bool  ttViewDowngradable(Type *want, Type *got);
/* `mut slice<T>`：拿只读实例的影子（共用 C 名字，不进实例表）。mut=false 就原样返回 */
Type *ttViewMut(TypeTable *tt, Type *base, bool mut);
/* 拿掉 `mut`（可写视图 → 只读视图）；本来就是只读的原样返回 */
Type *ttViewReadonly(TypeTable *tt, Type *t);

/* 固定数组：也驻留（`[15]i32` 全局只有一份） */
Type *ttArray(TypeTable *tt, int64_t n, Type *elem);

/* 把类型里的 TY_PARAM 换成实际类型（单态化用） */
/* 这个类型里**提到了类型参数**吗？（泛型模板里的 `T`）
 * 用途：泛型体的引用规矩要"推迟到实例化再查" —— 见 check.c 的 RefCheck ✓ */
bool ttHasParam(Type *t);

Type *ttSubstitute(TypeTable *tt, Type *t, Vec *params, Vec *args);

/* 类型 → C 标识符：`Pair<i32, u8>` → `Pair_i32_u8` */
const char *ttMangle(TypeTable *tt, Type *t);

bool  ttIsParam(Type *t, const char *name);

bool  ttEquals(Type *a, Type *b);
bool  ttIs(Type *t, const char *builtinName);
bool  ttIsInteger(Type *t);
bool  ttIsFloat(Type *t);
bool  ttIsNumeric(Type *t);
int   ttIntBits(Type *t);      /* 整数位宽；非整数返回 0 */
bool  ttIntSigned(Type *t);
bool  ttIsError(Type *t);

/* 剥掉所有 ref */
Type *ttBase(Type *t);

void  ttRender(Type *t, Buf *out);

/* 拓宽规则：允许**无损失**的隐式转换。收窄一律禁止。 */
bool  ttCanWiden(Type *from, Type *to);

#endif /* EXTC_TYPES_H */
/* 这个类型是不是「视图」？（协议：名字叫 slice 且带一个类型实参）
 * 编译器认它，是因为 `s[i]` 是一条语法 —— 跟 `data` + `len` 那条协议同源。 */
bool ttIsViewType(Type *t);
