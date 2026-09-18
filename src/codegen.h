#ifndef EXTC_CODEGEN_H
#define EXTC_CODEGEN_H

#include "ast.h"
#include "base.h"

/* 把 AST 生成 C 源码，写进 out。
 * lineMap 打开时会生成 `#line` 指令，让 gcc 的报错映射回 .extc 的行号。 */
bool generateC(Ctx *ctx, Arena *arena, Module *m, bool lineMap, Buf *out);

#endif /* EXTC_CODEGEN_H */
