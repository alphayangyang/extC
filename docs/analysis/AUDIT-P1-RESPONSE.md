# AUDIT-P1 处置记录（Lead）

> 输入：`docs/analysis/AUDIT-P1.md`（173 行，doc-audit，A12 / B8 / C9 / D0）。
> 本文件只记录**处置决定**与落地位置；不重复审计内容。日期：本轮 P1 收尾。

## §0 结论

- **A 类 12 条全部处置**：11 条已改文档、1 条（A9）经复核文档中已无该陈旧数字（`grep 185.3` = 0），无需改动。
- **B 类 8 条全部处置**：B1（§17.2 表 26 行行号）用「按 `#define` 实际行号重写」的方式修，逐条 `grep -n` 复核；B2 锚点、B6 附录 D 口径（并整段按 v7 源码重生成）、B3/B4/B5/B7/B8 全部加口径标注。
- **C 类**：C6 已改量具（单侧判据），C9 已加注释，C1/C2/C3/C5 记为「未归档、下轮补」，C4 保留归档原样并登记，C7/C8 已在正文标注。
- **D 类 0 条**：无内存安全缺陷；三条残余风险（`ownDel` 推迟需 closing 标志、`ELIDE_VERIFY` 默认不验、`freeQueueDrainAll` 无生产调用点）已进 `§8.2`/`§19.5` 的下一轮清单。

## §1 A 类处置

| # | 处置 | 落地位置 |
|---|---|---|
| A1 | §6.7.1 表拆成 A / B / B′ / C / C′ 五行，明确「峰值 5–9× 只在 deadline=800 时发生；关掉 deadline 峰值回 26.6 MB，代价是分配内停顿」 | `GC-ROOFLINE.md` §6.7.1 表 + 表下读法① |
| A2 | G20 拆两行：`G20-P0`（仅 P0 轮口径，promote −20.2% / sizeratio −56.4%）与 `G20`（v7 净账，step_total 实为 +0.03% / −5.50% / −6.17%，并把 `ab.py`/`g20.py` 对称判据改为单侧） | `GC-ROOFLINE.md` §7 |
| A3 | pb 100M 改 sync **13.2–14.2 ms**、通道 **14.2–14.8 ms**、`left_slabs=24,316`/25.7 GB；新增 pb **50M** 行（12,255 slab、6.35–7.39 ms、12,019–12,130）；§19.3-1、§19.2、§20-20 同步 | `GC-ROOFLINE.md` §6.7.3/§7 G3b；`MEMORY-MODEL-CURRENT.md` §19.2/§19.3-1/§20-20 |
| A4 | 删「32 µs 结构性下限」；改为「一段典型 **4–16 µs**（p50 4–8、p99.9 58.06；每段 6.16 µs(pd)/12.76 µs(dense) = 0.75/1.56 ns per word）⇒ 5 µs < 一段」 | `GC-ROOFLINE.md` §6.7.8 末段；`MEMORY-MODEL-CURRENT.md` §19.5.3/§20-22 |
| A5 | R13 限定为「chunk=8192 的 58–329 µs 已归因；小 chunk 的 871 µs 未测」，并标「对照 worst 870–2345 µs 是高负载观测，空载 458–522 µs ⇒ 引用须带 load/时间戳」 | `GC-ROOFLINE.md` §8.1 R13 |
| A6 | G1②(b) 改「pd_bigobj **N/A**（点值 58.06 < 64，跨桶）；dense **RED**（90.74 > 64 兜底，`B_seg`=137.1 不绑定判决；独立构建两者均 N/A）」 | `GC-ROOFLINE.md` §7 G1② |
| A7 | §7 after 列加构建口径说明；G4/G7/G8/G14/G15/G16/G17 七行换成 **p1-after** 实测（G8 0.518×、G7 2,996 kB、G4 9,108/41,820/83,040、G14 0.936×、G15 14.678×、G16 0.044 s、G17 A 27,812/D 25,700/B=C 64,100 且结论改「A>D、A<B<C 不成立」） | `GC-ROOFLINE.md` §7 |
| A8 | §19.2 大对象行改标「**P0 轮**强制 chunk 对照，产物集 `build-emp`」，并补 v7 交付口径（n=26,116 / total 144,038.6 µs / max 113.4） | `MEMORY-MODEL-CURRENT.md` §19.2 |
| A9 | 复核后文档中已无「185.3」与「66→185」（应为上一轮已修）；保留 §22-5 的 v7 数（×28.7 / 144,038.6 / 113.4） | — |
| A10 | §22-2 改「pd 已闭合 493–743 µs；dense 1,365–1,849 µs RED；pb 100M 13.2–14.8 ms RED；下一优先 = region 级 closing 队列」 | `MEMORY-MODEL-CURRENT.md` §22-2 |
| A11 | G17 换 v7 数并改结论（A>D、B=C、A<B<C 不成立，饱和因 thr 先于 M_max） | `GC-ROOFLINE.md` §7 G17 |
| A12 | G3a 判据重构为「同步段 free 次数 ≤ `#classes+3`（默认 DEFER=1 下 `_frees += 9+big` 不执行）」，总量看 `[freequeue] drained_total`；旧判据仅在 DEFER=0 构建成立 | `GC-ROOFLINE.md` §7 G3a |

## §2 B/C 类处置

- **B1**：§17.2 表 26 行行号按 `grep -n '#define EXTC_RC_<NAME> '` 重写（SLAB_SIZE 330、SWEEP_BUDGET 1013、SEED_PUSHES 1104、RELEASE_BUDGET 1079、SCAN_CHUNK 2875……），逐条复核 == 审计表。
- **B2**：§19.1 锚点 `1104/1107/1119/1122/1326/1116`。
- **B3**：G18 行代价列已注明「单 rep（§22-5）」与 5-rep 合并（131,245 / 720,193.1）的区别。
- **B4**：G1② pb 区间已加「跨构建 9.99e-5–4.00e-4 ⇒ 必须标 build」。
- **B5**：§6.7.3 三源散度（pd 493/622/743 µs、dense 1,365/1,647 µs）已在表内并列。
- **B6**：附录 D 标题与口径改 **FREEZE v7**（codegen `d3393828`/16,017、reclaim `e17080d1`/4,031、driver `7b9ac61a`/1,523、build-p1 `b9fd5784`/build-lead7 `a7f2a873`/build-da2 `10960ce7`），表体按 v7 源码重生成（46 行，补回 markAdvance `reclaim.c:1350`）。
- **B7**：MEMORY §19.1 的 78–98 µs 标注为 **max 族**（含冷页/抢占），并给 task-32 的典型值 4–16 µs、0.75–1.56 ns/word；§19.4 `c_c` 行同步。
- **B8**：G3b 行写「判据 ≤1 ms、目标 0.75 ms」。
- **C6**：`tests/gc-p0/ab.py` 与 `g20.py` 的墙钟/RSS 判据改**单侧**（`Δ ≤ +3%`，改善不判红）；`g20.py` 的 step_total 三条 id 改名 `G20P0-*`（P0 轮口径），`--selftest` 仍 PASS。
- **C9**：`g1c.py` 增加注释：`[segsteps]` 不含 stale-cursor 步（`scanObj≠0` 但 `markResume` 返回 0）⇒ `(a)` 分母含这类步，over-50 占比偏保守。
- **C1/C2/C3/C5**：callgrind 原始输出、`P1-RQ3-CHUNK` 的 bench/allocstall 原始日志、`ab7` 原始行、`c_seg` 标定 pct 输出**未入库** ⇒ 登记为下一轮「证据归档」项（不阻塞结论；相关数字均已在报告正文标注来源与构建）。
- **C4**：`tests/gc-p0/out/p1-after/summary.txt:53` 的 G18 RED 是旧量具产物，**保留归档原样**（证据不可变），判决以 `p1-after/g18.json` 与 `MEASUREMENTS-P1.md` 为准；该文件头部已由 empirics 记录 supersede 说明。
- **C7**：G18 写「同一次跑内 60 = 200 = 26,072–26,076 kB（跨构建 +4 kB）」。
- **C8**：见 A5（R13 已标 load/时间戳要求）。

## §3 未采纳

- **A9**：无对象（文档中已无该数字）。
- 其余全部采纳；无「审计误报」条目。
