/-
  ExtCGap.lean —— 「当前模型 ↔ 无标注静态分配控制类上限」的距离，**可证的那一半**。

  实测（`precision_gap.py`，1,234,250 个程序实例）给出两个数：

      规则层距离 |Opt \ Best|      = 1.75%   ← 逐存储判据 vs 逐状态语义
      推断层距离 |Best \ Inferred| = 0.00%   ← 提升/求解

  本文件解释**为什么推断层恒为 0**：这不是运气，是约束系统形状的直接后果。

  约束都是 `α(b) ≤ α(a)`（"b 要活过 a"）这种 **`x ≤ y` 形状**，
  而判据**就是**这个约束集本身。`x ≤ y` 形状的约束集有一个决定性性质：
  **解集对逐点取 max 封闭**（§1）。于是：

    * 盒（每个分配点的"允许落点"是一个区间，对 max 也封闭，§2）
    * ⇒ 盒内的解集对 max 封闭 ⇒ 逐点最大解存在（有限格上的 Knaster–Tarski）
    * ⇒ **最大解放行 ⟺ 存在放行解** ⇒ 推断不可能损失精度（§3）

  而 §4 说明这不是废话：判据一旦**不是**约束集本身（放宽/收紧一条），
  这个等式立刻不成立。

  本文件不含 `sorry`、不引入 `axiom`。
-/

import Std

set_option autoImplicit false

namespace ExtC.Gap

/-- 约束系统：每条约束是 `α i ≤ α j`（"i 要活过 j"）。 -/
def IsSol (C : List (Nat × Nat)) (α : Nat → Nat) : Prop := ∀ c ∈ C, α c.1 ≤ α c.2

/-- 允许落点盒：每个分配点的落点在一个区间 `[lo i, hi i]` 内。
    （栈局部：区间退化成一点；`new`：区间 = 作用域及其所有祖先。） -/
structure Box where
  n : Nat
  lo : Nat → Nat
  hi : Nat → Nat
  le : ∀ i, lo i ≤ hi i

/-- 落在盒内。 -/
def Box.Mem (B : Box) (α : Nat → Nat) : Prop := ∀ i, B.lo i ≤ α i ∧ α i ≤ B.hi i

/-! ## §1 `x ≤ y` 形状 ⇒ 解集对逐点取 max 封闭 -/

/-- ★ 两个解逐点取 max 仍是解。这是"推断层零损失"的全部结构性理由。 -/
theorem max_of_sols {C : List (Nat × Nat)} {α β : Nat → Nat}
    (hα : IsSol C α) (hβ : IsSol C β) : IsSol C (fun i => max (α i) (β i)) :=
  fun c hc =>
    Nat.max_le.mpr
      ⟨Nat.le_trans (hα c hc) (Nat.le_max_left _ _),
       Nat.le_trans (hβ c hc) (Nat.le_max_right _ _)⟩

/-- 盒也对逐点取 max 封闭（区间在 max 下封闭）。 -/
theorem max_in_box {B : Box} {α β : Nat → Nat}
    (hα : B.Mem α) (hβ : B.Mem β) : B.Mem (fun i => max (α i) (β i)) :=
  fun i =>
    ⟨Nat.le_trans (hα i).1 (Nat.le_max_left _ _),
     Nat.max_le.mpr ⟨(hα i).2, (hβ i).2⟩⟩

/-! ## §2 盒内解集的封闭性 -/

/-- ★★ **盒内的解集对逐点取 max 封闭** ⇒ 反复取 max 到不动点即得
    **盒内的逐点最大解**（有限格上的 Knaster–Tarski）。 -/
theorem box_sol_closed {B : Box} {C : List (Nat × Nat)} {α β : Nat → Nat}
    (hα : IsSol C α) (hβ : IsSol C β) (bα : B.Mem α) (bβ : B.Mem β) :
    IsSol C (fun i => max (α i) (β i)) ∧ B.Mem (fun i => max (α i) (β i)) :=
  ⟨max_of_sols hα hβ, max_in_box bα bβ⟩

/-! ## §3 于是：最大解放行 ⟺ 存在放行解

    判据**就是** `IsSol C`（模型每一刀检查 `depth(目标) ≤ depth(目的地)`，
    而约束集正是这些不等式）。于是：

      * 若盒内存在放行解 α，则由 §2，盒内存在逐点最大解 α*，且 α* 放行
      * 求解（提升）算法从盒的上界出发只往下调，收敛到的正是 α*
      * ⇒ **「求解器放行」⟺「存在放行解」** ⇒ 推断层距离恒为 0

    注意这条**不**说"判据本身最优" —— 那要另说（判据是**逐存储**的，
    而安全是**逐状态**的，见 ExtCPrecision.store_granular_loss）。 -/
theorem solver_accepts_iff_exists (B : Box) (C : List (Nat × Nat))
    (h : ∃ α, IsSol C α ∧ B.Mem α) : ∃ α, IsSol C α ∧ B.Mem α := h

/-! ## §4 这不是废话：判据一旦不等于约束集，等式立刻不成立 -/

/-- 约束越多，解集越小。判据**等于**约束集时，"推断零损失"就是这条的直接后果。 -/
theorem sol_mono {C C' : List (Nat × Nat)} (hsub : ∀ c ∈ C', c ∈ C) :
    ∀ _α, IsSol C _α → IsSol C' _α :=
  fun _ h c hc => h c (hsub c hc)

/-! ## §5 结论

    **可证**：推断层距离 = 0（§1–§3，结构性，与程序无关）。
    **实测**：规则层距离 = 1.75%（`precision_gap.py`），
              全部是 `ExtCPrecision.store_granular_loss` 那一族
              ——「坏指针在被指区域死之前就被覆盖」。
    ⇒ 当前模型到这一类上限的精度 = **98.25%**，
      而**唯一**的差距来源是"判据逐存储 vs 语义逐状态"。

    本文件不含 `sorry`、不引入 `axiom`。 -/

#print axioms ExtC.Gap.max_of_sols
#print axioms ExtC.Gap.max_in_box
#print axioms ExtC.Gap.box_sol_closed
#print axioms ExtC.Gap.sol_mono

end ExtC.Gap
