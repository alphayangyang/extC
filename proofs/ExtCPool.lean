/-
  ExtCPool.lean —— extC 池（pool）档内存模型的健全性及其**边界**，机器检查版。

  区域档（ExtCRegion.lean）证明的是"裸引用不会悬垂"，它的全部力量来自
  `outlives` 与"释放向下封闭"。池是**另一套**存储：块可以单独 `give`/`resize`、
  槽可以回收复用、容器可以增长搬家 —— 于是区域公理不再适用，
  池只能靠**动态身份**（世代句柄）保证"要么读到当年那个对象，要么 trap"。

    §1  槽状态与换代契约
    §2  身份引理：世代不变 ⇒ 占用者不变
    §3  反例：ABA（释放+复用但不换代）⇒ 旧句柄指向另一个对象
    §4  反例：世代回绕（ABA 的另一种成因）
    §5  正例：换代 ⇒ 旧句柄失效、新句柄有效

  本文件不含 `sorry`、不引入 `axiom`。
-/

import Std

set_option autoImplicit false
set_option linter.deprecated false

namespace ExtC.Pool

/-- 一个槽：世代 + 当前占用（`none` = 空闲 / 墓碑）。 -/
structure Slot where
  gen : Nat
  occ : Option Nat
deriving DecidableEq, Repr

/-- 池状态：槽表。 -/
abbrev State := Nat → Slot

/-- **换代契约（INV-G / INV-A 的可检查形式）**：

    * 世代单调不减（旧句柄携带的世代只会"过期"，不会"复活"）；
    * **占用状态一变，世代必须严格增长** —— 释放、复用、reset、搬迁都算。 -/
def GoodStep (s t : State) : Prop :=
  (∀ i, (s i).gen ≤ (t i).gen) ∧
  (∀ i, (t i).occ ≠ (s i).occ → (s i).gen < (t i).gen)

/-- 一串良好转移（显式链式定义，便于对列表归纳）。 -/
def Chained : State → List State → Prop
  | _, [] => True
  | s, t :: rest => GoodStep s t ∧ Chained t rest

/-- 句柄：槽号 + **签发时**的世代。它是纯值，可自由复制。 -/
structure Handle where
  slot : Nat
  gen : Nat
deriving DecidableEq, Repr

/-- 句柄在某状态下**通过校验**。真实实现里还包含"池仍活着且是对象表池"。 -/
def Handle.ValidAt (h : Handle) (s : State) : Prop :=
  (s h.slot).gen = h.gen ∧ (s h.slot).occ ≠ none

/-! ## §2 身份引理 -/

/-- 核心引理：**世代不变 ⇒ 占用者不变**。

    这就是"旧句柄要么指向当年那个对象，要么 trap"的全部理由：
    占用者一旦被换过，换代契约就强制世代严格增长；世代既然没变，占用者就没被换过。 -/
theorem gen_stable_occ_stable : ∀ (l : List State) (s : State), Chained s l →
    ∀ i, (∀ t ∈ l, (t i).gen = (s i).gen) → ∀ t ∈ l, (t i).occ = (s i).occ := by
  intro l
  induction l with
  | nil => intro s _ i _ t ht; exact absurd ht (by simp)
  | cons u rest ih =>
      intro s hch i hgen t ht
      obtain ⟨hstep, hrest⟩ := hch
      have hhead : (u i).occ = (s i).occ := by
        by_cases hEq : (u i).occ = (s i).occ
        · exact hEq
        · have hlt := hstep.2 i hEq
          have heq := hgen u (by simp)
          omega
      rcases List.mem_cons.mp ht with rfl | ht'
      · exact hhead
      · have h' : ∀ v ∈ rest, (v i).gen = (u i).gen := by
          intro v hv
          have h1 := hgen v (by simp [hv])
          have h2 := hgen u (by simp)
          omega
        have hocc := ih u hrest i h' t ht'
        exact hocc.trans hhead

/-- ★ **身份引理（池档主定理）**：
    句柄在签发之后，只要它槽上的世代一直没变，它就**仍然指向签发时那个对象**。 -/
theorem handle_does_not_rebind {s : State} {l : List State} (h : Chained s l) (hd : Handle)
    (hIssued : (s hd.slot).gen = hd.gen)
    (hNever : ∀ t ∈ l, (t hd.slot).gen = hd.gen) :
    ∀ t ∈ l, (t hd.slot).occ = (s hd.slot).occ := by
  apply gen_stable_occ_stable l s h hd.slot
  intro t ht
  rw [hNever t ht, hIssued]

/-! ## §3 反例：ABA（释放 + 复用，但不换代） -/

/-- 槽 0 上签发对象 42，世代 0。 -/
def s1 : State := fun i => if i = 0 then ⟨0, some 42⟩ else ⟨0, none⟩
/-- 释放（占用者变了，**世代没变**）。 -/
def s2 : State := fun i => if i = 0 then ⟨0, none⟩ else ⟨0, none⟩
/-- 复用给另一个对象 99（占用者又变了，**世代还是没变**）。 -/
def s3 : State := fun i => if i = 0 then ⟨0, some 99⟩ else ⟨0, none⟩

/-- 复用这一步**违反**换代契约。 -/
theorem aba_step_not_good : ¬ GoodStep s2 s3 := by
  intro h
  have h2 := h.2 0 (by decide : (s3 0).occ ≠ (s2 0).occ)
  simp [s2, s3] at h2

/-- 旧句柄 `{slot 0, gen 0}` 在 s3 上**通过校验**。 -/
theorem stale_handle_validates : Handle.ValidAt ⟨0, 0⟩ s3 :=
  ⟨by simp [s3], by simp [s3]⟩

/-- ……但它现在指向的是**另一个对象**（99，而不是签发时的 42）—— 读到别人 / 类型混淆。 -/
theorem stale_handle_confused :
    (s3 0).occ = some 99 ∧ (s1 0).occ = some 42 ∧ Handle.ValidAt ⟨0, 0⟩ s1 :=
  ⟨by simp [s3], by simp [s1], ⟨by simp [s1], by simp [s1]⟩⟩

/-- ★ **换代契约的必要性**：去掉"占用一变就换代"，旧句柄会指向另一个对象。 -/
theorem necessity_generation :
    ∃ (s t : State) (hd : Handle),
      Handle.ValidAt hd s ∧ (t hd.slot).gen = hd.gen ∧
      (t hd.slot).occ ≠ none ∧ (t hd.slot).occ ≠ (s hd.slot).occ :=
  ⟨s1, s3, ⟨0, 0⟩,
   ⟨by simp [s1], by simp [s1]⟩,
   by simp [s3],
   by simp [s3],
   by simp [s1, s3]⟩

/-! ### §3′ 反例：**只比世代**不够 —— 死池的世代 0 会与活池的 0 撞上

    文档规定"非 live 时 `generation()` 返回 0"，而 0 也是活池的合法世代。
    所以世代相等**必须**与 `occ ≠ none`（以及"池还活着""是对象表池"）一起校验；
    只比世代，一个远古句柄就会在池死后"通过"。 -/

/-- 活池：槽 0 世代 0、占用 42。 -/
def live0 : State := fun i => if i = 0 then ⟨0, some 42⟩ else ⟨0, none⟩
/-- 死池：同一个 rid 上报世代 0（文档口径），槽已空。 -/
def dead0 : State := fun i => if i = 0 then ⟨0, none⟩ else ⟨0, none⟩

/-- ★ **只比世代 ⇒ 校验失效**：同一个句柄在两边的世代都对得上，
    而一边是活对象、另一边是空的死池。 -/
theorem generation_only_check_unsound :
    ∃ (hd : Handle) (s t : State),
      (s hd.slot).gen = hd.gen ∧ (t hd.slot).gen = hd.gen ∧
      (s hd.slot).occ = some 42 ∧ (t hd.slot).occ = none :=
  ⟨⟨0, 0⟩, live0, dead0, by simp [live0], by simp [dead0], by simp [live0], by simp [dead0]⟩

/-- 正例：`ValidAt` 因为带上了占用检查，在死池上**拒绝**该句柄。 -/
theorem validAt_rejects_dead : ¬ Handle.ValidAt ⟨0, 0⟩ dead0 := by
  intro h
  have h2 := h.2
  simp [dead0] at h2

/-! ## §4 反例：世代回绕（ABA 的另一种成因）

    世代单调是契约的一半；**回绕**会让它不成立：`5 → 0` 既不是"不减"，
    还会让一个**远古**句柄重新匹配。 -/

/-- 槽 0 的世代用到 5，占用者 3。 -/
def w1 : State := fun i => if i = 0 then ⟨5, some 3⟩ else ⟨0, none⟩
/-- 回绕到 0，占用者换成 7。 -/
def w2 : State := fun i => if i = 0 then ⟨0, some 7⟩ else ⟨0, none⟩

theorem wrap_not_good : ¬ GoodStep w1 w2 := by
  intro h
  have h1 := h.1 0
  simp [w1, w2] at h1

/-- 一个**远古**句柄（世代 0）在回绕之后重新通过校验。 -/
theorem wrap_aba : Handle.ValidAt ⟨0, 0⟩ w2 ∧ (w2 0).occ = some 7 :=
  ⟨⟨by simp [w2], by simp [w2]⟩, by simp [w2]⟩

/-- ★ **不回绕（或在回绕前毒化/拒绝/重建）是必需的**。 -/
theorem necessity_no_wrap : ¬ GoodStep w1 w2 := wrap_not_good

/-! ## §5 正例：换代 ⇒ 旧句柄失效、新句柄有效 -/

/-- 签发 42，世代 0。 -/
def g1 : State := fun i => if i = 0 then ⟨0, some 42⟩ else ⟨0, none⟩
/-- 释放并**换代**。 -/
def g2 : State := fun i => if i = 0 then ⟨1, none⟩ else ⟨0, none⟩
/-- 复用并**再换代**，装入 99。 -/
def g3 : State := fun i => if i = 0 then ⟨2, some 99⟩ else ⟨0, none⟩

theorem good_chain : Chained g1 [g2, g3] := by
  refine ⟨?_, ?_, trivial⟩
  · refine ⟨?_, ?_⟩
    · intro i; by_cases hi : i = 0
      · simp [g1, g2, hi]
      · simp [g1, g2, hi]
    · intro i hne; by_cases hi : i = 0
      · simp [g1, g2, hi]
      · simp [g1, g2, hi] at hne
  · refine ⟨?_, ?_⟩
    · intro i; by_cases hi : i = 0
      · simp [g2, g3, hi]
      · simp [g2, g3, hi]
    · intro i hne; by_cases hi : i = 0
      · simp [g2, g3, hi]
      · simp [g2, g3, hi] at hne

/-- 旧句柄失效（世代对不上）⇒ 取用 trap，而不是读到 99。 -/
theorem old_handle_rejected : ¬ Handle.ValidAt ⟨0, 0⟩ g3 := by
  intro h
  have h1 := h.1
  simp [g3] at h1

/-- 新句柄有效，且指向的就是新对象。 -/
theorem new_handle_ok : Handle.ValidAt ⟨0, 2⟩ g3 ∧ (g3 0).occ = some 99 :=
  ⟨⟨by simp [g3], by simp [g3]⟩, by simp [g3]⟩

/-- 身份引理在本例上的落地：句柄 `{0,2}` 签发之后没有换代 ⇒ 一直指向 99。 -/
theorem new_handle_stable : ∀ t ∈ [g3], (t 0).occ = (g3 0).occ :=
  handle_does_not_rebind (s := g3) (l := [g3])
    ⟨⟨fun i => Nat.le_refl _, fun i h => absurd rfl h⟩, trivial⟩
    ⟨0, 2⟩ (by simp [g3])
    (by intro t ht; simp only [List.mem_singleton] at ht; subst ht; simp [g3])

/-! ## §6 结论

    **定理**：`handle_does_not_rebind` —— 换代契约成立时，句柄的世代不变
    蕴含它仍指向签发时的对象（"要么读到当年那个，要么 trap"）。

    **边界（各自机器检查）**：
      `necessity_generation` —— 去掉"占用一变就换代" ⇒ ABA，旧句柄指向别人
      `wrap_not_good`        —— 世代回绕 ⇒ 契约失效，远古句柄复活

    extC 池档的三条不变量里，**本模型只覆盖第一条**：
      * INV-G（换代覆盖）—— 本文件的 `GoodStep`：**槽粒度**的充分版，已证；
      * INV-A（块不搬家 / 只追加）—— **未覆盖**：模型里没有块、没有地址、没有 `live`，
        所以"块在句柄活着期间不得 free/realloc"这句话在本模型里**无法陈述**；
        实现侧由"对象表模式下 `give`/`resize` 直接 trap"顶上，那是一条 trap 而不是本模型的转移；
      * INV-V（视图不进池）—— **未覆盖**：裸 `slice` 没有世代可校验，
        `subView` 那一族靠静态规则或签字，本模型不予表达。

    模型外的其余路径（同属未覆盖）：`compact` 的**重编号**（模型把"槽身份 = 下标"
    当成结构性的）、槽下标**无上界**（实现要求"槽在界内"）、
    以及"池仍活着且是对象表池"这条 `ValidAt` 注释里自认缺失的前提。

    本文件不含 `sorry`、不引入 `axiom`。 -/

#print axioms ExtC.Pool.handle_does_not_rebind
#print axioms ExtC.Pool.necessity_generation
#print axioms ExtC.Pool.wrap_not_good
#print axioms ExtC.Pool.old_handle_rejected
#print axioms ExtC.Pool.generation_only_check_unsound
#print axioms ExtC.Pool.validAt_rejects_dead

end ExtC.Pool
