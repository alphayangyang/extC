// 与 writes.c 同形状的写密集负载：预分配 + 大量改链，极低分配率。
package main
import ("fmt";"os";"strconv";"time")
type N struct { key, val int64; next, prev *N }
const CHAINS = 64
func main() {
	POOL, _ := strconv.Atoi(os.Args[1])
	ITER, _ := strconv.Atoi(os.Args[2])
	AE, _ := strconv.Atoi(os.Args[3])
	var head, tail [CHAINS]*N
	for i := 0; i < POOL; i++ {
		n := &N{key: int64(i)}
		c := i % CHAINS
		if head[c] != nil { n.next = head[c]; head[c].prev = n }
		head[c] = n
		if tail[c] == nil { tail[c] = n }
	}
	var sink int64
	t0 := time.Now()
	for i := 0; i < ITER; i++ {
		c := i % CHAINS
		n := head[c]
		if n == nil || n.next == nil { continue }      // 链内轮转：头节点挪到尾部
		nx, tl := n.next, tail[c]
		nx.prev = nil
		head[c] = nx
		tl.next = n
		n.prev = tl
		n.next = nil
		tail[c] = n
		sink += n.key
		if AE != 0 && i%AE == 0 { m := &N{key: int64(-i)}; m.next = head[c]; head[c] = m }
	}
	dt := time.Since(t0)
	fmt.Printf("writes-go 耗时=%.1fms 每轮=%.2fns sink=%d\n", float64(dt.Microseconds())/1000.0, float64(dt.Nanoseconds())/float64(ITER), sink)
}
