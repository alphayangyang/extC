# idle 判据收敛重构：独立复核报告

**被复核的改动**：`38a94d4`（"idle 判据收敛第一步（严格无行为变化）：3 个台账 → 1 个 IdleWrite，
13 个出口 → 1 个判据"），复核时的等价源码修订 `a170faf`。
**复核人**：`idle-fingerprint`（独立于 T1 的实现与路径）。
**结论**：语料指纹 **1234 行逐行相同（0 差异）**；指名门禁全部通过；3+3 抽样与重构前产物逐字节
相同、ASan 干净、判 idle 一侧峰值比 0.96–1.01×。

## 0. 绑定与口径（先说清楚"钉的是什么"）

编译器二进制带 `-g`，DWARF 里是构建目录 ⇒ 同一份源码换路径构建，二进制 md5 必然不同。
所以本报告**不钉二进制 md5**，钉的是：**源码修订 + 逐文件生成物**（与 T1 的做法一致）。

| 角色 | 源码修订 | 构建位置 | `build/extc` md5 |
|---|---|---|---|
| 重构前（冻结基线用的） | `a614c06`（`git archive`） | `/tmp/extc-idle-base` | `485f47875539d73eb48e648e01d9a68c` |
| 重构后（判据） | `38a94d4`／复核树 `a170faf`（`git worktree`） | `/tmp/extc-idle-verify` | `7d968d12bde81b8b86848e5b2949bd4a` |
| 复核时主树（附加读数，不属本判据） | `fa168ea` | 主工作树 | `fb6acf10bd3327b3e3b0d60ca52378b4` |

复核树与重构后提交的等价性（自己跑的）：

```sh
git diff --stat 38a94d4 a170faf          # 只有 tests/reclaim-perf/IDLE-PROJECTION-RECON.md | 32 +
git diff --quiet 38a94d4 a170faf -- src stdlib && echo identical   # → identical
```

两棵隔离树（都不动主工作树）：

```sh
mkdir -p /tmp/extc-idle-base  && git archive a614c06 | tar -x -C /tmp/extc-idle-base  && make -C /tmp/extc-idle-base -j
git worktree add --detach /tmp/extc-idle-verify a170faf && make -C /tmp/extc-idle-verify -j
```

基线：`tools/idle-fingerprint-baseline.tsv`，1234 行，头部
`build/extc md5=485f4787… HEAD=a614c06`。口径与复现命令见同目录
[`README.md`](README.md) 与 [`fingerprint.py`](fingerprint.py)。

> 复核窗口内主工作树在被人改（我在 20:17 看到 `src/check/check_top.c` 未提交改动、
> `build/extc` 被重建）。因此指名电池跑在**独立 worktree** `/tmp/extc-idle-verify`（`a170faf`，
> 见上表），不在共享树上跑 —— 那条线上的任何失败都无法归因到某个修订。
> 副作用：主仓库 `.git/worktrees/` 多了一条 `extc-idle-verify`（`git worktree remove
> /tmp/extc-idle-verify` 可清掉；本报告的证据路径引用它，先留着）。

## 1. 语料指纹：逐行差异 **0 行**

```sh
# 重构后编译器（git archive 38a94d4 独立构建）+ 主树语料
python3 tests/reclaim-idle/fingerprint.py check \
    --tree /home/alphayang/extC_Compiler --extc /tmp/extc-idle-post/build/extc
# 复核树 a170faf 自己的 build/extc（第三个独立构建路径）
python3 tests/reclaim-idle/fingerprint.py check \
    --tree /tmp/extc-idle-verify --extc /tmp/extc-idle-verify/build/extc
```

原始输出：[`out/check-C-post-refactor.txt`](out/check-C-post-refactor.txt)（38a94d4 独立构建）·
[`out/check-F-worktree.txt`](out/check-F-worktree.txt)（a170faf 复核树构建）·
[`out/check-D-main-build.txt`](out/check-D-main-build.txt)（主树 20:16 的 `build/extc`
`e8d5aabe…`，源码同为 38a94d4）· [`out/check-E-main-build.txt`](out/check-E-main-build.txt)
（附加：`fa168ea` 的 `build/extc` `fb6acf10…`）。

四次都是：

```
rows: baseline 1234 · current 1234 · OK 852 · FAIL 382 · TIMEOUT 0 · same=YES 618 · same=NO 234
PASS: 逐行相同（每一格）
```

**逐行差异：没有。** 六列（路径 + `ok` / `md5_default` / `md5_reclaim0` / `same` / `rc_syms`）
一格没动，语料集合也没有增删。

判据自身的三条口径验证（task-2 交付，原始输出在 `out/`）：

* 同树同编译器重跑 → `PASS`（运行间确定性，[`out/check-A-same-tree.txt`](out/check-A-same-tree.txt)）；
* 同编译器、换语料树路径 → `PASS`（指纹不绑 checkout 位置，
  [`out/check-B-main-tree-same-compiler.txt`](out/check-B-main-tree-same-compiler.txt)）；
* 手工改掉一行 `md5_default` → `exit 1`、恰好 1 行 changed
  （[`out/selfcheck-red.txt`](out/selfcheck-red.txt)）——判据能红。

与 T1 的粗查交叉：T1 报"852 个可比对文件逐字节 0 差异"，我的独立口径（不同工具、不同构建路径、
不同语料树调用方式）得到同数同结论；另外我这把尺复现了 TAXMAP 记录的普查数
**618 ZERO / 234 PAYS / 382 GENFAIL**。

**附加读数（不属于本次验收）**：复核期间主树走到了 `fa168ea`，它又改了
`src/check/check_top.c`（27+/17−）。拿主树当时 `build/extc`（`fb6acf10…`）跑同一份指纹，也是
**PASS（0 差异）**，见 `out/check-E-main-build.txt`。这条只说"`fa168ea` 在语料上也没改发射
行为"，不替代对 `38a94d4` 的验收。

## 2. 指名门禁（全部自己跑，不采信转述）

跑在 `/tmp/extc-idle-verify`（`a170faf`，`src == 38a94d4`）。

| 门禁 | 命令 | 结果 | 原始输出 |
|---|---|---|---|
| nobench | `./check.sh nobench` | **通过 100，失败 0**（EXIT=0） | [`out/nobench-summary.txt`](out/nobench-summary.txt) |
| docs | `./check.sh docs` | **通过 12，失败 0** | [`out/docs.txt`](out/docs.txt) |
| gate-idle-projection | `python3 tests/reclaim-perf/gate-idle-projection.py` | **通过（已知欠账 9 条，棘轮只许缩）** | [`out/gate-idle-projection.txt`](out/gate-idle-projection.txt) |
| gate-idle-bounded | `./tests/reclaim-perf/gate-idle-bounded.sh` | **退出码 0**；7 个形状判 idle、对照 5/5 仍在回收档 | [`out/gate-idle-bounded.txt`](out/gate-idle-bounded.txt) |
| gate-tax | `./tests/reclaim-perf/gate-tax.sh` | **退出码 0**；`bt` 税 **1.000×**、`bt-mid` **1.000×** | [`out/gate-tax.txt`](out/gate-tax.txt) |

细节（原始行）：

* `gate-idle-projection`：红集 9 个形状（`rg_walk_chain` · `rg_walk_tree` · `rg_scan_parent` ·
  `rg_walk_buckets` · `rg_insert_hash` · `rg_grow_tail` · `rg_scan_graph` · `spectrum_p2` ·
  `spectrum_p4`）仍然"还没判 idle"（= 已知欠账 9 条，与预期一致）；护栏 6/6 仍付钱：
  `rg_relink_delete`（PAYS，语义一致）· `rg_overwrite_ring`（1.00×）· `rg_walk_array`（0.98×）·
  `rg_evict_cache`（1.02×）· `rg_drop_component`（1.00×）· `ring_per_round`（1.00×）。
* `gate-idle-bounded`：判 idle 的 5 个形状（§①）全部"产物 == 关档"且峰值比 1.00–1.04×；
  §④ 的 2 个 O(N) 活集形状 `persist_nodes` 21 MB ≤ 31 MB、`p3834_nodes` 496 MB ≤ 747 MB；
  对照 `cyclic_pressure` / `outer_alloc_inner_write` / `byval_temp` / `field_overwrite_lost_root` /
  `fixed_slot_overwrite` 全部仍在回收档。
* `gate-tax`：`bt` 开档 1 898 965 Ir == 关档 1 898 965 Ir；`bt-mid` 开档 102 373 858 == 关档
  102 373 858。

### 2.1 nobench 第一跑的 4 个失败：都不是改动引入的

第一跑（全新 worktree）是 **96 通过 / 4 失败**。逐条归因（原始日志
`/tmp/gates/nobench-clean.txt`，摘要落盘 `out/nobench-summary.txt`）：

1. `gate-reclaim-perf-shapes` 与 `gate-reclaim-tax`：`cannot open output file build/gen:
   No such file or directory` —— 全新 worktree 缺 **gitignored** 的生成输入目录。
   补 `mkdir -p bench/bigmatrix/{build,in-small}` 并跑一次 `./build/gen small in-small` 后，
   两条**单独跑都退出码 0**（[`out/gate-shapes.txt`](out/gate-shapes.txt) ·
   [`out/gate-tax.txt`](out/gate-tax.txt)），随后第二跑进全绿。
2. `tools/gate_escape.py`：报 `P0-1_try_escape.extc` NEW。单独跑（post 与 pre 编译器**都**）
   `失败 1（已知 1 / 新增 0），ok`。最小复现见
   [`out/p0-1-escape-minimal-repro.txt`](out/p0-1-escape-minimal-repro.txt)：重构前/后编译器生成的
   C **同 md5 `c6a48bd58a622d440adf8ba2aec9f5d5`**，ASan+UBSan 下都打印 `head = 7`。
3. `tests/pool/run.sh`：单独跑退出码 0（[`out/pool-run.txt`](out/pool-run.txt)）。

同一棵树的**第二跑**：`通过 100，失败 0`（`out/nobench-summary.txt`）。
第一跑另有一份"主树 dirty 期"的日志（`/tmp/gates/nobench.txt`）失败项完全不同，其中一条是
`PermissionError: build/extc` —— 那是与别人重建编译器撞车留下的，两条都不作证据用。

## 3. 抽样加严（3 个判 idle + 3 个仍发射 rc）

工具：[`sample-asan-bounded.sh`](sample-asan-bounded.sh)。每个形状做三件事：① 默认档产物与
`EXTC_RECLAIM=0` 比、并证"类别没变"；② 生成物 md5 与**冻结基线那一行**对拍（⇒ 与重构前产物
逐字节相同）；③ 普通档 `/usr/bin/time -f %M` 量 N 与 4N 峰值（**不用 fork+wait4 的
`ru_maxrss`** —— 父进程 RSS 会污染读数），ASan 档跑两个规模查内存报告。

```sh
ROOT=/tmp/extc-idle-verify BASE=$PWD/tools/idle-fingerprint-baseline.tsv \
    tests/reclaim-idle/sample-asan-bounded.sh                 # N=50000 / 4N=200000
ROOT=/tmp/extc-idle-verify BASE=$PWD/tools/idle-fingerprint-baseline.tsv N=500000 \
    tests/reclaim-idle/sample-asan-bounded.sh                 # 与 gate-idle-projection 的护栏同规模
```

原始输出：[`out/sample-asan-bounded.txt`](out/sample-asan-bounded.txt) ·
[`out/sample-asan-bounded-N500k.txt`](out/sample-asan-bounded-N500k.txt)。

| 形状 | 侧 | 生成物 vs 基线 | N=50k 峰值比 | N=500k 峰值比 | ASan |
|---|---|---|---|---|---|
| `blind_localhold` | idle | 逐字节同（`8895ceed`）| 0.96× | 1.00× | 干净 |
| `bounded_churn` | idle | 逐字节同（`41624055`）| 1.00× | 1.00× | 干净 |
| `rebuild_per_round` | idle | 逐字节同（`f8c22eb2`）| 1.01× | 1.00× | 干净 |
| `cyclic_pressure` | pays | 逐字节同（`5ef44bc4`）| 1.19× | 1.00× | 干净 |
| `outer_alloc_inner_write` | pays | 逐字节同（`02bb0024`）| 1.17× | 0.99× | 干净 |
| `rg_overwrite_ring` | pays | 逐字节同（`d116af4b`）| 1.78× | 1.00× | 干净 |

读法：

* **六个形状的生成物都与重构前冻结产物逐字节相同** ⇒ 重构没有在"判 idle"或"仍发射 rc"任一
  侧改变这些形状；运行时差异不可能来自这次改动。
* 判 idle 一侧：峰值比 0.96–1.01×（< 1.5×）⇒ 没有因为改动而把垃圾攒起来。
* 仍发射 rc 一侧只判"仍付钱 + ASan 干净"，**峰值比不判**（有界性归回收侧门禁管）。N=50k 档
  `rg_overwrite_ring` 是 1.78×（绝对 2→3 MB，固定底噪占大头）；在护栏门禁自己的规模
  N=500k/2M 上是 1.00×，`gate-idle-projection` 同一形状也判"PAYS 且有界（1.00×）"。

## 4. 未覆盖（明确没量到的）

* **指纹语料不含 `tools/` 下的语料**（census roots = `examples/` `tests/` `bench/`）：
  `tools/escape-corpus/`（25 份）、`tools/callsite-corpus/` 等**不在**逐行指纹里。本次只在
  `P0-1_try_escape.extc` 上手工做了 pre/post 对拍（§2.1），其余没做。
* **`FAIL` 只记"编不过"，不记为什么**。382 个编译失败的文件，指纹只保证"仍然失败"，
  不保证"失败原因相同"（刻意的：诊断文案不是发射行为）。
* **只比默认档与 `EXTC_RECLAIM=0`**。计数档（`EXTC_RC_NOCOUNT=0`）、分片
  （`EXTC_RC_SHARDS>1`）、事件档（`EXTC_RC_SWEEPFREE=1`）没进指纹。
* **抽样是 6 个门禁列名的形状，不是随机抽样**；1234 个文件里其余 1228 个只过指纹，没过运行期。
* **抽样只覆盖两档规模**（N=5e4/2e5 与 N=5e5/2e6），未 pin 核、未记 load，单机单次会话。
* **共享主树在复核窗口内不稳定**（有人在改 `check_top.c`），所以完整电池跑在独立 worktree 里；
  主树走到 `fa168ea` 之后的最终验收（`./check.sh nobench` 100/0）仍应在主树上再跑一次。

## 5. 残留风险

* **nobench 第一跑的两条失败（`gate_escape` / `pool`）没有被我主动复现**。证据是"单独跑都过 +
  P0-1 的 pre/post 产物逐字节同 + 第二跑 100/0"，指向并行电池下的抖动/环境；但我没有构造出
  稳定复现。若它再次出现在同一位置，值得单独查，而不是当成已解释。
* **指纹是逐字节强判据，不是语义判据**：它证明"生成物相同"，运行期正确性由 §2 的门禁与
  §3 的 ASan 覆盖，两者都只到语料与样例的边界。
* **基线绑定在具体二进制上**（`485f4787…`）。换构建目录会让头部 md5 对不上；`check` 会打印
  NOTE 但判据仍以逐行为准 —— 这是设计，不是缺口。
* **`rg_overwrite_ring` 在小 N 档峰值比 1.78×**：与本次改动无关（生成物逐字节同），但说明
  "N vs 4N < 1.5×"在小规模上会被固定底噪放大，规模选择要跟门禁一致。
* **主树已在 `fa168ea` 上继续改 `src/check/check_top.c`**；本报告的"零行为变化"只覆盖
  `38a94d4`（附加读数覆盖 `fa168ea` 的语料发射行为，不覆盖它可能引入的其他语义改动）。

## 6. 原始输出索引（都在 [`out/`](out/)）

| 文件 | 是什么 |
|---|---|
| `check-C-post-refactor.txt` | 指纹：38a94d4 独立构建的编译器，1234 行逐行相同 |
| `check-F-worktree.txt` | 指纹：复核树 a170faf 自己的 `build/extc`，逐行相同 |
| `check-D-main-build.txt` | 指纹：主树 20:16 的 `build/extc`（38a94d4 源码），逐行相同 |
| `check-E-main-build.txt` | 附加：主树 `fa168ea` 的 `build/extc`，逐行相同 |
| `check-A-same-tree.txt` · `check-B-main-tree-same-compiler.txt` | 判据口径验证（确定性 / 位置无关）|
| `selfcheck-red.txt` | 判据能红（改一行 md5 ⇒ exit 1、1 行 changed）|
| `nobench-summary.txt` | 电池两跑（96/4 → 100/0）与失败归因 |
| `docs.txt` | `./check.sh docs`：12/0 |
| `gate-idle-projection.txt` | 9 条已知欠账 + 护栏 6/6 付钱 |
| `gate-idle-bounded.txt` | 7 形状判 idle + 对照 5/5 仍在回收档 |
| `gate-tax.txt` · `gate-shapes.txt` | 税 1.000× / 真实形状判据 |
| `pool-run.txt` | `tests/pool/run.sh` 单独跑通过 |
| `p0-1-escape-minimal-repro.txt` | escape 门禁抖动的 pre/post 最小复现 |
| `sample-asan-bounded.txt` · `sample-asan-bounded-N500k.txt` | 3+3 抽样两档 |
| `freeze-pre-refactor.log` | 基线冻结的进度与总数 |
