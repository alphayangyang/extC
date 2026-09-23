// Shape 3: bt -- Go port of bt.c.
//
// Read `maxdepth bigdepth iters`; build perfect binary trees bottom-up, count
// nodes; then count one big tree `iters` times.  Same as bench/heavy/bt.
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

// tree is the same mutable node the C reference uses: a leaf is a node with no
// children, so conditionally filling left/right is what gives bottomUp its
// shape.  The C reference freeTree()s each tree right after counting it; here
// the chain needs nothing, the collector reclaims it once the variable dies.
type tree struct {
	left, right *tree
}

func bottomUp(depth int64) *tree {
	t := &tree{}
	if depth > 0 {
		t.left = bottomUp(depth - 1)
		t.right = bottomUp(depth - 1)
	}
	return t
}

func count(t *tree) int64 {
	if t.left == nil {
		return 1
	}
	return 1 + count(t.left) + count(t.right)
}

func main() {
	in := newScanner()
	maxdepth := in.nextInt64()
	bigdepth := in.nextInt64()
	iters := in.nextInt64()

	var total int64
	for d := int64(4); d <= maxdepth; d++ {
		t := bottomUp(d)
		total += count(t)
	}
	big := bottomUp(bigdepth)
	for i := int64(0); i < iters; i++ {
		total += count(big)
	}
	fmt.Printf("trees=%d\n", total)
}
