# bench/fusion —— 元素链融合（第 2 层 S1/S3）基准

## 形状

同一份源码编两臂（默认融合 / `--no-fuse`），再加一个**手写融合**的 C 臂作参照。两个形状：

**S1 元素链**（`pipeline.extc` / `pipeline.c`）：

```
n, iters = argv（默认 1048576, 8）
a, res   = 两块 n 个 f64 的视图（a 按 (i*7+3) % 17 填，res 清零）
while k < iters:
    tmpv = new f64[n]
    for i in 0..n: tmpv[i] = a[i] * 2.0 + 1.0     // A：写临时视图
    for i in 0..n: res[i]  = tmpv[i] * 0.5        // B：相邻、同下标读
cs = sum(res)
```

**S3 流式归约**（`stream.extc` / `stream.c`）：两层元素链 + 一段升序归约，三处全化掉：

```
while k < iters:
    tmpv = new f64[n];  for i: tmpv[i] = a[i] * 2.0 + 1.0
    tv2  = new f64[n];  for i: tv2[i]  = tmpv[i] * 0.5
    part = 0.0;         for i: part = part + tv2[i]
    cs = cs + part
```

两臂必须 stdout 逐字节一致；融合臂的生成 C 里不能出现 `slice_f64 tmpv`（S3 还有 `tv2`），
`--no-fuse` 臂里必须还在。

## 口径

- 机器与口径同 `bench/par`：`cc -O2 -std=c11 -fwrapv`（C 参照臂同一组），`taskset -c 6`，
  RUNS=5 取中位数；计时用外部 `date +%s%N` 包整个进程（含每轮 `new f64[n]` 的分配），毫秒。
- 判据（`../extc-docs/design/FUSION.md`）：S1 ≥ 1.3×、S3 ≥ 2× 相对未融合；不满足判据的写法不许变慢。
- 三个臂：`fused`（编译器融合）、`unfused`（`--no-fuse`）、`hand`（`pipeline.c` / `stream.c`
  里的人肉融合写法）。

结果见 `RESULTS.md`（`bash bench/fusion/run.sh` 重跑后抄录）。
