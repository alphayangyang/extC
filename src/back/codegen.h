/**
 * @file codegen.h
 * @brief C 代码生成：把检查过的模块变成 C 源文本。
 *
 * 它不自己推导任何类型 —— 每个需要的决定都已经由检查器写在树上。
 */
#ifndef EXTC_CODEGEN_H
#define EXTC_CODEGEN_H

#include "ast.h"
#include "base.h"
#include "types.h"

/**
 * @brief 把检查过的模块翻译成 C 源，追加到 `out`。
 *
 * @param ctx      诊断上下文，`#line` 用的路径也在里面。
 * @param arena    生成器分配的一切所用的 arena。
 * @param tt       类型表；泛型实例也在其中，单态化后的代码由它们生成。
 * @param m        检查过的模块。
 * @param lineMap  发射 `#line` 指令，让 C 编译器的诊断指回代码来自的 `.extc` 行。
 * @param out      追加 C 文本的缓冲区。
 * @return         生成过程中在 `ctx` 里记下错误则返回 `false`。
 */
bool generateC(Ctx *ctx, Arena *arena, TypeTable *tt, Module *m, bool lineMap, Buf *out);

#endif /* EXTC_CODEGEN_H */
