/* tests/reclaim/emit_main.c —— 用仓库自己的发射口把回收运行期文本导到 stdout。
 *
 * run.sh 把它和 `src/back/reclaim.c` / `src/base/base.c` 一起编译成一个独立程序：被测的文本就是
 * 这个程序吐出来的那份，不存在"另抄一份实现来对拍"的漂移。
 */

#include <stdio.h>
#include <stdlib.h>

#include "base.h"
#include "reclaim.h"

int main(void) {
    Arena a;
    Buf out;
    arenaInit(&a, 1 << 16);
    bufInit(&out, &a);
    /* 与编译器保持一致：**默认无计数**，`EXTC_RC_NOCOUNT=0` 才导计数档。
     * （此前这里硬写 0 ⇒ 驱动一直在跑计数档，默认档的编译分支从没被单测过，
     *   而"计数档单测门禁"于是只是同一件事跑两遍。） */
    const char *nc = getenv("EXTC_RC_NOCOUNT");
    int noCount = (nc && nc[0] == '0') ? 0 : 1;
    reclaimEmitRuntime(&a, &out, noCount, getenv("EXTC_RC_TIME") != NULL,
                          getenv("EXTC_RC_SWEEPFREE") != NULL, getenv("EXTC_RC_LOCALSTORE") != 0 ? 1 : 0);
    fputs(bufCstr(&out), stdout);
    return 0;
}
