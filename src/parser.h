/* extC 的递归下降 parser。
 *
 * week-0 的语法目标（奶昔自己拍的，待主人确认，见 DECISIONS.md）：
 *   fn name(p: T, q: T2) -> T { ... }       // 省略 `->` 即无返回值
 *   struct Name { field: T <换行> field: T }
 *   let x: T = e / var x: T = e             // 类型标注可省，从初始化式推导
 *   if / else if / else / while / return / break / continue
 *   a.f(x)  等价于  f(a, x)                 // 方法 = self 作首参数的自由函数
 *   语句以换行结束，分号可选且会被忽略
 */
#ifndef EXTC_PARSER_H
#define EXTC_PARSER_H

#include "ast.h"
#include "base.h"

/* 成功返回 Module，失败返回 NULL 并已在 ctx 上设置错误。 */
bool parseModule(Ctx *ctx, Arena *arena, Vec *toks, Module *out);

#endif /* EXTC_PARSER_H */
