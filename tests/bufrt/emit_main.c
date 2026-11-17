/* tests/bufrt/emit_main.c —— 用仓库自己的发射口把 buf 基质（`src/back/bufrt.c`）的文本导到 stdout。
 *
 * run.sh 把这份文本和驱动一起编译：被测的就是生成文件里真正跑的那份代码，不存在另抄一份实现对拍的漂移。
 * `EXTC_BUF_MT_TEST=1` 导多线程档（锁保留），0 导单线程档（锁编译掉）。
 */

#include <stdio.h>
#include <stdlib.h>

#include "base.h"
#include "bufrt.h"

int main(void) {
    Arena a;
    Buf out;
    const char *mt = getenv("EXTC_BUF_MT_TEST");
    arenaInit(&a, 1 << 16);
    bufInit(&out, &a);
    bufRtEmitRuntime(&a, &out, (mt && mt[0] == 49) ? 1 : 0);
    fputs(bufCstr(&out), stdout);
    return 0;
}
