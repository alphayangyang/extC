/* 编译器内部的**调试断言** —— 用一个运行时开关代替条件编译。
 *
 * 为什么需要它（而不是直接用 `<assert.h>`）：R1/R3 与 U 批次反复遇到同一类病灶 ——
 * **一个值同时表达两件事**，然后静默取兜底：
 *
 *   - `0` 既表示"还没见过任何目的地"，又表示"活得最久"（P0-2：于是求成了最大值）；
 *   - `placeDepth` 认不出的形状返回 `0`（P0-6c：兜底方向反了）；
 *   - 递归预算耗尽时 `return 0 / true / false / val`（四处，极性各不相同）；
 *   - 表满（字段 4 槽、实参 8 槽、参数 64）时转成 `otherDepth` / `overflow` 兜底。
 *
 * 兜底本身有时是对的（保守方向），但**读那一行看不出极性**，而"按设计不该被走到"的兜底
 * 一旦真被走到，就是一个应该被发现的信号 —— 这正是断言要抓的东西。
 *
 * 两条规矩：
 *   1. **不变量**用 `EXTC_DBG_ASSERT`：内部一致性，破了就是编译器自己的 bug。
 *   2. **兜底路径**用 `EXTC_DBG_FALLBACK`：预算耗尽、来源环、表满这些"不该被走到"的路。
 *
 * 开关是**运行时**的（`EXTC_DBG=1`，见 `src/dbg.c`），所以：
 *   · 不需要第二套构建，同一只二进制既能当发布用也能当断言用；
 *   · 头文件里不出现任何条件编译 —— 本仓库 `tools/check_guards.py` 的房规是
 *     "include guard 内不许有第二个 `#endif`"，条件编译放在头文件里会直接判红。
 * 关掉时每个点只花一次分支（`extcDbgHit` 立刻返回），不打印、不 abort。*/
#ifndef EXTC_DBG_H
#define EXTC_DBG_H

/* 由 `src/dbg.c` 实现：开关关着时立即返回；开着时打印并按 abort 结束。
 * `kind` 是 `"assert"` 或 `"fallback"`，`what` 是断言式或"哪条兜底被走到了"的说明。 */
void extcDbgHit(const char *kind, const char *what, const char *file, int line);
void extcDbgHitF(const char *kind, const char *file, int line, const char *fmt, ...);
void extcDbgNote(const char *what, const char *file, int line);
void extcDbgNoteF(const char *file, int line, const char *fmt, ...);

/* 不变量：不成立就是编译器自己的 bug。 */
#define EXTC_DBG_ASSERT(cond)          ((cond) ? (void)0 : extcDbgHit("assert", #cond, __FILE__, __LINE__))
#define EXTC_DBG_ASSERT_MSG(cond, msg) ((cond) ? (void)0 : extcDbgHit("assert", (msg), __FILE__, __LINE__))

/* 同上，带格式化细节（断言失败时要看到是哪个绑定/哪个数值）。*/
#define EXTC_DBG_ASSERT_MSGF(cond, ...) \
    ((cond) ? (void)0 : extcDbgHitF("assert", __FILE__, __LINE__, __VA_ARGS__))

/* 兜底：按设计不该被走到；被走到时（开关开着）立刻暴露，而不是让哨兵悄悄流下去。 */
#define EXTC_DBG_FALLBACK(what)        extcDbgHit("fallback", (what), __FILE__, __LINE__)

/* **已知会被走到的**保守路径（例如行数不够用的递归预算）：只报告、不 abort。
 * 实测：`promoteInto2` 的 32 跳预算在 examples 里就会命中（coalesce / escape-promotion /
 * list-return / new / nullable-ref / out-param）⇒ 它是活路径，不该当"不该发生"处理。
 * 这类路径的价值是**可计数**：计数变了就说明分析形态变了。 */
#define EXTC_DBG_NOTE(what)            extcDbgNote((what), __FILE__, __LINE__)

/* 同上，但带格式化细节（例如"是哪个 `ExprKind` 走到了兜底"）—— 定位哨兵时最需要的信息。
 * `dbg.c` 里关掉时立即返回，开关打开才 `vfprintf`。*/
#define EXTC_DBG_NOTEF(...)            extcDbgNoteF(__FILE__, __LINE__, __VA_ARGS__)

#endif /* EXTC_DBG_H */
