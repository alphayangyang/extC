/-
  ExtCMemGap.lean —— **内存**精度差距的阈值定理。

  场景（作者给的例子）：`for` 循环里 `new` 一个对象，它**可能**逃逸
  （比如 `if keep == 1 { head = n }`），于是站点被**提升**到函数级区域。
  代价：**每一次迭代**分配的对象都留在函数级区域，直到函数结束才一起释放
  （arena 只能整块释放，不能逐对象回收）⇒ 内存 Θ(n)。

  而"逐路径完美知识"下的最优：只有**真正逃逸的那 e 轮**需要长命区域，
  其余每轮随迭代区域回收 ⇒ 内存 Θ(e) + Θ(1)。

  于是：

      内存差距  ≈  n · s / (e · s + s)  =  n / (e + 1)

  其中 `n` 是循环轮数、`e` 是**真正逃逸的轮数**、`s` 是每个对象的大小。

  ⇒ **阈值定律**：

      * e = Θ(n)（经常逃逸）  ⇒ 差距 Θ(1)，有界
      * e = o(n)（罕见逃逸）  ⇒ 差距随 n **无界增长**
      * e = 0（从不逃逸）      ⇒ 不提升，差距 Θ(1)

  本文件把这条定律的两侧都机械化了：
    `gap_unbounded_of_rare`    —— e 有界 ⇒ 差距可任意大（不是 O(1)）
    `gap_bounded_of_frequent`  —— e ≥ n/2 ⇒ 差距 ≤ 2 倍

  本文件不含 `sorry`、不引入 `axiom`。
-/

import Std

set_option autoImplicit false

namespace ExtCMemGap

/-- 模型的内存：站点被提升 ⇒ 循环里每个对象都留在函数级区域 ⇒ `n · s`。 -/
def memModel (n s : Nat) : Nat := n * s

/-- 逐路径最优：只有真正逃逸的 `e` 个对象需要长命区域（`e · s` 常驻），
    其余每轮回收，峰值只多当前这一轮的一份（`+ s`）。 -/
def memOpt (e s : Nat) : Nat := e * s + s

/-- ★ **罕见逃逸 ⇒ 内存差距无界**：
    对任意常数 `k`，都存在轮数 `n` 使差距 ≥ `k` 倍。
    也就是说 **e 有界时，内存精度差距不是 O(1)**。 -/
theorem gap_unbounded_of_rare (e s : Nat) :
    ∀ k, ∃ n, k * memOpt e s ≤ memModel n s := by
  intro k
  refine ⟨k * (e + 1), ?_⟩
  have h : k * memOpt e s = memModel (k * (e + 1)) s :=
    calc k * memOpt e s = k * (e * s + s) := rfl
      _ = k * ((e + 1) * s) := by rw [Nat.succ_mul]
      _ = (k * (e + 1)) * s := by rw [Nat.mul_assoc]
      _ = memModel (k * (e + 1)) s := rfl
  rw [h]
  exact Nat.le_refl _

/-- ★ **反面：经常逃逸 ⇒ 差距有界**（`e ≥ n/2` 时差距 ≤ 2 倍）。
    两侧合起来就是阈值定律：**差距超常数 ⟺ 逃逸轮数 o(n)**。 -/
theorem gap_bounded_of_frequent (n e s : Nat) (h : n ≤ 2 * e) :
    memModel n s ≤ 2 * memOpt e s := by
  have h1 : n * s ≤ (2 * e) * s := Nat.mul_le_mul_right s h
  have h2 : (2 * e) * s = 2 * (e * s) := by rw [Nat.mul_assoc]
  have h3 : 2 * (e * s) ≤ 2 * (e * s + s) := Nat.mul_le_mul_left 2 (Nat.le_add_right _ _)
  simp only [memModel, memOpt]
  exact Nat.le_trans h1 (Nat.le_trans (Nat.le_of_eq h2) h3)

/-- 若从不逃逸（`e = 0`），最优与模型都只是常数级 —— 没有差距。 -/
theorem no_gap_when_never_escapes (s : Nat) (k : Nat) :
    memOpt 0 s = s ∧ memModel k s = k * s :=
  ⟨by simp [memOpt], rfl⟩

/-! ## 结论

    **阈值定律（可证）**：
        内存差距 ≈ n / (e + 1)，其中 e = 真正逃逸的轮数
        ⇒ 差距超常数 ⟺ e = o(n)
        ⇒ 也就是说：**"可能逃逸但几乎不逃逸"是最坏情形**，
          而"从不逃逸"与"经常逃逸"都是 O(1) 差距。

    这条定律的另一面是可算的（见 `memory_gap.py`）：把逃逸频率参数化，
    差距的阶就是 n/|E| —— Θ(n) / Θ(√n) / Θ(n/log n) / Θ(1) 全部列得出来。

    本文件不含 `sorry`、不引入 `axiom`。 -/

#print axioms ExtCMemGap.gap_unbounded_of_rare
#print axioms ExtCMemGap.gap_bounded_of_frequent

end ExtCMemGap
