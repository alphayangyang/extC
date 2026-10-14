/**
 * @file reclaim.h
 * @brief 回收运行期的按需发射口：位图 slab 分配器 + 持有者计数归零 + 区域清扫。
 *
 * 形状与 pools.h / plate.h 那几扇门一致：codegen 里一个标志，按需把这段 C 追加进同一个编译单元。
 * 触发点是"程序里有需要回收的堆存储"，所以不用回收的程序一个字节都不带。
 *
 * 发射出来的文本是**自足**的：只依赖生成文件顶部已经 include 的
 * `<stdint.h>` / `<stddef.h>` / `<stdlib.h>` / `<stdio.h>`，并且可以在用户代码**之前**发射
 * —— 它不引用任何用户类型：类型的指针字段布局由调用方通过 `ExtcRcType` 表交给区域。文本里所有
 * 函数与数据都是 `static`，不产生外部符号，也不会与用户代码重名（一律带 `extc_rc_` 前缀）。
 *
 * 机制：区域是一串按 `EXTC_RC_SLAB_BITS` 对齐的 slab；载荷按 16 字节取整成槽（不是 2 的幂，免得
 * 80 字节的对象白占 128），由对象指针找 slab 一次按位与、找槽号一次乘倒数右移；持有者计数只挂在
 * **指针赋值**上，归零当场把槽还给空闲链；环、以及计数还被已死对象的出边
 * 拖着的那部分，由区域清扫兜底 —— 清扫从根 + 跨区登记位图出发标记，再顺序扫 64 位字释放未标记者。
 * 标记按**每个槽自带的类型描述符**走指针字段；描述符由分配点（codegen）随槽交给运行期，
 * 所以这里没有区域级类型表。
 *
 * 板/池/栈/C 返回的指针不是本运行期的槽：每个 slab 创建时登记进一张按基址排序的表，
 * `inc`/`dec`/`register`/标记**先问一次 owns**，表外或空闲槽一律跳过。没有这条守卫，那些指针
 * 会被当成 slab 地址按位与后读到无关内存（板的 4 GiB 保留区甚至没 commit）⇒ 段错误。
 */
#ifndef EXTC_RECLAIM_H
#define EXTC_RECLAIM_H

#include "base.h"

/**
 * @brief 把回收运行期的 C 文本追加到 `out`。
 *
 * 追加一次即可，位置在**调用它的代码之前**（生成文件的顶部、用户函数之前）：文本里的函数是
 * `static`，调用点必须先见到它们。文本不含任何用户类型，所以放在用户类型定义的前后都成立。
 * 只追加一次 —— 重复追加会让同一批 `static` 函数重复定义。
 *
 * 追加的文本提供这些入口：区域 `extc_rc_regionNew` / `extc_rc_regionFloor` / `extc_rc_regionFree`，
 * 槽分配 `extc_rc_alloc(区域, 字节, 类型描述符)`，解析区 `extc_rc_bump` / `extc_rc_bumpReset`，
 * 计数 `extc_rc_inc` / `extc_rc_dec` / `extc_rc_initRef` / `extc_rc_storeRef`，跨区登记
 * `extc_rc_storeRefOwned(slot, v, owner)`（来源可证）与 `extc_rc_storeRefUnknown(slot, v)`
 * （来源不可证 ⇒ 目标区域整个退出清扫），清扫 `extc_rc_mark` / `extc_rc_sweep` / `extc_rc_collect` /
 * `extc_rc_maybeCollect(区域, 根表, 根数)`，以及只读的记账查询。类型描述符 `ExtcRcType`
 * 与兜底描述符 `EXTC_RC_CONSERVATIVE`（标记扫整个槽）也由这里定义。
 *
 * @param a    arena（本函数不用，为与其他发射口同形保留）。
 * @param out  正在写的生成 C。
 * @param noCount    `EXTC_RC_NOCOUNT`：计数入口编译成空操作。
 * @param timeOn     `EXTC_RC_TIME`：运行期分项计时。
 * @param sweepFree  `EXTC_RC_SWEEP_FREE`：事件档，`shouldSweep` 恒 0。
 */
void reclaimEmitRuntime(Arena *a, Buf *out, int noCount, int timeOn, int sweepFree, int localStore);

#endif /* EXTC_RECLAIM_H */
