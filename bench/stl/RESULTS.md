# bench/stl —— STL 容器横评（extC 的容器 vs C++ STL）

> **自动生成**（`bash bench/stl/run.sh`），别手改。
> 口径：两边**同算法同参数**，跑同一个工作量，打印出来的校验和**逐位相同**才算数；
> 时间与 RSS 都是 `best of 3`（外设 `/usr/bin/time`，时间精度 0.01s）。

机器：Intel(R) Core(TM) Ultra 9 275HX · kernel 6.18.33.2-microsoft-standard-WSL2

## 读这张表前要知道的三件事

1. **我们的取元素是有界检查的**：`vector::get` / `string::at` 之类越界会带源位置 trap，
   而 C++ 那边用的是裸 `v[i]`（`unordered_map::find` 两边都是查表，可比）。这一项算 extC 的明账。
2. **RSS 现在看情况**：容器的存储自 2026-09-26 起住在**自己的池板块**上（POOLS.md §2.1），
   扩容时旧块当场还回去，所以 vector / map 这两格已经低于 C++。仍然偏大的两格（hashmap / set）
   不是漏掉：它们按基准给的容量提示**一次性要满** 2 的幂张桶表（掩码寻址要求），而 C++ 的
   `reserve` 不保证同样的装载因子；这一项属于「同算法同参数」以外的容量策略差，不是泄漏。
   长期存储之外只剩三处 `new`：临时草稿、返回值、定长缓冲（都写在 DEVLOG 周期 37）。
3. **WSL2 + 笔记本 CPU 有 3~5% 波动**：看量级与排序，不要读最后一位小数。

## 结果

| 容器 | extC（同算法同参数） | C++ STL | 比值 | extC RSS | C++ RSS | 校验和 |
|---|---|---|---|---|---|---|
| `vector` | TIME 0.03s | TIME 0.03s | 1.00x | 40620 KB | 42876 KB | `len=0 cap=10000000 sum=49999995000000 popped=49999995000000` |
| `hashmap` | TIME 0.15s | TIME 0.22s | 0.68x | 69036 KB | 43276 KB | `len=500000 sum=1075342615883296 removed=500000` |
| `map` | TIME 1.01s | TIME 1.78s | 0.57x | 41772 KB | 66716 KB | `len=500000 sum=1075343114875592 removed=500000` |
| `set` | TIME 0.12s | TIME 0.20s | 0.60x | 62892 KB | 43240 KB | `len=500000 hits=1000000 removed=500000` |
| `string` | TIME 0.02s | TIME 0.01s | 2.00x | 21484 KB | 21308 KB | `len=10000000 alen=8000000 found=0` |

对比物：`vector<i32>` 对 `std::vector` · `hashMapI64<i32>` 对 `std::unordered_map` ·
`map<i64,i32>`（B+ 树）对 `std::map`（红黑树）· `hashSetI64` 对 `std::unordered_set` ·
`string` 对 `std::string`。

结论：**有序表这一格是设计目的兑现的地方** —— B+ 树在 100 万次插入 + 100 万次查找 + 100 万次
有序遍历 + 50 万次删除上明显快过红黑树（cache miss 少一个数量级，遍历是叶子链线性扫描）；
哈希与集合打平；连续容器（vector / string）慢在「每次访问都过方法 + 有界检查」上。
