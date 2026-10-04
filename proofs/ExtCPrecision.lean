/-
  ExtCPrecision.lean —— **精度**的定理部分。

  精度必须先定义清楚，否则数字没有内容。这里用最标准的口径：

      抽象点 a 的**最大健全集合** `MaxSound γ a` := a 的所有具体化都安全

  于是：

    §1  最优性：`MaxSound` 本身健全，且**支配任何**健全规则
        ⇒ 只看得见同一个抽象点的规则里，没有比它更宽的 ⇒ **损失不可能来自规则**。
    §2  被迫性：抽象点里只要混进**一个**不安全的具体化，任何健全规则都必须拒绝它
        ⇒ 这部分损失改规则消不掉（只能靠**细化抽象**）。
    §3  存储判定的最优性：把抽象取成"记的数 dr + 目的地深度 destD"，
        最大的健全规则**恰好**就是检查器那条 `dr ≤ destD`
        ⇒ 规则层**零松弛**；`拒绝 ⟺ 存在不安全的一致情形`。
    §4  **逐存储 vs 逐状态**：规则是逐存储判的、安全是逐状态的
        ⇒ 必然误拒"坏指针在目标区域死之前就被覆盖"那一族。
        （实测：无分支片段里 138/138 = 100% 的误拒都是这一族，见 precision.py。）

  本文件不含 `sorry`、不引入 `axiom`。
-/

import ExtCRegion

set_option autoImplicit false
set_option linter.deprecated false

namespace ExtC.Precision

open ExtC.Region

/-! ## §1 一般刻画：最优规则与"损失只在抽象" -/

/-- 抽象点 `a`（经具体化关系 `γ`）的**最大健全集合**：
    该抽象点的**所有**具体化都安全。 -/
def MaxSound {α : Type} (γ : α → World → Prop) (a : α) : Prop :=
  ∀ w, γ a w → w.Safe

/-- `MaxSound` 本身健全。 -/
theorem maxSound_sound {α : Type} (γ : α → World → Prop) :
    ∀ a w, MaxSound γ a → γ a w → w.Safe :=
  fun _ w h hw => h w hw

/-- ★ **任何健全规则都不比它更宽**：
    只看得见抽象点 `a` 的规则 `R`，若健全（放行的每个具体化都安全），
    则 `R a → MaxSound γ a`。
    ⇒ **精度损失不可能来自"规则写得不好"，只能来自抽象本身。** -/
theorem any_sound_rule_le_maxSound {α : Type} (γ : α → World → Prop) (R : α → Prop)
    (hSound : ∀ a w, R a → γ a w → w.Safe) :
    ∀ a, R a → MaxSound γ a :=
  fun a ha w hw => hSound a w ha hw

/-- ★ **损失是被迫的**：抽象点里若存在一个不安全的具体化，
    则**任何**健全规则在该点都必须拒绝 —— 这一部分改规则消不掉。 -/
theorem loss_is_forced {α : Type} (γ : α → World → Prop) (R : α → Prop)
    (hSound : ∀ a w, R a → γ a w → w.Safe) {a : α}
    (hBad : ∃ w, γ a w ∧ ¬ w.Safe) : ¬ R a := by
  obtain ⟨w, hw, hns⟩ := hBad
  intro hR
  exact hns (hSound a w hR hw)

/-- 反过来：抽象点里所有具体化都安全 ⇒ 最优规则放行它
    ⇒ **损失只发生在"混淆点"（混进了不安全具体化的抽象点）上**。 -/
theorem no_loss_at_clean_points {α : Type} (γ : α → World → Prop) {a : α}
    (hClean : ∀ w, γ a w → w.Safe) : MaxSound γ a :=
  fun w hw => hClean w hw

/-! ## §2 存储判定：抽象 = "记的数 + 目的地深度" -/

/-- 存储判定所看得见的全部信息。 -/
structure StoreView where
  dr : Nat        -- 检查器为被存的**值**记的深度（越小活得越久）
  destD : Nat     -- 目的地所在区域的深度
deriving DecidableEq, Repr

/-- 一次存储在该抽象点下安全 ⟺ 值的真实深度不超过目的地深度。 -/
def StoreSafe (t destD : Nat) : Prop := t ≤ destD

/-- "与抽象点一致"的真实情况：真实深度不超过记的数（这正是 INV-H 的方向）。 -/
def Conforms (v : StoreView) (t : Nat) : Prop := t ≤ v.dr

/-- ★ **最大的健全存储规则恰好就是检查器那条判据** `dr ≤ destD`：
    只看 `(dr, destD)` 的规则里，没有比它更宽而又健全的
    ⇒ **规则层零松弛**。 -/
theorem best_store_rule (v : StoreView) :
    (∀ t, Conforms v t → StoreSafe t v.destD) ↔ v.dr ≤ v.destD := by
  constructor
  · intro h; exact h v.dr (Nat.le_refl v.dr)
  · intro h t ht; exact Nat.le_trans ht h

/-- ★ **拒绝 ⟺ 存在一个不安全的一致情形**：
    被拒的抽象点被"混进了不安全具体化"**精确刻画**，不多不少。 -/
theorem store_reject_iff (v : StoreView) :
    (∃ t, Conforms v t ∧ ¬ StoreSafe t v.destD) ↔ ¬ (v.dr ≤ v.destD) := by
  constructor
  · intro ⟨t, ht, hns⟩ hle; exact hns (Nat.le_trans ht hle)
  · intro h; exact ⟨v.dr, Nat.le_refl v.dr, h⟩

/-- ★ **被迫性（存储规则版）**：抽象点里存在不安全的一致情形
    ⇒ 任何健全规则都得拒绝它。 -/
theorem store_loss_forced {v : StoreView} {R : StoreView → Prop}
    (hSound : ∀ v t, R v → Conforms v t → StoreSafe t v.destD)
    (hBad : ∃ t, Conforms v t ∧ ¬ StoreSafe t v.destD) : ¬ R v := by
  obtain ⟨t, ht, hns⟩ := hBad
  intro hR
  exact hns (hSound v t hR ht)

/-! ## §3 逐存储 ≠ 逐状态：损失的**位置**

    §2 说的是"给定这一刀的抽象，规则最优"。但规则是**逐存储**判的，
    而安全是**逐状态**的：一个指针只在"目标区域死亡时它还在"才构成悬垂。
    于是"坏指针在目标区域死之前就被覆盖掉"的程序虽然安全，却必然被逐存储纪律拒掉。

    下面给最小见证：**同一刀存储**（因此任何只看这一刀的判据对两者给同一答案），
    一个后续是覆盖 ⇒ 安全，另一个后续是释放 ⇒ 不安全。 -/

/-- 最小见证的起点：单元 0 在长寿区域 0，单元 1 在短命区域 1，单元都是空的。 -/
def Wg : World where
  F := Undershoot.frame
  alive := Undershoot.inTwo
  reg := fun x => if x = 0 then 0 else 1
  ptr := fun _ => none

/-- ★ **逐存储纪律必然误拒"坏指针先被覆盖"那一族**。 -/
theorem store_granular_loss :
    ∃ (a b : Addr),
      -- ① 检查器对这一刀一律拒绝（只看 dr 与目的地深度）
      ¬ (Wg.F.depth (Wg.reg b) ≤ Wg.F.depth (Wg.reg a)) ∧
      -- ② 后续把该单元覆盖 ⇒ 具体安全
      ((Wg.store a b).store a a).Safe ∧
      -- ③ 后续释放那只区域 ⇒ 具体不安全
      ¬ ((Wg.store a b).kill (fun r => r = 1)).Safe := by
  refine ⟨0, 1, ?_, ?_, ?_⟩
  · change ¬ ((if (1 : Nat) = 0 then 0 else 1) ≤ (if (0 : Nat) = 0 then 0 else 1))
    simp
  · intro x y hAlive hPtr
    by_cases hx : x = 0
    · subst hx
      have hp : ((Wg.store 0 1).store 0 0).ptr 0 = some 0 := by simp [Wg, World.store]
      rw [hp] at hPtr
      have hy : y = 0 := (Option.some.inj hPtr).symm
      subst hy
      have hr : ((Wg.store 0 1).store 0 0).reg 0 = 0 := by
        change (if (0 : Addr) = 0 then 0 else 1) = 0
        simp
      rw [hr]
      change Undershoot.inTwo 0
      exact Or.inl rfl
    · have hp : ((Wg.store 0 1).store 0 0).ptr x = none := by simp [Wg, World.store, hx]
      rw [hp] at hPtr
      exact absurd hPtr (by simp)
  · intro h
    have hcell : ((Wg.store 0 1).kill (fun r => r = 1)).alive
        (((Wg.store 0 1).kill (fun r => r = 1)).reg 0) := by
      change (Undershoot.inTwo 0 ∧ ¬ ((0 : Nat) = 1))
      exact ⟨Or.inl rfl, by decide⟩
    have hptr : ((Wg.store 0 1).kill (fun r => r = 1)).ptr 0 = some 1 := by
      change (if (0 : Addr) = 0 then some 1 else none) = some 1
      simp
    have hbad := h 0 1 hcell hptr
    change (Undershoot.inTwo 1 ∧ ¬ ((1 : Nat) = 1)) at hbad
    exact hbad.2 rfl

/-! ## §4 结论：精度可以这样读

    **规则层（可证）**
      `best_store_rule`   —— 检查器那条判据就是该抽象的最大健全规则 ⇒ **零松弛**
      `store_reject_iff`  —— 拒绝 ⟺ 存在不安全的一致情形（不多不少）
      `loss_is_forced`    —— 抽象点里混进不安全具体化 ⇒ 任何健全规则都得拒
      ⇒ **精度损失不可能是"规则写保守了"，只能是"抽象太粗"。**

    **实测（precision.py，296,910 个程序）**
      健全性缺口 0/296,910 ✅ ；精度 94.64%（误拒 5.36%）
      无分支片段里 138/138 = 100% 的误拒都是同一族：
      **坏指针在被指区域死之前就被同单元覆盖**（§3 的形状）
      ⇒ 这一族是**可以**靠细化抽象去掉的（文档 ARENA-FORMAL §2.2 提到的那条放宽）；
        其余损失由 §2 保证是抽象层被迫的。

    本文件不含 `sorry`、不引入 `axiom`。 -/

#print axioms ExtC.Precision.any_sound_rule_le_maxSound
#print axioms ExtC.Precision.loss_is_forced
#print axioms ExtC.Precision.best_store_rule
#print axioms ExtC.Precision.store_reject_iff
#print axioms ExtC.Precision.store_loss_forced
#print axioms ExtC.Precision.store_granular_loss

end ExtC.Precision
