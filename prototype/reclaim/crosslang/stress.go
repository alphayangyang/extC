// 压力形状：双向链表(LRU) + 带父指针的树 + 缓存 + 索引 —— 天然形成环。
package main
import ("fmt";"os";"strconv";"time")
type Node struct { key, val int64; next, prev, parent, child *Node }
func main() {
	N, _ := strconv.Atoi(os.Args[1])
	var head, tail [64]*Node; var size [64]int
	var cache [1024]*Node; idx := make([]*Node, 0, 200001); nodes := make([]*Node, 0, 1<<20)
	var sink int64; t0 := time.Now()
	for i := 0; i < N; i++ {
		b := i % 64
		nd := &Node{key: int64(i), val: int64(i * 3)}
		if head[b] != nil { head[b].prev = nd }
		nd.next = head[b]; head[b] = nd
		if tail[b] == nil { tail[b] = nd }
		size[b]++
		cache[i%1024] = nd
		if i < 200000 { idx = append(idx, nd) }
		if i&15 == 0 { nodes = append(nodes, nd); if len(nodes) > 1 { par := nodes[len(nodes)-2]; par.child = nd; nd.parent = par } }
		for q := head[b]; q != nil; q = q.next { if q.key == int64(i-7) { q.val++; break } }
		if size[b] > 8 && tail[b] != nil { pr := tail[b].prev; if pr != nil { pr.next = nil; tail[b] = pr } else { head[b], tail[b] = nil, nil }; size[b]-- }
	}
	dt := time.Since(t0); _ = sink
	fmt.Printf("go-stress 耗时=%.1fms idx=%d\n", float64(dt.Microseconds())/1000.0, len(idx))
}
