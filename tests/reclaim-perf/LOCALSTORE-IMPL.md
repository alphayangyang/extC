# 本地/形参槽专用存储入口（`EXTC_RC_LOCALSTORE`）实现记录

## 0. 开关语义（先写死）
* `EXTC_RC_LOCALSTORE=1`（**编译期，默认关**）⇒ `owner == "0"` 的存储点改发 `extc_rc_storeRefLocal`；
  **默认关 ⇒ 生成物逐字节不变**（语料 `IDENTICAL=696 DIFF=0`，对 pin `e4c3a869…`）。
* 只在**无计数档**（`EXTC_RC_NOCOUNT=1`，默认）生效；`EXTC_RC_NOCOUNT=0` 一律不动。

## 1. 改动面
| 文件 | 改动 |
|---|---|
| `src/back/codegen.c` | `Cg.rcLocalStore` 字段 + `getenv("EXTC_RC_LOCALSTORE")`（默认 0）；在 **`rcEmitStoreRelease` 之后**、`storeRefOwned` 之前加分支（`rcLocalStore && rcNoCount && hostBase == NULL` ⇒ 发 `storeRefLocal`）；`reclaimEmitRuntime(...)` 多传一个参数 |
| `src/back/reclaim.c` | `reclaimEmitRuntime` 增参 `localStore`；**仅当开关开**时追加一段运行时文本：`#if EXTC_RC_NOCOUNT` 下 `storeRefLocal(slot,v){ if(slot){*slot=v;return;} storeRef(slot,v); }` |
| `src/back/reclaim.h` | 原型同步 |
| `src/main.c` | `--help` 里记这个开关（否则 `tools/check_switches.py` 红，见 §5） |
| `tests/reclaim/emit_main.c` | **发射器驱动同步新签名**（API 改动的必要跟进；见 §5 的回归说明） |

未动：6134 的 `rcEmitStoreRelease` 定点释放路径、6095 的 `storeRefUnknown`/noSweep 语义（新分支放在它们**之后**）。

## 2. 主判据：墙钟（P4 `units=2000`，`taskset -c 6`）
口径：`/usr/bin/time -f %e` 只有 10 ms 分辨率（两档都打 0.02），改用**10 次一组的 ns 计时**、三组取中位：

| 档 | 三组（10 次/组） | 中位（每次） |
|---|---|---|
| 关 | 243 / 240 / 244 ms | **24.3 ms** |
| 开 | 207 / 207 / 205 ms | **20.7 ms** |

⇒ **改善 14.8%**（≥5% ✓）。未劣化：`digraph @1M` 479 → 470 ms（**−1.9%**，在 2% 内且方向更好）；
`cyclic_pressure @100k` 7 → 6 ms。stdout 与关档**逐字节一致**（P4/P1–P5 实测）。

## 3. 机制验证
`EXTC_RC_TIME=1` 的逐入口计数（P4 units=2000，同一份生成物开/关各编一次）：

| 入口 | 关 | 开 | 差 |
|---|---|---|---|
| `storeOwned` | 3,552,955 | **11,081** | **−3,541,874（−99.7%）** |
| `storeReg` | 1,547,825 | 1,547,825 | 0 |
| `alloc` / `collect` / `slabNew` / `freeSlot` / `regionNew` | 1,547,826 / 47 / 4 / 1,521,662 / 2 | 同 | 0 |

`storeOwned` 剩下的 11,081 恰好 = 唯一带对象宿主的那条站点（`inserts`）。
**`ownSlab` 是 `static inline`**：callgrind 里没有独立符号（0 处命中）⇒ "调用数"不可直接观测；
按实测计数推导：被删掉的 3,541,874 次调用 × 每次 2 个 `ownSlab` = **7,083,748 次查询**，
占背景 `ownSlab` 总数 **8,644,163** 的 **81.95%**（与 ≈82% 的判据一致）。这是**由实测计数推导**，不是估算。

## 4. 正确性与门禁
* 语义：`spectrum_p1..p5` 的 stdout 与 `EXTC_RC_STATS` 行，开/关**逐字节一致**；
  `cross_region`（钉住对照）`rs_inc=200000 rs_dec=196611 rs_live=3389 rs_hwm=65537` 开/关**完全相同**。
* 八个钉住对照在开档下**仍发 rc**（计数与关档逐格相同）：`cross_region` 399、`scale_growth` 393、
  `live_churn` 398、`cyclic_pressure` 424、`outer_alloc_inner_write` 396、
  `field_overwrite_lost_root` 398、`fixed_slot_overwrite` 397、`byval_temp` 392。
* 门禁（**开档**跑的）：`gate-rc-idle-corpus`、`gate-reclaim-idle`、`gate-shard-cycles`、
  `gate-reclaim-tax`、`gate-asan-corpus`、`gate-reclaim-count`（计数档 101 条那一套）、
  `gate-reclaim-elide` 全绿（各 2/0）；`smoke 16/0`、`docs 12/0`（开档与默认档都绿）；
  默认档 `nobench 100/0`；`MIN_IDLE` 未动；孤儿 0。
* `nobench`：**默认档 100/0、开档 100/0**（修掉 §5 的两个登记类回归后复跑）。

## 5. 我引入并已修掉的两个回归（诚实记录）
1. **`reclaimEmitRuntime` 加参后 `tests/reclaim/emit_main.c` 没同步** ⇒ 发射器编不过 ⇒ `smoke` 一开始报
   5 个 FAIL（`check_note_part`/`check_invariant_table`/`check_skip_table`/`tests/reclaim/run.sh`/
   `tests/reclaim-perf/gate.sh`）。已把该调用补上新参数（读同一个环境开关），`smoke` 回到 **16/0**。
2. **新开关没进 `--help`** ⇒ `tools/check_switches.py` 红（这是登记棘轮，做得对）。
   已在 `src/main.c` 的开关段记上 `EXTC_RC_LOCALSTORE=1`，该检查恢复绿（51 开关：21 进 `--help`、30 挂 UNDOCUMENTED）。
3. **加了 `--help` 之后 `docs/usage/flags.md` 过期**（单一真源页）⇒ `nobench` 红在这一格。
   按该判据自己的提示跑了 `python3 tools/gen_flags.py` 重新生成（+4 行），`nobench` 回到 **100/0**（默认与开档都验过）。

## 6. 牙（注入发）—— **已实测会红**
* 注入（在 `/tmp` 副本树里做，未落工作树）：把对象宿主（`hreg != NULL`）的存储也改发 `storeRefLocal`
  ⇒ **跨区登记整段消失**。
* 实测原文：`cross_region` 关档/正常开档 `rs_inc=200000 rs_dec=196611 rs_live=3389 rs_hwm=65537`；
  **注入档 `rs_inc=0 rs_dec=0 rs_live=0 rs_hwm=0`** ⇒ 登记机制被杀掉 ✓。
* 门禁：`EXTC=/tmp/teeth/build/extc EXTC_RC_LOCALSTORE=1 ./check.sh gate-reclaim-switch`
  ⇒ **通过 1，失败 1，exit 1** ✓（"不能被误用到有对象宿主的存储上"成立）。
* **未覆盖**：这发注入**没有加进 `tools/inject.py` 的注集** —— `tools/` 不在本件写范围内
  （需要你或另开一件带 `tools/` 权限的改动去加；名字建议 `localstore-on-objhost`）。

## 7. 顺带对账：`storeRefOwnedReg` 的 0 vs 1,547,825
* 我的口径：**运行期逐入口计数器**（`EXTC_RC_TIME=1` 编进生成物，`reclaim.c:705` 的 `EXTC_RC_TICK(1)`
  就在 `storeRefOwnedReg` 里）⇒ P4 units=2000 实测 **1,547,825 次**，静态 2 个调用点（`s->next`、`p->sib`）。
* 我在 `bench/gc/*.md` 里**没有找到**那张"入口分布"表（`grep storeRefOwnedReg/33.1/66.9` 只命中
  `DIGRAPH.md` 的无关行与 `HUMAN-SHAPES.md:113` 的一句说明）⇒ **未查清**对方的计数器覆盖了哪些调用点。
  **消不掉这条不一致的账**：需要你给出 `gcbench` 那份计数的来源（哪个探针/哪次运行）我才能对上。
  可确定的是：本报告的 30.3%（Reg）/69.7%（Owned）用的是**生成物内建计数**，与 `storeOwned` 的
  3,552,955 和背景的 3.55M 完全吻合 ⇒ 我这边的口径自洽。

## 8. 下一步（不在本件做）
1. **翻默认**：按你的规矩单开一次提交（本件交付的是"开关默认关、判据全绿"）。
2. 把牙写进 `tools/inject.py` 注集（需 `tools/` 范围）。
3. `docs/RECLAIM.md` 义务表已加一行（本地槽入口的等价性与它的牙）。

---

# 翻默认（2026-10，Lead 批准）—— 判据重跑的实测

* **默认改为开**（`g->rcLocalStore = 1`；`EXTC_RC_LOCALSTORE=0` 是 kill switch），
  `--help` 与 `docs/usage/flags.md` 已改成"默认开 / `=0` 回到翻默认前的产物"。
* **kill switch 逐字节回到旧产物**：`EXTC_RC_LOCALSTORE=0` 的 P4 产物与 pin `e4c3a869…` 档
  `cmp` 相同 ✓；默认档产物**不同**且 P4 里有 3 个 `storeRefLocal` 调用点 ✓。
* **墙钟（安静机，3 组×10 次 ns 计时，P4 units=2000）**：新默认 **20 ms/次**（三组都是 20），
  kill switch **24 ms/次** ⇒ 相对改动前基线 **24.3 ms** = **−17.7%**（≥5% ✓）。
* **不劣化**：`digraph@1M` 479 vs 480 ms；`cyclic_pressure@100k` 7 vs 7 ms。
* **语义**：`EXTC_RECLAIM=0` 档 stdout 逐字节一致（`spectrum_p1/p4`、`cross_region`、`persist_nodes` 实测 SAME）。
* **`rs_*` 不变**：`cross_region` 新默认与 kill switch 都是
  `rs_inc=200000 rs_dec=196611 rs_live=3389 rs_hwm=65537` ✓。
* **门禁**：`nobench` **默认 100/0、kill switch 100/0**；`smoke 16/0`、`docs 12/0`；
  `gate-doc-refs`/`gate-invariant-table`/`gate-skip-table` 全绿；`MIN_IDLE` 未动；孤儿 0。
* **棘轮收紧**：`tests/reclaim-perf/gate.sh` 的 `LIMIT` 由 `161930000` → **`161653535`**
  （新实测 `cyclic_pressure` N=200000 总指令数 **153,955,748** × 1.05；棘轮只许往下）✓ 复跑绿。
  `gate-tax.sh` 的 `bt` 税线**无对应棘轮可收**：`bt` 早已判 idle（开/关档产物逐字节相同、税 1.000×），
  本次改动不碰它的生成物 ⇒ 线保持 1050‰ 不动。
* **牙注册进注集**（Lead 特批改 `tools/inject.py`）：`localstore-on-objhost`
  （打 `codegen.c` 的 `hreg != NULL` 分支，让对象宿主也走只写入口；期望 `gate-reclaim-switch` 红）。
  复现（本轮实跑）：`python3 tools/inject.py localstore-on-objhost`（默认 `--tree copy`）⇒
  `ok：注入后 gate-reclaim-switch 变红（FAIL cross-region：登记量判据破了（N: rs_inc=0 rs_hwm=0 / 4N: rs_inc=0 rs_hwm=0））`，
  随后 `还原：逐字相同 ✓ / 还原后复绿 ✓ / 原树核对：未被改动 ✓`，**exit 0**。
* **`gcbench` 的 Reg=0 对账（Lead 给出来源）**：它那张表的 Owned 格 `3,552,955` 与本草稿内建计数**完全一致**；
  Reg 格的 `0` 是它的 `HPSTORE` 计数器放置问题（宏没落在真正执行的站点），
  **以本方生成物内建 `TICK` 计数为准（1,547,825）** ⇒ **`gcbench` 的 Reg=0 已被本方实测取代**，不再阻塞。
