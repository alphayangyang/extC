// bench/gc/v2 形状 G2 sizeratio —— 4 个尺寸类（32/64/96/128 字节）对象池。
//
// 与 src/sizeratio.extc **同算法 / 同 PRNG / 同 LCG 调用顺序**，stdout 必须逐字节相同。
// 构建：go build -o b/sizeratio.go.bin src/sizeratio.go
package main

import (
	"fmt"
	"os"
)

const mask int64 = 4611686018427387903 // 2^62-1

const slots = 1024

// 四个尺寸类：8 + 8*{3,7,11,15} = 32 / 64 / 96 / 128 字节（无填充）。
type c0 struct {
	k int64
	p [3]int64
}

type c1 struct {
	k int64
	p [7]int64
}

type c2 struct {
	k int64
	p [11]int64
}

type c3 struct {
	k int64
	p [15]int64
}

// 每类一张 1024 槽定长表（普通指针数组，不用任何库容器）。
type tbl struct {
	a0 [slots]*c0
	a1 [slots]*c1
	a2 [slots]*c2
	a3 [slots]*c3
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
	n := arg(1, 2000000)
	gstate = arg(2, 20261013) & 2147483647

	t := &tbl{}
	var sum, kept, livebytes int64

	for i := int64(0); i < n; i++ {
		cls := pick(4)
		if cls == 0 {
			o := &c0{k: i}
			sum = (sum + o.k) & mask
			if i%8 == 0 {
				b := (i / 8) % slots
				if t.a0[b] != nil {
					livebytes -= 32
				} else {
					kept++
				}
				t.a0[b] = o
				livebytes += 32
			}
		} else if cls == 1 {
			o := &c1{k: i}
			sum = (sum + o.k) & mask
			if i%8 == 0 {
				b := (i / 8) % slots
				if t.a1[b] != nil {
					livebytes -= 64
				} else {
					kept++
				}
				t.a1[b] = o
				livebytes += 64
			}
		} else if cls == 2 {
			o := &c2{k: i}
			sum = (sum + o.k) & mask
			if i%8 == 0 {
				b := (i / 8) % slots
				if t.a2[b] != nil {
					livebytes -= 96
				} else {
					kept++
				}
				t.a2[b] = o
				livebytes += 96
			}
		} else {
			o := &c3{k: i}
			sum = (sum + o.k) & mask
			if i%8 == 0 {
				b := (i / 8) % slots
				if t.a3[b] != nil {
					livebytes -= 128
				} else {
					kept++
				}
				t.a3[b] = o
				livebytes += 128
			}
		}
	}

	fmt.Printf("ops=%d kept=%d livebytes=%d sum=%d\n", n, kept, livebytes, sum)
	fmt.Printf("classes=4 sizes=32..128 slots=1024\n")
}
