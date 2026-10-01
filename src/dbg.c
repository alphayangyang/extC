/* 调试断言的实现（见 src/dbg.h 的说明）。
 *
 * 开关是环境变量 `EXTC_DBG`：设成任何非空且非 `0` 的值就打开。第一次使用时读一次，
 * 之后只看一个静态变量 —— 关掉时每个断言点只花一次分支，发布构建不需要重新编译。*/
#include "dbg.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static int g_on = -1;    /* -1 = 还没判断过；0 = 关；1 = 开 */

static int dbgEnabled(void) {
    if (g_on < 0) {
        const char *e = getenv("EXTC_DBG");
        g_on = (e && *e && *e != '0') ? 1 : 0;
    }
    return g_on;
}

void extcDbgNoteF(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    if (!dbgEnabled()) return;
    fprintf(stderr, "[note] %s:%d: ", file ? file : "?", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void extcDbgNote(const char *what, const char *file, int line) {
    if (!dbgEnabled()) return;
    fprintf(stderr, "[note] %s:%d: %s\n", file ? file : "?", line, what ? what : "?");
}

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

void extcDbgHit(const char *kind, const char *what, const char *file, int line) {
    if (!dbgEnabled()) return;
    /* `[assert]` / `[fallback]` 前缀是给闸门与 `check.sh` 抓的：一节红不靠"输出里有字样"，
     * 而靠这只编译器**在这里 abort** —— 走到兜底本身就是信号。 */
    fprintf(stderr, "[%s] %s:%d: %s\n", kind ? kind : "dbg", file ? file : "?", line, what ? what : "?");
    fflush(stderr);
    abort();
}
