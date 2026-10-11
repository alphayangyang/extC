/-
  ExtCReclaim.lean —— 「覆盖即回收」在**区域粒度**下能走到哪一步，机器检查版。

  背景（见 ExtCMemGap.lean 的阈值定律）：内存差距有两个来源，
  ② 是「长命区域里有**可达但已死**的对象」（典型：`outer = p` 覆盖式使用）。
  本文回答：**在区域模型里，这一块能不能回收？**

  三条结论，全部机器检查：

    §1  回收的**安全条件**：杀掉一个「没有任何活单元指向」的区域集合保持 `Safe`，
        而且**不需要向下封闭** —— 这是「覆盖即回收」比现有 `Step.kill` 宽松的地方。
    §2  现有 `Step.kill`（向下封闭）在 INV-I 下**必然连引用者一起杀**。
    §3  ⇒ **区域粒度的两难**：想回收一个仍被活单元指向的区域，`Safe` 必破。
        于是"单元还活着、它指向的那一块被单独回收"在区域粒度下**不可表达** ——
        这正是 ExtCRegion.lean §8 里那条「`INV-A（块不搬家/只追加）无法陈述」的根源，
        也是运行期提权（块转交）必须把**块**引入模型的原因。

  记号沿用 ExtCRegion.lean：`outlives r s` 读作「r 活得 ≥ s」；深度越小活得越久。
-/
import ExtCRegion

namespace ExtC.Region

namespace Reclaim

variable {W : World}

/-! ## §1 安全条件：杀掉"没人指向"的区域，不需要向下封闭 -/

/-- **核心引理**：若没有任何**活单元**指向被杀的任一区域，则 `Safe` 保持。

    与 `World.liveClosed_kill` 对比：那一条要求 `k` 向下封闭（`k r → outlives r s → k s`），
    于是连同"指向它的单元"的区域一起被杀。这一条只要求"没人指向"，
    所以**单元可以活着，而它曾经指向的那一块被回收** —— 覆盖即回收要的正是这个。 -/
theorem safe_kill_unreachable (hS : W.Safe) {k : Reg → Prop}
    (hNoPtr : ∀ a b, W.alive (W.reg a) → W.ptr a = some b → ¬ k (W.reg b)) :
    (W.kill k).Safe := by
  intro a b ha hp
  show W.alive (W.reg b) ∧ ¬ k (W.reg b)
  exact ⟨hS a b ha.1 hp, hNoPtr a b ha.1 hp⟩

/-- §1 的另一半：条件**不可省**。只要还有一个活单元指着被杀的区域，`Safe` 就破。 -/
theorem kill_pointed_region_unsafe {a b : Addr} (hp : W.ptr a = some b)
    (hAliveCell : W.alive (W.reg a)) {k : Reg → Prop}
    (hKillTarget : k (W.reg b)) (hKeepCell : ¬ k (W.reg a)) :
    ¬ (W.kill k).Safe := by
  intro hSafe
  exact (hSafe a b ⟨hAliveCell, hKeepCell⟩ hp).2 hKillTarget

/-! ## §2 现有 `Step.kill` 会连引用者一起杀 -/

/-- 由 INV-I（`outlives (reg b) (reg a)`，目标活得 ≥ 单元），
    向下封闭的释放**必然**把指向它的单元的区域一起杀掉。 -/
theorem downward_kill_takes_referrers (hC : W.Contains) {k : Reg → Prop}
    (hDown : ∀ r s, k r → W.F.outlives r s → k s) {a b : Addr}
    (hp : W.ptr a = some b) (hk : k (W.reg b)) : k (W.reg a) :=
  hDown (W.reg b) (W.reg a) hk (hC a b hp)

/-! ## §3 区域粒度的两难（为什么必须引入「块」）

    把 §1 与 §2 拼起来：
      · 要 `Safe`，就必须"没人指向"才能回收（`kill_pointed_region_unsafe`）；
      · 而被提升过的对象与持有它的单元**同区**（提升的定义），单元活着 ⇒ 区域活着
        ⇒ 区域粒度下**无法只回收那个对象**。
    于是两条路都走不通：
      (a) 向下封闭地杀掉那个区域 ⇒ 连单元一起杀（§2）—— 单元是栈上的变量，不能杀；
      (b) 不向下封闭地杀 ⇒ `Safe` 破（§1 的另一半）或缺 `LiveClosed`。
    ⇒ 唯一出路是引入**比区域更细的单位**：块。块可单独回收，区域不动。 -/

/-- §3 的极简形式：只要"杀目标、留单元"，`Safe` 必破。
    把它与 §2 并读，就是"区域粒度下覆盖即回收不可表达"的完整论证。 -/
theorem region_granularity_dilemma {a b : Addr} (hp : W.ptr a = some b)
    (hAliveCell : W.alive (W.reg a)) {k : Reg → Prop}
    (hKillTarget : k (W.reg b)) (hKeepCell : ¬ k (W.reg a)) :
    ¬ (W.kill k).Safe :=
  kill_pointed_region_unsafe hp hAliveCell hKillTarget hKeepCell

/-- 一致性检查：`Safe` 的定义就是逐指针的，所以上面的反证只需一步展开。 -/
example (hS : W.Safe) {a b : Addr} (ha : W.alive (W.reg a)) (hp : W.ptr a = some b) :
    W.alive (W.reg b) := hS a b ha hp

end Reclaim

end ExtC.Region
