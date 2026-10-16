// bench/gc/v2 形状 G4 cycleratio —— 环垃圾占比 f（256 槽窗口）。
//
// 与 src/cycleratio.extc **同算法 / 同 PRNG / 同 LCG 调用顺序**，stdout 必须逐字节相同。
// 构建：go build -o b/cycleratio.go.bin src/cycleratio.go
package main

import (
	"fmt"
	"os"
)

const mask int64 = 4611686018427387903 // 2^62-1

const winSlots = 256

type node struct {
	k    int64
	v    int64
	next *node
}

// 窗口：每槽存（该组头节点, 该组节点数）
type win struct {
	s [winSlots]*node
	n [winSlots]int64
}

var gstate int64 = 1

func nxt() int64 {
	gstate = (gstate*1103515245 + 12345) & 2147483647
	return gstate
}

func pick(k int64) int64 {
	return (nxt() / 1024) % k
}

func mkNode(k, v int64) *node {
	return &node{k: k, v: v}
}

func parseI64(s string) int64 {
	var v int64
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c < '0' || c > '9' {
			break
		}
		v = v*10 + int64(c-'0')
	}
	return v
}

func arg(i int, def int64) int64 {
	if len(os.Args) > i {
		return parseI64(os.Args[i])
	}
	return def
}

func main() {
	g := arg(1, 400000)
	gstate = arg(2, 20261013) & 2147483647
	f := arg(3, 100)

	w := &win{}
	var sum, made, evicted, dropnodes, rings int64

	for i := int64(0); i < g; i++ {
		k := 3 + pick(4)
		isring := pick(100) < f

		var first, prev *node
		for j := int64(0); j < k; j++ {
			s := mkNode(i, nxt()%1000)
			if first == nil {
				first = s
			}
			if prev != nil {
				prev.next = s
			}
			prev = s
			sum = (sum + s.v) & mask
		}
		if isring {
			rings++
			if prev != nil {
				prev.next = first // 闭成环：整组自引用，无外部引用
			}
		}
		made += k

		b := i % winSlots
		if w.s[b] != nil {
			evicted++
			dropnodes += w.n[b]
		}
		w.s[b] = first
		w.n[b] = k
	}

	fmt.Printf("groups=%d made=%d evicted=%d dropnodes=%d sum=%d\n", g, made, evicted, dropnodes, sum)
	fmt.Printf("f=%d window=256 kmin=3 kmax=6 rings=%d\n", f, rings)
}
