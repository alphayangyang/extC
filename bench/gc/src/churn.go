// 形状 1：churn（Go）—— 与 churn.extc 逐字同语义。
package main

import "fmt"

type node struct {
v    int64
next *node
}

func main() {
const rounds = 32000000
var acc int64
for i := int64(0); i < rounds; i++ {
var a, b *node
p := &node{v: i}
if i&1 == 0 {
if a != nil {
acc += a.v
}
a = p
} else {
if b != nil {
acc += b.v
}
b = p
}
if a != nil {
acc += a.v
}
if b != nil {
acc += b.v
}
}
fmt.Println(acc)
}
