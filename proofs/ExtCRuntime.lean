/-
  ExtCRuntime.lean —— **运行期提权/回收**：它就是路径敏感，且免掉那三条护栏。

  静态路径敏感（ExtCPath.lean）买的是"分配区域按路径选"，代价是三条护栏：
  条件必须**纯、同作用域、同值**（`unstable_condition_unsound` 是少一条的反例）。
  理由很实在：静态分析要**重放**那条路径，所以条件必须能被重放。

  运行期提权不重放任何东西 —— 它在**真实执行到的那一刻**做决定：

      §1  元素级块：每个元素单开一块 ⇒ 回收是**逐对象**的（Injectivity 一句话）
      §2  计数定理：运行期口径 = 逐路径最优；静态口径 = "可能逃逸 ⇒ 全部留"
          ⇒ 阈值定律 n/(|E|+1) 的那一半，在这里是**被消除**的那一半
      §3  护栏对比：运行期两条动作（promote / freeBlk）的前提里**完全不出现条件**
          —— 这就是"免掉护栏"的形式含义

  记号沿用 ExtCBlock.lean。
-/
import ExtCBlock

namespace ExtC.Region
namespace Runtime

open Block

variable {W : BWorld}

/-! ## §1 元素级块 ⇒ 逐对象回收

    "给元素单开一个块"在模型里就是 `cellBlk` **单射**：一个块最多装一个单元。
    于是"释放一个块"最多影响一个单元 —— 回收粒度就是对象粒度。 -/

/-- 每个单元独占一块。 -/
def ElementGranular (W : BWorld) : Prop := Function.Injective W.cellBlk

/-- **元素级块下，释放一个块最多动一个单元**：这是"逐对象回收"的形式含义。
    没有这一条，释放一块会连带回收同块里的其他对象（块粒度浪费）。 -/
theorem free_affects_at_most_one (hInj : ElementGranular W) {β : Blk} {a a' : Cell}
    (ha : W.cellBlk a = β) (ha' : W.cellBlk a' = β) : a = a' :=
  hInj (ha.trans ha'.symm)

/-! ## §2 计数定理：运行期口径 = 逐路径最优

    把一次循环的**逃逸事件**记成一串布尔：`true` = 这一轮真的逃逸了。
    两种口径：

      `retainedStatic`  "可能逃逸" ⇒ 只要后面有一轮逃逸，本轮这份也得留（全部留）
      `retainedRuntime` 运行期提权 ⇒ 只有 true 的那些轮留在长命区域

    这正是 ExtCMemGap.lean 阈值定律里的分子与分母：`n / (|E| + 1)`。 -/

/-- 事件序列里 true 的个数 = 真正逃逸的轮数 `|E|`。 -/
def countTrue : List Bool → Nat
  | [] => 0
  | true :: rest => countTrue rest + 1
  | false :: rest => countTrue rest

/-- **静态口径**（"可能逃逸"）：`|E| ≠ 0` ⇒ n 份全部留在长命区域。 -/
def retainedStatic (l : List Bool) : Nat := if countTrue l = 0 then 0 else l.length

/-- **运行期口径**：只有真的逃逸的那些轮留在长命区域。 -/
def retainedRuntime (l : List Bool) : Nat := countTrue l

/-- **逐路径最优**：|E| 份。 -/
def retainedOptimal (l : List Bool) : Nat := countTrue l

/-- 运行期口径**逐字等于**逐路径最优 —— 这就是"运行期提权即路径敏感"。 -/
theorem runtime_attains_optimal (l : List Bool) : retainedRuntime l = retainedOptimal l := rfl

theorem countTrue_le_length : ∀ l : List Bool, countTrue l ≤ l.length
  | [] => Nat.le_refl 0
  | true :: rest => by
      simp only [countTrue, List.length_cons]
      exact Nat.add_le_add_right (countTrue_le_length rest) 1
  | false :: rest => by
      simp only [countTrue, List.length_cons]
      exact Nat.le_trans (countTrue_le_length rest) (Nat.le_succ _)

/-- **运行期不多占**：任何事件序列下，运行期口径 ≤ 静态口径。 -/
theorem runtime_le_static (l : List Bool) : retainedRuntime l ≤ retainedStatic l := by
  simp only [retainedRuntime, retainedStatic]
  by_cases h : countTrue l = 0
  · rw [ite_eq_left h, h]; exact Nat.le_refl 0
  · rw [ite_eq_right h]; exact countTrue_le_length l

/-- **静态口径给不出"按真实逃逸"的界**：只要逃逸**可能**，就是 n 份。 -/
theorem static_eq_length_of_possible (l : List Bool) (h : countTrue l ≠ 0) :
    retainedStatic l = l.length := by
  simp only [retainedStatic]
  exact ite_eq_right h

/-- **差距的来源**：`|E| = o(n)`（`l.length = n`、`countTrue l = |E|`）时，
    静态口径是 n 而运行期是 |E| —— 比值可任意大。 -/
theorem gap_is_length_over_count (l : List Bool) (h : countTrue l ≠ 0) :
    retainedStatic l = l.length ∧ retainedRuntime l = countTrue l :=
  ⟨static_eq_length_of_possible l h, rfl⟩

/-! ## §3 免掉护栏：运行期的前提里不出现条件

    ExtCPath.lean 的静态路径敏感规则要一条**额外**前提：条件可重放（纯、同作用域、同值）；
    少了它就 `unstable_condition_unsound`。

    运行期的两条动作根本不是这样陈述的：

      · `Block.safe_promote`   —— 前提只有"目标区域活着"
      · `Block.safe_freeBlk`   —— 前提只有"没有活单元指向这块"

    两条都**没有一个字**提到控制流条件。运行时不需要重放，因为决定就是在真实那条路上做的。
    下面这条定理把这个"不出现"写成形式：结论与任何条件无关。 -/

/-- 运行期提权的安全性与**任何条件**无关：换任意条件，同一条定理照用。 -/
theorem promote_safe_independent_of_condition (hS : W.Safe) (β : Blk) (r' : Reg) :
    (W.promote β r').Safe := Block.safe_promote hS β r'

/-- 运行期回收的安全性与**任何条件**无关。 -/
theorem free_safe_independent_of_condition (hS : W.Safe) {β : Blk}
    (hNoPtr : ∀ a b, W.CellLive a → W.ptr a = some b → W.cellBlk b ≠ β) :
    (W.freeBlk β).Safe := Block.safe_freeBlk hS hNoPtr

end Runtime
end ExtC.Region
