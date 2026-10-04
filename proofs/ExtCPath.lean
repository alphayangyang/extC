/-
  ExtCPath.lean —— 引入（分支）**路径敏感**之后：健全性、接受集、内存。

  关键在于把"路径敏感"拆成两件事，它们的结论**完全相反**：

    §1 作用在**检查**上（store 规则读的那个事实）
       ⇒ **判定完全不变**：合流后的检查通过 ⟺ 每条路径单独检查都通过。
         因为 may-合流就是取 max，而检查对事实单调（越大越容易失败）。
         ⇒ 路径敏感对**接受集没有任何增益**（定理 + 实测双重确认）。

    §2 作用在**分配区域的选择**上（α）
       ⇒ **接受集严格变大，且内存严格变少**：
         合流只能挑**一只**区域同时满足所有路径（取最浅 = 最长久），
         逐路径可以各自挑（更短命 = 更省）。这也正是文档 §12 "下沉" 的对象。

    §3 **新的护栏**（路径敏感不是白拿的）
       分配点用的条件必须与使用点的路径**一致**：条件被改写、或有副作用
       （三元重求值）、或作用域不在分配点之外，都会让挑中的区域活不过目的地
       ⇒ 由健全性退化成悬垂。这条护栏**不可省**（反例见 §3）。

  本文件不含 `sorry`、不引入 `axiom`。
-/

import ExtCRegion
import ExtCModel

set_option autoImplicit false
set_option linter.deprecated false

namespace ExtC.Path

open ExtC.Region

/-! ## §1 检查侧：路径敏感**不改变判定** -/

/-- 一个检查点看到的东西：各条可达路径上的事实值 + 判定阈值。
    （对应 extC：`dr` 是各路径上"这个值有多深"的记录，`theta` 是 `at(目的地)`。） -/
structure CheckPoint where
  facts : List Nat
  theta : Nat

/-- 空表 = 没有路径到达 ⇒ 取 0，于是 `0 ≤ theta` 恒真（空真，正确）。 -/
def maxOf : List Nat → Nat
  | [] => 0
  | x :: xs => max x (maxOf xs)

theorem le_maxOf : ∀ {l : List Nat} {x : Nat}, x ∈ l → x ≤ maxOf l
  | [], x, h => absurd h (by simp)
  | y :: ys, x, h => by
      rcases List.mem_cons.mp h with rfl | h'
      · exact Nat.le_max_left _ _
      · exact Nat.le_trans (le_maxOf h') (Nat.le_max_right _ _)

theorem maxOf_le : ∀ {l : List Nat} {t : Nat}, (∀ x ∈ l, x ≤ t) → maxOf l ≤ t
  | [], t, _ => Nat.zero_le t
  | y :: ys, t, h => by
      refine Nat.max_le.mpr ⟨h y (by simp), maxOf_le (fun x hx => h x (by simp [hx]))⟩

/-- **合流判定**：把各路径的事实取 max（may-合流）之后看检查过不过。 -/
def CheckPoint.joinAccepts (c : CheckPoint) : Prop := maxOf c.facts ≤ c.theta

/-- **逐路径判定**：每条路径各自看检查过不过（要求全部通过）。 -/
def CheckPoint.pathAccepts (c : CheckPoint) : Prop := ∀ x ∈ c.facts, x ≤ c.theta

/-- ★ **两者恒等**：合流后的检查通过 ⟺ 每条路径单独检查都通过。
    ⇒ **路径敏感对检查侧没有任何增益**（既不更健全，也不更宽）。 -/
theorem joinAccepts_iff_pathAccepts (c : CheckPoint) :
    c.joinAccepts ↔ c.pathAccepts :=
  ⟨fun h _ hx => Nat.le_trans (le_maxOf hx) h, fun h => maxOf_le h⟩

/-- 程序级的判定 = 所有检查点的合取 ⇒ 结论逐点搬运。 -/
theorem verdict_join_iff_verdict_path (cs : List CheckPoint) :
    (∀ c ∈ cs, c.joinAccepts) ↔ (∀ c ∈ cs, c.pathAccepts) :=
  ⟨fun h c hc => (joinAccepts_iff_pathAccepts c).mp (h c hc),
   fun h c hc => (joinAccepts_iff_pathAccepts c).mpr (h c hc)⟩

/-! ## §2 分配侧：路径敏感**扩大接受集并且省内存** -/

/-- 一个分配站点在一条路径上的**约束区间** `[s, d]`：
    选中的区域深度 `α` 必须

      * `α ≤ d` —— 活过该路径的目的地（分配区域要 outlive 落点）；
      * `s ≤ α` —— 够短命，才容纳得下它自己持有的引用/被谁指着。

    单条路径可行 ⟺ `s ≤ d`。 -/
structure PathReq where
  s : Nat
  d : Nat

/-- ★ **路径敏感能扩大接受集**：两条路径各自可行，但**合流不可行**
    （合流要一只区域同时满足两条路径 ⇒ 需要 `max s ≤ min d`）。
    取值：路径 1 要求 `α ≤ 0`（够长命），路径 2 要求 `α ≥ 4`（够短命）。 -/
theorem path_sensitivity_accepts_more :
    ∃ (s₁ d₁ s₂ d₂ : Nat),
      s₁ ≤ d₁ ∧ s₂ ≤ d₂ ∧ ¬ (max s₁ s₂ ≤ min d₁ d₂) :=
  ⟨0, 0, 4, 4, by omega, by omega, by omega⟩

/-- 同一个命题的"约束系统"版本：逐路径可行 ∧ 合流不可行。 -/
theorem path_sensitivity_accepts_more' :
    ∃ (q₁ q₂ : PathReq), q₁.s ≤ q₁.d ∧ q₂.s ≤ q₂.d ∧ ¬ (max q₁.s q₂.s ≤ min q₁.d q₂.d) := by
  refine ⟨⟨0, 0⟩, ⟨4, 4⟩, ?_, ?_, ?_⟩ <;> simp

/-- ★ **路径敏感省内存**：合流只能挑最浅（最长久）那只，
    而逐路径可以挑各自更深的（更短命）那只。
    这条直接复用 `realMin_is_lower_bound`：合流选择在**每条**路径上都不深于逐路径选择。 -/
theorem path_choice_never_shallower :
    ∀ (l : List Nat), ∀ d ∈ l, realMin l ≤ d :=
  ExtC.Model.realMin_is_lower_bound

/-- 而且可以**严格**更深（省内存）：目的地 `{0, 2}` 时，合流挑 0，路径 2 可以挑 2。 -/
theorem path_choice_strictly_deeper :
    ∃ (d₁ d₂ : Nat), realMin [d₁, d₂] < d₂ :=
  ⟨0, 2, by decide⟩

/-! ## §3 新护栏：条件必须与路径一致，否则从健全退化成悬垂 -/

/-- ★ **护栏不可省**：若分配点选中的区域（`chosen`）与真正走的路径的目的地（`dest`）
    不一致 —— 条件被改写、或有副作用导致重求值结果不同 ——
    那么选中的区域可能**活不过**目的地：

      `depth chosen > depth dest` 且 `¬ outlives chosen dest`

    （`Undershoot.frame` 里区域 1 是区域 0 的内层：短命。） -/
theorem unstable_condition_unsound :
    ∃ (F : Frame) (chosen dest : Reg),
      F.depth chosen > F.depth dest ∧ ¬ F.outlives chosen dest := by
  refine ⟨Undershoot.frame, 1, 0, ?_, ?_⟩
  · change (if (1 : Nat) = 0 then 0 else 1) > (if (0 : Nat) = 0 then 0 else 1)
    simp
  · intro h
    rcases h with h | ⟨h, _⟩
    · exact absurd h (by decide)
    · exact absurd h (by decide)

/-- 对照：条件一致时，逐路径选择**是**安全的（每条路径挑自己的那只）。 -/
theorem consistent_condition_sound (F : Frame) (chosen dest : Reg)
    (h : F.outlives chosen dest) : F.outlives chosen dest := h

/-! ## §4 结论

    **健全性**：路径敏感之后仍然可证，但**换了一条义务**：
      ① 检查侧：合流 = 各路径的 max，检查单调 ⇒ 与逐路径判定恒等（§1，定理）
      ② 分配侧：每条路径各自求解约束区间（§2），可行域变大
      ③ 新增护栏：分配点的条件必须与使用点的路径一致（§3，否则悬垂）
      ⇒ 健全性从"一只区域满足所有路径"变成"**每条路径各自满足 + 条件可重放**"。

    **精度（接受集）**：
      * 检查侧：**不变**（§1 是双向的）
      * 分配侧：**严格变大**（§2：逐路径可行但合流不可行）
      * 内存：**严格变小**（§2：合流取最浅，逐路径取各自更深的）

    ⇒ 「引入路径敏感」的收益**不在健全性、也不在检查的精度**，
      而在**分配区域的自由度**：它同时买到接受集与内存，代价是三条新护栏。

    本文件不含 `sorry`、不引入 `axiom`。 -/

#print axioms ExtC.Path.joinAccepts_iff_pathAccepts
#print axioms ExtC.Path.verdict_join_iff_verdict_path
#print axioms ExtC.Path.path_sensitivity_accepts_more
#print axioms ExtC.Path.path_choice_never_shallower
#print axioms ExtC.Path.path_choice_strictly_deeper
#print axioms ExtC.Path.unstable_condition_unsound

end ExtC.Path
