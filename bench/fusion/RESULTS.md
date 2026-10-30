# bench/fusion —— 元素链融合（S1）实测

> 本文件由 `bash bench/fusion/run.sh` 的实测输出抄录（该脚本没有生成器，报告格式与
> `bench/par` 保持一致）；口径见 `SPEC.md`。

## 机器与口径

| 项 | 值 |
|---|---|
| CPU | `Intel(R) Core(TM) Ultra 9 275HX` |
| 内核 | `Linux 6.18.54.1-microsoft-standard-WSL2` |
| nproc | `24` |
| C 编译器 | `cc (Ubuntu 15.2.0-16ubuntu1) 15.2.0` |
| extC 前端 | `./build/extc` |
| 钉核（taskset） | `6` |
| RUNS（取中位数） | `5` |
| n | `1048576`（a/res/tmpv 各 8 MiB f64） |
| iters | `8` |
| 旗子 | `cc -O2 -std=c11 -fwrapv`；两臂同一份源码，差别只有 `--no-fuse` |

## 结果

| 臂 | n | 中位 ms | 相对未融合 | cs |
|---|---:|---:|---:|---:|
| 融合（默认） | 1048576 | 16.984 | **0.555×（快 1.80×）** | 1017735235 |
| 未融合（`--no-fuse`） | 1048576 | 30.628 | 1.000× | 1017735235 |

两臂 `cs` 相同 ⇒ 做的是同一份工作；融合臂的生成 C 里没有 `slice_f64 tmpv`（脚本断言）。
满足 `docs/FUSION.md` 的 S1 判据（≥ 1.3×）。剩下的差距来自每轮 `new`/清零与归约的最后一趟，
S2/S3（把归约也折进来）之后再看。
