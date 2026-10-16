// bench/gc/v2/shim.c - v2 大横评的测量垫片（digraph_run.c 的超集）。
//
// 为什么另写一份而不改 bench/gc/digraph_run.c：那份是 run-digraph.sh 的形状 7 卡具，
// 已被 1075 行的 DIGRAPH.md 引用；这里的口径要更宽（v2 四形状六语言）。
//
// 用法： ./v2run --pin <cpu> --out <file> [--err <file>] -- <cmd> <args...>
// 输出： ms=<浮点毫秒> rss_kb=<峰值RSS KB> utime_ms=<用户态CPU ms> stime_ms=<内核态CPU ms>
//        minflt=<次缺页> majflt=<主缺页> nvcsw=<自愿切换> nivcsw=<非自愿切换> rc=<退出码>
//   - ms = CLOCK_MONOTONIC 包住 fork->exec->wait（只含被测程序，10ms 分辨率问题不存在）；
//   - RSS/CPU/缺页/切换 = wait4 的 rusage（内核值，不是采样）；
//   - 绑核 = sched_setaffinity(0,...)，子进程经 fork 继承。
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

static double ms_of(struct timeval tv) {
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

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
        fprintf(stderr, "usage: v2run --pin <cpu> --out <file> [--err <file>] -- <cmd> <args...>\n");
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
            if (fd >= 0) { dup2(fd, 1); close(fd); }
        }
        if (err != NULL) {
            int fd2 = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd2 >= 0) { dup2(fd2, 2); close(fd2); }
        }
        execvp(argv[i], &argv[i]);
        _exit(127);
    }
    if (pid < 0) { perror("fork"); return 4; }
    int status = 0;
    struct rusage ru;
    if (wait4(pid, &status, 0, &ru) < 0) { perror("wait4"); return 5; }
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
    printf("ms=%.3f rss_kb=%ld utime_ms=%.3f stime_ms=%.3f minflt=%ld majflt=%ld nvcsw=%ld nivcsw=%ld rc=%d\n",
           ms, (long)ru.ru_maxrss, ms_of(ru.ru_utime), ms_of(ru.ru_stime),
           (long)ru.ru_minflt, (long)ru.ru_majflt, (long)ru.ru_nvcsw, (long)ru.ru_nivcsw, rc);
    return 0;
}
