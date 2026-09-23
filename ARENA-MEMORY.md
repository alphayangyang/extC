# arena 与「长运行服务」的内存 —— 一次未完成的尝试（记账）

> 起因：主人给的形状（`inner` 有 1/4 概率 append 到 `outer`、1/4 覆盖 `outer2`、
> 一半什么都不做），在 2e6 轮的循环里，**峰值 98 MB**，而同一形状自己内联写容器只要 50 MB。
> 结论先说：**根因找到了，机制也找到了，但落地时踩了四个坑，最后只收了"精度"那两处，
> 内存那一步没做进去。这份文档把"差一步"的确切位置写清楚。**

## 1 根因（已确证，不是猜测）

`__extc_home` 是"调用者选的那只 arena"，可以活到调用者的帧、甚至整个进程。
而**收尾 pass 里有一条无条件兜底**：

```c
/* check_top.c 收尾 pass（HEAD 原文）*/
if (site->kind == EX_NEW || site->kind == EX_GENCALL)
    if (site->arenaLevel != ARENA_HOME) site->arenaLevel = ARENA_HOME;   /* 有家 ⇒ 全进家 */
```

⇒ 只要函数"有家"，里面**每一个** `new` 都被钉进家 arena，
**包括那些根本没逃出本帧的**（每轮造了就用完的临时对象）⇒ 到帧结束才回收 ✗

实测（`/tmp/rg/r1.extc`，2e6 轮，值拷贝 append + `varArray`）：

| 形状 | 峰值 RSS |
|---|---|
| `new` 每轮 + 值拷贝 append（走 `varArray::push`） | **98 MB** |
| 同一形状，容器**自己内联**写（不走 varArray） | 50 MB |
| 每轮只造了又扔（不 append） | 1.7 MB |

50 MB 那一档是**真的活着**的数据（容器里 1e6 × 24B + 倍增垃圾），
所以 98→50 的那 48 MB **全部**是"没逃出去却被钉在家"的每轮垃圾。

## 2 已经做进去的两处（精度，落地了 ✓）

1. **`valDepthForStore`：值拷贝不背源对象的寿命**（`check_escape.c`）
   存进去的是**字节的副本** ⇒ 源对象活多久与目标格子无关。
   判据 `typeCannotCarryRef`（跟 `exprRefDepth` 开头那个早退同一条）。

2. **`stmtStoresThroughDeref`：往参数里写纯值不算"发布"**（`check_top.c`）
   `varArray<T>::push` 的体是 `self.buf[self.len] = v` —— 值拷贝 ⇒
   整条传染链 `push → grow → …` 的第一环断掉，`main` 不再"因为有家而有家"。
   ⚠️ **判据必须是纯语法的**：这个函数跑在**查函数体之前**，那时 `v->type` 还是 NULL ✗
   （第一版依赖 `typeContainsRef` ⇒ 恒为假 ⇒ `fn stash(dest, v) { @overwrite var n = new node  dest.v = n }`
     丢了家 arena ⇒ `dest.v = n` 误拒。这就是 `tests/asan/overwrite-callee` 抓出来的 ✓）

3. **`promoteInto2` 补齐载体形状**（`EX_GENCALL` / `EX_SLICE` / `EX_FIELD` / `EX_INDEX`）
   —— 否则"装着新分配的聚合值"整类提不动（`{ buf: new T[cap], … }` 就是最典型的一处）✗

## 3 没做进去的那一步，以及为什么（四个坑，都实测过）

目标：把收尾 pass 那条无条件兜底改成**按站点判**——只有"真逃出去"的才留在家。
需要回答的唯一问题是：**`ARENA_HOME` 里混了两种东西，怎么分开？**
（"真逃出去了" vs "只是因为所在函数有家"）

| # | 试法 | 结果 |
|---|---|---|
| 1 | 在 `promoteInto2` 里记 `lexicalLevel`（词法层号），收尾 pass 按 `lexicalLevel >= 1` 放回块层 | `fn build() -> mut ref node { var head = new node  return head }` ⇒ `head` **已在 ARENA_HOME** ⇒ `promoteInto2` 走**早退**、没打标记 ⇒ 被放回块层 ⇒ `build` 一返回就 release ⇒ 调用者手里是已释放的链表（`examples/escape-promotion` 死循环 / 求和是乱数）✗ |
| 2 | 在早退处也打标记 | `withCap` 的 `return v?` 分支**绕过** `checkEscape` ⇒ 还是没标记 ✗ |
| 3 | 在 `checkEscape` 开头无条件先 `promoteInto(c, val, at)` | **把一个已经提到家 arena 的站点又拉回块层**（`dest.v = n` 那次 `at=1` 覆盖了 `at=0` 的决定）⇒ `overwrite-callee` / `arena_stale_origin` 误拒 ✗ |
| 4 | 用独立的 `escaped` 标记（只由 `promoteInto` 打）+ 收尾 pass 按它判 | `withCap` 的 `new T[cap]` 走的是 `EX_TRY` 返回分支，`promoteInto` 到不了 ⇒ **必须**同时保留 `RefCheck.depth` 的就地重算；而 `head` 在 `return head` 时 `promoteInto` 也没打到（未查清）⇒ `escape-promotion` 仍坏 ✗ |

**卡住的确切位置**：`build` 里 `return head` 走到了 `checkEscape`（`[ce0]` 有日志），
但**没有**进 `promoteInto2` 的 `EX_IDENT` 分支（`[idx]` 无该条），也没有报错。
⇒ 说明 `promoteInto` 在那条路上返回了 false 而没人管它。
**下一步就从这一句查起**（`check_stmt.c:510` 那句 `promoteInto(c, s->u.ret.value, 0)`）。

## 4 另一条没走的路（也记下来，免得以后重复想）

把 `promoteInto2` 的 `EX_IDENT` 早退（`if (sy->depth <= at) return true;`）去掉，
改成"无论如何都下潜"——能绕过坑 1/2，但代价是**每次提升都要下潜整棵树**，
而且 `sy->depth` 那半本身是"初始化式与槽位同层"的论据，删掉它要重新论证。
没试；如果要试，先量清楚成本。

## 5 复现材料

- `/tmp/rg/r0.extc`：每轮只造了又扔（1.7 MB，紧的 ✓）
- `/tmp/rg/r1.extc`：值拷贝 append + 覆盖（**98 MB** ← 主人的形状）
- `/tmp/rg/r2.extc`：递归函数里每轮 append（18.7 MB）
- `/tmp/rg/r3.extc`：容器自己内联写（50 MB，**这就是目标线**）
- `/tmp/rg/oc3.extc`：`@overwrite` + 出参形状（1.5 MB）

量法：`./build/extc --run X.extc && /usr/bin/time -v ./build/X 2>&1 | grep Maximum`
⚠️ **不要用"`--run` 的 stderr 非空"判失败** —— `--run` 会把程序输出也打到 stderr，
我在这上面误判了三次 ✗ 要看**退出码**。

## 6 验收判据（任何进一步改动都必须同时满足）

1. `tests/run.sh`：通过 255 / 失败 0
2. `tests/arena-soundness/run.sh`：洞还在 = 0（3 条该拒的仍然拒）
3. `tests/arena-promoted/run.sh`：22/22，且 ASan 干净
4. `tests/asan/run.sh` = 8、`tests/arena/run.sh` = 5
5. `tests/attacks/` 通过集合与 `BASELINE` **一字不差**
6. `tools/golden.sh check`：差异必须**逐条说得清**（本次改动 = 0 字节变化 ✓）

跑法：`bash tools/arena-fix/check.sh`（⚠️ 全套 ~7 分钟；**中间要能看进度**，
我只用 `tests/run.sh` 就抓到了 `examples/escape-promotion` 的死循环 ✓）
