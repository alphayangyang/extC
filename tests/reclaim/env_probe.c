/* tests/reclaim/env_probe.c —— 停顿探针环境闸门的回归测试（每个 case 起一个进程）。
 *
 * 被测的是 `src/back/reclaim.c` 发射出来的那份文本（run.sh 导出的 extc_reclaim_runtime.inc），
 * 所以断言的解析行为就是生成物里真正跑的行为：arc 只做一次，因此一个进程只验证一组环境变量。
 * 断言本身在 run.sh 里（stdout 的解析结果 + stderr 的告警行）。 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extc_reclaim_runtime.inc"

int main(void) {
    extc_rc_pauseArm();
    printf("slowUs=%lld ringOn=%d pauseOn=%d\n", extc_rc_slowUs, extc_rc_slowRingOn, extc_rc_pauseOn);
    return 0;
}
