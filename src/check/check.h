/**
 * @file check.h
 * @brief 类型检查器：解析名字、推导类型、执行借用与逃逸规则。
 *
 * 每个结果都写回 ast.h 那棵树，代码生成因此不必做任何类型推导。
 */
#ifndef EXTC_CHECK_H
#define EXTC_CHECK_H

#include "ast.h"
#include "base.h"
#include "types.h"

/**
 * @brief 检查一个模块并原地标注它。
 *
 * 结果写在节点上：`Expr.type`、`Expr.func`、`Expr.field`、`Stmt.type`。代码生成因此完全
 * 不需要类型推导 —— 每加一个语言特性不必实现两遍，一遍在检查器、一遍在生成器。
 *
 * @param ctx    被检查文件的诊断上下文。
 * @param arena  这一遍分配的一切所用的 arena。
 * @param tt     prelude 与每个模块共用的类型表。
 * @param m      要检查的模块，已加载并注册进 `tt`。
 * @return       在 `ctx` 里记下错误则返回 `false`。
 */
bool checkModule(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m);

/**
 * @brief 把 stdlib 目录交给检查器，用于从 `INDEX` 读"没有这个方法"的提示。
 *
 * 加载器（更后的一层）解析这个目录，而提示本身住在检查器这一侧、紧挨着用它的诊断，所以
 * 这次调用是**向下**的：加载器经由这个公开入口调进来，检查器不去够模块层的头文件。
 *
 * @param stdDir  stdlib 目录；提示在首次未命中、只走错误路径时读它。
 */
void hintIndexSetStdDir(const char *stdDir);

#endif /* EXTC_CHECK_H */
