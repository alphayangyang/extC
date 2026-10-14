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
type node struct{ v int64; next *node }
func main() {
	rounds := int64(5000)
	if len(os.Args) > 1 { if v, e := strconv.ParseInt(os.Args[1], 10, 64); e == nil { rounds = v } }
	sd := int64(20261013)
	if len(os.Args) > 2 { if v, e := strconv.ParseInt(os.Args[2], 10, 64); e == nil { sd = v } }
	gstate = sd & 2147483647
	var sum, total int64
	usInit(rounds + 2)
	for r := int64(0); r < rounds; r++ {
		us()
		var head *node
		k := 40 + nxt()%40
		for i := int64(0); i < k; i++ { head = &node{v: nxt() % 1000, next: head} }
		for cur := head; cur != nil; cur = cur.next { sum = (sum + cur.v) & 4611686018427387903 }
		total += k
	}
	us()
	usDump()
	fmt.Printf("rounds=%d nodes=%d sum=%d\n", rounds, total, sum)
	fmt.Printf("drops=%d per_round_min=%d\n", rounds, 40)
}
