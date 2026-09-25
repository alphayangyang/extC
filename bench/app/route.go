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
)

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

	m := make(map[int64]int32, ops)
	var ins, lkp, scan, del int64

	// ① 插入 / 更新：键是 /24 前缀（低 8 位清零），值与键有关以便校验。
	var u uint64 = 12345
	for i := int64(0); i < ops; i++ {
		u = u*6364136223846793005 + 1442695040888963407
		key := int64(u & 0xFFFFFF00)
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
		u = u*6364136223846793005 + 1442695040888963407
		key := int64(u & 0xFFFFFF00)
		if _, ok := m[key]; ok {
			delete(m, key)
			del++
		}
	}

	fmt.Printf("b.ins=%d b.lkp=%d b.scan=%d b.del=%d b.len=%d\n", ins, lkp%1000000007, scan, del, len(m))
}
