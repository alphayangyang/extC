/-
  ExtCMechanism.lean —— **机制层**的硬限制：为什么"GC 级精度"不是分析能单独买到的。

  区域（arena）模型里，"存活"是**区域**的属性（`World.alive : Reg → Prop`），
  不是对象的属性。于是**同一区域里的对象同生共死** —— 这跟分析有多准**无关**。

  这给出一个精确的三段式结论：

    ① **分析层**：堆可达性不可判定 ⇒ 对任意程序做到"死的那一刻就释放"不可能。
       但在**唯一所有权**片段里它是可判、可做的（Rust 的 drop 消解就是这一档）。
    ② **机制层**：即使分析完美，区域粒度也把同区对象绑在一起（本文件）。
       要做到逐对象，唯一办法是让 `reg` 区分它们 —— 极端是"一对象一区域"，
       那就放弃了 bump 共享（仓库实测：逐对象池 26 ns / 2.9× 内存 vs arena 0.5 ns）。
    ③ **成本层**：逐对象回收有**每对象开销** h；当对象大小 s < h 时，
       区域粒度的"拖到区域末"比逐对象回收**更便宜** ⇒ GC 级精度是负收益。

  ⇒ 所以问题是**机制 × 分析 × 语言约束**三者的联合，不是"静态分析行不行"。

  本文件不含 `sorry`、不引入 `axiom`。
-/

import ExtCRegion

set_option autoImplicit false

namespace ExtCMechanism

open ExtC.Region

/-- ★ **机制层限制**：同一区域里的两个对象，**存活是同一条命题** ——
    不可能出现"一个活、另一个已经释放"。这就是"区域粒度"。 -/
theorem same_region_same_lifetime (W : World) {a b : Addr} (h : W.reg a = W.reg b) :
    (W.alive (W.reg a) ↔ W.alive (W.reg b)) := by
  rw [h]

/-- 同区 ⇒ **不可能**单独释放其中一个。 -/
theorem cannot_free_separately (W : World) {a b : Addr} (h : W.reg a = W.reg b) :
    ¬ (W.alive (W.reg a) ∧ ¬ W.alive (W.reg b)) := by
  rw [h]
  simp

/-- ★ **充要刻画（这一半）**：只要 `reg` 是单射（一对象一区域），
    **任意**逐对象存活模式都可表达 —— 这就是"一对象一区域"买到的东西。 -/
theorem injective_reg_realizes_any_liveness (reg : Addr → Reg)
    (hinj : ∀ a b : Addr, reg a = reg b → a = b) (L : Addr → Prop) :
    ∃ alive : Reg → Prop, ∀ a, alive (reg a) ↔ L a := by
  refine ⟨fun r => ∃ a, reg a = r ∧ L a, ?_⟩
  intro a
  constructor
  · rintro ⟨b, hb, hL⟩
    rwa [hinj b a hb] at hL
  · intro hL
    exact ⟨a, rfl, hL⟩

/-- ★ **充要刻画（另一半）**：若 `reg` 把两个对象映到同一区域，
    那么**不存在任何** `alive` 能让它们一个活一个死。 -/
theorem noninjective_blocks_liveness (reg : Addr → Reg) (alive : Reg → Prop)
    {a b : Addr} (h : reg a = reg b) : ¬ (alive (reg a) ∧ ¬ alive (reg b)) := by
  rw [h]
  simp

/-! ## 结论

    **机制层的硬限制（本文件）**：区域粒度把同区对象绑成同一条存活命题
    （`same_region_same_lifetime` / `cannot_free_separately`）。
    因此"GC 级逐对象精度"**不可能**只靠提高分析精度拿到 ——
    必须同时改机制（一对象一区域 / 池 / RC），而每一种都有每对象的固定开销。

    **再加上分析层**（堆可达性不可判定）与**语言层**（extC 设计上允许别名 ⇒
    没有唯一性可推），三者合起来才是"做不到"的完整理由；
    松开其中任何一个（加线性类型 / 换机制 / 接受运行时簿记）都能拿到一部分。

    本文件不含 `sorry`、不引入 `axiom`。 -/

#print axioms ExtCMechanism.same_region_same_lifetime
#print axioms ExtCMechanism.cannot_free_separately
#print axioms ExtCMechanism.injective_reg_realizes_any_liveness
#print axioms ExtCMechanism.noninjective_blocks_liveness

end ExtCMechanism
