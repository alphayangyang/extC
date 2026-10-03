<!-- 本页原来是 docs/MANUAL.md 的「7.8 驱动开关（`-O` / `-march`）」一节（原 §7.8）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · [← 7.7 分配：`new`（`new T` / `new [N]T` / `new T[n]`）](11-alloc.md) · [7.9 动态数组 `varArray<T>`（对标 `std::vector` 的**基础部分**） →](13-vararray.md)

---

# 7.8 驱动开关（`-O` / `-march`）

```bash
extc foo.extc                  # 默认 -O2
extc -O3 foo.extc -o foo.c     # 指定优化级别（-O0..-O3）
extc -O2 -march=native --run foo.extc   # 让 gcc 用本机指令集（牺牲可移植性）
```

**实测教训**：这两项**收益完全看负载** —— 矩阵乘上 `-march=native` 曾让时间**翻倍**，
而 mandelbrot / binary-trees 在本机**几乎没变化**（`-O2` 已经吃干净了）⇒
**别把它当万能加速**，要压性能先看算法/数据布局（我们那边 matmul 的真正杠杆是**转置 b**）
默认 `-O2` 保持不变：`-O3` 收益小、`-march=native` 牺牲可移植性

---

# 7.8.1 链接 C 库（`-l` / `-L` / `--ccflag` / `--pkg-config`）

从前 `extern!("libc")` 里的库名**只用于诊断**：它不产生任何链接动作，消费第三方 C 库只能走
`dlopen`（`std::dl`）。driver 现在认识四条链接开关，`extern!` 因此真正能"用 C 的库"：

```bash
extc -l z --run hello.extc                        # cc 收到 -lz
extc --ccflag -l:libsqlite3.so.0 --run q.extc     # soname 形式（-l 拼不出来）
extc --pkg-config openssl --run tls.extc          # pkg-config --cflags --libs
```

| 开关 | 作用 | 备注 |
|---|---|---|
| `-l <name>` | 交给 cc 一个 `-l<name>` | 可重复 |
| `-L <dir>` | 交给 cc 一个 `-L<dir>` | 可重复 |
| `--ccflag <flag>` | 把**一个** flag 原样交给 cc | 逃生舱：`-I…`、`-Wl,-rpath,…`、`-l:libX.so.N` |
| `--pkg-config <name>` | 把 `pkg-config --cflags --libs <name>` 的输出按空白切成若干 flag | 可重复 |

`--check-c` 与 `--run` 都接受这些开关：前者需要 cflags 才能找到头文件，后者需要 cflags 与 libs
才能编译并链接。

`--build` 与 `--run` 走同一条编译路径（同样的 flag 处理、同样读每个模块的 `.link`），但**不执行**结果：
二进制写到 `-o <file>`（没给就 `build/<name>`）。包的构建步骤要的就是这个形状
（`extpkg build` 用它，见 `tools/extpkg.py` 与 `C-ABI.md` §9.23）。

**链接需求写进生成物**（只在用到时出现；同一组库按名字排序去重，与书写顺序无关）：

```c
/* extc-libs: ssl z */
/* extc-pkg-config: openssl */
```

于是构建系统（Makefile、包管理器）可以从产物里把需求读回去，不必维护第二份清单。**不给任何
链接开关时，产物与从前逐字节相同**（414 份快照没有变化，由 `tools/golden.sh --strict-bytes` 与
`tests/linkflags/` 两侧钉住）。

**边界**：缺参数的开关、`pkg-config` 找不到的包都是退出码 2 的硬错误（不是警告）；`-l` 之间的
顺序按书写顺序原样传给 cc（静态库靠顺序解析符号）；生成物里的注释只记录 `-l` 与 `--pkg-config`
的名字，环境相关的路径（`-L`、`--ccflag`）留在命令行里。

**模块可以自带链接需求**：把 `<module>.link` 放在模块旁边，程序 `use` 到它时 driver 自动把需求
加进链接行 —— `use zmod` 不必在命令行重复库名。只对**真正加载**的模块读取（直接或间接 `use` 到），
没被用到的模块不参与链接；缺库时不下载任何东西，直接在链接处报 C 编译器的错。

```
# zmod.link —— 与 zmod.extc 同目录，空行与 `#` 注释忽略
lib z                          # ⇒ -lz
lib :libsqlite3.so.0           # soname 形式（本机 sqlite 没有 dev 包时只有这一种写法）
pkgconfig openssl              # ⇒ pkg-config --cflags --libs openssl 的每个 token
ccflag -Wl,-rpath,/opt/lib     # 逃生舱：一个 flag 原样交给 cc
```

未知指令是编译错误（一条被静默忽略的链接需求正是这张表要防的事）。模块带来的库同样写进产物的
`extc-libs:` 行。

判据：`tests/linkflags/run.sh`（三条路各自真编译真跑 · 不给 flag 必须链不上 · 生成物里的需求行 ·
不带 flag 产物不变 · 库不在本机时显式跳过）。

---

**[索引](README.md)** · [← 7.7 分配：`new`（`new T` / `new [N]T` / `new T[n]`）](11-alloc.md) · [7.9 动态数组 `varArray<T>`（对标 `std::vector` 的**基础部分**） →](13-vararray.md)
