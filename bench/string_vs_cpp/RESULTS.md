# extC `stl::string`（SSO）对 C++ `std::string` —— 同口径横评

> **自动生成**：`bench/string_vs_cpp/run.sh` 量出 `raw.txt`，`report.py` 出这张表。别手改。
> 12 个场景，两边**同样的次数、同样的字节、同样的公式**；每个场景各打印一个整数校验和，
> **两边逐位相同**才算做了等价的工作。

一句话：**12 格里 extC 慢 11 格、快 1 格、打平 0 格**；最大的一格是 `find`（23.6×），最大的一次领先是 `long_build_chunk`（0.62×）；
绝大多数格子差在 1.0×~6×，来源是**生成的 C 没被内联**（方法调用 + 池守卫）与
**libstdc++ 用了手写向量化的 `memchr`/`memcmp`**，而不是容器的数据结构本身。

## 机器与工具链

| 项 | 值 |
|---|---|
| CPU / 核数 | Intel(R) Core(TM) Ultra 9 275HX · 6.18.33.2-microsoft-standard-WSL2（`nproc` = 24） |
| 内核 | 6.18.33.2-microsoft-standard-WSL2 |
| `gcc` | gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0 |
| `g++` | g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 |
| extC 前端 | `./build/extc` |

编译命令（两边都是 `-O2`，extC 走它自己的既定契约）：

```
./build/extc -w --no-line-map -o s.c bench/string_vs_cpp/s.extc && gcc -O2 -std=c11 -fwrapv -o s s.c
g++ -O2 -std=c++17 -o cpp bench/string_vs_cpp/cpp.cpp
C++ 侧 short_many 另有一道 asm volatile barrier（见 cpp.cpp / RESULTS.md）
```

extC 侧**不用 `--run`**：前端只吐 C，编译时间不进测量窗口；两边量到的都是编译产物。

## 方法

- **计时**：外部 python `time.perf_counter()` 包住整个进程（程序里不放计时器，两边口径一样）。
  每个场景先**冷启动预热一次**（不计时），再 **best of 5**（报毫秒），
  每次测量都是一个独立进程、只跑一个场景（RSS 才干净）。
- **RSS**：/usr/bin/time -v 的 Maximum resident set size (kbytes)，每格单独跑 2 次取最大
- **校验和**：每格跑完打印 `名字 cs=整数`，run.sh 比对两边的 cs；
  同一次测量的预热/计时/RSS 各次之间也要求 cs 一致（不一致直接报错退出）。
- **随机性**：所有字节由同一条公式 `((i*1103515245+12345)>>16)&255` 产生；
  `read_at` 的下标用同一个 xorshift64（常数 88172645463325252）。两边一个字都不差。
- **反折叠**：`short_many` / `short_cross` 的内容是常量，`g++` 会把整个循环折成常数
  （实测内层 0.000 ms），所以两边都接受一个可选的 `argv[2]` 盐水（默认 0，此时字节与不加盐相同）；
  C++ 侧在 `short_many` 里还加了一道 `asm volatile` 空 barrier。见「不公平的地方」。

## 逐场景结果

比值 = **extC ÷ C++**（>1 = extC 慢，<1 = extC 快）。时间 = best of 5，毫秒。
RSS = `/usr/bin/time -v` 的 Maximum resident set size（KB，每格单独跑 2 次取最大）。

| 场景 | 做什么 | extC 时间 | C++ 时间 | 比值 | extC RSS | C++ RSS | 校验和 |
|---|---|---|---|---|---|---|---|
| `short_many` | 100 万次「默认构造 + push 3 字节 + 读回长度」 | 4.411 ms | 1.650 ms | **2.67×** | 1452 KB | 4312 KB | `102000000` |
| `short_cross` | 100 万次「默认构造 + push 到 20 字节」（跨过 16 字节的内联缓冲） | 78.6 ms | 48.0 ms | **1.64×** | 1452 KB | 4312 KB | `71000000` |
| `long_build_byte` | 一条长串逐字节 push 到 400 万字节（不预分配） | 13.2 ms | 7.353 ms | **1.79×** | 5276 KB | 11364 KB | `4126991` |
| `long_build_chunk` | 一条长串用 4 KB 块 append 到 64 MB | 37.4 ms | 60.4 ms | **0.62×** | 66972 KB | 69336 KB | `67116943` |
| `reserve_growth` | reserve 到 64 MB 再逐字节写满 | 201.6 ms | 49.7 ms | **4.06×** | 66780 KB | 68992 KB | `67116943` |
| `read_at` | 1 MB 串上随机位置读 1000 万次 | 25.4 ms | 16.8 ms | **1.51×** | 2268 KB | 4480 KB | `1275976381` |
| `find` | 1 MB 串上找 1 万个不同的短模式（长 4~15，各一次） | 3175.6 ms | 134.3 ms | **23.64×** | 2264 KB | 4600 KB | `30744656` |
| `compare` | 100 万次两条 32 字节串的 == 与 < | 47.4 ms | 16.8 ms | **2.82×** | 1512 KB | 4236 KB | `500019` |
| `substr_copy` | 10 万次 sub(1000,2000)（拷贝 1000 字节）到 1 MB 串上 | 5.671 ms | 2.709 ms | **2.09×** | 2268 KB | 4472 KB | `128800000` |
| `clone_many` | 100 万次 clone 一条 32 字节串 | 51.3 ms | 8.225 ms | **6.24×** | 1452 KB | 4224 KB | `134000000` |
| `clear_shrink_cycle` | 1 万轮「写满到 64 KB → clear → shrink」 | 150.4 ms | 145.3 ms | **1.04×** | 1516 KB | 4140 KB | `657909904` |
| `many_live_short` | 同时活着 100 万条 8 字节串（数组里），最后汇总长度 | 57.1 ms | 23.9 ms | **2.39×** | 71516 KB | 34616 KB | `85500014` |

<details><summary>best of 5 的离散度（最小/中位/最大，毫秒）</summary>

```
short_many         extc min=   4.411 med=   4.443 max=   4.835
short_many         cpp  min=   1.650 med=   1.690 max=   1.778
short_cross        extc min=  78.572 med=  79.516 max=  79.646
short_cross        cpp  min=  48.025 med=  49.998 max=  52.779
long_build_byte    extc min=  13.186 med=  13.438 max=  13.781
long_build_byte    cpp  min=   7.353 med=   7.504 max=   7.709
long_build_chunk   extc min=  37.424 med=  38.577 max=  46.789
long_build_chunk   cpp  min=  60.403 med=  61.333 max=  62.041
reserve_growth     extc min= 201.599 med= 205.457 max= 208.947
reserve_growth     cpp  min=  49.668 med=  50.044 max=  51.004
read_at            extc min=  25.369 med=  26.126 max=  27.581
read_at            cpp  min=  16.761 med=  17.253 max=  17.529
find               extc min=3175.645 med=3187.156 max=3193.935
find               cpp  min= 134.327 med= 158.986 max= 165.531
compare            extc min=  47.393 med=  47.758 max=  59.658
compare            cpp  min=  16.798 med=  16.862 max=  17.305
substr_copy        extc min=   5.671 med=   5.708 max=   5.968
substr_copy        cpp  min=   2.709 med=   2.857 max=   2.931
clone_many         extc min=  51.308 med=  52.022 max=  54.531
clone_many         cpp  min=   8.225 med=   8.367 max=   8.812
clear_shrink_cycle extc min= 150.403 med= 152.038 max= 154.500
clear_shrink_cycle cpp  min= 145.281 med= 146.323 max= 149.024
many_live_short    extc min=  57.122 med=  59.191 max=  69.288
many_live_short    cpp  min=  23.870 med=  24.749 max=  27.403
```
</details>

## 逐格读法

- **`short_many`（2.67×）**：两边都**一次分配都没有**（extC 内联 16 字节 / libstdc++ SSO 15 字节），RSS 都等于各自空程序的地板（1452 KB / 4312 KB）。差的是每字节的成本：生成的 C 里 `push` 是一次真实调用（还要过一次池世代守卫 `pStale()`），而 `push_back` 被内联、容量检查在常量传播之后消失了。
- **`short_cross`（1.64×）**：第 17 个字节升级 —— extC 建池拿 32 字节块，libstdc++ 在第 16 个字节 malloc 30 字节。每轮都归还（`release()` / 析构），所以 RSS 只有地板值。这一格是「一次升级 + 一次归还」多少钱：**78.6 ns/轮 vs 48.0 ns/轮**。
- **`long_build_byte`（1.79×）**：400 万次逐字节 push。extC 走 0→内联→32→**1.5 倍**（30 次增长，末容量 3.9 MiB），libstdc++ 走 SSO→30→**2 倍**（19 次分配，末容量 7.5 MiB）。所以 extC 时间慢一点（13.2 vs 7.4 ms），**RSS 只有一半**（5276 vs 11364 KB）—— 1.5 倍策略拿复制次数换峰值内存，这一格它划算。
- **`long_build_chunk`（0.62×，extC 赢）**：4 KB 块 append 到 64 MB。extC 的 `grow` 走 `poolResizeRaw` → `realloc`（大块上 glibc 通常 `mremap` **原地长**，不复制），libstdc++ 每次扩容都是「新分配 + memcpy + 释放」。按模型算，extC 名义上要搬 133 MiB、C++ 只要搬 64 MiB，实测却是 extC 快（37.4 vs 60.4 ms）—— 差额就是「原地长」省下的复制。RSS 也更低（66972 vs 69336 KB）：extC 末容量 66.5 MiB 但尾部没写过，不计入常驻。
- **`reserve_growth`（4.06×）**：一次 reserve 到 64 MB，之后 6400 万次逐字节写。两边都没有扩容，纯粹是「每字节的写入成本」：extC 3.0 ns/字节 vs C++ 0.74 ns/字节。差的就是那一次跨函数调用 + 每字节的池守卫；C++ 侧被内联成一条存储。
- **`read_at`（1.51×）**：**这一格两边不同口径**：extC 用有界检查的 `at(i) -> ?u8`，C++ 用无检查的 `s[i]`。2.5 ns/次 vs 1.7 ns/次，多出来的就是边界比较 + option 解包 （外加同样没被内联的那次调用）—— 这是 extC 安全契约的明账，不是白丢的。
- **`find`（23.64×，最大的一格）**：两个都是线性算法（extC 是 Two-Way，libstdc++ 是 `memchr` 找首字节 + `memcmp` 比其余），扫的是同样的字节。差在实现：`memchr`/`memcmp` 是手写向量化（AVX2）的 libc 例程，Two-Way 是逐字节标量循环。3176 ms vs 134 ms ⇒ **1.39 GB/s vs 32.8 GB/s** 的有效扫描带宽（这 1 万个模式按公式算下来共扫 4.41 GB：约 42% 的模式找不到、要扫满 1 MiB，命中的那些大多在前几百字节就命中了；1 MiB 的串能待在片上缓存里，正是向量化的甜点）。顺带：这 1 万次 `find` 的结果在 python 里独立重算过一遍，和两边的 cs 一字不差。
- **`compare`（2.82×）**：两条 32 字节串每轮各转一格（push + removeFront / push_back + erase），然后 `==` 与 `<`。extC 的 `==`/`<` 是自己写的逐字节循环（`bytesEq`/`bytesLt`），C++ 的是 `memcmp` —— 32 字节的数据在寄存器里，memcmp 的常数开销更小。另外 extC 每次比较都经过一次未内联的方法调用。
- **`substr_copy`（2.09×）**：10 万次拷 1000 字节。extC 的 `sub` 是 `new u8[1000]` + `copyInto`，分配落在**循环体那一轮的 arena** 里、每轮整块回收（bump 分配，没有 free 开销）；C++ 是 `substr` 的 malloc + free。RSS 2268 vs 4472 KB —— arena 的整块回收比 malloc/free 更稳。
- **`clone_many`（6.24×，第二大）**：100 万次 32 字节深拷贝。C++ 是 `operator new(32)` + memcpy + `operator delete`（8.2 ns/次）；extC 是 `clone()` → `append()` → `grow()`：建池（`extc_pool_new_at` + 槽表/区域链）+ 拿 32 字节板块 + 拷贝，归还时还要 `poolResize(0)` + `pool_reset` + `pool_drop`。池的簿记比 malloc/free 重，这一格是它的账。
- **`clear_shrink_cycle`（1.04×，打平）**：1 万轮写满 64 KB 再缩回去。extC 每轮 20 次增长、C++ 每轮 5 次分配，但两边都被 640 MB 的 memcpy/清零支配，所以打平（150.4 vs 145.3 ms）。
- **`many_live_short`（2.39× 时间、RSS 2.07×）**：两边都零池零堆（8 字节装得下内联/SSO），差别就在**值的大小**：extC 的 `string` 是 **72 字节**（`small[16]` + `n` + `cap` + `?mut slice<u8>`（tagged option 24 字节）+ `pid` + `pidGen`），libstdc++ 的 `std::string` 是 **32 字节**（16 内联 + size + 指针借用位）。100 万条 ⇒ 71516 KB vs 34616 KB，正好是 72:32（另外 extC 侧 `new string[1e6]` 要清零 72 MB，C++ 的 vector 只清零 32 MB —— 这一项也算在 extC 头上）。

## 这张表说明了什么

1. **短串路径确实一次分配都没有**：`short_many` / `short_cross` 的 RSS 就是各自的地板（extC 两格都是 1452 KB，C++ 两格都是 4312 KB；对照 `noop` 地板 extC 1516 KB / C++ 4224 KB），100 万次构造 + 写入没有让常驻内存动一下；`short_cross` 每轮「升级 + 归还」也没有留下一代旧块。
2. **常规操作的差距主要来自 codegen，不是容器设计**：`push` / `at` / `==` 在生成的 C 里都是**未内联的真实调用**（`push` 还每字节过一次池世代守卫），而 `push_back` / `operator[]` / `memcmp` 都被内联或直接调 libc。1.0×~2.8× 的那几格基本就是这个差。
3. **`find` 是唯一一个数量级级别的差距**（24×）：两个都是线性算法，差的是 libstdc++ 用 `memchr`/`memcmp`（向量化）而 extC 的 Two-Way 是标量循环。这不是「Two-Way 选错了」，是「没有用上 libc 的向量化例程」。
4. **增长策略是一笔真账**：1.5 倍（extC）少占内存（400 万字节那格 5276 KB vs 11364 KB，约一半），2 倍（libstdc++）少复制。有意思的是 4 KB 块 append 到 64 MB 那一格 **extC 反而快**（37.4 vs 60.4 ms），因为它的扩容走 `realloc`，大块上 glibc 会 `mremap` 原地长 —— 名义上要搬 133 MiB，实际几乎没搬。
5. **池的簿记比 malloc/free 重**：`clone_many`（建池 + 拿块 + 归还）6.2×、`short_cross`（升级 + 归还）1.6× —— 这是 extC 内存模型（可校验的世代 + 区域回收）的价格。
6. **同时活着的短串，extC 的值更胖**：72 字节 vs 32 字节（RSS 2.07×，71516 KB vs 34616 KB）。SSO 省下的是分配，不是常驻结构；`?mut slice<u8>` 这个 tagged option 一个人就占 24 字节，换算成 100 万条就是 24 MB。
7. **RSS 的地板不一样，而且差在地板上**：`noop`（什么都不做、**连 `std::string` 都没碰**）的 RSS 就已经是 extC **1516 KB** / C++ **4224 KB** —— 这 2.6 MB 的差是 C++ 运行时（libstdc++ 的静态初始化与被触及的页）的地板，**每一格都含它**。所以小的那几格 RSS 差（~1.5 MB vs ~4.3 MB）不是字符串数据占的；`many_live_short` 那 2.07×（71516 vs 34616 KB）才是真数据。

## 这张表不能说明什么

- **不能读成「extC 比 C++ 慢 N 倍」**。11/12 格落在 1.0×~6×，且如上面第 2 条所说，主要来源是「生成的 C 有没有被内联 / 有没有调 libc 向量化例程」。要拿这张表说容器设计，只能看内存那几列和增长策略那两条。
- **单线程、单机、WSL2、笔记本 CPU**：best of 5 只能压掉一部分噪声，同机负载与降频仍有百分之几的漂移；不要读最后一位小数，也别拿它预测别的机器。
- **只有墙钟和峰值 RSS**：没有 perf counter、没有 cache miss、没有分配器统计，也测不到「谁在什么时候占的内存」。RSS 是**峰值**，且包含运行时地板。
- **计时窗口含进程启动 + 运行时**（同一口径实测的 `noop` 地板：extC 0.38 ms / C++ 0.68 ms）：对 `short_many` 这种小格（extC 4.41 ms / C++ 1.65 ms）就是 9% / 41% 的常数项。两边都有，但它会**压缩比值**（把真实的倍数拉小），所以小格子的比值要按「扣掉地板」再读一遍。
- **校验和只证明「长度与内容一致」**，不证明每次操作的次数一模一样（比如容量策略就没进校验和：extC 20 字节后 capacity=32，libstdc++ 是 30，两边都不打印 capacity）。
- **没测的**：多线程/并发、异常与错误路径、真实 IO、长时间运行下的碎片、以及 `string` 的 `==`/`<` 之外的操作符重载（`+`/`+=` 在 extC 里还没有）。

## 我这边不公平 / 不对称的地方（诚实清单）

1. **C++ 的 `short_many` 加了一道 `asm volatile` barrier**（`BENCH_BARRIER(s)`，不产生指令）。不加的话 `g++` 会把这 100 万次 push 折成常数（实测内层 0.000 ms）——那一格就没量到东西。extC 侧没加，也没必要加：它生成的 C 里 `push` 是真实跨函数调用，`gcc` 折不掉。**这道 barrier 只阻止过度优化，但它确实是单边的。**
2. **`release()` 的账算在 extC 头上**：场景 2 与 10 里，C++ 靠析构函数归还，extC 必须显式调 `release()`（`poolResize(0)` + `pool_reset` + `pool_drop` + 世代检查），比「比较 + `operator delete`」重。反过来，**不写 `release()` 池就不会在轮次间回收**：旁证那次 100 万轮 20 字节串留下了 155052 KB（对照场景 `short_cross` 的 extC RSS 1452 KB）。
3. **`read_at` 两边不同口径**：extC 用有界检查的 `at() -> ?u8`，C++ 用无检查的 `operator[]`。这一格的 extC 数字里含安全契约的成本（边界比较 + option 解包），这是**明账**。
4. **`find` 的入参构造对 C++ 略不利**：extC 传的是切片（零拷贝），C++ 每轮构造一个临时 `std::string`（长度 ≤15 ⇒ SSO，不分配）。相对于 1 MB 的扫描可以忽略，但它是存在的。
5. **`substr_copy` 的下标语义容易看错**：extC 的 `sub(lo, hi)` 是半开区间，所以 C++ 侧写的是 `s.substr(1000, 1000)`（起点, **长度**）——两边都是同一段 1000 字节。
6. **`many_live_short` 的清零量也不一样**：extC 的 `new string[1e6]` 由语言承诺清零 72 MB，C++ 的 `std::vector<std::string>(1e6)` 值初始化 32 MB。RSS 的 2.06× 里有一部分是「清零的字节数」，不全是常驻结构的大小（结构本身是 72 vs 32 字节/条）。
7. **哪边被内联是编译器的运气**：extC 的 `push`/`at`/`==` 这次都没被 `gcc` 内联，C++ 的 `push_back` 被内联且容量检查被常量传播消掉。如果哪天 `gcc` 决定内联 `string$string_push`，`reserve_growth` / `short_many` 那几格会变，而这不是字符串实现变了。
8. **RSS 地板**：C++ 侧只要用了 `std::string` 就多约 2.6 MB libstdc++ 常驻，extC 侧是纯 C 运行时；小格子的 RSS 差主要是它，不是数据。
9. **启动地板也是单边的（这一条对 C++ 不利）**：同一口径量的 `noop` 是 extC 0.380 ms vs C++ 0.683 ms，C++ 每一格都多带 0.303 ms 的固定成本（更大的二进制 + libstdc++ 初始化）。对 `short_many` 这种小格，那是 C++ 计时里的 18% —— 也就是说，**小格子里 C++ 的真实优势比表上更大**；反过来 `find` 那种长格子可以忽略。

## 旁证：池的寿命挂在「地方」上（不是 12 个场景之一）

`run.sh` 里还有一个小程序：100 万轮「默认构造 + push 到 20 字节」，**但一轮都不 `release()`**。
它的 RSS 是 **155052 KB**（cs=71000000，与场景 `short_cross` 的 cs 相同 —— 干的是同一件事），而场景 `short_cross`（每轮 `release()`）的 extC RSS 只有 **1452 KB**。
原因是 `string` 升级时建的池落在 `push` 的 **home 地方**上，而这个 home 是**函数体**那个地方：
循环体每轮退出并不会回收它，函数不返回就一直留着。也就是说 ——
**在循环里反复构造短串（>16 字节）而不 `release()`，峰值内存会随轮数线性涨**（实测 100 万轮 155052 KB ≈ 158 B/轮）。
这不是「SSO 会泄漏」：内联态（≤16 字节）完全不碰池，`short_many` 那一格已经证明了。
这是一条使用纪律：**谁的生命周期跨过一轮以上，谁就要自己 `release()`**；
（C++ 那边由析构函数兜着，所以同样的源代码不会出现这个坑。）

## 复现

```sh
bash bench/string_vs_cpp/run.sh          # 编译 + 预热 + best of 5 + RSS，写 raw.txt 与这份 RESULTS.md
RUNS=3 bench/string_vs_cpp/run.sh        # 想快一点
ONLY="find compare" bench/string_vs_cpp/run.sh
```

单跑一格看校验和：

```sh
./build/extc -w --no-line-map -o s.c bench/string_vs_cpp/s.extc && gcc -O2 -std=c11 -fwrapv -o s s.c
g++ -O2 -std=c++17 -o cpp bench/string_vs_cpp/cpp.cpp
./s find ; ./cpp find     # 两行的 cs= 必须逐位相同
```

原始记录：`bench/string_vs_cpp/raw.txt`（本文件由 `report.py` 从它生成，数字全部来自那里）。

