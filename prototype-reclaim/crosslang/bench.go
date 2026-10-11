// 同一份功能：有界符号表（64 桶 × 最多 8 条，满则淘汰尾节点）+ 1024 槽缓存 + 20 万项索引 + 查询。
package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"time"
)

type Node struct {
	key, val int64
	next     *Node
}
type Idx struct {
	next   *Idx
	target *Node
}

const (
	BUCKETS    = 64
	BUCKET_MAX = 8
	CACHE      = 1024
	INDEXMAX   = 200000
)

func main() {
	N, _ := strconv.Atoi(os.Args[1])
	head := make([]*Node, BUCKETS)
	cache := make([]*Node, CACHE)
	var ihead *Idx
	var sink int64
	t0 := time.Now()
	for i := 0; i < N; i++ {
		b := i % BUCKETS
		t1 := &Node{}; t2 := &Node{}; t3 := &Node{}
		t1.next = t2; t2.next = t3; t3.val = int64(i)
		for q := t1; q != nil; q = q.next { sink += q.val }
		nd := &Node{key: int64(i), val: int64(i * 3)}
		nd.next = head[b]; head[b] = nd
		cache[i%CACHE] = nd
		if i < INDEXMAX { ihead = &Idx{next: ihead, target: nd} }
		for q := head[b]; q != nil; q = q.next { if q.key == int64(i-7) { q.val++; break } }
		var prev *Node; cur := head[b]; cnt := 0
		for cur != nil && cur.next != nil { prev = cur; cur = cur.next; cnt++; if cnt > BUCKET_MAX { break } }
		if cur != nil && cnt >= BUCKET_MAX { if prev != nil { prev.next = nil } else { head[b] = nil } }
	}
	dt := time.Since(t0)
	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	var hwm int64
	if b, err := os.ReadFile("/proc/self/status"); err == nil {
		s := string(b); _ = s
	}
	_ = hwm
	fmt.Printf("go      耗时=%.1fms HeapInuse=%dKiB TotalAlloc=%dMiB GC次数=%d 最大GC停顿ns=%d\n",
		float64(dt.Microseconds())/1000.0, ms.HeapInuse/1024, ms.TotalAlloc/(1<<20), ms.NumGC, ms.PauseNs[(ms.NumGC+255)%256])
	_ = sink; _ = ihead; _ = cache
}
