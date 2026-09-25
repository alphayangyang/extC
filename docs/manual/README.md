# extC 语言手册

> **这份手册只描述「已经实现」的东西。**
> 已定案但还没实现的在第 10 节，还没定的在第 11 节。
> 一份说谎的手册比没有手册更坏 —— 所以这里宁可写得少，也不写没实现的东西。
>
> 当前版本：**week-0**（2026-09-18）
>
> 设计理由看 [`DESIGN.md`](../docs/DESIGN.md)；为什么这么定看 [`DECISIONS.md`](../docs/DECISIONS.md)；接下来做什么看 [`PLAN.md`](../docs/PLAN.md)。

---

---

## 目录

### 上路

- [0. 三分钟上手](00-quickstart.md)
- [0.5 内存安全：承诺 × 现状](01-safety.md)

### 语言

- [1. 程序结构](02-program.md)
- [2. 词法](03-lexical.md)
- [3. 类型](04-types.md)
- [4. 变量与声明](05-vars.md)
- [5. 表达式与运算符](06-expr.md)
- [6. 语句](07-stmt.md)

### 结构与内存

- [7. 结构体与方法](08-struct.md)
- [7.5 引用：`ref` 是表达式](09-ref.md)
- [7.6 可空引用 `?ref T`（定案 ㊻）](10-nullref.md)
- [7.7 分配：`new`（`new T` / `new [N]T` / `new T[n]`）](11-alloc.md)
- [7.8 驱动开关（`-O` / `-march`）](12-driver.md)
- [7.9 动态数组 `varArray<T>`（对标 `std::vector` 的**基础部分**）](13-vararray.md)

### 标准库与工具

- [8. 内建函数](14-builtins.md)
- [9. 错误信息](15-errors.md)
- [12. 库与模块（2026-09-22 新增）](18-modules.md)

### 状态与示例

- [10. 已定案、但还没实现](16-unimplemented.md)
- [11. 还没定的](17-undecided.md)
- [13. 完整示例](19-examples.md)

## 三条阅读路径

1. **想跑起来** → [三分钟上手](00-quickstart.md) → [内存安全：承诺 × 现状](01-safety.md) → [完整示例](19-examples.md)
2. **想写代码** → [类型](04-types.md) → [结构体与方法](08-struct.md) → [引用](09-ref.md) → [库与模块](18-modules.md)
3. **想查细节** → [表达式与运算符](06-expr.md) · [内建函数](14-builtins.md) · [错误信息](15-errors.md)

## 单页版

`docs/MANUAL.md` 现在只是一个**指针**（防止两份真源漂移）。要全文搜索或打印，直接搜 `docs/manual/` 这个目录。

## 这份手册怎么维护

- **改 Markdown，别改 HTML**：`docs/manual/*.md` 是唯一真源，`docs/manual/html/` 是**生成物**
  （`python3 tools/build_manual.py` 重新生成；`check.sh` 里有 `--check` 闸门，漂了就红）。
- HTML 站点是**自包含**的（内联 CSS/JS、零外部依赖、离线可读、可打印）：直接开 `html/index.html`。
- 侧栏搜索按 `/` 聚焦；代码块对 extC 做了轻量高亮。

## 文体约定（正式书面语）

- **不用第一人称「我 / 咱」**，也不用第二人称「你 / 您」：省主语，或写「使用者」「读者」。
- **不用口语与语气词**（这玩意儿 / 说白了 / 其实 / 吧 / 呢 / 嘛）。
- **陈述代替反问**：把「为什么这么定？」写成「这样定的原因是……」。
- 术语与代码用行内代码体（`` `ref` `` `` `impl` ``）；否定结论直接陈述，不用感叹号。
- 检查：`python3 tools/check_tone.py`（报告）/ `--gate`（出现即失败，`check.sh` 调用）。
  基线 `tools/tone-baseline.txt` **已归零**（2026-09-26 全量改写完成）⇒ 现在是硬规则：
  新增任何一处非正式写法都会让 `check.sh` 变红。引号内的**引文**（如引用作者原话）不计。
