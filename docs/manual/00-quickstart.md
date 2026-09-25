<!-- 本页原来是 docs/MANUAL.md 的「0. 三分钟上手」一节（原 §0.）；所有编辑都改这里，不要把 MANUAL.md 当第二份真源。 -->

**[索引](README.md)** · ← 上一页 · [0.5 内存安全：承诺 × 现状 →](01-safety.md)

---

# 0. 三分钟上手

```sh
make                                   # 产出 build/extc
./build/extc --run examples/hello.extc # 生成 C、编译、直接运行
```

第一个程序：

```extc
use std::io
// 这是注释
fn add(a: i64, b: i64) -> i64 {
    return a + b
}

fn main() -> i32 {
    let x: i64 = 3
    var y = 4                  // 局部变量可以省类型标注，从初始化式推导
    y = y + 1
    if x < y {
        io::cout << "sum = " << add(x, y) << "\n"   // 控制台唯一出口，见第 8 节
    }
    return 0
}
```

extC 编译到 C，再由系统的 C 编译器编成可执行文件。生成的 C 带 `#line`，所以 **gcc 的报错会指回 `.extc` 的行号**。

---

**[索引](README.md)** · ← 上一页 · [0.5 内存安全：承诺 × 现状 →](01-safety.md)
