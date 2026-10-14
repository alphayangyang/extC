// bench/gc/digraph_run.c —— 形状 7 的极小测量垫片（新写的；不动 measure.py）。
//
// 为什么需要它：
//   - `/usr/bin/time -f %M` 的 RSS 是准确的内核值，但 `%e` 只有 **10 ms** 分辨率 ✗
//     （本形状 N=250k 量级的样本本身只有几十 ms ⇒ 分辨率不够）；
//   - 用 python 自己 fork/exec 量：python 解释器 ~10 MB RSS，子进程在 exec 前
//     就把它算进 maxrss ⇒ /bin/true 也报 5.8–11.7 MB（**量具地板假象** ✗）。
// 这个垫片自己只有 ~1.5 MB RSS，于是：
//   - 峰值 RSS = `wait4` 的 `rusage.ru_maxrss`（内核值，KB）；
//   - 时间 = `clock_gettime(CLOCK_MONOTONIC)` 包住 fork→exec→wait（只含被测程序，
//     不含任何 shell/date/taskset 的启动开销）；
//   - 绑核 = `sched_setaffinity(0, ...)`，子进程经 fork **继承**（等价 `taskset -c PIN`）。
//
// 用法： ./digraph_run --pin <cpu> --out <file> [--err <file>] -- <cmd> <args...>
//   `--err` 把子进程 stderr 收进文件（停顿统计解析用）；不给就透出（默认行为不变）。
// 输出： ms=<浮点毫秒> rss_kb=<整数> rc=<子进程退出码>
//       （子进程 stdout 写进 <file>，stderr 原样透出）
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int pin = -1;
    const char *out = NULL;
    const char *err = NULL;
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
            pin = atoi(argv[i + 1]);
            i += 2;
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--err") == 0 && i + 1 < argc) {
            err = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--") == 0) {
            i += 1;
            break;
        } else {
            break;
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: digraph_run --pin <cpu> --out <file> [--err <file>] -- <cmd> <args...>\n");
        return 2;
    }
    if (pin >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(pin, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0) {
            perror("sched_setaffinity");
            return 3;
        }
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t pid = fork();
    if (pid == 0) {
        if (out != NULL) {
            int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, 1);
                close(fd);
            }
        }
        if (err != NULL) {
            int fd2 = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd2 >= 0) {
                dup2(fd2, 2);
                close(fd2);
            }
        }
        execvp(argv[i], &argv[i]);   /* PATH 查找：java/node 这类命令需要 */
        _exit(127);
    }
    if (pid < 0) {
        perror("fork");
        return 4;
    }
    int status = 0;
    struct rusage ru;
    if (wait4(pid, &status, 0, &ru) < 0) {
        perror("wait4");
        return 5;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    int rc;
    if (WIFEXITED(status)) {
        rc = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        rc = 128 + WTERMSIG(status);
    } else {
        rc = -1;
    }
    printf("ms=%.3f rss_kb=%ld rc=%d\n", ms, (long)ru.ru_maxrss, rc);
    return 0;
}
