# idle 证明推宽到"纯税族"：规则 P（投影）工作单

> 状态：**工作单**。规则未实现；本文只钉"要放宽什么、凭什么安全、红绿判据是哪些"。
> 事实来源：[`TAXMAP.md`](TAXMAP.md) §4（A′ 之后重取）与 `out/reasons.tsv`。

## 0. 为什么做这个

重表（§4，A′ 之后）里，"纯税族"**不比关档省内存**（`spectrum_p4` 反而 2.0–2.25× 更费），
却要付 **1.0–2.5×** 的墙钟：

| 形状 | 税× | RSS 比（开/关）| 首条挡路理由 |
|---|---|---|---|
| `rg_walk_chain` | 1.56 | 1.27 | D 引用写进栈上槽 |
| `rg_walk_buckets` | 2.53 | 1.27 | E 循环里写 ref 字段 |
| `rg_walk_tree` | 1.34 | 1.51 | D |
| `rg_scan_parent` | 1.65 | 1.27 | D |
| `rg_grow_tail` | 2.95 | 1.23 | E |
| `rg_scan_graph` | 5.46 | 1.49 | E |
| `rg_insert_hash` | 0.89 | 1.27 | C 宿主不是本帧 `new` |
| `spectrum_p2` | 1.01 | 1.01 | C + D |
| `spectrum_p4` | 1.55 | **2.01** | C + D |

这些形状里**对象从头活到尾**（建好—遍历—块退出整片丢），清扫本来无事可做。它们付的钱是
"发射了回收运行期"本身（每个分配点、根表、每次引用存储的 wrapper）。A′ 已经拿掉释放那条
O(活集) 破口；这一项是**本来就不必付**的。

## 1. 现状：idle 是**整函数合取**，卡在 D/E/C

* D 的判据在 `idleStoreQualified`（`src/check/check_top.c`）：写进槽的值必须是 `null`，或它的
  arena 层 **≥ 本次写入所在的最内层循环体**（细则 (b′)："随本迭代的 arena 一起死"）。
* 游标不满足：`cur = (*cur).next` 里值的层是**长命链所在的层**（实测 `rg_walk_chain` 上
  链在层 1、循环体是层 2）⇒ 判否。
* 其余事实（哪些绑定是"本帧 fresh"、哪些字段被写、写在谁身上）**已经在台账里**
  （`IdleFresh` / `IdleFieldStore` / `IdleSlotSite`，由 `idleSolveLvls` 求最小不动点）。

## 2. 规则 P：**覆盖一个槽不丢根，当旧值仍从某个活根可达**

两个 flavor（都是"局部可判"的充分条件，不声称充要）：

**(i) 新值链住旧值**：`v.f = <槽的旧值>` 先发生，随后 `slot = v`，且 `v` 是本帧 fresh、
两次写之间没有别的 `slot = …`。⇒ 旧值由 `v.f` 可达。（头插 `p.next = head; head = p`。）

**(ii) 旧值是某个活根的字段投影**：`slot = (*r).f₁…fₙ`，且

* `r` 的投影链**只由"新鲜宿主的首次初始化"改写** —— 即字段 `fᵢ` 的每个写站点宿主都是本帧 fresh
  且同一宿主不写第二次（这条正好是 (a′) 已经在算的事实）；
* `r` 的绑定本身不被重赋值（或它的重赋值也被 (i) 链住）；
* 投影沿链**传递**（`cur` ← `head` ← …），所以它要并进 `idleSolveLvls` 的不动点一起解，而不是
  单点判断。

两条合起来覆盖"建链 + 遍历"这一整类：建链用 (i)、遍历用 (ii)。

## 3. 判据（**先红后绿**）

**红（今天必红，实现后必须转 ZERO 且 N vs 4N 峰值 <1.5×）**：
`rg_walk_chain` · `rg_walk_tree` · `rg_scan_parent` · `rg_walk_buckets` · `rg_insert_hash` ·
`rg_grow_tail` · `rg_scan_graph` · `spectrum_p2` · `spectrum_p4`。

**护栏（必须仍 PAYS —— 它们真的丢引用）**：
`rg_relink_delete`（中间摘链）· `rg_overwrite_ring` / `rg_walk_array`（覆盖）·
`rg_evict_cache`（淘汰）· `rg_drop_component` / `cyclic_pressure` / `ring_per_round`（环）。

**棘轮**：`gate-rc-idle-corpus` 的"无 rc 形状数"只许**升**（当前 14）；`gate-tax`（`bt` 1.000×）
不许退；`gate-idle-bounded` 的形状比不许退。**反空跑**：护栏那组必须仍然付钱（判 idle 就红）。

## 4. 实现落点与难点

* 落点：`idleWalkStmt` 的 D 分支（`check_top.c:6111` 起）——把"槽写入是否合格"从
  `idleStoreQualified(lvl, loopScope)` 换成"或 P 成立"；事实在 `idleSolveLvls`（`:6046` 起）。
* 难点：
  1. `head` 在建链循环里**被重赋值**（每轮一次）⇒ 不能只靠"绑定不重赋值"；要靠 (i) 的
     "新值链住旧值"；
  2. 字段 `next` **有写**，但宿主全是 fresh（(a′) 的事实已有）⇒ 路径稳定性能用现成台账；
  3. 跨函数（P4 的 `tableInsert`，宿主是**形参**）属于 C 规则，不在 P 里 —— 那一条要另一份
     "形参 `partN`/区域归属"的事实，单开；
  4. **别把 `rg_relink_delete` 放行**：`(*prev).next = (*cur).next` 之后 `cur` 真的不可达，
     它是 P 的反例，必须留在语料里当护栏。

## 5. 为什么这一轮不直接落进 `src/`

P 是**整函数合取**里的一项：放行一个站点 ⇒ 整个函数不再发射回收运行期，而误判的失效方向是
**内存无界**（不是悬垂），且只在"跑得足够久"的形状上暴露。按仓库纪律：**先把判据接上**
（护栏 + 红集），再逐条放宽、每次复跑语料与门禁。所以顺序是：

1. 把 §3 的红集/护栏接成一个可跑的判据脚本（现在只有 `gate-rc-idle-corpus` 的计数棘轮）；
2. 在 `idleSolveLvls` 里加"投影"维度，只放宽 flavor (i)，跑一遍；
3. 再加 (ii) 的传递投影，跑一遍；
4. 全部绿了才翻进默认（`scanUnitReclaim` 的判据）。
