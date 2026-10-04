/-
  ExtCRegion.lean —— extC 区域（栈/arena）内存模型的健全性及其**边界**，机器检查版。

  这份文件回答一个精确的问题：

    「哪些假设一旦成立，检查器接受的程序就不会解引用已死区域；
      而每一条假设又各自**不可省**（去掉就有机器检查的反例轨迹）？」

  结构：

    §1  区域帧 Frame        —— 静态几何（寿命序、局部/Ground 类、深度）
    §2  线性化引理          —— 深度比较何时蕴含寿命比较（边界条件 CC + ChainOK）
    §3  动态世界 World      —— 指针图 + 两条不变量
    §4  规则层语义 Step     —— 检查器允许的转移；保持不变量 ⇒ 无悬垂（主定理）
    §5  三个必要性反例      —— CC / INV-H / 释放向下封闭，各自不可省
    §6  链上线性化公理的独立性
    §7  哨兵 0 的 min/max 反例（审计 P0-2 的抽象形式）
    §8  放大引理            —— 「精度只买内存」

  记号：深度**越小活得越久**；`outlives r s` 读作「r 活得 ≥ s」。

  ## 模型 ↔ 实现：三处必须说清的接口

  1. **`dr` 是对"已记录深度"的抽象，不是运行时算出来的。**
     `Step.store` 的 `hInvH : depth(目标) ≤ dr` 就是 **INV-H**，它是**检查器的义务**
     （分析必须维护它），在模型里以**前提**出现。定理说的是"**记账正确 ⇒ 检查正确**"，
     不是"记账天然正确"。记账为什么正确要另证（上界性 / 单调性 / 载体穿透 / 写点完整性 /
     单一指称），其中写点完整性与单一指称在本目录 `ExtCModel.lean` 有定理。
  2. **目标活性不再是外加假设。** `World.target_alive_of_read` 从"值取自活单元" +
     两条不变量把它**推**出来 —— 实现里检查器确实只比两个静态数（`if (d <= at) return false`），
     从不问"目标区域是否已释放"，那一半由不变量承担。
  3. **`CC` 就是 `ARENA_HOME` 那一类记账的去处。** 模型**没有**"深度 0 ⇒ 必属 Ground"
     这条断言；`CC` 要求"每个活着的区域要么是本激活的局部、要么活过本激活"。
     实现把 `ARENA_HOME`（调用者按落点选的块 arena）记成 0 —— 它既不是被调激活的 Local、
     也不是它的 Ground ⇒ **那正是 CC 失败的世界**，由 `ForeignRegion.necessity_CC` 覆盖。
     换句话说：模型不"排除"这种记账，而是**证明它一旦出现规则就不健全**。

  本文件不含 `sorry`、不引入 `axiom`（末尾 `#print axioms` 可核）。
-/

import Std

set_option autoImplicit false
set_option linter.deprecated false

namespace ExtC.Region

/-- 地址（模型只关心它的区域与内容） -/
abbrev Addr := Nat
/-- 区域实例（一次 arena / 一个块） -/
abbrev Reg := Nat

/-! ## §1 区域帧：静态几何 -/

/-- 一个「帧」= 一次激活看到的区域**几何**（不随运行改变的部分）。

    * `outlives r s`：区域 r 活得至少和 s 一样久（寿命序）
    * `Local`：该区域属于**当前激活**
    * `Ground`：该区域**活过当前激活的全部**（程序级、参数所指、调用者选的落点）
    * `depth`：记账深度（词法层号），越小活得越久 -/
structure Frame where
  outlives : Reg → Reg → Prop
  out_refl : ∀ r, outlives r r
  out_trans : ∀ a b c, outlives a b → outlives b c → outlives a c
  Local : Reg → Prop
  Ground : Reg → Prop
  depth : Reg → Nat
  /-- 深度保序：活得久的区域，深度不大。 -/
  depth_mono : ∀ r s, outlives r s → depth r ≤ depth s
  /-- Ground 区域按约定记深度 0。 -/
  depth_ground : ∀ r, Ground r → depth r = 0
  /-- Ground 区域活得比任何局部区域久（CALL-EXTENT 的抽象形式）。 -/
  ground_outlives : ∀ r s, Ground r → Local s → outlives r s

/-! ## §2 动态世界 -/

/-- 一个世界：区域帧 + 当前活着的区域 + 每个地址属于哪个区域 + 每个单元持有的指针。 -/
structure World where
  F : Frame
  alive : Reg → Prop
  reg : Addr → Reg
  ptr : Addr → Option Addr

/-- **边界条件 ① CC（Comparability Condition）**：
    每个活着的区域，要么属于当前激活，要么活过当前激活的全部。
    单线程嵌套调用时成立；**池板块、挂起任务、并发线程下不成立**（见 §5.1）。 -/
def World.CC (W : World) : Prop := ∀ r, W.alive r → W.F.Local r ∨ W.F.Ground r

/-- **边界条件 ② ChainOK（链上线性化）**：
    两个*同时活着且局部*的区域位于同一条嵌套链上，于是词法深度如实线性化该链。
    兄弟区域（两个挂起任务的地方）同时活着时**不成立**（见 §6）。 -/
def World.ChainOK (W : World) : Prop :=
  ∀ r s, W.alive r → W.alive s → W.F.Local r → W.F.Local s →
    W.F.depth r ≤ W.F.depth s → W.F.outlives r s

/-- **INV-I（结构不变量）**：每个单元持有的指针，其目标区域活得 ≥ 该单元的区域。 -/
def World.Contains (W : World) : Prop :=
  ∀ a b, W.ptr a = some b → W.F.outlives (W.reg b) (W.reg a)

/-- **INV-L（区域系统不变量）**：活着的区域，其"祖先"也活着。
    等价说法：释放一个区域必须连同它 outlive 的一切一起释放（**向下封闭**）。 -/
def World.LiveClosed (W : World) : Prop :=
  ∀ r s, W.alive s → W.F.outlives r s → W.alive r

/-- **安全（S1，时间安全）**：活着的单元里的每个指针都指向活着的单元。 -/
def World.Safe (W : World) : Prop :=
  ∀ a b, W.alive (W.reg a) → W.ptr a = some b → W.alive (W.reg b)

/-- 主引理：INV-I 与 INV-L 一起蕴含安全 —— 这就是"区域纪律"的全部内容。 -/
theorem World.safe_of_invariants (W : World) (hC : W.Contains) (hL : W.LiveClosed) :
    W.Safe := by
  intro a b hAlive hPtr
  exact hL (W.reg b) (W.reg a) hAlive (hC a b hPtr)

/-- **线性化引理（点态）**：检查器真正用的那一步推导。
    `depth r ≤ depth s` 蕴含 `outlives r s`，需要四条：
    目标活、目的地活、目的地是局部、以及目标属于 `Local ∪ Ground`。 -/
theorem linearization_pointwise (F : Frame) (alive : Reg → Prop) {r s : Reg}
    (hr : alive r) (hs : alive s) (hLocal : F.Local s)
    (hClass : F.Local r ∨ F.Ground r)
    (hChain : ∀ r s, alive r → alive s → F.Local r → F.Local s →
      F.depth r ≤ F.depth s → F.outlives r s)
    (hDepth : F.depth r ≤ F.depth s) : F.outlives r s := by
  rcases hClass with h | h
  · exact hChain r s hr hs h hLocal hDepth
  · exact F.ground_outlives r s h hLocal

/-- **聚合值的桥**：一个值里的指针通常不止一个（结构体 / 数组 / 切片 / 枚举载荷）。
    记的那个数 `dr` 只要同时是**每一个**目标区域深度的上界，存储检查就对整组指针有效。
    （模型为了最小化用了单指针单元；这条把结论搬回聚合值，
     也就是检查器字段表 + `otherDepth` 兜底真正要答的问题。） -/
theorem linearization_all_targets (F : Frame) (alive : Reg → Prop) {dest : Reg}
    (rs : List Reg)
    (hAliveDest : alive dest) (hLocalDest : F.Local dest)
    (hChain : ∀ r s, alive r → alive s → F.Local r → F.Local s →
      F.depth r ≤ F.depth s → F.outlives r s)
    (hAliveAll : ∀ r ∈ rs, alive r)
    (hClassAll : ∀ r ∈ rs, F.Local r ∨ F.Ground r)
    (hDepthAll : ∀ r ∈ rs, F.depth r ≤ F.depth dest) :
    ∀ r ∈ rs, F.outlives r dest := by
  intro r hr
  exact linearization_pointwise F alive (hAliveAll r hr) hAliveDest hLocalDest
    (hClassAll r hr) hChain (hDepthAll r hr)

/-- 世界级形式：CC 与 ChainOK 都成立时，整个深度比较可靠。 -/
theorem World.linearization (W : World) {r s : Reg}
    (hr : W.alive r) (hs : W.alive s) (hLocal : W.F.Local s)
    (hClass : W.F.Local r ∨ W.F.Ground r) (hChain : W.ChainOK)
    (hDepth : W.F.depth r ≤ W.F.depth s) : W.F.outlives r s :=
  linearization_pointwise W.F W.alive hr hs hLocal hClass hChain hDepth

/-! ## §3 存储操作 -/

/-- 把 b 写进单元 a（覆盖式写；新边替换旧边）。 -/
def World.store (W : World) (a b : Addr) : World :=
  { W with ptr := fun x => if x = a then some b else W.ptr x }

/-- 释放区域集合 k。 -/
def World.kill (W : World) (k : Reg → Prop) : World :=
  { W with alive := fun r => W.alive r ∧ ¬ k r }

theorem World.contains_store {W : World} (hC : W.Contains) {a b : Addr}
    (h : W.F.outlives (W.reg b) (W.reg a)) : (W.store a b).Contains := by
  intro x y hxy
  have hptr : (if x = a then some b else W.ptr x) = some y := hxy
  show W.F.outlives (W.reg y) (W.reg x)
  by_cases hx : x = a
  · rw [if_pos hx] at hptr
    subst hx
    have hy : y = b := (Option.some.inj hptr).symm
    subst hy
    exact h
  · rw [if_neg hx] at hptr
    exact hC x y hptr

theorem World.contains_kill {W : World} (hC : W.Contains) (k : Reg → Prop) :
    (W.kill k).Contains := by
  intro a b h
  show W.F.outlives (W.reg b) (W.reg a)
  exact hC a b h

/-- 释放保持 INV-L **当且仅当** k 向下封闭。 -/
theorem World.liveClosed_kill {W : World} (hL : W.LiveClosed) {k : Reg → Prop}
    (hk : ∀ r s, k r → W.F.outlives r s → k s) : (W.kill k).LiveClosed := by
  intro r s hs hout
  change W.alive s ∧ ¬ k s at hs
  change W.F.outlives r s at hout
  change W.alive r ∧ ¬ k r
  exact ⟨hL r s hs.1 hout, fun hkr => hs.2 (hk r s hkr hout)⟩

/-! ## §4 规则层语义与主定理 -/

/-- **规则层语义**：检查器放行的转移。四条边界条件全部显式：
    ① `hCC`（目标的类）② `hChain`（链上线性化）③ `hInvH`（记账是上界）
    ④ `hDown`（释放向下封闭）。 -/
inductive Step : World → World → Prop
  | store (W : World) (a b : Addr) (dr : Nat)
      (hAcc : dr ≤ W.F.depth (W.reg a))
      (hInvH : W.F.depth (W.reg b) ≤ dr)
      (hAliveCell : W.alive (W.reg a))
      (hLocalCell : W.F.Local (W.reg a))
      (hAliveTarget : W.alive (W.reg b))
      (hCC : W.CC)
      (hChain : W.ChainOK) :
      Step W (W.store a b)
  | kill (W : World) (k : Reg → Prop)
      (hDown : ∀ r s, k r → W.F.outlives r s → k s) :
      Step W (W.kill k)

/-- **保持性（存储）**：CC + ChainOK + INV-H 下，被放行的存储不破坏 INV-I。 -/
theorem Step.contains {W W' : World} (h : Step W W') (hC : W.Contains) : W'.Contains := by
  cases h with
  | store a b dr hAcc hInvH hAliveCell hLocalCell hAliveTarget hCC hChain =>
      have hOut : W.F.outlives (W.reg b) (W.reg a) :=
        World.linearization W hAliveTarget hAliveCell hLocalCell
          (hCC (W.reg b) hAliveTarget) hChain (Nat.le_trans hInvH hAcc)
      exact World.contains_store hC hOut
  | kill k hDown => exact World.contains_kill hC k

/-- **目标活性不是外加假设**：存储的值若来自一个**活着的**单元，
    它的目标区域活着这件事由前状态的两条不变量推出（实现里检查器确实只比两个静态数，
    从不问"目标区域是否已释放"—— 那一半由不变量承担）。 -/
theorem World.target_alive_of_read {W : World} (hC : W.Contains) (hL : W.LiveClosed)
    {x b : Addr} (hx : W.alive (W.reg x)) (hp : W.ptr x = some b) : W.alive (W.reg b) :=
  hL (W.reg b) (W.reg x) hx (hC x b hp)

/-- 于是 `Step.store` 的前提里，**只有三条**不是检查器当场能查的：
    CC、ChainOK（两条边界条件）与 INV-H（记账义务）。目标活性是推出来的。 -/
theorem Step.store_of_read {W : World} (hC : W.Contains) (hL : W.LiveClosed)
    {a b x : Addr} {dr : Nat}
    (hAcc : dr ≤ W.F.depth (W.reg a)) (hInvH : W.F.depth (W.reg b) ≤ dr)
    (hAliveCell : W.alive (W.reg a)) (hLocalCell : W.F.Local (W.reg a))
    (hx : W.alive (W.reg x)) (hp : W.ptr x = some b)
    (hCC : W.CC) (hChain : W.ChainOK) :
    Step W (W.store a b) :=
  Step.store _ _ _ _ hAcc hInvH hAliveCell hLocalCell
    (World.target_alive_of_read hC hL hx hp) hCC hChain

/-- **保持性（释放）**。 -/
theorem Step.liveClosed {W W' : World} (h : Step W W') (hL : W.LiveClosed) : W'.LiveClosed := by
  cases h with
  | store a b dr hAcc hInvH hAliveCell hLocalCell hAliveTarget hCC hChain =>
      intro r s hs hout
      change W.alive s at hs
      change W.F.outlives r s at hout
      change W.alive r
      exact hL r s hs hout
  | kill k hDown => exact World.liveClosed_kill hL hDown

/-- 一次运行的转移闭包。 -/
inductive Runs : World → World → Prop
  | refl (W : World) : Runs W W
  | step {W₁ W₂ W₃ : World} : Runs W₁ W₂ → Step W₂ W₃ → Runs W₁ W₃

/-- 沿整条运行保持两条不变量。 -/
theorem Runs.invariants {W W' : World} (h : Runs W W') :
    W.Contains → W.LiveClosed → W'.Contains ∧ W'.LiveClosed := by
  induction h with
  | refl => intro hC hL; exact ⟨hC, hL⟩
  | step hRest hStep ih =>
      intro hC hL
      exact ⟨hStep.contains (ih hC hL).1, hStep.liveClosed (ih hC hL).2⟩

/-- ★ **主定理（区域档健全性）**：
    若初始世界满足 INV-I 与 INV-L，则沿任意一条被检查器放行的转移序列，
    每个活着的单元里的指针都指向活着的单元 —— **没有悬垂解引用**。

    前提（全部显式，缺一不可，见 §5）：区域公理（`Frame`）、
    边界条件 CC（`Step.hCC`）、链上线性化（`Step.hChain`）、
    记账上界 INV-H（`Step.hInvH`）、释放向下封闭（`Step.hDown`）。 -/
theorem Runs.safe {W W' : World} (h : Runs W W') (hC : W.Contains) (hL : W.LiveClosed) :
    W'.Safe :=
  World.safe_of_invariants W' (Runs.invariants h hC hL).1 (Runs.invariants h hC hL).2

/-! ## §5 边界：每条假设各自不可省

    每个反例都是一条**机器检查的轨迹**：
    初始世界满足两条不变量 → 检查器放行一步 → 最终世界不安全（`¬ Safe`）。
    唯一的区别是"少给哪条假设"。 -/

/-! ### §5.1 CC 不可省：外来区域（池板块 / 另一个任务的地方）

    区域 1 = 本激活的局部块；区域 0 = 既不属于本激活、也不活过本激活的东西。
    深度都是 0，`outlives` 只有自反 —— 于是 `depth 0 ≤ depth 1` 成立，
    而 `outlives 0 1` 不成立。 -/
namespace ForeignRegion

def frame : Frame where
  outlives := fun a b => a = b
  out_refl := fun _ => rfl
  out_trans := fun _ _ _ h1 h2 => h1.trans h2
  Local := fun r => r = 1
  Ground := fun _ => False
  depth := fun _ => 0
  depth_mono := fun _ _ _ => Nat.le_refl 0
  depth_ground := fun _ h => absurd h (by simp)
  ground_outlives := fun _ _ h _ => absurd h (by simp)

/-- 单元 0 在本激活的局部区域 1 里；单元 1 在外来区域 0 里。 -/
def W0 : World where
  F := frame
  alive := fun r => r = 0 ∨ r = 1
  reg := fun a => if a = 0 then 1 else 0
  ptr := fun _ => none

theorem W0_contains : W0.Contains := by
  intro a b h
  simp [W0] at h

theorem W0_liveClosed : W0.LiveClosed := by
  intro r s hs hout
  change (fun r => r = 0 ∨ r = 1) s at hs
  change r = s at hout
  subst hout
  exact hs

theorem W0_chainOK : W0.ChainOK := by
  intro r s _ _ hlr hls _
  change r = 1 at hlr
  change s = 1 at hls
  subst hlr; subst hls; rfl

/-- **CC 在目标区域上失败**：区域 0 既不是局部、也不是 Ground。 -/
theorem target_not_in_class : ¬ (frame.Local 0 ∨ frame.Ground 0) := by
  intro h
  rcases h with h | h
  · change (0 : Nat) = 1 at h; exact absurd h (by decide)
  · exact h

/-- **检查器实际做的检查**：与 `Step` 相比**只少 CC 这一条**（ChainOK 仍然在场），
    于是反例把边界条件**精确隔离**到 CC 上。 -/
inductive CheckedStep : World → World → Prop
  | store (W : World) (a b : Addr) (dr : Nat)
      (hAcc : dr ≤ W.F.depth (W.reg a))
      (hInvH : W.F.depth (W.reg b) ≤ dr)
      (hAliveCell : W.alive (W.reg a))
      (hLocalCell : W.F.Local (W.reg a))
      (hAliveTarget : W.alive (W.reg b))
      (hChain : W.ChainOK) :
      CheckedStep W (W.store a b)
  | kill (W : World) (k : Reg → Prop)
      (hDown : ∀ r s, k r → W.F.outlives r s → k s) :
      CheckedStep W (W.kill k)

theorem checked_store : CheckedStep W0 (W0.store 0 1) :=
  CheckedStep.store W0 0 1 0
    (by change 0 ≤ 0; exact Nat.le_refl 0)
    (by change 0 ≤ 0; exact Nat.le_refl 0)
    (by change ((fun r => r = 0 ∨ r = 1) 1); exact Or.inr rfl)
    (by change (1 : Nat) = 1; rfl)
    (by change ((fun r => r = 0 ∨ r = 1) 0); exact Or.inl rfl)
    W0_chainOK

/-- 此后该外来区域**正常死亡**（一次合法的、向下封闭的释放）。 -/
theorem kill_is_a_step : Step (W0.store 0 1) ((W0.store 0 1).kill (fun r => r = 0)) :=
  Step.kill _ _ (by
    intro r s hr hs
    change r = s at hs
    subst hs
    exact hr)

/-- 结果：活着的单元 0 指向已死区域里的单元 1 —— **悬垂**。 -/
theorem dead_pointer : ¬ ((W0.store 0 1).kill (fun r => r = 0)).Safe := by
  intro h
  have hcell : ((W0.store 0 1).kill (fun r => r = 0)).alive
      (((W0.store 0 1).kill (fun r => r = 0)).reg 0) := by
    change ((fun r => r = 0 ∨ r = 1) 1 ∧ ¬ (1 = 0))
    exact ⟨Or.inr rfl, by decide⟩
  have hptr : ((W0.store 0 1).kill (fun r => r = 0)).ptr 0 = some 1 := by
    change (if (0 : Nat) = 0 then some 1 else none) = some 1
    simp
  have hbad := h 0 1 hcell hptr
  change ((fun r => r = 0 ∨ r = 1) 0 ∧ ¬ (0 = 0)) at hbad
  exact hbad.2 rfl

/-- ★ **CC 的必要性**：存在一条轨迹 —— 初始满足两条不变量、检查器（无 CC 版）放行、
    最终**不安全**。 -/
theorem necessity_CC :
    ∃ (W W' W'' : World),
      W.Contains ∧ W.LiveClosed ∧ CheckedStep W W' ∧ Step W' W'' ∧ ¬ W''.Safe :=
  ⟨W0, W0.store 0 1, (W0.store 0 1).kill (fun r => r = 0),
   W0_contains, W0_liveClosed, checked_store, kill_is_a_step, dead_pointer⟩

end ForeignRegion

/-! ### §5.2 INV-H 不可省：记账比真实深度**小**

    区域 0 活得比区域 1 久（`outlives 0 1`），深度分别是 0 / 1。
    检查器把"指向区域 1 的值"记成深度 0（= 活得最久）—— **记小了**。
    于是把它存进区域 0 的单元被放行；区域 1 正常退出后该单元悬垂。 -/
namespace Undershoot

/-- 本模型只使用区域 0（长寿）与 1（短命）。 -/
def inTwo (r : Reg) : Prop := r = 0 ∨ r = 1

def frame : Frame where
  outlives := fun a b => a = b ∨ (a = 0 ∧ b = 1)
  out_refl := fun r => Or.inl rfl
  out_trans := by
    intro a b c h1 h2
    rcases h1 with h | ⟨h1a, h1b⟩
    · subst h; exact h2
    · subst h1a; subst h1b
      rcases h2 with h | ⟨h2a, h2b⟩
      · subst h; exact Or.inr ⟨rfl, rfl⟩
      · exact absurd h2a (by decide)
  Local := inTwo
  Ground := fun _ => False
  depth := fun r => if r = 0 then 0 else 1
  depth_mono := by
    intro r s h
    rcases h with h | ⟨h1, h2⟩
    · subst h; exact Nat.le_refl _
    · subst h1; subst h2; decide
  depth_ground := fun _ h => absurd h (by simp)
  ground_outlives := fun _ _ h _ => absurd h (by simp)

/-- 单元 0 在长寿区域 0；单元 1 在短命区域 1。 -/
def W0 : World where
  F := frame
  alive := inTwo
  reg := fun a => if a = 0 then 0 else 1
  ptr := fun _ => none

theorem W0_contains : W0.Contains := by
  intro a b h
  simp [W0] at h

theorem W0_liveClosed : W0.LiveClosed := by
  intro r s _ hout
  change inTwo r
  rcases hout with h | ⟨h1, h2⟩
  · subst h; assumption
  · subst h1; exact Or.inl rfl

theorem W0_cc : W0.CC := by
  intro r hr
  exact Or.inl hr

theorem W0_chainOK : W0.ChainOK := by
  intro r s hr hs hlr hls hd
  change inTwo r at hlr
  change inTwo s at hls
  change (if r = 0 then 0 else 1) ≤ (if s = 0 then 0 else 1) at hd
  rcases hlr with h | h
  · subst h
    rcases hls with h' | h'
    · subst h'; exact Or.inl rfl
    · subst h'; exact Or.inr ⟨rfl, rfl⟩
  · subst h
    rcases hls with h' | h'
    · subst h'; simp at hd
    · subst h'; exact Or.inl rfl

/-- **记账错误版**检查器：把值的深度记成真值之下（INV-H 失效），CC 与 ChainOK 照旧。 -/
inductive BadFactStep : World → World → Prop
  | store (W : World) (a b : Addr) (dr : Nat)
      (hAcc : dr ≤ W.F.depth (W.reg a))
      (hWrong : W.F.depth (W.reg b) > dr)
      (hAliveCell : W.alive (W.reg a))
      (hLocalCell : W.F.Local (W.reg a))
      (hAliveTarget : W.alive (W.reg b))
      (hCC : W.CC)
      (hChain : W.ChainOK) :
      BadFactStep W (W.store a b)
  | kill (W : World) (k : Reg → Prop)
      (hDown : ∀ r s, k r → W.F.outlives r s → k s) :
      BadFactStep W (W.kill k)

/-- 真实深度是 1，记账深度是 0 ⇒ 存储被放行。 -/
theorem bad_store : BadFactStep W0 (W0.store 0 1) :=
  BadFactStep.store W0 0 1 0
    (by change 0 ≤ 0; exact Nat.le_refl 0)
    (by change (1 : Nat) > 0; omega)
    (by change inTwo 0; exact Or.inl rfl)
    (by change inTwo 0; exact Or.inl rfl)
    (by change inTwo 1; exact Or.inr rfl)
    W0_cc W0_chainOK

/-- 区域 1（内层块）正常退出：一次合法的、向下封闭的释放。 -/
theorem kill_is_a_step : Step (W0.store 0 1) ((W0.store 0 1).kill (fun r => r = 1)) :=
  Step.kill _ _ (by
    intro r s hr hs
    rcases hs with h | ⟨h1, h2⟩
    · subst h; exact hr
    · subst h1; exact h2)

theorem dead_pointer : ¬ ((W0.store 0 1).kill (fun r => r = 1)).Safe := by
  intro h
  have hcell : ((W0.store 0 1).kill (fun r => r = 1)).alive
      (((W0.store 0 1).kill (fun r => r = 1)).reg 0) := by
    change (inTwo 0 ∧ ¬ ((0 : Nat) = 1))
    exact ⟨Or.inl rfl, by decide⟩
  have hptr : ((W0.store 0 1).kill (fun r => r = 1)).ptr 0 = some 1 := by
    change (if (0 : Nat) = 0 then some 1 else none) = some 1
    simp
  have hbad := h 0 1 hcell hptr
  change (inTwo 1 ∧ ¬ ((1 : Nat) = 1)) at hbad
  exact hbad.2 rfl

/-- ★ **INV-H 的必要性**：记账偏小（哪怕 CC 与链上线性化都成立、释放也合法）就能造出悬垂。 -/
theorem necessity_INVH :
    ∃ (W W' W'' : World),
      W.Contains ∧ W.LiveClosed ∧ BadFactStep W W' ∧ Step W' W'' ∧ ¬ W''.Safe :=
  ⟨W0, W0.store 0 1, (W0.store 0 1).kill (fun r => r = 1),
   W0_contains, W0_liveClosed, bad_store, kill_is_a_step, dead_pointer⟩

end Undershoot

/-! ### §5.3 「释放向下封闭」不可省：**非 LIFO 释放区域**

    正确的对应是"一个区域被释放，而它 outlive 的（更内层的）区域还活着"：
    任务 zone 被按 LIFO 弹出而**别的活任务**仍指着它（审计 P0-20 / 定案 99）。

    **不要**把池的 `give`/`resize` 记在这一族：它们让"块"在池活着时失效/搬家，
    而块根本不是按区域嵌套回收的存储 —— 那属于 §5.1 的 CC 族，
    池的处置是世代（INV-A / INV-G），不是这里。

    区域 0 活得比区域 1 久。单元 0 在区域 1 里、指向区域 0 里的单元 1 —— 这条边**合法**。
    但若释放区域 0 却**不**释放区域 1，存活单元 0 立刻悬垂。 -/
namespace LooseKill

def inTwo (r : Reg) : Prop := r = 0 ∨ r = 1

def frame : Frame where
  outlives := fun a b => a = b ∨ (a = 0 ∧ b = 1)
  out_refl := fun r => Or.inl rfl
  out_trans := by
    intro a b c h1 h2
    rcases h1 with h | ⟨h1a, h1b⟩
    · subst h; exact h2
    · subst h1a; subst h1b
      rcases h2 with h | ⟨h2a, h2b⟩
      · subst h; exact Or.inr ⟨rfl, rfl⟩
      · exact absurd h2a (by decide)
  Local := inTwo
  Ground := fun _ => False
  depth := fun r => if r = 0 then 0 else 1
  depth_mono := by
    intro r s h
    rcases h with h | ⟨h1, h2⟩
    · subst h; exact Nat.le_refl _
    · subst h1; subst h2; decide
  depth_ground := fun _ h => absurd h (by simp)
  ground_outlives := fun _ _ h _ => absurd h (by simp)

/-- 单元 0 在区域 1（内层），指向区域 0（外层）里的单元 1。 -/
def W0 : World where
  F := frame
  alive := inTwo
  reg := fun a => if a = 0 then 1 else 0
  ptr := fun a => if a = 0 then some 1 else none

theorem W0_contains : W0.Contains := by
  intro a b h
  by_cases ha : a = 0
  · subst ha
    change (if (0 : Nat) = 0 then some (1 : Addr) else none) = some b at h
    simp at h
    subst h
    change frame.outlives 0 1
    exact Or.inr ⟨rfl, rfl⟩
  · change (if a = 0 then some (1 : Addr) else none) = some b at h
    rw [if_neg ha] at h
    exact absurd h (by simp)

theorem W0_liveClosed : W0.LiveClosed := by
  intro r s _ hout
  change inTwo r
  rcases hout with h | ⟨h1, h2⟩
  · subst h; assumption
  · subst h1; exact Or.inl rfl

theorem W0_cc : W0.CC := by
  intro r hr; exact Or.inl hr

theorem W0_chainOK : W0.ChainOK := by
  intro r s hr hs hlr hls hd
  change inTwo r at hlr
  change inTwo s at hls
  change (if r = 0 then 0 else 1) ≤ (if s = 0 then 0 else 1) at hd
  rcases hlr with h | h
  · subst h
    rcases hls with h' | h'
    · subst h'; exact Or.inl rfl
    · subst h'; exact Or.inr ⟨rfl, rfl⟩
  · subst h
    rcases hls with h' | h'
    · subst h'; simp at hd
    · subst h'; exact Or.inl rfl

/-- **释放不向下封闭**的语义：可以杀死区域 0，却让区域 1 活着。 -/
inductive LooseStep : World → World → Prop
  | store (W : World) (a b : Addr) (dr : Nat)
      (hAcc : dr ≤ W.F.depth (W.reg a))
      (hInvH : W.F.depth (W.reg b) ≤ dr)
      (hAliveCell : W.alive (W.reg a))
      (hLocalCell : W.F.Local (W.reg a))
      (hAliveTarget : W.alive (W.reg b))
      (hCC : W.CC)
      (hChain : W.ChainOK) :
      LooseStep W (W.store a b)
  | kill (W : World) (k : Reg → Prop) : LooseStep W (W.kill k)

theorem loose_kill : LooseStep W0 (W0.kill (fun r => r = 0)) :=
  LooseStep.kill W0 (fun r => r = 0)

theorem dead_pointer : ¬ (W0.kill (fun r => r = 0)).Safe := by
  intro h
  have hcell : (W0.kill (fun r => r = 0)).alive ((W0.kill (fun r => r = 0)).reg 0) := by
    change (inTwo 1 ∧ ¬ ((1 : Nat) = 0))
    exact ⟨Or.inr rfl, by decide⟩
  have hptr : (W0.kill (fun r => r = 0)).ptr 0 = some 1 := by
    change (if (0 : Nat) = 0 then some 1 else none) = some 1
    simp
  have hbad := h 0 1 hcell hptr
  change (inTwo 0 ∧ ¬ ((0 : Nat) = 0)) at hbad
  exact hbad.2 rfl

/-- ★ **「释放必须向下封闭」的必要性**。 -/
theorem necessity_kill_closed :
    ∃ (W W' : World), W.Contains ∧ W.LiveClosed ∧ LooseStep W W' ∧ ¬ W'.Safe :=
  ⟨W0, W0.kill (fun r => r = 0), W0_contains, W0_liveClosed, loose_kill, dead_pointer⟩

end LooseKill

/-! ## §6 链上线性化公理（ChainOK）是独立的

    两个**同时活着的兄弟区域**深度相同（都是 0），谁也不 outlive 谁。
    此时 `depth r ≤ depth s` 成立而 `outlives r s` 不成立 —— ChainOK 失效。
    在 extC 里这正是"两个挂起任务的地方同时活着"的形状。 -/
theorem chainOK_independent :
    ∃ (outlives : Reg → Reg → Prop) (Local Ground alive : Reg → Prop) (depth : Reg → Nat),
      (∀ r, outlives r r) ∧
      (∀ a b c, outlives a b → outlives b c → outlives a c) ∧
      (∀ r s, outlives r s → depth r ≤ depth s) ∧
      (∀ r, Ground r → depth r = 0) ∧
      (∀ r s, Ground r → Local s → outlives r s) ∧
      ¬ (∀ r s, alive r → alive s → Local r → Local s → depth r ≤ depth s → outlives r s) := by
  refine ⟨(fun a b => a = b), (fun _ => True), (fun _ => False), (fun _ => True),
          (fun _ => 0), ?_, ?_, ?_, ?_, ?_, ?_⟩
  · intro r; rfl
  · intro a b c h1 h2; exact h1.trans h2
  · intro r s _; exact Nat.le_refl 0
  · intro r h; exact absurd h (by simp)
  · intro r s h _; exact absurd h (by simp)
  · intro h
    have h01 := h 0 1 trivial trivial trivial trivial (Nat.le_refl 0)
    exact absurd h01 (by decide)

/-! ## §7 哨兵 0：`min` 被写成 `max`（审计 P0-2 的抽象形式）

    调用点的 arena 深度用 `0` 同时表示"还没见到"与"depth 0 = 活得最久"。
    结果 `[0, 1]` 求出的不是 0 而是 1 ⇒ 选中的是**本帧块 arena** 而不是家的 arena
    ⇒ 被调方按约定长期持有的对象落在会退出的块里。 -/

/-- 正确的：取最小。 -/
def realMin : List Nat → Nat
  | [] => 0
  | x :: xs => min x (realMin xs)

/-- 出错的：用 0 兼作"未设置"，于是 `0` 会被后出现的更大值顶掉。 -/
def buggyMin : List Nat → Nat
  | [] => 0
  | x :: xs =>
      let rest := buggyMin xs
      if rest == 0 then x else if x == 0 then rest else min x rest

/-- 错的这一版**只会记大**（数值更大 = 以为对象活得短）。
    而"记大"在这里恰恰是**不安全**：深度记大 ⇒ 选更浅的 arena ⇒ 对象比使用点先死。 -/
theorem buggyMin_ge_realMin : ∀ l : List Nat, realMin l ≤ buggyMin l := by
  intro l
  induction l with
  | nil => simp [realMin, buggyMin]
  | cons x xs ih =>
      simp only [realMin, buggyMin]
      split
      · omega
      · split <;> omega

/-- `[0,1]` 上：正确解 0（活得最久的家），错解 1（本帧块）。 -/
theorem buggyMin_picks_block : buggyMin [0, 1] = 1 ∧ realMin [0, 1] = 0 := by
  decide

/-! ## §8 放大引理：把分配区域换得更长寿，永远安全

    「精度只买内存，不买健全性」的形式化：任何误拒都能靠"把区域放大"在源码层修好。 -/

/-- 把单元 b 的区域改成一个更长寿的区域 r'。 -/
def World.retarget (W : World) (b : Addr) (r' : Reg) : World :=
  { W with reg := fun x => if x = b then r' else W.reg x }

/-- **放大引理**：把一个**没有出边**的对象的区域换成更长寿的区域，INV-I 保持。

    注意"没有出边"这个前提**不是装饰**：对象搬到更长寿的区域之后，
    它自己持有的指针也必须活得更久，否则新位置反而撑不住自己的出边。
    这正是"提升必须整组搬"的形式化理由（见 README 的修正 C5）。 -/
theorem World.contains_retarget {W : World} (hC : W.Contains) {b : Addr} {r' : Reg}
    (hLeaf : W.ptr b = none) (hAmp : W.F.outlives r' (W.reg b)) :
    (W.retarget b r').Contains := by
  intro x y hxy
  have hptr : W.ptr x = some y := hxy
  change W.F.outlives (if y = b then r' else W.reg y) (if x = b then r' else W.reg x)
  by_cases hy : y = b
  · rw [if_pos hy]
    cases hy
    have hx : x ≠ b := by
      intro hxb; subst hxb; rw [hLeaf] at hptr; exact absurd hptr (by simp)
    rw [if_neg hx]
    exact W.F.out_trans r' (W.reg b) (W.reg x) hAmp (hC x b hptr)
  · rw [if_neg hy]
    by_cases hx : x = b
    · subst hx; rw [hLeaf] at hptr; exact absurd hptr (by simp)
    · rw [if_neg hx]
      exact hC x y hptr

/-! ## §9 结论

    **定理（区域档健全性）** `Runs.safe`
      前提：区域几何公理 + 边界条件 CC + ChainOK + INV-H + 释放向下封闭
      结论：运行中每一步，活单元里的指针都指向活单元。

    **边界（各自机器检查）**
      `ForeignRegion.necessity_CC`        —— 去掉 CC ⇒ 悬垂
      `Undershoot.necessity_INVH`         —— 去掉 INV-H ⇒ 悬垂
      `LooseKill.necessity_kill_closed`   —— 去掉"释放向下封闭" ⇒ 悬垂
      `chainOK_independent`               —— ChainOK 独立于其余公理
      `buggyMin_picks_block`              —— 哨兵 0 的具体后果

    本文件不含 `sorry`、不引入 `axiom`。 -/

#print axioms ExtC.Region.Runs.safe
#print axioms ExtC.Region.ForeignRegion.necessity_CC
#print axioms ExtC.Region.Undershoot.necessity_INVH
#print axioms ExtC.Region.LooseKill.necessity_kill_closed
#print axioms ExtC.Region.chainOK_independent
#print axioms ExtC.Region.World.contains_retarget
#print axioms ExtC.Region.linearization_all_targets
#print axioms ExtC.Region.World.target_alive_of_read
#print axioms ExtC.Region.Step.store_of_read

end ExtC.Region
