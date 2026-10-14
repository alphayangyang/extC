// bench/gc 形状 7：digraph —— Go（*Node + 追踪 GC），档位由 GOGC 控制。
// 与 bench/gc/src/digraph.extc **逐字同算法/同 PRNG/同顺序**，输出必须逐字节一致。
//
//   GOGC 默认（100）：环状垃圾被追踪 GC 收回。
//   GOGC=off        ：从不回收 ⇒ 内存随累计分配线性涨（对照地板）。
//
// 构建：go build -o digraph_go digraph.go
package main

import (
	"fmt"
	"os"
	"runtime"
	"time"
)

const mask int64 = 4611686018427387903

type Edge struct {
	to   *Node
	next *Edge
	kind int64
}

type Node struct {
	id      int64
	gid     int64
	payload int64
	first   *Edge
	ord     *Node
	back    *Node // 命名回指（Go 是追踪 GC：这里只是普通指针）
	mark    int64
	cmark   int64
	snext   *Node
	gpin    bool
}

type Handle struct {
	target *Node
	newest *Node
	size   int64
	prev   *Handle
	next   *Handle
}

type Ctx struct {
	head     *Handle
	tail     *Handle
	n        int64
	hubs     [8]*Node
	hubCount int64
}

var gstate int64 = 1
var nextID int64 = 0
var gidCounter int64 = 0
var backedges int64 = 0
var nodesM int64 = 0
var groupsM int64 = 0
var droppedG int64 = 0
var droppedN int64 = 0
var rejected int64 = 0
var cyclesDropped int64 = 0
var acc int64 = 0
var xacc int64 = 0
var lastNodes int64 = 0
var lastEdges int64 = 0
var epoch int64 = 0
var cepoch int64 = 0

func nxt() int64 {
	gstate = (gstate*1103515245 + 12345) & 2147483647
	return gstate
}

func mixv(a, b int64) int64 {
	x := (a*2654435761 + b*40503 + 12345) & mask
	x = x ^ (x >> 12)
	return x & mask
}

func parseNum(s string, def int64) int64 {
	if len(s) == 0 {
		return def
	}
	var v int64 = 0
	for i := 0; i < len(s); i++ {
		ch := s[i]
		if ch < '0' || ch > '9' {
			return def
		}
		v = v*10 + int64(ch-'0')
	}
	return v
}

func handleAt(c *Ctx, idx int64) *Handle {
	h := c.head
	for i := int64(0); i < idx && h != nil; i++ {
		h = h.next
	}
	return h
}

func nodeInGroup(h *Handle, j int64) *Node {
	x := h.newest
	for i := int64(0); i < j && x != nil; i++ {
		x = x.ord
	}
	return x
}

func addEdge(s, t *Node, k int64) {
	e := &Edge{to: t, next: s.first, kind: k}
	s.first = e
}

// 组内“更老”（id 更小）的一个随机祖先；没有就退回 hub
func olderInGroup(s, hub *Node) *Node {
	var c1 int64 = 0
	for y := s.ord; y != nil; y = y.ord {
		c1++
	}
	if c1 == 0 {
		return hub
	}
	j := nxt() % c1
	z := s.ord
	for i2 := int64(0); z != nil; i2++ {
		if i2 == j {
			return z
		}
		z = z.ord
	}
	return hub
}

// 组内“更新”（id 更大）的一个随机后代；没有就退回 hub
func newerInGroup(s, newest, hub *Node) *Node {
	var c2 int64 = 0
	for y := newest; y != nil; y = y.ord {
		if y.id > s.id {
			c2++
		}
	}
	if c2 == 0 {
		return hub
	}
	j := nxt() % c2
	z := newest
	i2 := int64(0)
	for ; z != nil; z = z.ord {
		if z.id > s.id {
			if i2 == j {
				return z
			}
			i2++
		}
	}
	return hub
}

// 建组时的弦边目标：60% 组内更老 / 20% 组内更新 / 10% 组内 hub / 10% 组内长程随机
func pickChordTarget(s, hub, newest *Node, size int64) *Node {
	r := nxt() % 100
	if r < 60 {
		return olderInGroup(s, hub)
	}
	if r < 80 {
		return newerInGroup(s, newest, hub)
	}
	if r < 90 {
		return hub
	}
	j3 := nxt() % size
	z3 := newest
	for i4 := int64(0); z3 != nil; i4++ {
		if i4 == j3 {
			return z3
		}
		z3 = z3.ord
	}
	return hub
}

// 组内是否存在有向环（丢弃时现场验证；只用组内边，独立于“构造上必有环”）
func groupHasCycle(newest *Node, gid int64) bool {
	for u := newest; u != nil; u = u.ord {
		cepoch++
		ep := cepoch
		var top *Node = nil
		for e := u.first; e != nil; e = e.next {
			t := e.to
			if t.gid == gid {
				if t.id == u.id {
					return true
				}
				if t.cmark != ep {
					t.cmark = ep
					t.snext = top
					top = t
				}
			}
		}
		found := false
		for top != nil {
			x := top
			top = x.snext
			x.snext = nil
			if x.id == u.id {
				found = true
			}
			if !found {
				for e2 := x.first; e2 != nil; e2 = e2.next {
					t2 := e2.to
					if t2.gid == gid && t2.cmark != ep {
						t2.cmark = ep
						t2.snext = top
						top = t2
					}
				}
			}
		}
		if found {
			return true
		}
	}
	return false
}

func createGroup(c *Ctx, k int64, pin bool, isHub bool) {
	gid := gidCounter
	gidCounter++
	groupsM++

	hub := &Node{}
	hub.id = nextID
	hub.gid = gid
	hub.payload = nxt() % 1000
	hub.gpin = pin
	nextID++
	nodesM++
	if isHub {
		c.hubs[0] = hub
		c.hubCount = 1
	}

	prev := hub
	for cnt := int64(1); cnt < k; cnt++ {
		n := &Node{}
		n.id = nextID
		n.gid = gid
		n.payload = nxt() % 1000
		n.ord = prev
		n.back = prev
		n.gpin = pin
		nextID++
		nodesM++
		if isHub {
			c.hubs[cnt] = n
			c.hubCount = cnt + 1
		}
		addEdge(prev, n, 1)
		prev = n
	}

	addEdge(prev, hub, 1)
	backedges++

	for x := prev; x != nil; x = x.ord {
		d := nxt() % 3
		for j := int64(0); j < d; j++ {
			tgt := pickChordTarget(x, hub, prev, k)
			addEdge(x, tgt, 2)
		}
	}

	h := &Handle{target: hub, newest: prev, size: k, prev: c.tail}
	if c.tail != nil {
		c.tail.next = h
	} else {
		c.head = h
	}
	c.tail = h
	c.n++
}

// 丢一个组：(c) 步。优先摘无跨组入边（gpin==false）的组；顺手做现场环验证与计数。
func tryDrop(c *Ctx) {
	if c.n == 0 {
		return
	}
	base := nxt() % c.n
	for probe := int64(0); probe < 8; probe++ {
		idx := base + probe
		for idx >= c.n {
			idx -= c.n
		}
		h := handleAt(c, idx)
		cnt := int64(0)
		anypin := false
		for x := h.newest; x != nil; x = x.ord {
			cnt++
			if x.gpin {
				anypin = true
			}
		}
		if anypin {
			rejected++
			continue
		}
		droppedG++
		droppedN += cnt
		if groupHasCycle(h.newest, h.target.gid) {
			cyclesDropped++
		}
		hp := h.prev
		hn := h.next
		if hp != nil {
			hp.next = hn
		} else {
			c.head = hn
		}
		if hn != nil {
			hn.prev = hp
		} else {
			c.tail = hp
		}
		h.prev = nil
		h.next = nil
		c.n--
		return
	}
}

// (d) 步：从 handle 表出发对存活部分做一次遍历（显式栈，用 snext 串），累加校验和。
func traverse(c *Ctx, ep int64) {
	var top *Node = nil
	for h := c.head; h != nil; h = h.next {
		t := h.target
		if t.mark != ep {
			t.mark = ep
			t.snext = top
			top = t
		}
	}
	var rn int64 = 0
	var re int64 = 0
	var radd int64 = 0
	var rxor int64 = 0
	for top != nil {
		n := top
		top = n.snext
		n.snext = nil
		rn++
		radd = (radd + mixv(n.id, n.payload)) & mask
		for e := n.first; e != nil; e = e.next {
			re++
			rxor = rxor ^ mixv(n.id, e.to.id) ^ (e.kind * 4099)
			t2 := e.to
			if t2.mark != ep {
				t2.mark = ep
				t2.snext = top
				top = t2
			}
		}
	}
	acc = (acc + radd + rn*1000003 + re*31) & mask
	xacc = (xacc ^ (rxor ^ (rn*1000003 + re))) & mask
	lastNodes = rn
	lastEdges = re
}

func main() {
	wallStart := time.Now()
	N := int64(200000)
	sd := int64(20261013)
	if len(os.Args) > 1 {
		N = parseNum(os.Args[1], N)
	}
	if len(os.Args) > 2 {
		sd = parseNum(os.Args[2], sd)
	}
	if N < 256 {
		N = 256
	}
	gstate = sd & 2147483647

	c := &Ctx{}
	createGroup(c, 8, true, true)

	rounds := 16 + N/4096
	batch := N / rounds
	if batch < 24 {
		batch = 24
	}

	for round := int64(0); round < rounds; round++ {
		// (a) 建
		created := int64(0)
		groupsThisRound := int64(0)
		for created < batch {
			k := 3 + nxt()%4
			createGroup(c, k, false, false)
			created += k
			groupsThisRound++
		}
		// (b) 改：删头边 + 按 60/20/10/10 加边
		mOps := 2 + nxt()%3
		for m := int64(0); m < mOps; m++ {
			if c.n > 0 {
				idx := nxt() % c.n
				h := handleAt(c, idx)
				s := nodeInGroup(h, nxt()%h.size)
				f := s.first
				if f != nil && f.next != nil {
					s.first = f.next
				}
				r := nxt() % 100
				var tgt *Node = nil
				if r < 60 {
					tgt = olderInGroup(s, h.target)
				} else if r < 80 {
					tgt = newerInGroup(s, h.newest, h.target)
				} else if r < 90 {
					if c.hubCount > 0 {
						hj := nxt() % c.hubCount
						tgt = c.hubs[hj]
					}
				} else {
					idx2 := nxt() % c.n
					h2 := handleAt(c, idx2)
					tgt = nodeInGroup(h2, nxt()%h2.size)
				}
				if tgt != nil {
					if tgt.gid != s.gid {
						tgt.gpin = true
					}
					addEdge(s, tgt, 3)
				}
			}
		}
		// (c) 丢
		for d := int64(0); d < groupsThisRound; d++ {
			tryDrop(c)
		}
		// (d) 遍历
		epoch++
		traverse(c, epoch)
	}

	total := (acc + xacc) & mask
	fmt.Printf("nodes=%d edges=%d sum=%d\n", lastNodes, lastEdges, total)
	fmt.Printf("backedges=%d groups_made=%d nodes_made=%d dropped_groups=%d dropped_nodes=%d rejected_external=%d cycles_dropped=%d\n",
		backedges, groupsM, nodesM, droppedG, droppedN, rejected, cyclesDropped)

	// 停顿统计：**只写 stderr**，且由 DIGRAPH_GO_STATS 门控 ⇒ 默认路径 stdout 一个字都不多 ✗。
	// 口径：NumGC = GC 次数；PauseTotalNs = 累计 STW；PauseNs 是最近 256 次的环形缓冲
	// （最新一次在 (NumGC-1) % 256）⇒ 给 max 与 p99（≤256 条时的近似）。
	if os.Getenv("DIGRAPH_GO_STATS") != "" {
		var ms runtime.MemStats
		runtime.ReadMemStats(&ms)
		const cap256 = 256
		k := ms.NumGC
		if k > cap256 {
			k = cap256
		}
		var maxPause uint64
		pauses := make([]uint64, 0, k)
		for i := uint32(0); i < k; i++ {
			idx := (ms.NumGC - 1 - i) % cap256
			p := ms.PauseNs[idx]
			pauses = append(pauses, p)
			if p > maxPause {
				maxPause = p
			}
		}
		var p99 uint64
		if len(pauses) > 0 {
			// 小规模选择排序求 p99（≤256 条，代价可忽略）
			sorted := make([]uint64, len(pauses))
			copy(sorted, pauses)
			for i := 0; i < len(sorted); i++ {
				for j := i + 1; j < len(sorted); j++ {
					if sorted[j] < sorted[i] {
						sorted[i], sorted[j] = sorted[j], sorted[i]
					}
				}
			}
			pos := (99*len(sorted) + 99) / 100
			if pos >= len(sorted) {
				pos = len(sorted) - 1
			}
			p99 = sorted[pos]
		}
		wallMs := float64(time.Since(wallStart).Microseconds()) / 1000.0
		totalMs := float64(ms.PauseTotalNs) / 1e6
		share := 0.0
		if wallMs > 0 {
			share = totalMs / wallMs
		}
		fmt.Fprintf(os.Stderr,
			"GOSTATS num_gc=%d pause_total_ms=%.3f pause_max_us=%.1f pause_p99_us=%.1f samples=%d wall_ms=%.3f pause_share=%.5f\n",
			ms.NumGC, totalMs, float64(maxPause)/1000.0, float64(p99)/1000.0,
			len(pauses), wallMs, share)
	}
}
