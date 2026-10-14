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
type blk struct{ v [256]int64; next *blk }
type vec struct{ head, tail *blk; n int64 }
func push(x *vec, val int64) {
	if x.tail == nil || x.n%256 == 0 {
		b := &blk{}
		if x.tail == nil { x.head = b } else { x.tail.next = b }
		x.tail = b
	}
	x.tail.v[x.n%256] = val
	x.n++
}
func main() {
	n := int64(200000)
	if len(os.Args) > 1 { if v, e := strconv.ParseInt(os.Args[1], 10, 64); e == nil { n = v } }
	sd := int64(20261013)
	if len(os.Args) > 2 { if v, e := strconv.ParseInt(os.Args[2], 10, 64); e == nil { sd = v } }
	gstate = sd & 2147483647
	x := &vec{}
	usInit(n/256 + 2)
	for i := int64(0); i < n; i++ {
		if i%256 == 0 {
			us()
		}
		push(x, nxt()%1000)
	}
	us()
	usDump()
	var sum, idx int64
	for b := x.head; b != nil; {
		for j := int64(0); j < 256 && idx < x.n; j++ { sum = (sum + b.v[j]*(idx%7+1)) & 4611686018427387903; idx++ }
		b = b.next
	}
	fmt.Printf("n=%d sum=%d\n", x.n, sum)
	fmt.Printf("blocks=%d per=%d\n", (x.n+255)/256, 256)
}
