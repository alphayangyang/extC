/* 模块装载（定案 70）—— 见 modules.c 顶上的长注释 ✓ */
#ifndef EXTC_MODULES_H
#define EXTC_MODULES_H

#include "ast.h"

/* 按根文件的 `use` 递归装载模块，**拓扑序**合进 `out`（prelude 已经在里面了 ✓）。
 *   rootm      —— 根文件先解析到自己的 Module 里（模块要等它的 `use` 才知道装谁 ✓）
 *   rootCtx    —— 根文件的 Ctx（根文件里的错误走它 ✓）
 *   searchDirs —— `-I` 给的目录（const char*）
 *   outCtxs    —— 装好的模块各自的 Ctx（**上层要拿它们渲染报错** ✓）
 * 返回 false = 有错（诊断已经打出去了 ✓）*/
bool loadModules(Arena *a, Module *out, Module *rootm, Ctx *rootCtx,
                 const char *rootPath, Vec *searchDirs, Vec *outCtxs);

#endif
