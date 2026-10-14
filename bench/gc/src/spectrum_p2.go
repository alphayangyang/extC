package main
import ("fmt";"os";"strconv")
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
	for i := int64(0); i < n; i++ { push(x, nxt()%1000) }
	var sum, idx int64
	for b := x.head; b != nil; {
		for j := int64(0); j < 256 && idx < x.n; j++ { sum = (sum + b.v[j]*(idx%7+1)) & 4611686018427387903; idx++ }
		b = b.next
	}
	fmt.Printf("n=%d sum=%d\n", x.n, sum)
	fmt.Printf("blocks=%d per=%d\n", (x.n+255)/256, 256)
}
