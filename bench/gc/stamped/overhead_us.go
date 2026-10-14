// bench/gc/stamped/overhead_us.go —— 量"打一次应用侧时间戳"本身要多少 ns。
// 与 stamped/go/*_us.go 里的 us() 逐字同形（time.Since(base) 的单调部分）。
package main

import (
	"fmt"
	"time"
)

var usSt []int64
var usBase time.Time

func usInit(c int64) { usSt = make([]int64, 0, c); usBase = time.Now() }
func us()             { usSt = append(usSt, time.Since(usBase).Nanoseconds()) }

func main() {
	const n = 1000000
	usInit(n + 2)
	t0 := time.Now()
	for i := 0; i < n; i++ {
		us()
	}
	t1 := time.Now()
	fmt.Printf("USOH n=%d total_ns=%d stamps=%d\n", n, t1.Sub(t0).Nanoseconds(), len(usSt))
}
