# `tests/pool-soundness/` —— 池档健全性的反例库

配套文档：[`docs/topics/POOL-SOUNDNESS.md`](../../docs/topics/POOL-SOUNDNESS.md)（定理、三条不变量、判定）。

**oracle（沿用 `tests/arena-soundness/` 的规矩）**：`build/extc` **接受** + `gcc -fsanitize=address`
编过 + 运行时报 `heap-use-after-free` ⇒ 这一条是**洞还开着**。修好之后，这一条应当变成
**编译错误**（或运行期 trap）——判据因此是**双向**的。

| 文件 | 形状 | 现在 |
|---|---|---|
| `E1_string_sub_growth.extc` | `string.sub` 零拷贝视图 + 增长 ⇒ 旧池块被 `give`（`free`） | **接受 + ASan UAF**（洞开着） |
| `C1_vector_toslice_copy.extc` | 同一形状走 `vector.toSlice()`（**拷贝**）+ 20 万次 `push` | 对照：值仍然正确（拷贝 + `resize` 恰好原地） |
| `C2_stale_copy_release.extc` | `var b = a`（`@sharesStorage` 共享板块）→ `a.release()` → 槽位被别人拿走 → **`b.release()`** | 对照：**守卫承重** —— 有 `pStale()` 时 `b.release()` 是空操作（`c[0]=9 len=1`）；把守卫拿掉是 **ASan UAF**（`b` 把 `c` 的池 drop 了），而且**不开 ASan 时打印一模一样** |

机制（详见文档 §4）：

- `extc_pool_give` = **`free`**、`extc_pool_resize` = **`realloc`**，两者都**不动 `generation`** ⇒ **INV-A** 破；
- 全文 `generation++` **只有 1 处**（`extc_pool_new_at`）；`extc_pool_reset` 不换代 ⇒ **INV-G** 在元素粒度上破；
- 视图可以指向池块（`string::sub` 零拷贝）⇒ **INV-V** 破（`vector::toSlice` 有意拷贝，注释 66–69）。

    ./run.sh          # 跑两条并给出判定
