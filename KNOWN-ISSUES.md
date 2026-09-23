### 已知误拒（2026-09-23 合并 arena 健全性修复时记入）

`examples/field-strong-update.extc` 现在**被误拒**：

    struct holder { p: ?ref i32 }
    fn build() -> holder {
        var x: i32 = 5
        var h: holder = { p: ref x }
        h.p = null            // ← 那一格被置空 ⇒ 里面已经没有指向栈局部的引用了
        return h              // ⇒ 本该合法
    }

**为什么被拒**：A1 修的"根的记账深度只许长大"（`refreshRootDepth` 取 max）把
"`h.p = null` 之后根的有效深度降回 0"这条路堵死了 ⇒ 逃逸检查看到 depth 1 > 0 ⇒ 拒 ✗

**方向是安全的**（多拒不放过），所以它不阻塞合并；但它是一笔**精度账**。

**正解**是 `ARENA-FORMAL` §7.4 的**强更新**：允许"那一格被明确覆盖 ⇒ 从根的重算里去掉它"。
⚠️ 两条**码路**都要接（普通赋值 / **引用型目标**），只接一条会造出**新悬垂** ——
2026-09-23 实测过：只接普通赋值那一支时 `tests/run.sh` 从 254/1 变成 **255/0**，
但对抗用例 `tests/arena-soundness/H1_strongupdate_missed.extc` 当场抓到
`stack-use-after-scope`（`q` 格里那个指向栈局部的引用逃出去了）⇒ 已回退 ✓

**判据（做这件事时必须同时满足）**：
1. `tests/run.sh` 通过（`field-strong-update` 转绿）；
2. `tests/arena-soundness/run.sh` 里的 `H1_strongupdate_missed` **仍然 REJECT**（不许变成 ACCEPT+UAF）；
3. 反例库里 `B_field_table_stale` / `D_elemwrite_clears_table` **仍然被拒**；
4. 转正库 22 条**不许变脏**；攻击库基线**一字不动**。

---

## 已知的"未完成"（设计已给，实现没落地）

1. **路径敏感的 arena 选择（"下沉"）** —— 见 `ARENA-SOUNDNESS.md` §12。
   实测靶子：同一个 `new` 站点，只读不外存 **1.6MB** vs 多一条外存分支 **33.2MB**（200 万轮）。
   机制跑通过一次（生成出 `(*(keep == 1 ? &__extc_a[1] : &__extc_a[2]))`），但判据被我反复写错 ⇒ 未落地。
2. `alloc<T>` 不进 `promoteInto`（层号只能在 `needsHome` 那一档降，不能沿赋值边降）。
3. 报错仍分不清"真不安全"和"我算错了"（`ARENA-SOUNDNESS.md` §A1）。
