package main
import ("fmt";"os";"strconv")
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
	for i := int64(0); i < n; i++ {
		b := &buf{}
		for j := int64(0); j < 64; j++ { b.p[j] = nxt() % 1000; sum = (sum + b.p[j]*(j+1)) & 4611686018427387903 }
	}
	fmt.Printf("n=%d sum=%d\n", n, sum)
	fmt.Printf("allocs=%d per=%d\n", n, 64)
}
