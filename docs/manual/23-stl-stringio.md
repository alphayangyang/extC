# 标准库参考：`stringio`

`stl::stringio` 给 `string` 挂**流式 I/O**，方法本身定义在 `impl string { ... }` 块里 —— 这是
`impl` 的用途之一：类型不必知道文件描述符与缓冲读取器的存在，I/O 层在自己这一侧挂上行为。

用法：`use std::io` + `use stl::string` + `use stl::stringio`（**只有引入本模块，这些方法才存在**）。

## 方法

签名与语义见 [`string` 参考的「流式 I/O」一节](22-stl-string.md)（同一批方法的完整说明只写一处）。

要点：

- 行语义与 `>>` 一致：**去掉行尾换行**，CRLF 的 `\r` 一并处理，孤立 `\r` 与文件末尾的 `\r` 保留为内容；
- 读一行、读全部都会按需增长目标串，调用者不必预估容量；
- 写入不产生中间拷贝：`w.write` 本就把字节拷进 writer 的缓冲；
- 挂载在 `io::istream` 与 `fs::ifstream` 上的 `>>` 重载同样由本模块提供，因此
  `io::cin >> s` 与 `f >> s`（`s` 为 `string`）也要求引入本模块。
