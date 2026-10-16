// bench/gc/v2 形状 G1 promote —— 分代假设（Go 自然写法：*node 链 + 定长指针表）。
//
// 与 src/promote.extc **同算法 / 同 PRNG / 同 LCG 调用顺序**，stdout 必须逐字节相同。
// 构建：go build -o b/promote.go.bin src/promote.go
package main

import (
	"fmt"
	"os"
)

const mask int64 = 4611686018427387903 // 2^62-1

const (
	tableSize = 65536
	shortN    = 9
	every     = 10
	walkCap   = 8
)

type node struct {
	k    int64
	v    int64
	next *node
}

type table struct {
	b [tableSize]*node
}

var gstate int64 = 1

func nxt() int64 {
	gstate = (gstate*1103515245 + 12345) & 2147483647
	return gstate
}

// parseI64：逐字符取十进制前缀，遇非数字即停（与 extC parseI64 同规矩）。
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

// arg：os.Args[i]（含 argv[0]）存在就解析，否则用缺省值。
func arg(i int, def int64) int64 {
	if len(os.Args) > i {
		return parseI64(os.Args[i])
	}
	return def
}

func main() {
	n := arg(1, 500000)
	gstate = arg(2, 20261013) & 2147483647

	t := &table{}
	var sum, walksum, live, tailv int64

	for i := int64(0); i < n; i++ {
		var head *node
		for j := int64(0); j < shortN; j++ {
			s := &node{k: i*shortN + j, v: nxt() % 1000}
			s.next = head
			head = s
			sum = (sum + s.v) & mask
		}
		tailv = head.v // 尾端读回：短命链确实存在，分配不可被消（SPEC §0）
		if i%every == 0 {
			p := &node{k: i, v: nxt() % 1000}
			b := i % tableSize
			p.next = t.b[b]
			t.b[b] = p
			live++
		}
		b2 := (i * 40503) % tableSize
		c := t.b[b2]
		for cnt := int64(0); c != nil && cnt < walkCap; cnt++ {
			walksum = (walksum + c.v) & mask
			c = c.next
		}
	}

	fmt.Printf("rounds=%d promoted=%d walksum=%d sum=%d tailv=%d\n", n, live, walksum, sum, tailv)
	fmt.Printf("table=65536 short=9 every=10 walkcap=8\n")
}
