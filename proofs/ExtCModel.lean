/-
  ExtCModel.lean —— 区域模型在**设计层面**的补充定理。

  ExtCRegion.lean 证明了"检查器接受 + 四条边界条件 ⇒ 无悬垂"，并给出四条边界
  各自的必要性。本文件把另外四条**设计层面**的规则也钉成定理 —— 它们不是实现细节，
  而是模型必须满足的形状，缺了哪一条，模型就与真实系统不是同一个东西：

    §A  分配站点层：定理必须量化"编译器为每个分配站点选了哪只区域"，
        并且**只有一个权威**（检查器算的 α 就是运行时用的那只）。
    §B  提升 / 降低**不对称**：把分配搬得更长寿**永远安全**（不需要别名信息）；
        把记账**降低**（强更新）才需要"没有别名能写到它"。后者在抽象里做不到 ⇒
        健全分析必须同时拒掉一对同态程序 ⇒ **误拒是被迫的**（单向定理）。
    §C  未知必须记 ⊤：不存在"安全的有限兜底"；连接只能在**见过的上界**之间取最小。
    §D  单一指称：判定的健全性 ⟺ 读到的那个数是真实深度的上界；
        若判定可能读到两个查询，则两个都必须是上界。
    §E  写点完整性：写入之后不刷新记账 ⇒ 不变量被破坏。

  本文件不含 `sorry`、不引入 `axiom`。
-/

import ExtCRegion

set_option autoImplicit false
set_option linter.deprecated false

namespace ExtC.Model

open ExtC.Region

/-! ## §A 分配站点层与"单一权威"

    ARENA-FORMAL 的定理 3 说的是「**若实现按解 α 分配**，则所有 trace 安全」。
    所以模型里必须有"哪只区域"这一层 —— 否则证的是一个编译器并不执行的规则。 -/

/-- 分配计划：编译器为每个分配站点选的区域，以及为它记的深度。 -/
structure AllocPlan (F : Frame) where
  region : Nat → Reg
  recorded : Nat → Nat
  /-- 记账上界（INV-H 的分配侧）：记的深度不浅于**真正选中**的那只区域。 -/
  upper : ∀ S, F.depth (region S) ≤ recorded S

/-- ★ **分配站点层的健全性**：检查器放行 ∧ 运行时**确实按 `plan.region` 分配**
    ⇒ 分配点选的区域活得比目的地久。 -/
theorem alloc_site_sound (F : Frame) (alive : Reg → Prop) (plan : AllocPlan F)
    {S : Nat} {dest : Reg}
    (hAliveTarget : alive (plan.region S)) (hAliveDest : alive dest)
    (hLocalDest : F.Local dest)
    (hClass : F.Local (plan.region S) ∨ F.Ground (plan.region S))
    (hChain : ∀ r s, alive r → alive s → F.Local r → F.Local s →
      F.depth r ≤ F.depth s → F.outlives r s)
    (hAcc : plan.recorded S ≤ F.depth dest) :
    F.outlives (plan.region S) dest :=
  linearization_pointwise F alive hAliveTarget hAliveDest hLocalDest hClass hChain
    (Nat.le_trans (plan.upper S) hAcc)

/-- ★ **单一权威的必要性**：若实际落点不是检查器算的那只区域，整条检查形同虚设。
    （这是"同一件事算两遍、第二遍当兜底"这一族的形式化：α 是记 0 的，
    实际却落在短命区域 —— 检查通过，而目的地在它死后仍然活着。） -/
theorem necessity_single_authority :
    ∃ (F : Frame) (α actual dest : Reg) (rec : Nat),
      F.depth α ≤ rec ∧ rec ≤ F.depth dest ∧ ¬ F.outlives actual dest := by
  refine ⟨Undershoot.frame, 0, 1, 0, 0, ?_, ?_, ?_⟩
  · change (if (0 : Nat) = 0 then 0 else 1) ≤ 0; simp
  · change (0 : Nat) ≤ (if (0 : Nat) = 0 then 0 else 1); simp
  · intro h
    rcases h with h | ⟨h, _⟩
    · exact absurd h (by decide)
    · exact absurd h (by decide)

/-! ## §B 提升 / 降低的不对称 -/

/-- **提升（放大）永远安全**：把分配搬得更长寿，放行性与安全性都保持。
    它**不需要任何别名信息** —— 这是模型里唯一"免费"的方向。 -/
theorem amplification_preserves (F : Frame) {ρ ρ' dest : Reg}
    (hAmp : F.outlives ρ' ρ) (hOut : F.outlives ρ dest) : F.outlives ρ' dest :=
  F.out_trans ρ' ρ dest hAmp hOut

/-- 放大之后，记账深度只会更小（更保守），所以 INV-H 不会因此失效。 -/
theorem amplification_depth (F : Frame) {ρ ρ' : Reg} (hAmp : F.outlives ρ' ρ) :
    F.depth ρ' ≤ F.depth ρ :=
  F.depth_mono ρ' ρ hAmp

/-- 带记账的世界：`fact a = some d` = "分析认为单元 a 里的指针，目标深度不超过 d"。 -/
structure AbsWorld extends World where
  fact : Addr → Option Nat

/-- **INV-H**：记账是真实深度的上界。 -/
def AbsWorld.InvH (A : AbsWorld) : Prop :=
  ∀ a b d, A.ptr a = some b → A.fact a = some d → A.F.depth (A.reg b) ≤ d

/-- ★ **强更新（降低记账）在抽象里得不到辩护**：

    存在两个世界，它们在**分析可见的一切**（区域帧、活区域、地址→区域映射、
    记账表）上完全相同，却一个满足 INV-I、另一个不满足。

    ⇒ 只读记账表的判定**不可能**既健全又接受"降低"这一操作：
      要么它必须对两者都保守（误拒那个安全的），要么它对不安全那个漏判。
      这就是"没有别名信息 ⇒ 只能用弱更新"（ARENA-FORMAL §7.4）的形式化，
      也说明**误拒不是实现缺陷，而是这个抽象层的定理级后果**。 -/
theorem strong_update_indistinguishable :
    ∃ (A₁ A₂ : AbsWorld),
      A₁.toWorld.F = A₂.toWorld.F ∧ A₁.alive = A₂.alive ∧
      A₁.reg = A₂.reg ∧ A₁.fact = A₂.fact ∧
      A₁.toWorld.Contains ∧ ¬ A₂.toWorld.Contains := by
  let reg : Addr → Reg := fun a => if a = 0 then 0 else 1
  let A₁ : AbsWorld :=
    { F := Undershoot.frame, alive := Undershoot.inTwo, reg := reg,
      ptr := fun _ => none, fact := fun _ => some 0 }
  let A₂ : AbsWorld :=
    { F := Undershoot.frame, alive := Undershoot.inTwo, reg := reg,
      ptr := fun a => if a = 0 then some 1 else none, fact := fun _ => some 0 }
  refine ⟨A₁, A₂, rfl, rfl, rfl, rfl, ?_, ?_⟩
  · intro a b h
    exact absurd h (by simp [A₁])
  · intro hC
    have h := hC 0 1 (by simp [A₂])
    -- 定义等价地展开成具体命题（不变量由构造保证）
    have h' : (1 : Nat) = 0 ∨ ((1 : Nat) = 0 ∧ (0 : Nat) = 1) := h
    rcases h' with h1 | ⟨h1, _⟩ <;> exact absurd h1 (by decide)

/-! ## §C 未知必须记 ⊤ -/

/-- ★ **不存在"安全的有限兜底"**：任何固定的兜底深度 `c`，都存在真实深度更大的值，
    使 `c ≤ 目的地深度` 成立而 `真实深度 ≤ 目的地深度` 不成立。

    ⇒ 对"未识别"的唯一健全处置是**拒绝**（等价于记 ⊤），而不是挑一个"看起来保守"的数。
    这条一次性排除了"兜底方向反了"那一整族（`placeDepth` 兜底答 0 之类）。 -/
theorem no_sound_finite_fallback :
    ¬ ∃ c : Nat, ∀ trueDepth destDepth : Nat, c ≤ destDepth → trueDepth ≤ destDepth := by
  intro ⟨c, h⟩
  have := h (c + 1) c (Nat.le_refl c)
  omega

/-- 正确的连接：只在**见过的**上界之间取最小；一个都没见到 ⇒ `none`（= ⊤，不施加约束）。 -/
def joinSeen : List Nat → Option Nat
  | [] => none
  | x :: xs =>
      match joinSeen xs with
      | none => some x
      | some y => some (min x y)

/-- 只要每个**见过的**数都是上界，连接结果就是上界（`none` 表示"没有约束"）。 -/
theorem joinSeen_sound : ∀ (l : List Nat) (t : Nat), (∀ x ∈ l, t ≤ x) →
    ∀ d, joinSeen l = some d → t ≤ d := by
  intro l
  induction l with
  | nil => intro t _ d hd; simp [joinSeen] at hd
  | cons x xs ih =>
      intro t h d hd
      have hx : t ≤ x := h x (by simp)
      have htl : ∀ y ∈ xs, t ≤ y := fun y hy => h y (by simp [hy])
      simp only [joinSeen] at hd
      split at hd
      · cases hd; exact hx
      · rename_i y hy
        cases hd
        exact Nat.le_min.mpr ⟨hx, ih t htl y hy⟩

/-- 哨兵 0 的两种危害，各一条：

    (a) **寿命选择**：要用"真正的最小"（最浅 = 最长久）当落点深度；
        错的实现给出更大的数 ⇒ 对象比某个目的地先死。 -/
theorem realMin_is_lower_bound : ∀ (l : List Nat), ∀ d ∈ l, realMin l ≤ d := by
  intro l
  induction l with
  | nil => intro d hd; exact absurd hd (by simp)
  | cons x xs ih =>
      intro d hd
      rcases List.mem_cons.mp hd with rfl | hd'
      · exact Nat.min_le_left _ _
      · exact Nat.le_trans (Nat.min_le_right _ _) (ih d hd')

/-- (b) 错的那一版会**违反**这条界 ⇒ 存在一个目的地，对象活不过它。 -/
theorem buggyMin_violates_bound :
    ∃ (l : List Nat) (d : Nat), d ∈ l ∧ ¬ (buggyMin l ≤ d) :=
  ⟨[0, 1], 0, by simp, by decide⟩

/-- 后果：落点选深了 ⇒ 有一个目的地比它长寿 ⇒ 悬垂。 -/
theorem allocation_needs_true_min :
    ∃ (dests : List Nat) (chosen : Nat), chosen = buggyMin dests ∧ ∃ d ∈ dests, d < chosen :=
  ⟨[0, 1], 1, rfl, 0, by simp, by decide⟩

/-! ## §D 单一指称（一个事实只有一个指称） -/

/-- ★ **判定的健全性 ⟺ 读到的数是上界**。

    这是"一个事实只能有一个指称"的精确形式：判定是 `dr ≤ dest`，
    它可靠**当且仅当** `dr` 是该值真实深度的上界 —— 没有第二种可能。 -/
theorem accept_sound_iff (t dr : Nat) :
    (∀ dest, dr ≤ dest → t ≤ dest) ↔ t ≤ dr := by
  constructor
  · intro h; exact h dr (Nat.le_refl dr)
  · intro h dest hd; exact Nat.le_trans h hd

/-- ★ **若判定可能读到两个查询，则两个都必须是上界**。
    等价说法：它们的最小值仍然必须是上界。

    这正是"同一个事实有多条查询路径"为什么会致命：只要有一路记小，
    `min` 就被拉下去，判定放行的存储就不再有保证。 -/
theorem either_query_must_be_upper (t q₁ q₂ : Nat) :
    ((∀ dest, q₁ ≤ dest → t ≤ dest) ∧ (∀ dest, q₂ ≤ dest → t ≤ dest))
      ↔ t ≤ min q₁ q₂ := by
  constructor
  · intro ⟨h1, h2⟩
    exact Nat.le_min.mpr ⟨h1 q₁ (Nat.le_refl q₁), h2 q₂ (Nat.le_refl q₂)⟩
  · intro h
    refine ⟨?_, ?_⟩
    · intro dest hd
      exact Nat.le_trans h (Nat.le_trans (Nat.min_le_left q₁ q₂) hd)
    · intro dest hd
      exact Nat.le_trans h (Nat.le_trans (Nat.min_le_right q₁ q₂) hd)

/-! ## §E 写点完整性（写入必须刷新判定所读的那张表） -/

/-- 写单元 `a`，**不动**记账。 -/
def AbsWorld.write (A : AbsWorld) (a b : Addr) : AbsWorld :=
  { A with ptr := fun x => if x = a then some b else A.ptr x }

/-- 写单元 `a`，**同时**把记账刷成新目标深度的上界。 -/
def AbsWorld.writeFact (A : AbsWorld) (a b : Addr) (d : Nat) : AbsWorld :=
  { A with ptr := fun x => if x = a then some b else A.ptr x,
           fact := fun x => if x = a then some d else A.fact x }

/-- ★ **不刷新 ⇒ 不变量破**：存在一个满足 INV-H 的世界，写入一个更深的指针之后
    记账停在旧值，于是 INV-H 不再成立（这正是"出参写没人发布"那一族）。 -/
theorem stale_fact_breaks_invh :
    ∃ (A : AbsWorld) (a b : Addr) (d : Nat),
      A.InvH ∧ A.fact a = some d ∧ A.F.depth (A.reg b) > d ∧ ¬ (A.write a b).InvH := by
  let reg : Addr → Reg := fun a => if a = 0 then 0 else 1
  let A : AbsWorld :=
    { F := Undershoot.frame, alive := Undershoot.inTwo, reg := reg,
      ptr := fun _ => none, fact := fun _ => some 0 }
  refine ⟨A, 0, 1, 0, ?_, ?_, ?_, ?_⟩
  · intro a b d hp _
    exact absurd hp (by simp [A])
  · simp [A]
  · change (1 : Nat) > 0; omega
  · intro hInv
    have h := hInv 0 1 0 (by simp [AbsWorld.write, A]) (by simp [AbsWorld.write, A])
    change Undershoot.frame.depth 1 ≤ 0 at h
    simp [Undershoot.frame] at h

/-- ★ **刷新 ⇒ 不变量保持**（写点完整性的正面形式）：只要把记账刷成新目标的上界，
    INV-H 在写入之后仍然成立。 -/
theorem writeFact_preserves_invh (A : AbsWorld) (hA : A.InvH) {a b : Addr} {d : Nat}
    (hd : A.F.depth (A.reg b) ≤ d) : (A.writeFact a b d).InvH := by
  intro x y e hp hf
  simp only [AbsWorld.writeFact] at hp hf
  by_cases hx : x = a
  · subst hx
    rw [if_pos rfl] at hp hf
    cases hp
    cases hf
    exact hd
  · rw [if_neg hx] at hp hf
    exact hA x y e hp hf

/-! ## §F 结论：模型设计的边界（本文件证成的内容）

    **规则的不对称**（设计层的核心）：
      `amplification_preserves`  —— 放长寿：永远安全，不需要别名信息
      `strong_update_indistinguishable` —— 降低记账：抽象层**无法**辩护 ⇒
                                          健全分析必须同时拒掉两个同态程序
      ⇒ 模型是**单向**的：analysis ⇒ semantics。反向（完备）不成立，也不该去证。

    **记数的唯一正确契约**：
      `accept_sound_iff`        —— 判定健全 ⟺ 记的数是上界
      `either_query_must_be_upper` —— 多路查询时必须**每一路**都是上界
      `no_sound_finite_fallback`   —— 未知没有"安全的有限兜底"，只能记 ⊤

    **分配与写点**：
      `alloc_site_sound`        —— 定理必须带上"每个站点选了哪只区域"
      `necessity_single_authority` —— 两个权威（检查器 vs 运行时）不一致 ⇒ 检查作废
      `stale_fact_breaks_invh` / `writeFact_preserves_invh` —— 写点完整性

    本文件不含 `sorry`、不引入 `axiom`。 -/

#print axioms ExtC.Model.alloc_site_sound
#print axioms ExtC.Model.necessity_single_authority
#print axioms ExtC.Model.strong_update_indistinguishable
#print axioms ExtC.Model.no_sound_finite_fallback
#print axioms ExtC.Model.joinSeen_sound
#print axioms ExtC.Model.accept_sound_iff
#print axioms ExtC.Model.either_query_must_be_upper
#print axioms ExtC.Model.stale_fact_breaks_invh
#print axioms ExtC.Model.writeFact_preserves_invh

end ExtC.Model
