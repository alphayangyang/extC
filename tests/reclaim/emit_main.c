/* tests/reclaim/emit_main.c —— 用仓库自己的发射口把回收运行期文本导到 stdout。
 *
 * run.sh 把它和 `src/back/reclaim.c` / `src/base/base.c` 一起编译成一个独立程序：被测的文本就是
 * 这个程序吐出来的那份，不存在"另抄一份实现来对拍"的漂移。
 */

#include <stdio.h>

#include "base.h"
#include "reclaim.h"

int main(void) {
    Arena a;
    Buf out;
    arenaInit(&a, 1 << 16);
    bufInit(&out, &a);
    reclaimEmitRuntime(&a, &out);
    fputs(bufCstr(&out), stdout);
    return 0;
}
