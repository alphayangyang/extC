# 标准库现状

## `std::fs`：打开文件的三个名字

名字里必须能看出**读还是写、原内容还在不在**：

| 做什么 | 名字 | 返回 | 已有内容 |
|---|---|---|---|
| 读 | `fs::openRead(p)` | `fs::ifstream` | 不动 |
| 写（**截断**） | `fs::openWrite(p)` | `fs::ofstream` | 清掉 |
| 写（**追加**） | `fs::openAppend(p)` | `fs::ofstream` | 保留，写在后面 |
| 读全部 | `readAll(f, dest)` | `result<i64, ioError>` | 调用者给地方 |

**两个类型分开**：`fs::ifstream`（读型）· `fs::ofstream`（写型）。
读型上没有 `put`、写型上没有 `readSome` ⇒ **误用编不过**。

用户层代码里**不该出现** `O_*` 平台常量（它们只在 `std::sys::io` 定义）。
