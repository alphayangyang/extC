/* 调试断言的实现（见 src/dbg.h 的说明）。
 *
 * 开关是环境变量 `EXTC_DBG`：设成任何非空且非 `0` 的值就打开。第一次使用时读一次，
 * 之后只看一个静态变量 —— 关掉时每个断言点只花一次分支，发布构建不需要重新编译。*/
#include "dbg.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static int g_on = -1;    /* -1 = 还没判断过；0 = 关；1 = 开 */

/**
 * @brief 调试开关是否打开；第一次调用读环境变量，之后只看静态变量。
 *
 * 开关是环境变量 `EXTC_DBG`：非空、且首字符不是 `0` 即打开。关掉时每个断言点只花一次分支。
 *
 * @return  打开时为 1，关闭时为 0。
 */
static int dbgEnabled(void) {
    if (g_on < 0) {
        const char *e = getenv("EXTC_DBG");
        g_on = (e && *e && *e != '0') ? 1 : 0;
    }
    return g_on;
}

/**
 * @brief 同 `extcDbgNote`，但带格式化细节（哪个绑定、哪个数值）。
 *
 * @param file  调用点的源文件；传 `NULL` 打 `?`。
 * @param line  调用点的行号。
 * @param fmt   细节的格式串。
 */
void extcDbgNoteF(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    if (!dbgEnabled()) return;
    fprintf(stderr, "[note] %s:%d: ", file ? file : "?", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/**
 * @brief 报一条**已知会被走到**的保守路径；只报告，不 abort。
 *
 * @param what  说明；传 `NULL` 打 `?`。
 * @param file  调用点的源文件；传 `NULL` 打 `?`。
 * @param line  调用点的行号。
 */
void extcDbgNote(const char *what, const char *file, int line) {
    if (!dbgEnabled()) return;
    fprintf(stderr, "[note] %s:%d: %s\n", file ? file : "?", line, what ? what : "?");
}

/**
 * @brief 同 `extcDbgHit`，但带格式化细节；开关关着时立即返回，开着时打印并 abort。
 *
 * @param kind  是 `"assert"` 还是 `"fallback"`；传 `NULL` 打 `dbg`。
 * @param file  调用点的源文件；传 `NULL` 打 `?`。
 * @param line  调用点的行号。
 * @param fmt   细节的格式串。
 */
void extcDbgHitF(const char *kind, const char *file, int line, const char *fmt, ...) {
    va_list ap;
    if (!dbgEnabled()) return;
    fprintf(stderr, "[%s] %s:%d: ", kind ? kind : "dbg", file ? file : "?", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}

/**
 * @brief 调试开关是否打开（`EXTC_DBG=1`）。
 *
 * @return  打开时为 `true`。
 */
bool extcDbgOn(void) { return dbgEnabled(); }

/**
 * @brief 报一次断言/兜底命中；开关关着时立即返回，开着时打印并 abort。
 *
 * @param kind  是 `"assert"` 还是 `"fallback"`；传 `NULL` 打 `dbg`。
 * @param what  断言式，或"哪条兜底被走到了"的说明；传 `NULL` 打 `?`。
 * @param file  调用点的源文件；传 `NULL` 打 `?`。
 * @param line  调用点的行号。
 */
void extcDbgHit(const char *kind, const char *what, const char *file, int line) {
    if (!dbgEnabled()) return;
    /* `[assert]` / `[fallback]` 前缀是给闸门与 `check.sh` 抓的：一节红不靠"输出里有字样"，
     * 而靠这只编译器**在这里 abort** —— 走到兜底本身就是信号。 */
    fprintf(stderr, "[%s] %s:%d: %s\n", kind ? kind : "dbg", file ? file : "?", line, what ? what : "?");
    fflush(stderr);
    abort();
}
