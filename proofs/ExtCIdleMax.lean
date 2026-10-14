/-
  ExtCIdleMax.lean —— 「判 idle」的**最大健全范围**、**L1 传递健全**、**被迫性**。

  「判 idle」＝ 编译期判定"这个单元整段不需要发射回收运行期"。它的安全性质
  （见 `tests/reclaim-perf/IDLE-FORMAL-SPEC.md`）：

      Safe(D)  ⟺  垃圾有界（**活集可以随 n 涨** —— `rg_walk_chain` 就是这一类）

  这份文件回答三个"范围"问题，而不是"再加一条规则"：

    §1 **L1（链接入结构）传递健全**：最小堆模型上证 —— 宿主 `o` 从根 `r` 可达、且到 `o` 的
       那条路径不经过 `(o,f)` 本身（新建对象链入老结构时必然如此），把 `o.f := v` 之后
       `v` 从 `r` 可达。这条对应实现里的 `idleLinkedInto`（`rg_grow_tail` 的 `tail = p`）。
    §2 **最大范围**：抽象域内任何**健全且闭合**的不变量 `X`（`X ⊆ F X`）都被**每一次下降迭代**
       包含 ⇒ 迭代稳定时取到的就是域内最大范围；要再扩只能**扩域**（加事实），不能改规则。
    §3 **被迫性**：抽象点里只要混进一个"会丢根"的具体化，任何健全规则都必须拒绝它
       ⇒ 这部分损失改规则消不掉（与 `ExtCPrecision` §2 同一个论证）。

  本文件不含 `sorry`、不引入 `axiom`。
-/

set_option autoImplicit false
set_option linter.unusedSectionVars false

namespace ExtC.IdleMax

/-! ## §1 最小堆模型与 L1 传递 -/

variable {Obj Field Root : Type} [DecidableEq Obj] [DecidableEq Field]

/-- 极小的堆状态：字段表 + 根表。字段是**函数式**的（一个对象的一个字段只有一个当前值），
    所以"改指"就是覆盖一条边。 -/
structure Heap where
  field : Obj → Field → Option Obj
  root  : Root → Option Obj

/-- 一条从 `a` 出发、沿给定 (对象, 字段) 序列走、到 `b` 结束的**路径**。
    用路径（而不是传递闭包）是为了能说"这条路径没用到某条边"。 -/
inductive Valid (h : Heap (Obj := Obj) (Field := Field) (Root := Root)) : Obj → List (Obj × Field) → Obj → Prop
  | nil (o : Obj) : Valid h o [] o
  | cons {a b c : Obj} {f : Field} {rest : List (Obj × Field)} :
      h.field a f = some b → Valid h b rest c → Valid h a ((a, f) :: rest) c

/-- 从根 `r` 沿 `rest` 到 `o`（首跳由根表给出）。 -/
def FromRoot (h : Heap (Obj := Obj) (Field := Field) (Root := Root))
    (r : Root) (rest : List (Obj × Field)) (o : Obj) : Prop :=
  ∃ o0 : Obj, h.root r = some o0 ∧ Valid h o0 rest o

/-- 从根 `r` 到 `o` 的可达性（传递闭包口径，陈述用）。 -/
inductive Reach (h : Heap (Obj := Obj) (Field := Field) (Root := Root)) (r : Root) : Obj → Prop
  | root {o : Obj} : h.root r = some o → Reach h r o
  | step {o o' : Obj} (f : Field) : Reach h r o → h.field o f = some o' → Reach h r o'

/-- 从某点出发沿路径可达 ⇒ 从根可达（把路径接到已有的可达性上）。 -/
theorem valid_to_reach {h : Heap (Obj := Obj) (Field := Field) (Root := Root)}
    {r : Root} {a b : Obj} {rest : List (Obj × Field)}
    (hv : Valid h a rest b) (ha : Reach h r a) : Reach h r b := by
  revert ha
  induction hv with
  | nil o => intro ha; exact ha
  | cons hf _ ih => intro ha; exact ih (Reach.step _ ha hf)

/-- 有路径 ⇒ 可达。 -/
theorem reach_of_fromRoot {h : Heap (Obj := Obj) (Field := Field) (Root := Root)}
    {r : Root} {o : Obj} {rest : List (Obj × Field)}
    (hp : FromRoot h r rest o) : Reach h r o := by
  rcases hp with ⟨o0, hr, hv⟩
  exact valid_to_reach hv (Reach.root hr)

/-- 把 `o.f` 覆盖成 `some v`，其余不动。 -/
def store (h : Heap (Obj := Obj) (Field := Field) (Root := Root))
    (o : Obj) (f : Field) (v : Obj) : Heap (Obj := Obj) (Field := Field) (Root := Root) :=
  { field := fun o' f' => if o' = o ∧ f' = f then some v else h.field o' f'
  , root := h.root }

/-- 覆盖的边不是这一条 ⇒ 这条边在新堆里照旧。 -/
theorem store_field_ne {h : Heap (Obj := Obj) (Field := Field) (Root := Root)}
    {o v a b : Obj} {f fld : Field} (hne : (a, fld) ≠ (o, f))
    (hf : h.field a fld = some b) : (store h o f v).field a fld = some b := by
  unfold store
  by_cases hc : a = o ∧ fld = f
  · obtain ⟨h1, h2⟩ := hc
    subst h1; subst h2
    exact absurd rfl hne
  · simp [hc, hf]

/-- **不碰某条边的覆盖**：路径上的每条边都不是 `(o,f)` ⇒ 路径在新堆里依然有效。 -/
theorem valid_store {h : Heap (Obj := Obj) (Field := Field) (Root := Root)}
    {a b o v : Obj} {f : Field} {rest : List (Obj × Field)}
    (hv : Valid h a rest b) (hnot : ∀ e ∈ rest, e ≠ (o, f)) :
    Valid (store h o f v) a rest b := by
  induction hv with
  | nil o' => exact Valid.nil o'
  | cons hf hv ih =>
      exact Valid.cons (store_field_ne (hnot _ (by simp)) hf)
                       (ih (fun e he => hnot e (by simp [he])))

/-- **§1 主定理（L1 健全）**：宿主 `o` 从根 `r` 可达、到 `o` 的路径不经过 `(o,f)`
    ⇒ 把 `o.f := v` 之后，`v` 从 `r` 可达。

    语义上这就是 `idleLinkedInto`：新结点 `v` 被链进某个根（的投影）的结构里，
    于是它**成为该根的投影** —— 别处覆盖对它的引用时，它仍从该根可达。 -/
theorem reach_after_link {h : Heap (Obj := Obj) (Field := Field) (Root := Root)}
    {r : Root} {o v : Obj} {f : Field} {rest : List (Obj × Field)}
    (hp : FromRoot h r rest o) (hnot : ∀ e ∈ rest, e ≠ (o, f)) :
    Reach (store h o f v) r v := by
  rcases hp with ⟨o0, hr, hv⟩
  have hr' : (store h o f v).root r = some o0 := hr
  have hreach : Reach (store h o f v) r o :=
    valid_to_reach (valid_store hv hnot) (Reach.root hr')
  have hstep : (store h o f v).field o f = some v := by
    unfold store; simp
  exact Reach.step f hreach hstep

/-! ## §2 最大范围：下降迭代之下界住所有健全闭合不变量 -/

/-- 抽象域：每个槽位一组"来源"（纯 `Prop`，本仓库不依赖 Mathlib）。 -/
abbrev Dom (Slot Origin : Type) := Slot → Origin → Prop

/-- 抽象域上的包含序。 -/
def Sub {Slot Origin : Type} (X Y : Dom Slot Origin) : Prop := ∀ s o, X s o → Y s o

/-- 抽象域内的**闭合不变量**：`X ⊆ F X`（走一步规则不丢）。 -/
def Closed {Slot Origin : Type} (F : Dom Slot Origin → Dom Slot Origin)
    (X : Dom Slot Origin) : Prop := Sub X (F X)

/-- 下降迭代：从 ⊤（全都在）开始反复应用 `F`。实现里的"≤8 轮"就是它。 -/
def iter {Slot Origin : Type} (F : Dom Slot Origin → Dom Slot Origin) : Nat → Dom Slot Origin
  | 0     => fun _ _ => True
  | n + 1 => F (iter F n)

/-- **最大范围定理**：任何健全且闭合的不变量 `X` 都在**每一次**迭代结果之下。
    ⇒ 迭代稳定时取到的那个不变量就是抽象域内的**最大范围**：没有比它更宽的健全判据；
      要再扩大只能**扩域**（加事实），不能靠改规则。 -/
theorem closed_below_iter {Slot Origin : Type} {F : Dom Slot Origin → Dom Slot Origin}
    (hmono : ∀ X Y, Sub X Y → Sub (F X) (F Y)) {X : Dom Slot Origin} (hX : Closed F X) :
    ∀ n, Sub X (iter F n) := by
  intro n
  induction n with
  | zero => intro s o _; trivial
  | succ n ih => intro s o ho; exact hmono X (iter F n) ih s o (hX s o ho)

/-- 迭代在第 `n` 步稳定（`iter (n+1) ⊆ iter n`）⇒ `iter n` 是域内最大范围。 -/
theorem iter_is_max {Slot Origin : Type} {F : Dom Slot Origin → Dom Slot Origin}
    (hmono : ∀ X Y, Sub X Y → Sub (F X) (F Y)) {X : Dom Slot Origin} (hX : Closed F X)
    {n : Nat} (hfix : Sub (iter F (n + 1)) (iter F n)) : Sub X (iter F n) := by
  intro s o ho
  exact hfix s o (closed_below_iter hmono hX (n + 1) s o ho)

/-! ## §3 被迫性：混进不安全具体化 ⇒ 任何健全规则必须拒绝 -/

/-- **被迫性**：抽象域里只要有一个"会丢根"的具体化，任何健全规则都必须拒绝这个抽象点
    ⇒ 这部分范围损失**改规则消不掉**，只能靠细化抽象（补 L1/L2/L4 这些事实）。 -/
theorem forced_loss {α World : Type} (γ : α → World → Prop) (badW : World → Prop) (a : α)
    (hbad : ∃ w, γ a w ∧ badW w)
    (rule : α → Prop) (hsound : ∀ a, rule a → ¬ ∃ w, γ a w ∧ badW w) : ¬ rule a := by
  intro hr
  exact hsound a hr hbad

end ExtC.IdleMax
