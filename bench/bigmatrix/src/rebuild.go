// Shape 5: rebuild -- Go port of rebuild.c.
//
// Read `rounds k`; each round builds a k-node chain, sums it, then drops it.
// In C the dropping is an explicit free() per node behind a volatile function
// pointer, because gcc otherwise proves the whole malloc/free pair away and the
// shape measures nothing.  Go has no such problem: the nodes are ordinary heap
// objects, and each round's chain becomes unreachable when its head goes out of
// scope, so the collector does the reclaiming instead of free().
package main

import (
	"fmt"
	"os"
)

// ---------------------------------------------------------------------------
// Fast stdin reader (see radix.go for the rationale; every Go shape carries its
// own copy because each file is built on its own).
// ---------------------------------------------------------------------------

const blockSize = 1 << 20

type scanner struct {
	buf []byte
	pos int
	end int
}

func newScanner() *scanner {
	return &scanner{buf: make([]byte, blockSize)}
}

func (s *scanner) fill() bool {
	n, _ := os.Stdin.Read(s.buf)
	s.pos, s.end = 0, n
	return n > 0
}

func (s *scanner) nextByte() int {
	if s.pos == s.end && !s.fill() {
		return -1
	}
	c := s.buf[s.pos]
	s.pos++
	return int(c)
}

// nextInt64 mirrors fastio.h's bm_i64.
func (s *scanner) nextInt64() int64 {
	c := s.nextByte()
	for c == ' ' || c == '\n' || c == '\t' || c == '\r' {
		c = s.nextByte()
	}
	neg := c == '-'
	if neg {
		c = s.nextByte()
	}
	var v int64
	for c >= '0' && c <= '9' {
		v = v*10 + int64(c-'0')
		c = s.nextByte()
	}
	if neg {
		return -v
	}
	return v
}

// ---------------------------------------------------------------------------

// node is one link of the per-round chain.  It is deliberately a pointer type:
// each round allocates k of them, which is the allocation pressure this shape
// exists to measure.
type node struct {
	v    int64
	next *node
}

func main() {
	in := newScanner()
	rounds := in.nextInt64()
	k := in.nextInt64()

	var acc int64
	for r := int64(0); r < rounds; r++ {
		var head *node
		for i := int64(0); i < k; i++ {
			head = &node{v: i, next: head}
		}
		for p := head; p != nil; p = p.next {
			acc += p.v
		}
		// head dies here, so this round's whole chain is garbage; nothing to
		// free by hand, the collector takes it back.
	}
	fmt.Printf("acc=%d\n", acc)
}
