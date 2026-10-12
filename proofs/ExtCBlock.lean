/-
  ExtCBlock.lean —— **块级**内存模型：把「提权」与「覆盖即回收」都变成模型里的动作。

  为什么需要这一层（见 ExtCReclaim.lean §3）：区域粒度下，"单元还活着、它指向的那一块
  被单独回收"**不可表达** —— 向下封闭地杀会连引用者一起杀，不向下封闭则 `Safe` 破。
  引入**块**（比区域细、可单独释放）之后两件事都能陈述：

    §1  块级世界与四条不变量
    §2  **提权**：块从一个区域转交到另一个区域（载荷不动）
          · `safe_promote`    —— `Safe` 逐字不变（提权不碰 `ptr`、不碰 `blkAlive`）
          · `contains_promote` —— 真正的义务在**包含性**：目标区域必须活得够久
    §3  **回收**：释放一个没有活单元指向的块
          · `safe_freeBlk`    —— 充分条件：没人指
          · `free_pointed_blk_unsafe` —— 必要条件：只要还有人指，`Safe` 必破
          ⇒ 两者合起来 = **回收的判据就是"证明没人指"**，这就是残余差距的所在
    §4  与区域粒度的对比：回收一块之后，**引用者可以继续活着**（区域模型做不到）

  记号沿用 ExtCRegion.lean：`outlives r s` 读作「r 活得 ≥ s」。
-/
import ExtCRegion

namespace ExtC.Region
namespace Block

/-- 块实例。一个区域（一次 arena）由若干块组成，块可以单独释放。 -/
abbrev Blk := Nat
/-- 单元 = 一次分配出来的存储。 -/
abbrev Cell := Addr

/-- **块级世界**：区域树 + 块到区域的归属 + 块的生死 + 单元住在哪个块 + 指针图。

    与区域级 `World` 的差别只有一处，但正是关键的一处：
    `blkAlive` 让"释放"可以发生在**比区域更细**的粒度上。 -/
structure BWorld where
  F : Frame
  /-- 区域活着（帧级；释放只能向下封闭地整片释放）。 -/
  regAlive : Reg → Prop
  /-- 块属于哪个区域。**提权就是改这个映射**，不动载荷。 -/
  blkReg : Blk → Reg
  /-- 块活着。**回收就是改这个谓词**，不动区域。 -/
  blkAlive : Blk → Prop
  /-- 每个单元住在哪个块里。 -/
  cellBlk : Cell → Blk
  /-- 每个单元持有的指针。 -/
  ptr : Cell → Option Cell

/-! ## §1 四条不变量 -/

variable {W : BWorld}

/-- **INV-I′（包含性）**：单元持有的指针，其目标**块所在区域**活得 ≥ 该单元**块所在区域**。 -/
def BWorld.Contains (W : BWorld) : Prop :=
  ∀ a b, W.ptr a = some b → W.F.outlives (W.blkReg (W.cellBlk b)) (W.blkReg (W.cellBlk a))

/-- **INV-L（区域树向下封闭）**：活着的区域，其祖先也活着。与区域级模型同一条。 -/
def BWorld.RegClosed (W : BWorld) : Prop :=
  ∀ r s, W.regAlive s → W.F.outlives r s → W.regAlive r

/-- **INV-B（块有效性）**：活着的块，其所属区域也活着。
    这一条让"块活着"蕴含"区域活着"，于是块级 `Safe` 不必再提区域。 -/
def BWorld.BlkValid (W : BWorld) : Prop :=
  ∀ β, W.blkAlive β → W.regAlive (W.blkReg β)

/-- **单元活着** = 它住的块活着。 -/
def BWorld.CellLive (W : BWorld) (a : Cell) : Prop := W.blkAlive (W.cellBlk a)

/-- **安全（块级）**：活单元里的每个指针都指向活单元。
    与区域级 `Safe` 逐字同形，只是"活着"落在**块**上而不是区域上。 -/
def BWorld.Safe (W : BWorld) : Prop :=
  ∀ a b, W.CellLive a → W.ptr a = some b → W.CellLive b

/-! ## §2 提权：块转交到另一个区域 -/

/-- **提权**：把块 `β` 的归属区域改成 `r'`。载荷、地址、块的生死都不动。 -/
def BWorld.promote (W : BWorld) (β : Blk) (r' : Reg) : BWorld :=
  { W with blkReg := fun x => if x = β then r' else W.blkReg x }

/-- 提权**不动指针、不动块的生死**，所以 `Safe` 逐字不变 —— 提权本身是安全的。

    真正要付出的义务在 `contains_promote`（包含性）：转交之后，块里的单元所指向的
    目标区域必须仍然活得 ≥ 新的区域。这就是检查器在逃逸点算的那件事。 -/
theorem safe_promote (hS : W.Safe) (β : Blk) (r' : Reg) :
    (W.promote β r').Safe := hS

/-- **提权的义务**：设 `β` 转交到 `r'`。要求两块：
      · 从 `β` 里出去的指针，其目标区域活得 ≥ `r'`（出边）；
      · 从外面指进 `β` 的指针，其单元所在区域被 `r'` 覆盖（入边）。
    满足则包含性保持。 -/
theorem contains_promote (hC : W.Contains) {β : Blk} {r' : Reg}
    (hOut : ∀ a b, W.cellBlk a = β → W.ptr a = some b →
      W.F.outlives (W.blkReg (W.cellBlk b)) r')
    (hIn : ∀ a b, W.ptr a = some b → W.cellBlk b = β →
      W.F.outlives r' (W.blkReg (W.cellBlk a))) :
    (W.promote β r').Contains := by
  intro a b hp
  show W.F.outlives ((if W.cellBlk b = β then r' else W.blkReg (W.cellBlk b)))
                   ((if W.cellBlk a = β then r' else W.blkReg (W.cellBlk a)))
  by_cases hb : W.cellBlk b = β
  · by_cases ha : W.cellBlk a = β
    · rw [ite_eq_left hb, ite_eq_left ha]; exact W.F.out_refl r'
    · rw [ite_eq_left hb, ite_eq_right ha]; exact hIn a b hp hb
  · by_cases ha : W.cellBlk a = β
    · rw [ite_eq_right hb, ite_eq_left ha]; exact hOut a b ha hp
    · rw [ite_eq_right hb, ite_eq_right ha]; exact hC a b hp

/-- 提权保持 **INV-B**：只要目标区域活着，活块仍然落在活区域里。 -/
theorem blkValid_promote (hV : W.BlkValid) {r' : Reg} (hr' : W.regAlive r') :
    (W.promote β r').BlkValid := by
  intro γ hγ
  show W.regAlive (if γ = β then r' else W.blkReg γ)
  by_cases h : γ = β
  · rw [ite_eq_left h]; exact hr'
  · rw [ite_eq_right h]; exact hV γ hγ

/-- 提权不动区域树，所以 **INV-L** 逐字不变。 -/
theorem regClosed_promote (hL : W.RegClosed) (β : Blk) (r' : Reg) :
    (W.promote β r').RegClosed := by
  intro r s hs hout
  exact hL r s hs hout

/-! ## §3 回收：释放一个没有活单元指向的块 -/

/-- **回收**：把块 `β` 标成死的。区域、指针、归属都不动。 -/
def BWorld.freeBlk (W : BWorld) (β : Blk) : BWorld :=
  { W with blkAlive := fun x => W.blkAlive x ∧ x ≠ β }

/-- **充分条件**：没有任何活单元指向 `β` ⇒ 释放 `β` 保持 `Safe`。
    **不需要向下封闭** —— 这正是块级比区域级宽松的地方。 -/
theorem safe_freeBlk (hS : W.Safe) {β : Blk}
    (hNoPtr : ∀ a b, W.CellLive a → W.ptr a = some b → W.cellBlk b ≠ β) :
    (W.freeBlk β).Safe := by
  intro a b ha hp
  have ha' : W.CellLive a := ha.1
  have hb' : W.CellLive b := hS a b ha' hp
  exact ⟨hb', hNoPtr a b ha' hp⟩

/-- **必要条件**：只要还有一个活单元（住在别的块里）指向 `β`，释放 `β` 就破坏 `Safe`。
    与 `safe_freeBlk` 合起来：**回收的判据恰好是"证明没人指"**。 -/
theorem free_pointed_blk_unsafe {β : Blk} {a b : Cell}
    (ha : W.CellLive a) (hp : W.ptr a = some b) (hb : W.cellBlk b = β)
    (hcell : W.cellBlk a ≠ β) :
    ¬ (W.freeBlk β).Safe := by
  intro hSafe
  have ha' : (W.freeBlk β).CellLive a := ⟨ha, hcell⟩
  have := hSafe a b ha' hp
  exact this.2 (by simpa [BWorld.freeBlk] using hb)

/-- 回收不动指针与区域 ⇒ **INV-I′** 及 **INV-L** 逐字保持。 -/
theorem contains_freeBlk (hC : W.Contains) (β : Blk) : (W.freeBlk β).Contains := hC

/-- 回收不动区域树 ⇒ **INV-L** 逐字保持。 -/
theorem regClosed_freeBlk (hL : W.RegClosed) (β : Blk) : (W.freeBlk β).RegClosed := hL

/-! ## §4 与区域粒度的对比 -/

/-- **区域模型做不到的事，块模型做到了**：回收 `β` 之后，
    住在**别的块**里的引用者**照样活着** —— 它没有被连带杀掉。
    （对比 `ExtCReclaim.region_granularity_dilemma`：区域粒度下这必然破坏 `Safe`。） -/
theorem referrer_survives_free {β : Blk} {a : Cell} (ha : W.CellLive a) (hcell : W.cellBlk a ≠ β) :
    (W.freeBlk β).CellLive a := ⟨ha, hcell⟩

/-- 回收一块之后，**别的块里的活单元**完全不受影响 —— 这就是"块可以单独回收"的形式含义：
    两条不变量都保持（`Safe` 由 §3 的判据给出，包含性逐字不变）。

    注意这里写的是**合取**而不是 `(W.freeBlk β).Contains = W.Contains`：
    用 `rfl` 去证 Prop 之间的等式会让 Lean 在这条上耗掉几分钟（本文件实测），
    而结论完全一样，所以不这么写。 -/
theorem free_only_touches_its_block (hS : W.Safe) (hC : W.Contains) {β : Blk}
    (hNoPtr : ∀ a b, W.CellLive a → W.ptr a = some b → W.cellBlk b ≠ β) :
    (W.freeBlk β).Safe ∧ (W.freeBlk β).Contains :=
  ⟨safe_freeBlk hS hNoPtr, contains_freeBlk hC β⟩

end Block
end ExtC.Region
