# bench/viper — Viper 1.0.6-dev.194 端口与对比卡具

把 gcbench v2 的四个形状移植到 Viper，用与 extC 相同的交错计时卡具做同纪律横比。
结论与分析见 `docs/analysis/VIPER-CROSSLANG.md`；本文件只写怎么跑。

## 文件

| 文件 | 作用 |
| --- | --- |
| `chase.vp` | chase 端口（1M 节点 / 4M 步 / 每 256 步 4 个即抛对象） |
| `promote.vp` | promote 端口（500,000 个全垃圾对象 + 保留链） |
| `cycleratio.vp` | cycleratio 端口（256 槽窗口，槽成环） |
| `sizeratio.vp` | sizeratio 端口（4 个尺寸类，2M 次分配） |
| `ab.py` | 交错计时卡具：`ARMS="name=path,..." REPS=11` 绑核跑，stdout 逐字节比对，`/usr/bin/time` 取峰值 RSS |
| `setup.sh` | 取 MLIR 21（apt 包的 deb 解包，不动系统）并在 `/tmp/viper` 生成丢弃 Windows 导入库的 linkwrap |

每个 `.vp` 都是自包含的 viperbuild 项目（`import viperbuild as vb` + `proj = vb.Project(...)` +
`proj.config(...)` + `proj.build(...)`），因为 ViperC 要求入口文件里出现 `proj` 绑定，且会把
项目目录下所有 `.vp` 一起编译，所以每形状必须独立目录、一次只放一个文件。

## 前置条件

- 本仓库外的一份 Viper 分发物，解包后是 `/tmp/viper/V_L64/viperc` 与 `/tmp/viper/V_L64/includes/`。
  `.vp` 里的 `includes` 与 `linker` 是绝对路径，放在别处要一起改。
- MLIR 21 的 `mlir-opt` / `mlir-translate`（ViperC 在 Linux 上没有 `ircore.dll`，PhaseB 回落到
  调这三个子进程的旧路径）。`sh setup.sh` 会从 Ubuntu 源取 `libmlir-21` 与 `mlir-21-tools`
  的 `21.1.8-6ubuntu1` 解到 `/tmp/mlirroot`。
- `llc` / `clang`（系统 LLVM 21）。

## 跑法

    sh bench/viper/setup.sh
    export PATH=/tmp/mlirroot/usr/lib/llvm-21/bin:$PATH
    export LD_LIBRARY_PATH=/tmp/mlirroot/usr/lib/x86_64-linux-gnu:/tmp/mlirroot/usr/lib/llvm-21/lib

    # 每个形状一个干净目录；把对应的 .vp 拷进去再编（生成物落在 __vpcbuild__/output/）
    mkdir -p /tmp/vp-chase && cp bench/viper/chase.vp /tmp/vp-chase/
    cd /tmp/vp-chase && /tmp/viper/V_L64/viperc --src chase.vp
    ./__vpcbuild__/output/chase.exe

    # 六个臂交错计时（viper / cpp careful / cpp naive / extC default / extC off / rust）
    ARMS="viper=/tmp/vp-chase/__vpcbuild__/output/chase.exe,cpp_careful=...,extc_default=...,extc_off=...,rust=..." \
      REPS=11 python3 bench/viper/ab.py /tmp/vp-chase chase 1048576 20261013 4000000

其余形状的参数：`promote 500000`、`cycleratio 400000 20261013 100`、`sizeratio 2000000 20261013`。

## 口径

- Viper 端口统一用 `memhub.MemPool`（arena bump）分配、**永不释放**，与 extC 的
  `EXTC_RECLAIM=0` 臂同纪律。这不是 Viper 的语言最优解（`MemSlab` / `MemBuddy` 可以手动 free），
  选它是因为要让「回收 vs 不回收」这一个变量可读。
- 六个臂的 stdout 必须逐字节相同，卡具会把不一致标成 `DIFF` 并最终失败。
- 绝对毫秒只与同一轮次内的其它臂可比；本机跨会话漂移 5–13%。
