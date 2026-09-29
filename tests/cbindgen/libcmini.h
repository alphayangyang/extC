/* tests/cbindgen/libcmini.h —— 给"真跑一次"用的最小 libc 声明（任何机器上都有 libc）。
 * 生成 → 编译 → `dlopen("libc.so.6")` → 真的调用，判据是拿回来的进程号。 */
#ifndef LIBM_H
#define LIBM_H
typedef int pid_t;
pid_t getpid(void);
#endif
