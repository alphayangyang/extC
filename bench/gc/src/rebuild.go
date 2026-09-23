// GC 敏感形状 2：每轮重建（Go）。
package main

import "fmt"

type node struct {
v    int64
next *node
}

func main() {
const rounds, k = 20000, 1000
var acc int64
for r := 0; r < rounds; r++ {
var h *node
for i := int64(0); i < k; i++ {
h = &node{v: i, next: h}     // 上一轮的链在这一轮不再被引用 ⇒ 可回收
}
for p := h; p != nil; p = p.next {
acc += p.v
}
}
fmt.Println(acc)
}
