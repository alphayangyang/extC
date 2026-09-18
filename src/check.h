#ifndef EXTC_CHECK_H
#define EXTC_CHECK_H

#include "ast.h"
#include "base.h"
#include "types.h"

/* 一遍**独立的类型检查 pass**（T1 的核心）。
 *
 * 它把结果写回 AST：
 *   Expr.type / Expr.func / Expr.field / Stmt.type
 *
 * 于是代码生成**不需要任何类型推理** —— 它只负责把已经定好的东西翻译成 C。
 * 这是「类型检查必须独立于代码生成」的落地：不然每加一个特性，两边都要改一遍。
 */
bool checkModule(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m);

#endif /* EXTC_CHECK_H */
