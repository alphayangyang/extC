// bench/gc/v2 形状 G3 chase —— 大活集指针追逐 + churn。
//
// 与 src/chase.extc **同算法 / 同 PRNG / 同 LCG 调用顺序**，stdout 必须逐字节相同。
// 构建：go build -o b/chase.go.bin src/chase.go
package main

import (
	"fmt"
	"os"
)

const mask int64 = 4611686018427387903 // 2^62-1

const (
	capNodes   = 1048576
	outDeg     = 4
	churnEvery = 256
	churnN     = 4
)

type node struct {
	k int64
	v int64
	o [outDeg]*node
}

type graph struct {
	n [capNodes]*node
}

var gstate int64 = 1

func nxt() int64 {
	gstate = (gstate*1103515245 + 12345) & 2147483647
	return gstate
}

func pick(k int64) int64 {
	return (nxt() / 1024) % k
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
	m := arg(1, 1048576)
	gstate = arg(2, 20261013) & 2147483647
	w := arg(3, 4000000)

	g := &graph{}

	// 第一趟：建 M 个节点（活集）
	for i := int64(0); i < m; i++ {
		x := &node{k: i, v: nxt() % 1000}
		g.n[i] = x
	}
	// 第二趟：每个节点 4 条出边，从图里取目标 ⇒ 4M 条边的活图
	for i := int64(0); i < m; i++ {
		src := g.n[i]
		if src != nil {
			for j := int64(0); j < outDeg; j++ {
				t := nxt() % m
				src.o[j] = g.n[t]
			}
		}
	}

	cur := g.n[0]
	var sum, churn, tailv int64
	for step := int64(0); step < w; step++ {
		if cur != nil {
			sum = (sum + cur.v) & mask
		}
		r := pick(outDeg)
		if cur != nil {
			cur = cur.o[r]
		}
		if step%churnEvery == 0 {
			var h *node
			for j := int64(0); j < churnN; j++ {
				s := &node{k: step, v: nxt() % 1000}
				s.o[0] = h
				h = s
			}
			tailv = h.v // 尾端读回：churn 节点真的分配了（SPEC §0）
			churn += churnN
		}
	}

	fmt.Printf("nodes=%d edges=%d walk=%d churn=%d sum=%d tailv=%d\n", m, m*outDeg, w, churn, sum, tailv)
	fmt.Printf("outdeg=4 cap=1048576 churn_every=256 churn=4\n")
}
