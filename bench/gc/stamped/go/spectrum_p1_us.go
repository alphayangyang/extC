package main
import ("bufio";"fmt";"os";"runtime";"strconv";"time")

// ---- 应用侧单位时间戳（USER-STALL）：只写 stderr，stdout 不变 ----
var usSt []int64
var usBase time.Time

func usInit(cap int64) { usSt = make([]int64, 0, cap); usBase = time.Now() }
func us()             { usSt = append(usSt, time.Since(usBase).Nanoseconds()) }
func usDump() {
	w := bufio.NewWriterSize(os.Stderr, 1<<20)
	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	fmt.Fprintf(w, "USMETA n=%d gc=%d\n", len(usSt), m.NumGC)
	for _, v := range usSt {
		fmt.Fprintf(w, "US %d\n", v)
	}
	w.Flush()
}
var gstate int64 = 1
func nxt() int64 { gstate = (gstate*1103515245 + 12345) & 2147483647; return gstate }
type buf struct{ p [64]int64 }
func main() {
	n := int64(200000)
	if len(os.Args) > 1 { if v, e := strconv.ParseInt(os.Args[1], 10, 64); e == nil { n = v } }
	sd := int64(20261013)
	if len(os.Args) > 2 { if v, e := strconv.ParseInt(os.Args[2], 10, 64); e == nil { sd = v } }
	gstate = sd & 2147483647
	var sum int64
	usInit(n + 2)
	for i := int64(0); i < n; i++ {
		us()
		b := &buf{}
		for j := int64(0); j < 64; j++ { b.p[j] = nxt() % 1000; sum = (sum + b.p[j]*(j+1)) & 4611686018427387903 }
	}
	us()
	usDump()
	fmt.Printf("n=%d sum=%d\n", n, sum)
	fmt.Printf("allocs=%d per=%d\n", n, 64)
}
