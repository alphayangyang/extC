# P5 超线性诊断：第一轮拆分（2026-10-15）

## 结论

第一轮只做诊断，不改 `src/`：在同一份现场生成物、`cc -O2 -fwrapv`、core 6、同 seed 下，对 P5 的 6M / 20M 两点分别运行两次，并打开 `EXTC_RC_PAUSE=1`。

- `collects`：32 → 41（1.28×），规模 3.333×。
- `[step] n`：45,503 → 163,297（3.589×）；`[step] total_us`：约 0.55–0.61 s → 2.91–2.93 s（约 4.8–5.3×）。
- `[work] n`：21,313 → 76,495（3.589×）；每点 `walk_max=256`、`scan_max=0`、`push_max=0`，三条工作量界均满足；`[ubwork] mark_n=sweep_n=0`。
- `freeSlot` 调用：5,227,508 → 19,205,262（3.676×）。运行期组件调用数也基本随输入线性增长：`alloc` 6,750,002 → 22,500,002（3.333×），`storeReg` 6,750,000 → 22,500,000（3.333×），`storeOwned` 750,000 → 2,500,000（3.333×），`slabNew` 49 → 166。

因此，当前证据**不支持“清扫轮数随规模超线性”作为主因**：collect 只从 32 增至 41；工作量步数和 freeSlot 则约随输入线性。`[step]` 总时间的额外增长更像单步/槽管理的单位成本或缓存局部性，而不是无界排空（两点 `[ubwork]` 均为 0）。这仍是两点诊断，不是通用复杂度证明。

## 现场指令剖析

6M 的 callgrind 基线：总 Ir `3,688,824,451`；`extc_rc_alloc` 包含 Ir `1,981,883,548`（53.73%）。这是包含式归因，不能把它全部解释成 `alloc` 本体；下一轮必须继续做函数级/调用点级拆分，至少区分：分配槽管理、写屏障/计数、`freeSlot`、以及标记步。

## 可复现实验

```sh
build/extc -w --no-line-map -I stdlib bench/gc/src/spectrum_p5.extc -o /tmp/extc-p5diag/p5.c
cc -O2 -fwrapv -std=c11 -D_POSIX_C_SOURCE=200809L -o /tmp/extc-p5diag/p5 /tmp/extc-p5diag/p5.c
EXTC_RC_PAUSE=1 taskset -c 6 /tmp/extc-p5diag/p5 6000000 20261013
EXTC_RC_PAUSE=1 taskset -c 6 /tmp/extc-p5diag/p5 20000000 20261013
```

组件调用计数通过现场生成物把 `#define EXTC_RC_TIME 0` 临时改成 `1` 得到；仅用于诊断，没有进入仓库。callgrind 也只作用于 `/tmp` 副本。

## 下一步

1. 保持同一现场生成物，做 6M / 20M 的组件消融或调用点级剖析，优先验证 `extc_rc_alloc` 包含式成本是否由槽分配管理主导。
2. 再将 `[step]` 之外的墙钟残差（墙钟 − `[step]` 总量）与组件消融结果对齐；不要仅凭 `[step]` 总量修改触发策略。
3. 只有出现可红的回归判据后，才考虑改实现；当前没有足够证据证明应改触发轮数或阈值。

## 已覆盖 / 未覆盖 / 残留风险

- 已覆盖：两规模点、两次运行、collect/step/work/ubwork、组件调用计数、6M callgrind 总体与 `extc_rc_alloc` 包含式占比。
- 未覆盖：严格多点幂次、独立的组件消融结果、调用点级 cache/TLB 归因、不同编译档位与其他形状。
- 残留风险：两点不能证明渐进复杂度；callgrind 包含式函数归因会把被调函数成本归入 `extc_rc_alloc`；运行时测量仍可能受机器频率/cache 干扰。

## 第二轮：分配 slab 搜索的验证

在生成物副本中对 `extc_rc_alloc` 的 slab 选择循环做了计数。该循环按 `ss` 与“有空闲槽”条件从 `r->slabs` 链头逐块扫描：

| N | `ss=32` 扫描迭代 | 总扫描 | collects | step n |
|---:|---:|---:|---:|---:|
| 6M | 50,651,811 | 50,651,812 | 32 | 45,503 |
| 20M | 544,412,601 | 544,412,602 | 41 | 163,297 |

几乎全部扫描都发生在 32-byte `ent` slab；2048-byte 的 `idx/win` 只出现 1 次。扫描迭代从 6M 到 20M 增长 **10.74×**，显著高于输入的 3.33×，而 collects 仅 1.28×。这给出了比“轮数超线性”更直接的候选根因：**槽分配在不断穿过已满的 slab 链寻找可复用槽**。

### 单变量原型

在副本中加入一个只针对 `ss=32` 的当前 slab 缓存：若缓存 slab 仍有 freelist/bump 空间，直接从它开始；分配成功后更新缓存。两点 stdout 与基线均 `cmp` 相同，且 RSS 不变。

同一机器、core 6 的两次粗测：

| 变体 | 6M (s) | 20M (s) |
|---|---:|---:|
| baseline | 0.71–0.74 | 5.99–6.05 |
| cache prototype | 0.59–0.62 | 3.25–3.31 |

这是诊断原型，不是提交实现；测量没有做完整 best-of-5，故只把方向视为验证，不把具体加速比当承诺。原型仍保持 `[work]`、`[ubwork]` 形状不变；此前的工作量闸门在未修改主树时已通过。

### 当前归因

- 轮数爆炸：**未被数据支持**（collects 32→41）。
- 标记工作量本身：大体线性（`[work] n` 21,313→76,495，3.59×）。
- 分配 slab 搜索：**已被计数直接证实为超线性候选**（50.7M→544.4M，10.74×），并有保持语义的缓存原型改善墙钟作为因果旁证。
- `extc_rc_alloc` 仍不能简单等同于单一分配成本；callgrind 自成本表同时显示 `markStep`、`shade`、`freeSlot` 等独立热点，因此下一步实现应先做“按尺寸类维护可用 slab”的最小改动，并为扫描迭代数建立可红的诊断/回归量具。

## 第三轮：交错复测与剩余成本

20M 在 core 6 上按 `baseline → cache` 交错 3 轮，stdout 每轮对应 `cmp` 相同，RSS 均 183276 KB：

| 轮次 | baseline | cache prototype |
|---:|---:|---:|
| 1 | 5.89 s | 3.23 s |
| 2 | 5.92 s | 3.20 s |
| 3 | 6.01 s | 3.28 s |

6M 的 callgrind 对照：baseline `3,688,824,451 Ir`，cache prototype `3,231,938,331 Ir`，减少 **12.39%**。
缓存原型仍不是可提交实现：它只缓存一个 32-byte slab，且没有完整处理区域/ slab 生命周期；本轮只证明“避免反复从链头扫描”具有因果收益。

cache 版 6M 的非包含式热点仍为：`markStep` 15.92%、`alloc` 21.49%、`shade` 7.31%、`sweepStep` 6.24%；因此 slab 搜索不是全部剩余成本。剩余部分至少包括标记遍历、着色和清扫，不能据此直接改标记算法。

## 第四轮：运行时最小实现

在 `src/back/reclaim.c` 落地了按区域、按尺寸类的 `allocHot[16]` 缓存：

- 先按 `slotSize / 16` 尝试该 region 对应尺寸类最近一次成功选择的 slab；
- 只有尺寸不匹配或没有 freelist/bump 空间时，才从 `r->slabs` 链扫描；
- 超出 16 个尺寸类的对象安全走原链表备用路径；
- 选中或新建 slab 后回写对应尺寸类缓存；
- region 的 slab 仍由原有整区释放路径统一释放，因此缓存指针不会跨 region 存活；
- 没有引入全局尺寸表或新的释放路径。

新增 `tools/check_alloc_hot.py` 结构门禁。它逐项注入删除缓存字段、缓存读取、尺寸检查、容量检查、链表备用路径和成功回写，确认每一种违规都会变红；当前产物检查为绿。

实现后 P5 生成物结果：

| N | 墙钟 | RSS | `[step] n` | `[work] n` |
|---:|---:|---:|---:|---:|
| 6M | 0.65 s | 53100 KB | 45503 | 21313 |
| 20M | 3.31 s | 183276 KB | 163297 | 76495 |

此前单缓存原型约为 0.59 s / 3.13 s；该数字只作同机方向性对照，不作为跨机器承诺。

### 验证结果

- `make -j2 build/extc` 通过；
- `gate-reclaim-work`：6/6 通过；
- `gate-foreign-skip` 通过；
- `reclaim-unit`：61 条断言及 ASan/UBSan 全通过；
- `gate-reclaim-switch`：33 项全通过，覆盖 cross-region、shards、digraph 环负载和超大块；
- `gate-reclaim-perf`、`gate-reclaim-perf-shapes` 通过；
- P2/P4/P6/cross/shard 生成物的回收开/关 stdout 逐字相同；
- `gate-skip-table`、`gate-invariant-table`、`check-doc-facts`、`check-doc-examples` 通过。

## 第五轮：代码审查发现并修复的安全漏洞

审查 `markStep` 的同 slab 快捷路径时发现：它按 slab 基址判断后直接用 `slotOf` 计算位图索引，绕过了 `ownSlab` 的槽首校验。保守对象中若保存 slab 头地址，该候选值会进入快捷路径并读越界。

最小复现 `/tmp/p5-review/interior.c` 在修复前的 ASan 结果为：`SEGV ... extc_rc_markStep`；复现值是保守槽字段指向 slab header，而不是对象槽首。修复后改为在同 slab 快捷路径中先执行 offset/slot 边界与整除校验，再访问 `occ/mark` 位图；不合法候选回到普通 `ownSlab` 路径。

修复后：

- `bash tests/reclaim/run.sh`：61 条断言通过，ASan/UBSan 干净；
- `gate-reclaim-work`、`gate-reclaim-perf-shapes` 通过；
- P5 20M 三次：3.22 s / 183276 KB；
- `EXTC_RC_PAUSE=1`：`walk_max=256`、`over1ms=0`、`[ubwork]` 全为 0。


## 第六轮：代码审查与性能假设

对最新 P5 生成物的插桩结果：

| N | `alloc` | `shade` 调用 | `ownSlab` 调用 | `markStep` |
|---:|---:|---:|---:|---:|
| 6M | 6,750,002 | 50,250,002 | 8,196,258 | 21,313 |
| 20M | 22,500,002 | 167,500,002 | 29,521,751 | 76,495 |

`shade` 调用数约为分配数的 7.44 倍；6M 中只有约 10.14M 次处于写屏障开启状态，其余调用在入口早退。对 `shade` 和若干 wrapper 做 `inline` 的生成物实验没有稳定收益（20M 交错结果约 3.38–3.49 s），因此没有保留这条改动。`-O3` 相比 `-O2` 也没有稳定收益（约 3.37–3.44 s）。

`EXTC_RECLAIM=0` 的纯 bump 对照为约 0.26–0.33 s，但 RSS 约 533 MB；这不是纯 GC 税的等价对照，因为 allocator 和内存占用模型同时改变。

代码审查曾怀疑统一预算模式的 walk 会超出总额，但生成代码中的 `EXTC_RC_MS_WALK` 已包含 `wleft > 0`，生成物插桩没有红出违规；该假设已撤销，没有修改预算逻辑。

真正发现并修复的是标记安全漏洞：同 slab 快捷路径原先按 slab 基址直接对候选指针调用 `slotOf`，保守字段指向 slab header 时会越界访问位图。ASan 最小复现已在修复前触发 `markStep` SEGV；修复后对 offset/槽号/整除关系先验证，不合法候选不走快捷位图访问。该回归已加入 `tests/reclaim/driver.c` 的保守描述符测试。


## 第七轮：标记子对象预取

对照实验确认 `markStep` 的主要热路径之一是：从当前对象读取子引用后压入标记栈，下一步立即对该子对象做 `ownSlab`、槽位和载荷元数据访问。对 `extc_rc_markPushFast` 加入一次只读预取（`__builtin_prefetch(o, 0, 1)`）不改变栈内容、预算计数、标记位或扫描顺序。

生成物同核对照（P5，20M，`-O2`，每组 3 次）：

- 无预取：约 3.24–3.30 s；
- 预取：2.77–2.81 s；
- RSS：约 183276 KB，两组相同。

P5 6M 预取版约 0.51–0.54 s。20M `EXTC_RC_PAUSE=1` 结果：`walk_max=256`、`over1ms=0`、`[ubwork]` 各项为 0；停顿预算没有放宽。

已重新运行工作量门禁、reclaim 单测/ASan/UBSan，以及 reclaim performance、performance-shapes、switch 和 allocation-cache 门禁，均通过。预取只作用于已经通过引用读取的候选对象；它不改变保守指针验证和同 slab 槽首校验。
