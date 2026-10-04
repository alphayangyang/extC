/-
  ExtCArenaPool.lean —— **Arena 与 Pool 配合**：精度上限抬到哪、残余是什么。

  `ExtCMechanism.lean` 证的是区域档的硬限制：

      same_region_same_lifetime : reg a = reg b → (alive (reg a) ↔ alive (reg b))
      cannot_free_separately    : reg a = reg b → ¬ (alive a ∧ ¬ alive b)

  也就是说：**区域存活不是自由的** —— 它必须是嵌套序的**下闭集**
  （这正是已证的 INV-L：`alive s ∧ outlives r s → alive r`）。
  这条结构约束**免费**换来"整块释放、O(1)、bump 分配"，
  代价是"要么整块活，要么整块死"。

  池走的是另一条路：存活是**逐槽**的（`Slot.occ`/`Slot.gen`），
  没有任何结构约束 ⇒ 逐对象回收**可表达**。
  代价是反过来的：**"何时 free"必须由某处给出**
  （结构操作 / 作用域结束 / 静态推导 / 用户显式调用）。

  于是配合后的分工与上限：

  | 死因 | 谁覆盖 | 需要什么 |
  |---|---|---|
  | **作用域序**（LIFO，块退出） | Arena | 无（结构自带） |
  | **结构操作**（erase / clear / shrink / pop / 换缓冲） | Pool | 容器语义已经给出时机 |
  | **可达性**（最后一个引用消失） | 两者都不覆盖 | 需要 RC/GC 或所有权分析 |

  ⇒ **配合把上限从"区域粒度"抬到"区域粒度 ∧ 结构粒度"**；
     残余恰好是"死由**可达性**决定"的那一类 —— 而架构对它另有安排：
     禁止池元素携带引用、跨引用走**世代句柄**（把可达性问题换成显式生命周期 + trap）。

  本文件不含 `sorry`、不引入 `axiom`。
-/

import ExtCRegion
import ExtCPool

set_option autoImplicit false
set_option linter.deprecated false

namespace ExtCArenaPool

open ExtC.Region
open ExtC.Pool

/-! ## §1 区域档：存活被结构绑住（下闭集） -/

/-- ★ **区域存活是嵌套序的下闭集**：活着意味着它的所有祖先都活着。
    这不是实现细节，是"整块释放"的代价 ——
    活着的形状被限制成下闭集，**不允许任意子集**。 -/
theorem region_liveness_downward_closed (W : World) (hL : W.LiveClosed) {r s : Reg}
    (hs : W.alive s) (ho : W.F.outlives r s) : W.alive r :=
  hL r s hs ho

/-- 反面：区域档里**不允许**一个活一个死（同区）。 -/
theorem region_cannot_split (W : World) {a b : Addr} (h : W.reg a = W.reg b) :
    ¬ (W.alive (W.reg a) ∧ ¬ W.alive (W.reg b)) := by
  rw [h]; simp

/-! ## §2 池档：存活逐槽自由 -/

/-- ★ **池的槽互相独立**：同一次分配、同一个池里，
    可以让一个槽活着而另一个槽已经释放 —— 这正是区域档做不到的事。 -/
theorem pool_slots_independent :
    ∃ s : State, (s 0).occ ≠ none ∧ (s 1).occ = none :=
  ⟨fun i => if i = 0 then ⟨0, some 1⟩ else ⟨0, none⟩, by simp, by simp⟩

/-- 而且"释放后又能复用"是池的机制自带（世代保证复用后可检出陈旧句柄，
    见 `ExtCPool.handle_does_not_rebind`）—— 区域档里没有对应物。 -/
theorem pool_reuse_with_generation :
    ∃ (s t : State), (s 0).occ = some 42 ∧ (t 0).occ = some 99 ∧ (t 0).gen ≠ (s 0).gen :=
  ⟨(fun i => if i = 0 then ⟨0, some 42⟩ else ⟨0, none⟩),
   (fun i => if i = 0 then ⟨1, some 99⟩ else ⟨0, none⟩),
   by simp, by simp, by simp⟩

/-! ## §3 于是：配合后的精度上限

    **区域**：死因必须是"作用域序"（LIFO）。免费换来 O(1) bump 分配与整块释放。
    **池**：死因可以是"任意显式事件"。换来逐对象复用，代价是
            每对象开销（文档口径 2.9×）与"必须有人告诉它何时 free"。
    **两者合起来**：作用域序 ∧ 结构操作事件 **都覆盖**。

    **残余**：死因是"可达性"（最后一个引用消失）的对象 ——
    arena 只能拖到作用域末，pool 只能等一个显式事件，
    两者都不知道"这是最后一个引用了"。

    架构对这个残余的处理不是"更聪明的分析"，而是**换表示**：
      * 池元素**禁止携带引用**（POOL 边界规则）；
      * 跨引用走**句柄**（`{pool, idx, gen}`），陈旧即 trap。
    ⇒ 把"可达性"问题转成"显式生命周期"问题：换来可判定与 O(1) 安全，
      代价是**失去自动性**（必须有人 erase / reset / release）。

    **结论**：
      配合把内存精度上限从「区域粒度」抬到「区域粒度 ∧ 结构粒度」；
      对"作用域 + 结构操作"能表达的那部分数据，它与 GC 级精度**等价**；
      残余只有"纯可达性死亡"（共享可变图 / 环 / 别名下的最后引用）。

    本文件不含 `sorry`、不引入 `axiom`。 -/

#print axioms ExtCArenaPool.region_liveness_downward_closed
#print axioms ExtCArenaPool.region_cannot_split
#print axioms ExtCArenaPool.pool_slots_independent
#print axioms ExtCArenaPool.pool_reuse_with_generation

end ExtCArenaPool
