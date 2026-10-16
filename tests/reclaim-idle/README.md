# idle 发射行为的语料指纹

`fingerprint.py` 把"这次改动有没有改变编译器发射回收运行期的行为"变成可复跑的事实的工具：
在**同一份语料**上，用**重构前的编译器**冻结逐文件指纹，用**重构后的编译器**重跑，逐行比对。

判 idle（"这个函数整段不需要回收运行期"）的证明在编译器内部。改它时，"没改变发射行为"是一句
主张；本工具用生成物本身当证据，而不是用编译器的诊断文案。

## 口径

语料集合照 `tests/reclaim-perf/taxmap.py` 的 census 口径：roots `examples/` · `tests/` ·
`bench/`，跳过 `archive` / `build` / `in` / `in-small` / `out` / `stamped` / `__pycache__`，
每个 `*.extc` 一行，按仓库相对路径排序。语料数随仓库漂，以基线行数为准。

每行六列，TAB 分列：

| 列 | 含义 |
|---|---|
| `path` | 仓库相对路径 |
| `ok` | 默认档编译结果：`OK` / `FAIL`（非零退出）/ `TIMEOUT`（超每文件限额）|
| `md5_default` | 默认档生成的 C 的 md5 |
| `md5_reclaim0` | `EXTC_RECLAIM=0` 档生成的 C 的 md5 |
| `same` | 两档生成物是否逐字节相同（`YES` / `NO`）|
| `rc_syms` | 默认档生成 C 里 `extc_rc_*` 标识符的排序去重集合，逗号连接 |

`same = YES` 等价于"该程序不发射回收运行期"——与 [`IDLE-PROJECTION-RECON.md`](../reclaim-perf/IDLE-PROJECTION-RECON.md)
和税务总表用的是同一把尺。编译不过的行里未编译出的格子记 `-`；默认档过、关档不过的组合也
记 `-`，此时行仍会被比对（`ok` 或 md5 变化即报差异）。

### 为什么量"生成物 md5 + rc 符号集"，不量诊断文案

`EXTC_DBG_RCIDLE` 一类诊断是给人看的，措辞与取值随时会变；判 idle 的**可观测后果**只有两件：

* **生成物变不变**：逐字节 md5。默认档与关档相同 ⇒ 这段程序根本没有回收运行期；
* **发射了哪些运行期入口**：`extc_rc_*` 的集合（如 `storeRefOwnedRelease` 只在覆盖点定点
  释放被发射时出现）。

诊断文案进指纹，会把"改了一句提示"变成假红；生成物进指纹，改的就必须是真的发射行为。

## 确定性：三条决定

1. **在源码树根目录下用相对路径调用编译器**。生成的 C 里内嵌源文件路径（陷阱/越界信息里
   带文件名与行号），绝对路径会让同一份语料的 md5 随 checkout 位置变。
2. **`-I` 必须指编译器自己的 stdlib，且走绝对路径**。编译器认定"哪些文件是标准库"用的是
   `<extc 所在目录>/../stdlib`（`/proc/self/exe`），所以 `-I` 指到别的树的 stdlib 会让那棵树
   的标准库整片变成普通程序文件（`@builtin` 报"standard-library only"）；相对的 `-I stdlib`
   又会被按源码文件所在目录解析。默认 `--stdlib = <extc>/../stdlib`，并显式设 `EXTC_STD`
   把"搜索目录"与"标准库认定"钉成同一个根。
3. **环境里继承来的 `EXTC_*` 全部剥掉**，只按档位设 `EXTC_RECLAIM=0`（另加决定 2 的
   `EXTC_STD`）。调试、分片、`--no-levelpass` 等开关都会改产物，指纹只许动一个旋钮。

头部四行记绑定：`build/extc` 的 md5、`HEAD`、编译器路径、语料树与 stdlib。**会漂的绑定写进
产物，不写进散文。** 基线里的编译器是**重构前**那一份——它的 md5 与主树当前的编译器不同，
`check` 会把两侧绑定都打出来并注明"绑定不同不算失败，逐行相同才是结论"。

## 复现

### 1. 取重构前的源码树并构建（不动主工作树）

`git archive` 出来的树没有 `.git`，所以 `--head` 要显式给；编译器带 `-g`，DWARF 里是构建
目录，因此**同一份源码在不同路径构建出的二进制 md5 不同**——基线钉的是"实际用来跑的那份
二进制"的 md5，行为等价性由下面的复现与此前的普查数字共同支撑。

```sh
mkdir -p /tmp/extc-idle-base
git archive <重构前提交> | tar -x -C /tmp/extc-idle-base
make -C /tmp/extc-idle-base -j"$(nproc)"
```

### 2. 冻结基线（写进版本的那一份）

```sh
python3 tests/reclaim-idle/fingerprint.py freeze \
    --tree /tmp/extc-idle-base --head <重构前提交> \
    --out tools/idle-fingerprint-baseline.tsv
```

### 3. 重构后重跑比对（任何一格不同 ⇒ 退出码 1，逐行打印前后值）

```sh
python3 tests/reclaim-idle/fingerprint.py check            # 默认：主树 build/extc + 主树语料
python3 tests/reclaim-idle/fingerprint.py check \
    --tree /tmp/extc-idle-post --extc /tmp/extc-idle-post/build/extc   # 或钉在某个提交上
```

`--only <子串>`（可重复）把两侧都收窄到匹配的行，用于最小复现与调试。

### 4. 自检：判据能红

```sh
cp tools/idle-fingerprint-baseline.tsv /tmp/b.tsv
# 手工改掉某一行的 md5_default（或任意一格）
python3 tests/reclaim-idle/fingerprint.py check --baseline /tmp/b.tsv \
    --tree /tmp/extc-idle-base --head <重构前提交>          # 期望 exit 1
```

用与基线相同的编译器跑，差异才只剩被改掉的那一格。

原始输出在 `out/selfcheck-red.txt`（**不入库**，跑一遍上面的自检即得）：改掉
`tests/reclaim-perf/shapes/refgraph/rg_walk_chain.extc` 的 `md5_default` 后，`check` 报出
**恰好 1 行 changed**、其余 1233 行不动，退出码 1。

## 证据文件（`out/`）

| 文件 | 是什么 |
|---|---|
| `out/freeze-pre-refactor.log` | 冻结基线的进度与总数 |
| `out/check-A-same-tree.txt` | 同树同编译器重跑：逐行相同（运行间确定性）|
| `out/check-B-main-tree-same-compiler.txt` | 换了语料树、同一个编译器：逐行相同（指纹不绑定 checkout 位置）|
| `out/selfcheck-red.txt` | 自检能红的原始输出 |

> `out/` 下都是跑出来的原始输出，**不入库**（复跑就重写；`check.sh` 的 `check-tree` 盯着这条）；
> 上表是文件名索引，不是仓库里的链接。

## 未覆盖 / 残留风险

* **`FAIL` 只记"编不过"，不记为什么编不过。** 报错文案改动不会让指纹变化；这是刻意的
  （文案不是发射行为）。代价是"编译失败原因变了、但仍是失败"这一类漂移看不见。
* **指纹是逐字节的强判据，不是语义判据。** 它证明"生成物相同"，不证明"运行结果相同"；
  运行期正确性仍要靠 `check.sh nobench`、ASan 与各回收门禁。
* **语料是分类别的，不是随机抽样。** 指纹只覆盖 `examples/` `tests/` `bench/` 里的
  `*.extc`；新增/删除语料文件本身也会让 `check` 报差异（按新增/删除行处理），这是有意的。
* **`rc_syms` 非空时几乎是常量**（回收运行期整段内联进生成物），它的分辨力主要在
  "空 vs 非空"与运行期入口的增删上；逐文件的差异主要由 `md5_default` 承担。
* **只比默认档与 `EXTC_RECLAIM=0` 两档。** 计数档（`EXTC_RC_NOCOUNT=0`）、分片
  （`EXTC_RC_SHARDS>1`）、事件档（`EXTC_RC_SWEEPFREE=1`）不在指纹内。
* **绑定钉在具体二进制上。** 换了构建目录/编译器版本，基线头部的 md5 就对不上；`check`
  会 NOTE 提示，结论仍以逐行为准。
