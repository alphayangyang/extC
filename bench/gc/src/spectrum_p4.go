// P4 = human_a 的 Go 版（长命只读符号表 + 每单元短命 AST）。输出须与 spectrum_p4.extc(human_a.extc) 逐字节一致。
package main
import ("fmt";"os";"strconv")
type sym struct{ name, val int64; next *sym }
type unitNode struct{ id, kind, payload int64; sib *unitNode }
type table struct{ buckets [2048]*sym; count int64 }
var gstate int64 = 1
func nxt() int64 { gstate = (gstate*1103515245 + 12345) & 2147483647; return gstate }
func bucketOf(n int64) int64 { return (n * 2654435761) & 2047 }
func tableInsert(t *table, name, val int64) {
	h := bucketOf(name)
	s := &sym{name: name, val: val, next: t.buckets[h]}
	t.buckets[h] = s
	t.count++
}
func tableFind(t *table, name int64) int64 {
	h := bucketOf(name)
	var probes int64
	for cur := t.buckets[h]; cur != nil; cur = cur.next {
		probes++
		if cur.name == name { return (cur.val << 20) | probes }
	}
	return -1
}
func main() {
	units := int64(2000)
	if len(os.Args) > 1 { if v, e := strconv.ParseInt(os.Args[1], 10, 64); e == nil { units = v } }
	sd := int64(20261013)
	if len(os.Args) > 2 { if v, e := strconv.ParseInt(os.Args[2], 10, 64); e == nil { sd = v } }
	if units < 8 { units = 8 }
	gstate = sd & 2147483647
	t := &table{}
	var sum, asts, inserts, hits, misses, probes, nmake int64
	for pre := int64(0); pre < 4096; pre++ { tableInsert(t, pre, pre*7+11); inserts++ }
	for u := int64(0); u < units; u++ {
		var ast *unitNode
		m := 640 + nxt()%256
		for j := int64(0); j < m; j++ {
			p := &unitNode{id: nmake, kind: nxt() % 7, payload: nxt() % 1000, sib: ast}
			nmake++
			ast = p
		}
		asts += m
		for cur := ast; cur != nil; cur = cur.sib { sum = (sum + cur.id*3 + cur.payload + cur.kind) & 4611686018427387903 }
		for ins := int64(0); ins < 8; ins++ {
			name := 4096 + nxt()%(units*4+64)
			if tableFind(t, name) < 0 { tableInsert(t, name, name*13+u); inserts++ }
		}
		for q := int64(0); q < 96; q++ {
			name2 := nxt() % (4096 + u*3 + 8)
			res := tableFind(t, name2)
			if res < 0 { misses++ } else { hits++; probes += res & 1048575; sum = (sum + (res >> 20)) & 4611686018427387903 }
		}
	}
	fmt.Printf("units=%d symbols=%d asts=%d sum=%d\n", units, t.count, asts, sum)
	fmt.Printf("inserts=%d hits=%d misses=%d probes=%d\n", inserts, hits, misses, probes)
}
