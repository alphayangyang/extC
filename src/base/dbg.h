/**
 * @file dbg.h
 * @brief 编译器内部的**调试断言**：用一个运行时开关代替条件编译。
 *
 * 用途是把"按设计不该被走到、但读那一行看不出极性"的兜底暴露出来。两条规矩：
 *   1. **不变量**用 `EXTC_DBG_ASSERT` —— 不成立就是编译器自己的 bug；
 *   2. **兜底路径**用 `EXTC_DBG_FALLBACK` —— 预算耗尽、来源环、表满这类路径。
 *
 * 开关是**运行时**的（`EXTC_DBG=1`），所以同一只二进制既能当发布用也能当断言用，
 * 头文件里不出现任何条件编译。关掉时每个点只花一次分支（`extcDbgHit` 立刻返回），
 * 不打印、不 abort。
 */
#ifndef EXTC_DBG_H

#define EXTC_DBG_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief 调试开关是否打开（`EXTC_DBG=1`）。
 * @return 打开时为 `true`。
 */
bool extcDbgOn(void);

/**
 * @brief 报一次断言/兜底命中；开关关着时立即返回，开着时打印并 abort。
 *
 * @param kind  是 `"assert"` 还是 `"fallback"`。
 * @param what  断言式，或"哪条兜底被走到了"的说明。
 * @param file  调用点的源文件。
 * @param line  调用点的行号。
 */
void extcDbgHit(const char *kind, const char *what, const char *file, int line);

/**
 * @brief 同 `extcDbgHit`，但带格式化细节（哪个绑定、哪个数值）。
 *
 * @param kind  是 `"assert"` 还是 `"fallback"`。
 * @param file  调用点的源文件。
 * @param line  调用点的行号。
 * @param fmt   细节的格式串。
 */
void extcDbgHitF(const char *kind, const char *file, int line, const char *fmt, ...);

/**
 * @brief 报一条**已知会被走到**的保守路径；只报告，不 abort。
 *
 * @param what  说明。
 * @param file  调用点的源文件。
 * @param line  调用点的行号。
 */
void extcDbgNote(const char *what, const char *file, int line);

/**
 * @brief 同 `extcDbgNote`，但带格式化细节。
 *
 * @param file  调用点的源文件。
 * @param line  调用点的行号。
 * @param fmt   细节的格式串。
 */
void extcDbgNoteF(const char *file, int line, const char *fmt, ...);

/* 不变量：不成立就是编译器自己的 bug。 */
#define EXTC_DBG_ASSERT(cond)          ((cond) ? (void)0 : extcDbgHit("assert", #cond, __FILE__, __LINE__))
#define EXTC_DBG_ASSERT_MSG(cond, msg) ((cond) ? (void)0 : extcDbgHit("assert", (msg), __FILE__, __LINE__))

/* 同上，带格式化细节。 */
#define EXTC_DBG_ASSERT_MSGF(cond, ...) \
    ((cond) ? (void)0 : extcDbgHitF("assert", __FILE__, __LINE__, __VA_ARGS__))

/* 兜底：按设计不该被走到；被走到时（开关开着）立刻暴露。 */
#define EXTC_DBG_FALLBACK(what)        extcDbgHit("fallback", (what), __FILE__, __LINE__)

/* **已知会被走到的**保守路径（例如行数不够用的递归预算）：只报告、不 abort。
 * 这类路径的价值是**可计数**：计数变了就说明分析形态变了。 */
#define EXTC_DBG_NOTE(what)            extcDbgNote((what), __FILE__, __LINE__)

/* 同上，但带格式化细节（例如"是哪个写法走到了兜底"）。 */
#define EXTC_DBG_NOTEF(...)            extcDbgNoteF(__FILE__, __LINE__, __VA_ARGS__)

#endif /* EXTC_DBG_H */
