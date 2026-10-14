package main
import ("fmt";"os";"strconv")
type ent struct{ k, v int64; next *ent }
type idx struct{ buckets [1024]*ent; n, hit, ins int64 }
type win struct{ slots [256]*ent }
var gstate int64 = 1
func nxt() int64 { gstate = (gstate*1103515245 + 12345) & 2147483647; return gstate }
func idxPut(x *idx, k, v int64) {
	h := (k * 2654435761) & 1023
	x.buckets[h] = &ent{k: k, v: v, next: x.buckets[h]}
	x.n++; x.ins++
}
func idxGet(x *idx, k int64) int64 {
	h := (k * 2654435761) & 1023
	for c := x.buckets[h]; c != nil; c = c.next { if c.k == k { x.hit++; return c.v } }
	return -1
}
func main() {
	ops := int64(300000)
	if len(os.Args) > 1 { if v, e := strconv.ParseInt(os.Args[1], 10, 64); e == nil { ops = v } }
	sd := int64(20261013)
	if len(os.Args) > 2 { if v, e := strconv.ParseInt(os.Args[2], 10, 64); e == nil { sd = v } }
	gstate = sd & 2147483647
	x := &idx{}; w := &win{}
	var sum int64
	for i := int64(0); i < ops; i++ {
		k := nxt() % 4096
		w.slots[i%256] = &ent{k: k, v: i % 1000}
		if i%8 == 0 { idxPut(x, k, i%1000) }
		if g := idxGet(x, k); g >= 0 { sum = (sum + g) & 4611686018427387903 }
		var s int64
		for j := int64(0); j < 4; j++ { if t := w.slots[(i+j)%256]; t != nil { s += t.k } }
		sum = (sum + s) & 4611686018427387903
	}
	fmt.Printf("ops=%d idx=%d sum=%d\n", ops, x.n, sum)
	fmt.Printf("ins=%d hit=%d win=%d\n", x.ins, x.hit, 256)
}
