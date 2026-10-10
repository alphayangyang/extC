/**
 * @file types.h
 * @brief 类型表：驻留、相等、渲染，以及内建类型。
 *
 * 类型是**驻留**的：一个类型只有一个实例，所以除引用、类型参数与泛型实例以外的一切类型，
 * "同型"就是"同指针"，类型检查因此大半是指针比较。
 *
 * 各层各守一件事：
 *   ast.h       只描述数据的形状
 *   types.h     类型表：驻留、相等、渲染、内建类型
 *   check.h     类型推导与检查（唯一做推导的地方）
 *   codegen.h   只读检查器写回 AST 的东西，从不推导
 */

#ifndef EXTC_TYPES_H
#define EXTC_TYPES_H

#include "ast.h"
#include "base.h"

typedef struct {
    Arena *arena;       /* arena that owns this table and every type in it */
    Vec    builtins;    /* Type* - the built-in types; interned */
    Vec    structs;     /* StructDef* - every registered struct */
    Vec    enums;       /* TypeDef* - every registered enum declaration */
    /* One entry per module declaration whose name had to be prefixed to stay unique:
     * the source name and the name it was given (`pair` -> `liba$pair`). The type
     * table consults them when a bare name has to be resolved, and two modules that
     * export the same name produce two entries, which is what lets the resolver count
     * the matches and refuse the ambiguity instead of picking one. */
    Vec    aliases;     /* Alias* */
    Vec    instances;   /* Type* - generic instances; interned */
    /* Instances of generic *enums* (`option<i64>`), kept in a table of their own:
     * their owner is `edef` (a TypeDef) and `sdef` is NULL, so mixing them into
     * `instances` breaks every loop that walks that table by `sdef`. */
    Vec    enumInstances; /* Type* */
    /* Shadow instances for writable views (`mut slice<T>`).
     * A shadow shares the C struct name of the read-only instance, so it must NOT
     * enter `instances`: it would make codegen emit two identical C structs. That
     * is what keeps one view type with one method set (Rust instead mints two
     * distinct slice types). */
    Vec    viewShadows; /* Type* */
    /* Function types (`fn(A) -> R`), interned like generic instances: a function type is a
     * constructor, so `fn(i64) -> i64` written twice is the same type. Codegen emits one C
     * `typedef` per entry here -- and this table **is** the demand set, which is why there is no
     * separate "which function types does the program use" scan: every function type the program
     * mentions was resolved through `ttFn` at some point, whether it was written in a struct
     * field, a parameter, a local, or produced by substituting a generic. */
    Vec    fnTypes;     /* Type* - function types; interned */
    Type  *tVoid;       /* the interned `void` type, which is the only one */
    Type  *tError;      /* the error type; every failed resolution returns it, and
                         * widening to or from it succeeds, so one bad type does not
                         * produce a cascade of follow-up errors */
} TypeTable;

/**
 * @brief 建类型表，并把内建类型、`void` 与错误类型驻留进去。
 *
 * @param a  arena：拥有这张表与它里面每一个类型。
 * @param m  立刻注册其声明的模块；NULL 表示不注册。
 * @return   新表，可以直接交给 `ttRegister` 或 `ttResolve`。
 */
TypeTable *ttNew(Arena *a, Module *m);

/**
 * @brief 注册一个模块的结构体与枚举声明，已存在的名字跳过。
 *
 * prelude 与用户文件共用一张表：类型靠驻留、相等靠指针，两张表会让同一个 `bool` 有两个
 * 指针，于是每次检查都报"bool 不是 bool"。重复调用是预期内的，且无副作用。
 *
 * @param tt  要注册进去的类型表。
 * @param m   要读的模块；NULL 忽略。
 */
void ttRegister(TypeTable *tt, Module *m);

/**
 * @brief 判断 `name` 是不是内建类型名。
 * @param name  类型名。
 * @return      是内建类型名则 `true`。
 */
bool  ttIsBuiltinName(const char *name);

/**
 * @brief 按名字查已注册的类型：先内建，再结构体，最后枚举。
 *
 * 只做**裸名**查找：两个模块导出同一个名字时两个都命中，不能随便选一个的调用方得自己
 * 数命中数（见 `ttResolve`）。
 *
 * @param tt    类型表。
 * @param name  源码里的名字，不是模块改名后的名字。
 * @return      那个类型；没有注册过则 `NULL`。结构体 / 枚举的类型节点在首次用到时建好，
 *              并缓存在它的声明上。
 */
Type *ttFromName(TypeTable *tt, const char *name);

/**
 * @brief 取那唯一的 `void` 类型。
 * @param tt  类型表。
 * @return    表里的 `void` 类型。
 */
Type *ttVoid(TypeTable *tt);

/**
 * @brief 取错误类型：解析失败后的结果，用来压住连锁诊断。
 * @param tt  类型表。
 * @return    错误类型。
 */
Type *ttError(TypeTable *tt);

/**
 * @brief 造一个 `ref T` 类型。
 *
 * 引用**故意不驻留**：它很少见，而且调用方随后会设 `mut` / `nullable`，所以 `ttEquals`
 * 对它们按字段比较。
 *
 * @param tt     拥有新类型的类型表。
 * @param inner  被引用的类型。
 * @return       一个新的、只读、不可空的引用。
 */
Type *ttRef(TypeTable *tt, Type *inner);

/**
 * @brief 把解析器造的类型解析成驻留类型。
 *
 * `TY_UNRESOLVED` 名字先与作用域里的类型参数比，再与已注册的声明比；`TY_REF` 保留自己的
 * 标志；带类型实参的泛型名变成对应的实例。
 *
 * 两个导入模块导出同一个裸名时，这是错误而绝不挑一个：所有命中都被数出来，诊断要求写
 * `module::Name`。挑第一个会悄悄绑错类型 —— 程序编过、类型是错的，比拒绝更糟。
 *
 * @param tt      类型表。
 * @param ctx     接收诊断的上下文。
 * @param t       解析器造的类型节点。
 * @param line    诊断归到哪一行。
 * @param params  作用域里的类型参数名；NULL 或空表示不在泛型声明里。
 * @return        驻留后的类型。失败时报告错误并返回错误类型，让调用方继续查下去而不产生
 *                连锁错误。
 */
Type *ttResolve(TypeTable *tt, Ctx *ctx, Type *t, int line, Vec *params);

/**
 * @brief 造（或复用）泛型结构体实例 `sd<args...>`。
 *
 * 只有**完全具体**的实例才驻留，也只有它需要发 C：检查模板时造出来的 `Pair<A, B>` 里是
 * 类型参数，那个实例只用来比类型，驻留它会让代码生成给 `Box_T_set` 发 C。
 *
 * @param tt    类型表。
 * @param sd    泛型结构体声明。
 * @param args  解析好的类型实参，每个声明参数一个。
 * @return      驻留的实例：同一个声明加相等的实参永远给同一个指针。
 */
Type *ttGeneric(TypeTable *tt, StructDef *sd, Vec *args);

/**
 * @brief 造（或复用）泛型枚举实例 `td<args...>`（`option<i64>`）。
 *
 * 与 `ttGeneric` 同形，两处不同：
 *   1. 主人是 `TypeDef`（`edef`）而不是 `StructDef`，所以实例驻留在自己的
 *      `enumInstances` 表里。
 *   2. 载荷类型**不**在这里替换。变体载荷写在 `TypeDef` 上（`some(T)`），在每个使用处用
 *      `t->targs` 对着 `edef->typeParams` 替换，于是一个 `TypeDef` 服务每个实例，而实例
 *      只是"名字加实参"。代码生成把实例压成替换上下文，载荷的 C 类型自己就解析出来了。
 *
 * @param tt    类型表。
 * @param td    泛型枚举声明。
 * @param args  解析好的类型实参，每个声明参数一个。
 * @return      驻留的实例；实参里提到类型参数时返回只在模板里用的实例（那个不驻留）。
 */
Type *ttEnumGeneric(TypeTable *tt, TypeDef *td, Vec *args);

/**
 * @brief 造（或复用）函数类型 `fn(params...) -> ret`。
 *
 * 只有每一部分都具体时才驻留，规则与 `ttGeneric` 相同：检查模板会造出 `fn(T) -> T`，它只
 * 用来比类型，不能发成 C。
 *
 * 函数类型的 C 拼法是**typedef 名**而不是声明符：本编译器把类型写成声明的前缀，而 C 的
 * 函数指针把名字包在里面（`int64_t (*)(int64_t)` 是类型，`int64_t (*f)(int64_t)` 不是
 * "类型加名字"）。名字由 `ttMangle` 产生，typedef 由代码生成发射。
 *
 * @param tt      类型表。
 * @param params  解析好的参数类型，按顺序（`Type *` 元素）；向量会被复制。
 * @param ret     解析好的返回类型；NULL 读作 `void`。
 * @return        驻留的函数类型：同一个签名永远给同一个指针。
 */
Type *ttFn(TypeTable *tt, Vec *params, Type *ret);

/**
 * @brief 判断 `got` 类型的值能不能用在期待 `want` 的地方，允许在任意深度去掉 `mut`。
 *
 * `mut` 只能去掉、不能加上：它是**许可**而不是布局 —— `slice<mut slice<i32>>` 与
 * `slice<slice<i32>>` 是同一个 C 结构体（都叫 `slice_slice_i32`），少一层许可不改变任何
 * 字节；反方向是在**要**一个许可，所以源码必须自己写出 `mut`。
 *
 * 这不是 `ttEquals`：相等问"是不是同一个类型"（那里 `mut` 是身份的一部分），这里问
 * "能不能当那个用"（`mut` 只是许可）。
 *
 * @param want  上下文期待的类型。
 * @param got   值的类型。
 * @return      `got` 可以当 `want` 用则 `true`。
 */
bool  ttViewDowngradable(Type *want, Type *got);
/**
 * @brief 取视图实例的可写影子（`mut slice<T>`）。
 *
 * 影子代替第二个实例，是因为可写视图与只读视图是**同一个 C 结构体** —— 同布局、同名、同
 * 方法集，只差检查器看到的许可。影子因此共用 `base` 的 C 名，也不进实例表，代码生成对两者
 * 只发一个结构体。`mut` 就是这样留成一个限定符，而不是第二个类型。
 *
 * @param tt    类型表。
 * @param base  只读视图实例。
 * @param mut   false 时原样返回 `base`。
 * @return      `base` 的影子实例，首次用到时建、之后复用；不是泛型实例（因而不是视图）的
 *              类型原样返回。
 */
Type *ttViewMut(TypeTable *tt, Type *base, bool mut);
/**
 * @brief 去掉视图类型上的 `mut`：可写视图 -> 只读视图。
 *
 * 递归是重点：元素类型也一起去掉 `mut`，所以 `mut slice<mut slice<T>>` 在任何期待只读
 * `slice<slice<T>>` 的地方都能用。递归是安全的，因为 `mut` 是许可而不是布局：
 * `slice<mut slice<i32>>` 与 `slice<slice<i32>>` 是同一个 C 结构体（都叫
 * `slice_slice_i32`，`mut` 从不出现在 C 类型里），把其中一个传去当另一个用，移动的字节
 * 没有不同。
 *
 * 反方向（只读 -> 可写）永远不允许：没有代码可以凭空造一个许可。
 *
 * @param tt  类型表。
 * @param t   要剥的类型。
 * @return    `t` 的只读形式；什么都没变时返回 `t` 自己。
 */
Type *ttViewReadonly(TypeTable *tt, Type *t);

/**
 * @brief 造（或复用）定长数组类型 `[n]elem`。
 *
 * 数组类型与泛型实例一起存在实例表里：两者都需要发 C 结构体，代码生成一样对待。
 *
 * @param tt    类型表。
 * @param n     元素个数，编译期常量。
 * @param elem  元素类型。
 * @return      驻留的数组类型：长度与元素类型都相等就给同一个指针。
 */
Type *ttArray(TypeTable *tt, int64_t n, Type *elem);

/**
 * @brief 判断类型在任意深度上有没有提到类型参数。
 *
 * 依赖参数的规则在泛型体内无法判定，要等函数体实例化后再查；这里漏一个 `true` 会让模板被
 * 当成具体类型检查。
 *
 * @param t  要看的类型。
 * @return   `t` 是类型参数，或在引用、数组、泛型实参、函数签名、枚举实例里含有一个，
 *           则 `true`。
 */
bool ttHasParam(Type *t);

/**
 * @brief 判断这个类型的值过 C 边界时是不是**一个** C 参数。
 *
 * 一个拼写给两个问的人：`extern!` 签名与函数类型（`fn(A) -> R`），这是够到 C 函数的两条
 * 路。`slice<T>` 是两个 C 参数，所以从来不是过一个；结构体或带载荷的枚举按值有一个本语言
 * 从未承诺对齐的布局，也不过（C-ABI.md 第 9 节第 2 步）。
 *
 * @param t         实参的类型，或返回值的类型。
 * @param isReturn  返回类型时为 true，那里 `void` 合法。
 * @return          是一个 C 参数则 `true`：标量，或**指向任何东西的指针**（`ref T` /
 *                  `mut ref T` / `?ref T` / `ref void`），或函数类型。
 */
bool ttCrossesC(Type *t, bool isReturn);

/**
 * @brief 把 `t` 里的类型参数换成具体类型。
 *
 * 用于单态化：`t` 里出现的每个 `params[i]` 都变 `args[i]`，递归穿过引用、数组、泛型实参、
 * 函数签名与枚举实例。这样重建出来的引用保留 `mut` 与 `nullable` 标志，因为它们是类型的一
 * 部分。
 *
 * @param tt      类型表。
 * @param t       要替换进去的类型。
 * @param params  类型参数名，按位置（`const char *` 元素）。
 * @param args    每个名字的具体类型，位置一一对应（`Type *` 元素）。
 * @return        `t` 换掉参数后的类型；无事可做时（没有参数、没有实参，或 `params` 里没有
 *                这个名字）原样返回 `t`。
 */
Type *ttSubstitute(TypeTable *tt, Type *t, Vec *params, Vec *args);

/**
 * @brief 把一个类型递归渲染成 C 标识符。
 *
 * @param tt  类型表，兼作生成字符串的 arena。
 * @param t   要渲染的类型；NULL 渲染成 `void`。
 * @return     C 标识符：`Pair<i32, u8>` 变 `Pair_i32_u8`；引用、数组与函数类型各有前缀，
 *             使不同的类型不会撞名。
 */
const char *ttMangle(TypeTable *tt, Type *t);
/**
 * @brief 按用户写的方式命名类型（`io::reader`），而不是按改名后的 C 名（`io$reader`）。
 *
 * @param srcName  模块声明记下的源码限定名；根文件的声明传 NULL 或空串。
 * @param name     没有记下源码名时显示的名字。
 * @return         `srcName` 有值就是它，否则 `name`，再否则 `"?"` —— 永不返回 NULL。
 */
const char *ttDispName(const char *srcName, const char *name);

/**
 * @brief 判断 `t` 是不是名为 `name` 的类型参数。
 * @param t     要看的类型。
 * @param name  类型参数名。
 * @return      是则 `true`。
 */
bool  ttIsParam(Type *t, const char *name);

/**
 * @brief 判断两个类型是不是同一个类型。
 *
 * 驻留的种类由指针比较结清，只有按需构建的种类才逐字段比较。泛型实例的 `mut` 是身份的一部
 * 分：可写视图不是只读视图。"能不能当那个用"是另一个问题，由 `ttViewDowngradable` 回答。
 *
 * @param a  一个类型；NULL 与任何类型都不相等。
 * @param b  另一个类型。
 * @return   两者描述同一个类型则 `true`。
 */
bool  ttEquals(Type *a, Type *b);

/**
 * @brief 判断 `t` 是不是内建类型 `builtinName`，忽略引用。
 * @param t            要看的类型。
 * @param builtinName  内建类型名。
 * @return             剥掉引用后是那个内建类型则 `true`。
 */
bool  ttIs(Type *t, const char *builtinName);

/**
 * @brief 判断 `t` 是不是某个内建整数类型。
 * @param t  要看的类型。
 * @return   剥掉引用后是整数则 `true`。
 */
bool  ttIsInteger(Type *t);

/**
 * @brief 判断 `t` 是不是 `f32` 或 `f64`。
 * @param t  要看的类型。
 * @return   剥掉引用后是浮点则 `true`。
 */
bool  ttIsFloat(Type *t);

/**
 * @brief 判断 `t` 是不是整数或浮点。
 * @param t  要看的类型。
 * @return   剥掉引用后是数值类型则 `true`。
 */
bool  ttIsNumeric(Type *t);

/**
 * @brief 整数类型的位宽。
 * @param t  要看的类型。
 * @return   位宽；剥掉引用后不是整数则返回 0。
 */
int   ttIntBits(Type *t);

/**
 * @brief 判断整数类型有没有符号。
 * @param t  要看的类型。
 * @return   有符号则 `true`；非整数类型返回 `false`。
 */
bool  ttIntSigned(Type *t);

/**
 * @brief 判断 `t` 是不是解析失败产生的错误类型。
 * @param t  要看的类型。
 * @return   是错误类型则 `true`。
 */
bool  ttIsError(Type *t);

/**
 * @brief 剥掉所有引用：`ref ref T` -> `T`。
 * @param t  要剥的类型。
 * @return   最里层的非引用类型；`t` 为 NULL 时返回 NULL。
 */
Type *ttBase(Type *t);

/**
 * @brief 把 `t` 面向用户的拼法追加到 `out`，递归进入它的各部分。
 *
 * 可写视图必须打出开头的 `mut`，否则消息读起来是"expects `slice<i32>`, found
 * `slice<i32>`"，什么也没告诉用户。声明名走 `ttDispName`，所以模块类型打印成
 * `io::reader`，绝不是改名后的 `io$reader`；暴露内部编码等于说出用户没写过的词。
 *
 * @param t    要渲染的类型；NULL 渲染成 `void`。
 * @param out  接收文本的缓冲区。
 */
void  ttRender(Type *t, Buf *out);

/**
 * @brief 判断 `from` 类型的值能不能用在期待 `to` 的地方（隐式加宽）。
 *
 * 只有无损转换才允许隐式发生。引用从不加宽：`ref T` 与 `mut ref T` 是两种许可，只能单向
 * 降级，那在赋值检查处处理。引用测试必须排在整数助手之前，因为那些助手会剥引用：少了它，
 * `ref i32` 到 `mut ref i32` 看起来像"i32 加宽到 i32"，于是被接受。
 *
 * 错误类型与一切互相加宽，于是一次失败的类型不会产生连锁诊断。
 *
 * @param from  值的类型。
 * @param to    会被转换成的类型。
 * @return      转换无损、因而允许隐式发生则 `true`。
 */
bool  ttCanWiden(Type *from, Type *to);

/**
 * @brief 判断 `t` 是不是视图类型。
 *
 * 视图按形状认：名字是 `slice`、正好带一个类型实参 —— 与编译器认 `data` + `len` 那个形状
 * （`s[i]` 的定义所在）是同一种做法。
 *
 * @param t  要看的类型。
 * @return   是视图则 `true`。
 */
bool ttIsViewType(Type *t);

#endif /* EXTC_TYPES_H */
