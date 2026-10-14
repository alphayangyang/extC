// bench/app/route.go —— **场景 B：路由表**（route.extc 的 Go 对照）
//
// 参考结构是有序 map；Go 里用最自然的等价物：map 做点操作 + 排序切片做二分。
// 用法：route <ops>          （默认 2000000）
// 输出：b.ins=<新插入条数> b.lkp=<前驱查找校验和> b.scan=<区间计数和> b.del=<删除条数> b.len=<最终条数>
package main

import (
	"fmt"
	"os"
	"sort"
	"time"
)


// ================ 应用侧逐单位时间戳（USER-STALL / bench/app） ================
// 只写**戳文件**（路径 = os.Args 最后一项）；stdout 一个字节都不动。
// 40 字节头 + (nstamps-1) 个 unsigned-LEB128 增量（小端）。
// 戳数 = 单位数 + 1：第一次 us() 记 t0，其后每次记一个增量，循环后补最后一次。
// ============================================================================
var (
	usBuf   []byte
	usCap   int64
	usN     int64
	usT0    int64
	usPrev  int64
	usNeg   int64
	usOvf   int64
	usStart bool
	usBase  time.Time
)

func usReserve(units int64) {
	usCap = units*5 + 64
	usBuf = make([]byte, 0, usCap)
	usBase = time.Now()
}

func us() {
	t := time.Since(usBase).Nanoseconds()
	if !usStart {
		usStart = true
		usT0 = t
		usPrev = t
		usN = 1
		return
	}
	d := t - usPrev
	if d < 0 {
		usNeg++
		d = 0
	}
	usPrev = t
	usN++
	v := uint64(d)
	for v >= 128 {
		if int64(len(usBuf)) < usCap {
			usBuf = append(usBuf, byte(v&127|128))
		} else {
			usOvf++
		}
		v >>= 7
	}
	if int64(len(usBuf)) < usCap {
		usBuf = append(usBuf, byte(v))
	} else {
		usOvf++
	}
}

func usDump(path string) {
	hdr := make([]byte, 40)
	copy(hdr, "USAPPST1")
	put := func(off int, val uint64) {
		for i := 0; i < 8; i++ {
			hdr[off+i] = byte(val >> (8 * uint(i)))
		}
	}
	put(8, uint64(usN))
	put(16, uint64(usT0))
	put(24, uint64(usNeg))
	put(32, uint64(usOvf))
	f, err := os.Create(path)
	if err != nil {
		fmt.Fprintln(os.Stderr, "USAPP: create stamp file failed:", err)
		return
	}
	f.Write(hdr)
	f.Write(usBuf)
	f.Close()
}
// toInt 与 extC 版 toInt 语义一致：空串或含非数字字符 => dflt。
func toInt(s string, dflt int64) int64 {
	if len(s) == 0 {
		return dflt
	}
	var v int64 = 0
	for i := 0; i < len(s); i++ {
		b := int64(s[i])
		if b < 48 || b > 57 {
			return dflt
		}
		v = v*10 + (b - 48)
	}
	return v
}

func main() {
	var ops int64 = 2000000
	if len(os.Args) > 1 {
		ops = toInt(os.Args[1], ops)
	}
	usPath := ""
	if len(os.Args) > 2 {
		usPath = os.Args[len(os.Args)-1]
	}
	usReserve(ops + ops + ops/4 + ops/2)

	m := make(map[int64]int32, ops)
	var ins, lkp, scan, del int64

	// ① 插入 / 更新：键是 /24 前缀（低 8 位清零），值与键有关以便校验。
	var u uint64 = 12345
	for i := int64(0); i < ops; i++ {
		u = u*6364136223846793005 + 1442695040888963407
		key := int64(u & 0xFFFFFF00)
		us()
		if _, exists := m[key]; !exists {
			ins++
		}
		m[key] = int32(key % 1024)
	}

	// 排序键切片：之后用二分做前驱 / 区间名次查询。
	keys := make([]int64, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(a, b int) bool { return keys[a] < keys[b] })

	// ② 前驱查找：lowerBound(q+1) - 1 即“最后一个 ≤ q 的前缀”的名次。
	u = 12345
	for i := int64(0); i < ops; i++ {
		u = u*6364136223846793005 + 1442695040888963407
		q := int64(u & 0xFFFFFFFF)
		us()
		idx := sort.Search(len(keys), func(j int) bool { return keys[j] > q }) - 1
		if idx < 0 {
			lkp--
		} else {
			lkp += int64(m[keys[idx]])
		}
	}

	// ③ 区间扫描：数 [lo, lo+65536) 里有多少条路由（两次名次查询之差）。
	u = 12345
	for i := int64(0); i < ops/4; i++ {
		us()
		u = u*6364136223846793005 + 1442695040888963407
		lo := int64(u & 0xFFFF0000)
		hi := lo + 65536
		a := sort.Search(len(keys), func(j int) bool { return keys[j] >= lo })
		b := sort.Search(len(keys), func(j int) bool { return keys[j] >= hi })
		scan += int64(b - a)
	}

	// ④ 删除一半。
	u = 12345
	for i := int64(0); i < ops/2; i++ {
		us()
		u = u*6364136223846793005 + 1442695040888963407
		key := int64(u & 0xFFFFFF00)
		if _, ok := m[key]; ok {
			delete(m, key)
			del++
		}
	}

	us()
	usDump(usPath)
	fmt.Printf("b.ins=%d b.lkp=%d b.scan=%d b.del=%d b.len=%d\n", ins, lkp%1000000007, scan, del, len(m))
}
