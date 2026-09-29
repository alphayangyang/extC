/* 不规范模块 ④：**釜底抽薪**。宿主把板的基址和长度都告诉我了（表里就有），那我就
 * `munmap` 掉它、在**同一个地址**映射我自己的页 —— 宿主的区间检查照样通过，可读到的是我的字节。
 * 这是"一道门"能挡住的极限，也是 HEAP.md 那句"防错不防敌"的具体形状。 */
#include <stdint.h>
#include <sys/mman.h>
uint8_t *hijack(uint8_t *base, int64_t len) {
    munmap(base, (size_t)len);
    void *p = mmap(base, (size_t)len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return 0;
    ((uint8_t *)p)[0] = 0x41;                     /* 我的字节 */
    return (uint8_t *)p;
}
