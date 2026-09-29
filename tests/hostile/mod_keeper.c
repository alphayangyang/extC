/* 不规范模块 ②：**食言**。声明上写着"我不留你的指针"（`effects Addr=0`），我留了：
 * 存进静态变量，等下一次调用（那时宿主的帧早没了）再往里写。 */
#include <stdint.h>
static uint8_t *kept;
int64_t take(uint8_t *p, int64_t n) { kept = p; return n; }
int64_t use_later(void) { if (!kept) return -1; kept[0] = 0x77; return kept[0]; }
