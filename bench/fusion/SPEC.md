# bench/fusion —— 元素链融合（第 2 层 S1）基准

## 形状

同一份源码编两次，唯一的差别是 `--no-fuse`：

```
n, iters = argv（默认 1048576, 8）
a, res   = 两块 n 个 f64 的视图（a 按 (i*7+3) % 17 填，res 清零）
while k < iters:                     // 每轮分配一块临时视图
    tmpv = new f64[n]
    for i in 0..n: tmpv[i] = a[i] * 2.0 + 1.0     // A：写临时视图
    for i in 0..n: res[i]  = tmpv[i] * 0.5        // B：读临时视图（相邻、同下标）
cs = sum(res)                        // 输出 cs=<i64(cs*1000)> n=<n>
```

融合把 A、B 合成一个循环，`tmpv` 的一次写 + 一次读整趟消失；`--no-fuse` 保留原样。
两臂必须 stdout 逐字节一致，且融合臂的生成 C 里不能出现 `slice_f64 tmpv`。

## 口径

- 机器与口径同 `bench/par`：`cc -O2 -std=c11 -fwrapv`，`taskset -c 6`，RUNS=5 取中位数。
- 计时是外部 `date +%s%N` 包整个进程（含每轮 `new f64[n]` 的分配），毫秒。
- 判据（`docs/FUSION.md`）：n=4M 时 S1 ≥ 1.3×；本机三份 8 MiB f64 在 n=1M 上已越过 L3，
  用 n=1M 作为主表口径。

结果见 `RESULTS.md`（`bash bench/fusion/run.sh` 重跑后抄录）。
