/**
 * @file parser.h
 * @brief extC 语法流水线的第二段：递归下降解析。
 *
 * 消费 `lexAll` 的 token 向量，建出 ast.h 的 `Module`。它什么都不解析：名字、模块路径与
 * 泛型实参都按写出来的样子传下去，交给加载器与类型检查器解释。语句在换行处结束，换行后可能
 * 跟的分号是可选的、会被忽略。
 */
#ifndef EXTC_PARSER_H
#define EXTC_PARSER_H

#include "ast.h"
#include "base.h"

/**
 * @brief 把一个文件的 token 解析进 `out`。
 *
 * `out` 是**追加**到的 Module：调用方初始化一次，prelude 与用户文件因此落在同一个模块里。
 * 接受的语法：
 *
 *     fn name(p: T, q: T2) -> T { ... }   // 没有 `->` 就是 void
 *     struct Name { field: T <newline> field: T }
 *     let x: T = e / var x: T = e         // 类型可以省略
 *     if / else if / else / while / return / break / continue / match
 *     a.f(x)  就是  f(a, x)               // 方法的第一个参数是 self
 *
 * token 向量必须以 TK_EOF 收尾（`lexAll` 总会补一个）。
 *
 * @param ctx    报告第一个语法错误；一旦置位就停止解析。
 * @param arena  拥有每个 AST 节点与每个拷贝的名字。
 * @param toks   `lexAll` 产出的 token 向量。
 * @param out    追加到的 Module。
 * @return       整个 token 向量被消费完则 `true`；`ctx->hasError` 置位则 `false`，此时
 *               `out` 里留的是已经解析出来的声明。
 */
bool parseModule(Ctx *ctx, Arena *arena, Vec *toks, Module *out);

#endif /* EXTC_PARSER_H */
