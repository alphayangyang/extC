# Windows / MinGW 跨 OS 复核（R3 附属实验）

**状态**：探针实验，不改 `src/`、不进 Linux 门禁。唯一入库产物是本文件（+ GC-ROOFLINE §6.9.8 + changelog 条目）。
**一句话**：把 extC 生成的 C 原样搬到 Windows 侧用 MinGW-w64 编成原生进程，复核 R3 的三个疑点 —— 结果是 **[work] 与 stdout 跨 OS 逐字相同**、**停顿环境地板在原生侧同样存在**（⇒ 它是这台机器的属性，不是 WSL2 的记账工件）、**`pd_pointer_dense` 的扇出洞在原生侧更严重**（⇒ 不能当成 WSL 现象）。

## 1. 动机（用户建议）

User said (m16294): 「其实你可以试试在Windows侧用mingw编译，对于没有用epoll的程序应该问题不大」。

R3 的核心论点之一是：G1② 里那些 `>10 µs` / `>50 µs` 的慢步，有相当一部分不是 GC 的工作量，而是**宿主抢占**落进了被观测的那一步。当时的证据全部来自 WSL2，而 WSL2 的线程 CPU 钟会把宿主抢占时间也记进 guest 线程（`cpu >= wall` 的来源），所以有一个无法在 guest 内排除的替代解释：**这些停顿是 WSL2 的记账工件**。

原生 Windows 进程给出了一个独立观测点：同一个内核、同一台机器、但有自己真实的线程时间记账。如果地板在原生侧同样存在，地板就是**机器的属性**；如果房子里的慢步在原生侧消失，那才是 WSL2 的问题。

## 2. 工具链

| 项 | 值 |
|---|---|
| WSL 侧 | 无 mingw、无 wine（不可用于交叉编译） |
| Windows 侧编译器 | `C:\ProgramData\mingw64\mingw64\bin\gcc.exe` |
| 版本 | gcc **16.1.0** (x86_64-posix-seh-rev0, Built by MinGW-Builds project) |
| target | `x86_64-w64-mingw32` |
| 线程库 | winpthreads（`libwinpthread.dll` / `libwinpthread.a`） |
| 调用方式 | 从 WSL 走 `cmd.exe /c "..."`（`/mnt/c/Windows/system32/cmd.exe`） |
| 交换目录 | `/mnt/c/extc-win/`（Windows 侧 `C:\extc-win`；Windows 看不到 WSL 的 `/home/...`） |

生成的 C 是单文件、无第三方依赖、不用 `epoll`/`mmap` 运行期原语，所以移植面很小 —— 实测只需要三处 shim。

## 3. 三处 shim（`winpatch.py`，每条 `replace` 都断言 `count == 1`）

### 3.1 `aligned_alloc` 在 MinGW 的 `<stdlib.h>` 里没有声明

生成 C 的 `reclaim.c` 段发射：

```c
s = (ExtcRcSlab *)aligned_alloc(EXTC_RC_SLAB_SIZE, EXTC_RC_SLAB_SIZE);
```

MinGW 的全部 include 里搜不到这个声明（C11 附录 K / POSIX 2001 函数，MSVCRT 没有）⇒ `error: implicit declaration of function aligned_alloc`。改成 `_aligned_malloc(size, align)`，**注意参数顺序相反**，并加 `#include <malloc.h>`。

### 3.2 配对的释放必须跟着改

两处 `free(s);` 都在「释放 slab」路径（`s` 是 `ExtcRcSlab`，`s->big` 区分 `calloc` 出来的大块）。`_aligned_malloc` 的内存**不能**用 `free`（MSVCRT 会 abort）。改成：

```c
if (s->big) free(s); else _aligned_free(s);
```

### 3.3 裸终端 extern 没有实现

生成 C 里有 `extern int32_t tcsetattr(int32_t, int32_t, uint8_t *);`，被 `extc_raw_leave` 引用 ⇒ 链接期 `undefined reference to tcsetattr`。加三个 stub（这些形状从不进 raw 模式）：

```c
extern int32_t tcsetattr(int32_t fd, int32_t act, uint8_t *t) { (void)fd; (void)act; (void)t; return 0; }
extern int32_t tcgetattr(int32_t fd, uint8_t *t) { (void)fd; (void)t; return 0; }
extern void cfmakeraw(uint8_t *b) { (void)b; }
```

插在 `#include <malloc.h>` 之后。**除此之外不需要改动**：`clock_gettime(CLOCK_MONOTONIC)`、`clock_gettime(CLOCK_THREAD_CPUTIME_ID)`、`__builtin_ia32_rdtsc` 在 MinGW 下都能编能跑。

## 4. 复现步骤

```bash
# 1) 生成 C（Linux 侧，正常 extC 工具链）
EXTC_RECLAIM=1 ./build/extc -I stdlib tests/reclaim-shapes/cyclic_pressure.extc -o /mnt/c/extc-win/cyc.c

# 2) 打三个 shim（读 cyc.c 写 cyc_win.c）
python3 /mnt/c/extc-win/winpatch.py /mnt/c/extc-win/cyc.c

# 3) 编（Windows 侧 gcc）
cmd.exe /c "C:\ProgramData\mingw64\mingw64\bin\gcc.exe -O2 -std=c11 -o C:\extc-win\cyc_win.exe C:\extc-win\cyc_win.c"

# 4) 跑（bat 里 set EXTC_RC_PAUSE=1 且 **清空** EXTC_RC_STEP_CPU，见 §7）
cmd.exe /c "C:\extc-win\run_wall.bat cyc_win.exe 20000000 cyc_w1"
```

`run_wall.bat` 的实质只有四行：`@echo off` / `cd /d C:\extc-win` / `set EXTC_RC_PAUSE=1` / `set EXTC_RC_STEP_CPU=` / `%1 %2 2> %3.err > %3.out`。形状源码与用途：

| 名字 | 源 | 参数 | 用途 |
|---|---|---|---|
| cyc | `tests/reclaim-shapes/cyclic_pressure.extc` | 20000000 轮 | 活集 2.7 MB 的环，`cold_steps=0` |
| pb | `tests/gc-p0/src/pb_escape_grow.extc` | 50000000 | 逃逸增长，巨活集（冷档） |
| pc | `tests/gc-p0/src/pc_cycle_churn.extc` | 150000000 | 环 churn，短步 |
| dense | `tests/gc-p0/src/pd_pointer_dense.extc` | 200 轮（8 MiB 块 / 1M 非空指针） | 扇出形状，R4 的洞 |
| stall | `tests/gc-p0/noise_floor.c` | `--rate 0.3` | 纯循环停顿探针 |

## 5. 结果一：`[work]` 与 stdout 跨 OS 逐字一致

这是本轮最干净的一条：**工作量契约是发射文本的性质，与 OS 无关**。

| 形状 | `[work] n` Linux (r3v29) | `[work] n` Windows 原生 | `walk_max` | 判定 |
|---|---:|---:|---|---|
| cyc 20M | 144,016 | **144,016** | 256 / 256 | 逐字相同 |
| pb 50M | 333,516 | **333,516** | 1024 / 1024 | 逐字相同 |
| pc 150M | 6,866 | **6,866** | 1 / 1 | 逐字相同 |
| dense 200 轮 | 1,469,841–1,480,248 | ≈1.48–1.53 M | 1024 | **不满足确定性**（见 §8） |

`[histo] n` 也逐字相同（cyc 156,486；pb 343,296；pc 41,196），stdout 内容 `cmp` 只差 `\r\n`（Windows 文本模式把 `\n` 写成 `\r\n`，所以必须用文本模式读、不要拿二进制 diff 当差异）。cyc 的 stdout 全文：

```
rounds=20000000 retired=2222208 live=128 fwd=2559991744 back=2559991744 idx=20000 cache=1024 hits=15555520 depth=208 tree=true
```

## 6. 结果二：停顿分布（wall 口径，`EXTC_RC_PAUSE=1`）

### 6.1 环境地板在原生侧同样存在

`stallrate.exe --rate 0.3`（纯整数循环，`noise_floor.c` 的 Windows 编译）两次：

| 量 | Windows 原生 | WSL2（R3 门禁轮） |
|---|---:|---:|
| `rate10_per_s`（每 10 µs 停顿的到达率） | **170.0 / 223.3** | 150–480 |
| `rate50_per_s` | **50.0 / 50.0** | 23–77 |
| `max_us` | **313.3 / 167.8** | 80–537 |
| `tsc_per_us` | 3072.0 | — |

同一个数量级 ⇒ 这些停顿**不是** WSL2 引入的。

### 6.2 三个「短步」形状

| 形状 | `[histo] n` | Linux raw `gt10us`/`gt50us`（r3v29 r1） | Windows 原生 `gt10us` | Windows 原生 `gt50us` | `over1ms` | 备注 |
|---|---:|---|---|---|---:|---|
| cyc 20M | 156,486 | 209 / 10 | 61 / 70 / 66 | 1 / 5 / 1 | 0 | 偶发 1 条落 64–128 µs 桶 |
| pb 50M | 343,296 | 239 / 26 | 98 / 89 | 14 / 16 | 0 | 64–128 µs 桶 11 / 13 |
| pc 150M | 41,196 | 5 / 0 | 0 / 1 | 0 / 1 | 0 | `[segsteps] n=0` |

三边读法不同，不要直接比数：Linux 那一列是 `min(cpu, wall)` 的**原始**计数（扣减前）；门禁判据用的是扣掉环境地板后的**净**计数（`cyc` `>50 µs` 净 8.73e-05、p99.9 净 `count(>10 µs)=252`/3 reps ≈ 84/rep；`pb` `>50 µs` 净 5.63e-05、p99.9 净 0；`pc` `>50 µs` 净 8.09e-06，均 PASS）。原生侧没有探针、也不扣减，所以它的 raw 里**本来就含地板**：用 §6.1 的到达率乘墙钟（pb 的墙钟 0.88 s）得地板期望 150–196 次，与实测 89–98 次同阶（相差 <2×）⇒ 原生侧这些计数主要是**机器停顿**，不是 GC 工作量。两个 OS 的结论一致：这三个形状的 GC 自身慢步在计数量级上不可见。

### 6.3 `dense`：扇出洞在原生侧更严重

| 量 | Linux（r3v29，5 reps） | Windows 原生（5 reps） |
|---|---:|---:|
| `[histo] n` | 1,512,013–1,522,692 | ≈1.52 M |
| `gt10us`（占比） | 22,800–24,731（≈1.5%） | 16,056 / 14,273 / 17,976 / 16,140 / 16,090（≈**1.06%**） |
| `gt50us` | 133–164 | 89 / 82 / 72 / 78 / 79 |
| `[segsteps] max_us` | **165.1 / 285.9 / 193.8 / 182.1 / 198.8** | **1411.4** |
| `[histo]` ≥1 ms 桶 | **0**（b512us 0–1） | **每 rep b1ms=1–2 且 b4ms=1** |
| `[work] n` | 1,469,841–1,480,248 | ≈1.48–1.53 M |

口径必须说清，否则会误读成「Linux 判绿、Windows 判红」：

- Linux 侧的 `(b)` 段通道 `pd_pointer_dense` 是 **PASS**（`B_seg=74.69 µs`，p99.9 PASS，`max=285.9` 按 m04591 的裁决 **只 OBS 不判**）。Linux 上真正判红的是 **`(a-cold)` 的 `tau_t`**：`[top] cpu_us=695.6 / pops=128` ⇒ **5.434 µs/pop**（判据 ≤2 µs），而同一批数据的 wall 遍只报 1.573 µs/pop —— 两支通道口径不一致（R4 的口径项）。机制是同一个：8 MiB 里 1 M 个非空指针把标记栈推到 `msp≈830k` / `mscap=1,048,576`，`realloc` 扩容 + 冷页缺页落在一步里；`[work] push_max=0` 说明 **push 完全没进步预算**。
- 原生 Windows 侧把同一条尾巴放大：Linux 的 5 reps 最大 285.9 µs、没有 ≥1 ms 的步；原生侧 `[segsteps] max=1411.4 µs`，且**每个 rep 都有 1–2 步落 ≥1 ms、1 步落 ≥4 ms**。⇒ 这个洞与 OS 无关，且在宿主机时间记账更粗的那一侧更严重。
- 给 R4 的下界：修完之后原生侧也必须复核（Linux 判据数字一个不改）。

## 7. MinGW 的 `CLOCK_THREAD_CPUTIME_ID` 不能用来量步

`EXTC_RC_STEP_CPU=1`（cpu 口径）时 cyc 的 `[histo]` 变成 `gt10us=11`、15.6 万条样本里 15.6 万条落 `<1 µs` 桶 ⇒ MinGW 的线程 CPU 钟粒度是 `GetThreadTimes` 量级（远粗于 Linux 的时钟精度），大量读数被截断为 0。

⇒ **Windows 侧只能用 wall 口径**；这不是判据问题，是移植限制（Linux 门禁继续用 `min(cpu, wall)`）。这也是为什么 §4 的 bat 里第一件事就是 `set EXTC_RC_STEP_CPU=` 清空。

## 8. 局限（本复核不能替代 Linux 门禁）

1. **量具不同**：Windows 侧没有 ASan / valgrind / Ir 口径，RSS 也没测（没有对等的 `%M`），所以它只覆盖「停顿分布 + work/stdout 一致性」，不覆盖内存与指令数契约。
2. **`dense` 的 `[work] n` 跨 rep 有 ±1% 波动**（Linux 1.4698–1.4802 M、原生 ≈1.48–1.53 M；两侧都如此，`pressure_steps≈11,780` 的压力触发路径）⇒ dense 不能套「三跑逐字相同」的棘轮；cyc/pb/pc 可以（实测逐字相同）。这条在 Linux 侧是隐藏的（该形状本来就没套 work 棘轮）。
3. **只复核了 4 个形状**，不是全部 1,300 个测试；`promote`/`chase`/`cycleratio`/`sizeratio` 与 digraph 没搬（依赖更多 stdlib 面）。
4. **未纳入 CI**：需要 Windows runner；本次的 gl 产物只在 `/mnt/c/extc-win/`（`winpatch.py`、`run_wall.bat`、`run_cyc.bat`、`stallrate.exe`、`*_win.exe`），不入库。

## 9. 由本实验得到的两条实结论

1. **环境地板是机器的属性，不是 WSL2 记账工件**（§6.1）。GC-ROOFLINE §6.9.2 的措辞据此收窄：WSL2 的影响只是「把它记进线程 CPU 钟」（这是 `cpu >= wall` 的来源），扣减协议与阈值**一个字不用改**。
2. **`dense` 的扇出洞与 OS 无关，原生侧更严重**（§6.3）⇒ R4 的两条修法（push 计入步预算 `charge = pops + pushes`、标记栈改块链）优先级最高；且 R4 验收时必须至少复核一次原生侧。同时 §6.3 也把 R3 那条红的**判据归属**讲清了：Linux 上红的是 `(a-cold)` 的 `tau_t`（cpu 遍 5.434 µs/pop），不是 `(b)` 段通道（PASS，`max` 只 OBS）—— 原生侧的 1.4 ms / ≥4 ms 步是同一机制的更粗一侧观测。

另一条附带事实：**`[work] n` / `walk_max` 是发射文本的性质**（cyc/pb/pc 跨 OS 逐字相同），这对「工作量界是唯一可靠窗口判据」这个前提是个正面证据。