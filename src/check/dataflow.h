/**
 * @file dataflow.h
 * @brief 前向、单调的数据流分析：算出每个绑定可能指向的存储有多深。
 *
 * 单独成模块，是因为逃逸检查需要每个绑定一个数 —— "它能持有的最深活引用有多深" —— 而这个
 * 数必须是**上界**。边检查边推导、带破坏性更新、控制流汇合处不做 join，给不出上界：汇合处
 * 后写的会盖掉先写的，答案取决于各分支被访问的次序。
 *
 * 格与转移函数就是第 9.3 节第 3-a 条那套：
 *
 *   lattice   值能达到的块深度，按"至少一样深"排序，另加一个表示"没有活引用"的底
 *   join      取最大：可能来自任一分支的值可以深到更深的那一支
 *   transfer  一次存储把目标的深度抬到源的深度
 *   loops     迭代到不动点；转移函数单调、格的高度有限，所以必然终止
 *
 * 结果对任何遍历次序都一样。
 */
#ifndef EXTC_DATAFLOW_H
#define EXTC_DATAFLOW_H

#include "ast.h"

#define DFA_MAX_VARS 256     /* 每个函数跟多少个绑定 */

/* The checker is passed through a pointer; `struct Checker` is the tag of the typedef in
 * `check_internal.h`, so this declaration and the definition agree on the type. The tag
 * has to be introduced at file scope, or it would be a distinct type visible only inside
 * the parameter list. */
struct Checker;

/* Depth of a binding, and the depth of each of its reference-carrying fields. */
typedef struct {
    const char *cname;      /* the C name of the binding, which is unique per function */
    int         depth;      /* depth of the deepest reference the binding can hold */
    int         nfields;
    struct { const char *name; int depth; } fields[4];
} DfVar;

/* The data-flow facts at the program point where the walk stopped. */
typedef struct {
    DfVar  vars[DFA_MAX_VARS];
    int    nvars;
    int    nblocks;         /* number of basic blocks, for diagnostics */
    int    rounds;          /* fixed-point iterations, for diagnostics */
    bool   overflow;        /* the variable table was too small, so the answer is not
                             * usable and the caller must keep its own numbers */
} DfResult;

/**
 * @brief 分析一个函数体并填 `out`。
 *
 * 在函数体检查完之后跑，此时表达式类型与解析好的名字都在树上；只读 AST，从不写它。
 *
 * @param c    checker，用于名字解析与类型表。
 * @param f    要分析其函数体的函数。
 * @param out  填入最终事实；它们不完整时置 `overflow`。
 */
void dfAnalyze(struct Checker *c, FuncDef *f, DfResult *out);

/**
 * @brief 取一个绑定记录下来的深度。
 *
 * @param r      分析结果。
 * @param cname  绑定的 C 名。
 * @return       深度；分析结果里没有这一条时返回 `-1`。
 */
int dfLookup(const DfResult *r, const char *cname);

/**
 * @brief 在 `r` 的不动点下算表达式 `e` 的深度。
 *
 * 同时给出 `r` 知道的、以 `e` 为根的绑定的字段数（不知道则是 0）。它用来修补字段表：那张表
 * 在检查函数体时写入，因此存的是不动点之前的答案。
 *
 * @param c  checker，用于名字解析。
 * @param r  分析结果。
 * @param e  要度量的表达式。
 * @return   `e` 能携带的引用深度的上界；没有可用的分析结果时返回 0。
 */
int dfValueDepth(struct Checker *c, const DfResult *r, Expr *e);

#endif /* EXTC_DATAFLOW_H */
