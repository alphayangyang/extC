/**
 * @file coroutine.h
 * @brief 协程任务表：`coroutine<T>` 的运行期那一半（docs/topics/CONCURRENCY.md 4.4）。
 *
 * 与池注册表（pools.h）单独成文件同理：协程牵动好几遍，把它的运行期摊进 `codegen.c` 会重演
 * "每个文件里都有一点，哪个文件里都不完整"。属于编译器的工作留在编译器的文件里（`yield`
 * 脱糖、帧、步进）；属于**运行期**的东西放这里。
 *
 * 文本按需发射：从不带任务地方孵化协程的程序一个字节都不带。
 */
#ifndef EXTC_COROUTINE_H
#define EXTC_COROUTINE_H

#include "base.h"

/**
 * @brief 把任务表追加到生成的编译单元。
 *
 * @param a    arena（本函数不用，为与其他发射口同形保留）。
 * @param out  正在写的生成 C。
 */
void coroutineEmitRuntime(Arena *a, Buf *out);

/**
 * @brief 追加文件层的 shim。
 *
 * `struct stat`、`off_t` 与路径的 NUL 终止都留在 C 侧，extC 只拿标量。它独立发射，不搭
 * 协程表的车：协程表的最终发射还被别的条件门着，拉不动这一块。
 *
 * @param a    arena（本函数不用，为与其他发射口同形保留）。
 * @param out  正在写的生成 C。
 */
void fileLayerEmitRuntime(Arena *a, Buf *out);

/**
 * @brief 追加 DNS 那一小块（同步 `getaddrinfo`）。
 *
 * 它跟协程 / epoll 没关系，而运行期是按需发射的，所以放进协程块会让"只想解析域名"的程序
 * 链接失败。这一块单独发射。
 *
 * @param a    arena（本函数不用，为与其他发射口同形保留）。
 * @param out  正在写的生成 C。
 */
void dnsEmitRuntime(Arena *a, Buf *out);

/**
 * @brief 追加事件层（epoll + AF_UNIX socket）。
 *
 * 只有标量与一个指针过边界：`epoll_event` 缓冲区留在这边，平台结构体不跨进 extC。它独立
 * 发射，因为程序可以只驱动 fd、从不孵化协程。
 *
 * @param arena  arena，给临时缓冲区用。
 * @param out    正在写的生成 C。
 */
void eventEmitRuntime(Arena *arena, Buf *out);

#endif
