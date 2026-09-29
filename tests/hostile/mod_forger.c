/* 不规范模块 ③：**伪造板内指针**。宿主会验"这个指针在板内吗" —— 我偏还一个我自己的静态变量
 * 的地址（板外），看它拦不拦。 */
#include <stdint.h>
static uint8_t mine[64];
uint8_t *forge(void) { mine[0] = 0x42; return mine; }
