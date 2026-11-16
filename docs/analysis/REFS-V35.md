# REFS-V35：R2b（extent 成为可解析宿主）行号口径

> 只用于把「v34 的旧引用」换到「v35 的渲染文本行号」。R2b 只改运行期发射文本（src/back/rt/*.inc）与一个测试驱动，编译器前端零改动。

## 0. 口径

| 口径 | 值 |
| --- | --- |
| v34（R2a，提交 85ed66b0）渲染文本 | 4,997 行 / 370,381 字符 / md5 `837071b7b5d835af0cec34e72065e7c5` |
| **v35（本轮）渲染文本** | **5,050 行 / 374,981 字符 / md5 `1dcea37a8ea7858a350035507b7364b1`** |
| 物理 `src/back/reclaim.c` | 242 行，include 列表未改（`b05bbde438d39f3254dc5ea5e8b8d220`）|
| 物理 `src/back/codegen.c` | 未改：75 行 / `6ba135e22e86ec89d5cd8aa416a994bc`（渲染 16,429 行 / 878,865 字符 / `aaaaf36f73ff859044acb222934ce9c8`）|
| `tests/reclaim/driver.c` | **1,655 行 / md5 `b82c9c83270d859ed51418e2ecb96d05`**（v34：1,637 / `28dd4009…`；新增 5 条断言块）|
| 源码指纹取渲染文本 md5 | 二进制 md5 随 BUILDDIR/DWARF 变，不能作溯源 |

## 1. 改了哪些文件（`git diff --stat`）

| 文件 | 增删 | 内容 |
| --- | --- | --- |
| `src/back/rt/32-extent.inc` | +47 −1 | `ExtcRcExtArena` 加 `ownQ`（2 MiB 量子 → slab 的归属表）；`extc_rc_extBind` / `extc_rc_extLookup`；`extGive` 清量子；`extNArena` 改原子；`#else` 两个 stub |
| `src/back/rt/33-hugefree.inc` | +1 | `slabNew` 的 big 分支在 `s->data` 定下来后绑定这个 slab |
| `src/back/rt/50-xregion.inc` | +1 −1 | `extc_rc_wmarkSlot`：掩码查不到宿主就查 extent 表 |
| `src/back/rt/57-xentry.inc` | +13 −4 | `extc_rc_slabOfInterior` 加 extent 第二级；`[pin]` 探针；pin 计数器声明提前；两条跳过标记 |
| `tests/reclaim/driver.c` | +30 −12 | 「超大块载荷不认」改成 v35 契约：内部地址能命名宿主、越过容量仍失手、不再进钉住表、跨区入边登记与撤销 |
| `docs/design/RECLAMATION.md` | +12 −1 | 跳过表加 10 行（ext-bind ×4、ext-lookup ×4、host-slab-mask/ext）|

## 2. 分段换算（v34 渲染行号 → v35）

| v34 | v35 | 说明 |
| --- | --- | --- |
| 1–961 | 1–961 | 同号 |
| 962–987 | 963–988 | +1（extent 段插入 1 行）|
| 988 | 989 | 替换（`ExtcRcExtArena` 加 `ownQ`）|
| 989–1045 | 990–1046 | +1 |
| 1046–1082 | 1048–1084 | +2 |
| 1083–1094 | 1127–1138 | +44（extBind/extLookup 插入 42 行）|
| 1095–1224 | 1141–1270 | +46 |
| 1225–2195 | 1272–2242 | +47 |
| 2196–2980 | 2244–3028 | +48 |
| 2981–2986 | 3030–3035 | +49 |
| 2987–3060 | 3037–3110 | +50 |
| 3061 | 3111 | 替换（slabOfInterior 第一级返回行加跳过标记）|
| 3062–3195 | 3112–3245 | +50 |
| 3196–3198 | 3246–3251 | 替换（两级反查 + 两条跳过标记，3 行 → 6 行）|
| 3199–4998 | 3252–5050 | +53 |

## 3. v35 绝对锚点

| 位置 | v35 行 |
| --- | --- |
| `[rt:extent]` 段首 | 962 |
| `ExtcRcExtArena` 定义（含 `ownQ`）| 984–1000 一带（本段 962 起）|
| `extc_rc_extBind` | 1,089（段内第二处同名 stub：1,139）|
| `extc_rc_extLookup` | 1,112（stub：1,140）|
| `slabNew` 的绑定调用 | 1,271 |
| `extc_rc_wmarkSlot` 的 extent 回退 | 2,243 |
| `extc_rc_slabOfInterior` 第一级返回（`skip:host-slab-mask`）| 3,246 |
| `extc_rc_slabOfInterior` extent 第二级 | 3,249 |
| `extc_rc_slabOfInterior` 末尾（`skip:host-slab-ext`）| 3,250 |
| `[pin]` 探针（statsDump）| 3,252 一带（与 `[quota]` 相邻）|

## 4. 使用规则

1. 只做**语义映射**：先按上表换算，再看目标行是不是那句话/那个函数。禁止用单一 offset 平移整份文档。
2. 文档里凡引「v34 行号」的，v35 起一律改指本文第 3 节锚点或按第 2 节换算。
3. `tools/emitted-identity-baseline.tsv` 必须重冻结：R2b 改了运行期发射文本，所以每个发射运行期的生成 C 都会变；变化应只落在 extent 段、big 绑定、wmark 回退、slabOfInterior、[pin] 探针这五处。
4. v34 的 `docs/analysis/REFS-V34.md` 仍有效（v33→v34 的换算），但 v34→当前必须走本文。