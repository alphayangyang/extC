# 待做（唯一清单）

**生命周期**：条目**做完 → 搬进 [`usage/`](usage/) 或舍弃**；**砍掉 → 直接删除**（不留"已取消"）。

> 每一条都要能被源码位置或一条能跑的判据支撑。写不出判据的，不是待做，是想法。

## 一、已排定顺序（做完一件再开下一件）

- [ ] **`view<T>` 改名**（原 `slice<T>`）：纯机械、单独一个 commit，不和语义改动混在一起。
- [ ] **S1 checked 原语** + **S1b trap 统计**（改到编译器侧）。
      S1 是纯 stdlib 新增（`slice::at` / `asI64` / `divChecked` / `shiftChecked` 之类），零语言改动；
      S1b 把"太容易 trap"变成数字，那个数字是 S4（隐式传播）设计的输入。
- [ ] **A′ 的红判据**：`mut` 权限挂根要落地，**前置条件是判据**——
      先建"把只读根洗白成可写视图"的攻击语料（字段 / 局部拷贝 / 结构体字面量 / deref 拷贝 /
      函数返回 / 容器 / 协程），复用 `tools/escape-corpus/` + 逃逸闸门。**没有红判据不实现 A′。**
- [ ] **heap 的 `free` 那一半**：`prototype-heap/host.c` 里有 size-class 空闲链的逐对象回收，
      `stdlib/std/heap.extc` 的 `plate` 只有 bump。把它搬进 `std::heap`（或做成第三个容器基座）。
- [ ] **账本收缩**：删掉活文档里残留的状态句 + `tools/check_status.py`（禁止裸状态词的棘轮）。
- [ ] **push 未同步的 commit**（远端落后）。

## 二、语言设计：还没定

- [ ] **大对象的值 / 句柄语义**（**搁置中**，作者再想）：
      `string` 要不要改成不可变值语义 + 显式 `StringBuilder`；
      可变容器（`varArray` / `hashMap`）要不要"共享必须显式"（`.clone()` / `.share()`）。
      —— 这是当前**唯一**没定的设计母题。

## 三、缺陷 / 已知洞

- [ ] **赋值目标里的指针表达式会被求值两次**：`(*f()).field = v` 调用 `f` 两次（两侧编译器都一样）。
      修法：赋值目标路径对"非 place 的指针表达式"用**指针临时量**（单次求值）。
- [ ] **`?ref` 的 `!` 不能作赋值目标**：`f()!.field = v` 被拒。
      它与上一条是同一个修法——修好后可以重新开放（且是单次求值）。
- [ ] **逃逸闸门基线上还剩 1 条**：`yield ref <帧内局部>` 被前端接受、生成物是畸形 C。
      见 `tools/gate-escape-known-bad.txt`。
- [ ] **AST 冻结 R1 施工单 67 条**：parser 之外的代码在写 AST。见 `tools/ast-freeze-known-bad.txt`。
- [ ] **产物指纹基线已过期**：`tools/codegen-baseline-2026-10-05-w4.sha256` 已与 HEAD 差 ~410/414，
      **不能当"我改了没有"的预言机**。要么重取，要么明确放弃它。

## 四、语言功能（已定案、未实现）

- [ ] **`f"…"` v2 格式说明符**（`{:>8}` `{:04}` `{:.2f}`）。落点：保留名方法标记 → 检查器按类型写
      plan 侧表 → codegen 发 `padText` / `padInt` / `padF`（不新增 AST 节点）。
- [ ] **`if let` 部分匹配**。设计：复用单臂 `ST_MATCH` + `partial` 标志；`else` 需要单独机制。
- [ ] **`owned`**：`extern!` 返回"调用者拥有的内存"。源码里明说 `not implemented`。
- [ ] **`@main` 注解**（不再硬编码 `main`）。
- [ ] **线程**：保守的 fork-join + 归约，"引用不过线程"。
- [ ] **字面量切片的深度放宽**（静态存储的字面量本来就是深度 0，现在偏保守）。
- [ ] **失败处理 S2 / S3 / S4**：`if let` → `?` 允许错误转换 → 隐式传播
      （载体是纯值 `err{code, file, line}`；mayFail 进 `fn` 类型但由编译器推断；`?` 目前在 4 个语句位置，
      任意表达式位置要能查通道）。
- [ ] **模块**：特权模块名单未封闭；类型名跨模块"必须限定"没挡严。
- [ ] **协程**：请求只能表达"等可读"，没有"等可写"；协议层（覆盖式 tag）仍是研究材料。
- [ ] **域 `ext`**：域内写 `ext` 仍是编译错误。
- [ ] **范围类型**（让索引可证明 ⇒ 零检查）尚未落地。

## 五、标准库

- [ ] 补齐 `split` / `join` / `trim` / `replace` / `enumerate` / `zip`（零语言风险）。
- [ ] `varArray::insertAt` / `eraseAt`；`vector::insertAt` / `removeAt`；`string::pop`。
- [ ] 浮点格式化；`readAll`；格式化输入。
- [ ] HTTP 客户端：chunked / header 折叠 / TLS / 代理 / cookie。

## 六、编译器内部（与作者体验无关，可整体搁置）

- [ ] 多文件输出 / 运行时独立 TU（收益未测）。
- [ ] 协程协议换枚举（需先修 `coroutine<A,B>` 的 A≠B 槽精度）。
- [ ] `dropUnusedLocals`（活函数内部）。
- [ ] 调度器"一个 fd 一个等待者"。

## 七、文档工程（本轮）

- [ ] `docs/usage/` 全部页面（从代码与实测写）。
- [ ] 归档 `docs/manual/`，把手册闸门（`check_manual.py` / `build_manual.py` / `check_tone.py`）
      重指到新文档。
- [ ] 重写 `COMMENT-STYLE.md` 为 Doxygen 规范。
- [ ] "禁止历史词"棘轮闸门（`定案 \d+` / `PLAN #\d+` / 以前 / 实测史 / 回退史）。
- [ ] `tools/check_doc_examples.py`：注释里的 `@code` 例子要有闸门。
- [ ] 按模块清注释（`src/` 按子系统、`stdlib/` 按包、`tools/` 的 docstring）。
