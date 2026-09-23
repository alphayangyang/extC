// Shape 2: cdq -- Go port of cdq.c (3D dominance, CDQ divide and conquer plus a
// weight Fenwick tree).
//
// Read n, then n triples (a b c) with values in [1,10^6].
//  1. sort by (a,b,c)   2. collapse duplicates, keeping cnt
//  3. CDQ over b, BIT over c: ans[j] += cnt[i] for every i<j that dominates j
//  4. f = ans + cnt (itself), reported answer = f - 1
//     print sum = SUM cnt_i * ans_i (u64 wrap) and max = max ans_i
package main

import (
	"fmt"
	"os"
	"sort"
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

// nextUint64 mirrors fastio.h's bm_u64.
func (s *scanner) nextUint64() uint64 {
	for {
		for s.pos < s.end && (s.buf[s.pos] < '0' || s.buf[s.pos] > '9') {
			s.pos++
		}
		if s.pos != s.end {
			break
		}
		if !s.fill() {
			return 0
		}
	}

	var v uint64
	for {
		for s.pos < s.end {
			c := s.buf[s.pos]
			if c < '0' || c > '9' {
				s.pos++ // consume the separator
				return v
			}
			v = v*10 + uint64(c-'0')
			s.pos++
		}
		if !s.fill() {
			return v
		}
	}
}

// ---------------------------------------------------------------------------

// maxV is the coordinate bound.  The generator emits c in [1,maxV], so the
// Fenwick tree is indexed by c itself -- no +1 shift -- and stays maxV+1 wide
// exactly like the C reference's calloc.  bitAdd only ever touches indices in
// [1,maxV] (it stops as soon as i > maxV), so c = maxV lands on bit[maxV], the
// last element, and never one past the end.
const maxV = 1000000

// pt keeps the C reference's field widths on purpose.  ans accumulates in int32
// and is allowed to wrap; the final answer is computed in 64 bits, so the wrap
// has to happen at the same place C wraps it.
type pt struct {
	a, b, c int32
	cnt     int32
	ans     int32
}

// dominance is the whole CDQ state: the deduplicated points, the merge buffer,
// and the weight Fenwick tree over c.
type dominance struct {
	pts []pt
	buf []pt
	bit []int32
}

func (d *dominance) bitAdd(i int64, v int32) {
	for ; i <= maxV; i += i & -i {
		d.bit[i] += v
	}
}

func (d *dominance) bitSum(i int64) int32 {
	var s int32
	for ; i > 0; i -= i & -i {
		s += d.bit[i]
	}
	return s
}

// cdq counts, for every point, the weight of the points that dominate it.  It
// splits on index (a is already sorted), recurses, then sweeps the right half
// with the left half's contributions added to the tree in b order and undone
// afterwards; the merge at the end leaves both halves ordered by b.
func (d *dominance) cdq(l, r int64) {
	if l >= r {
		return
	}
	mid := l + (r-l)/2
	d.cdq(l, mid)
	d.cdq(mid+1, r)

	i, j := l, mid+1
	for ; j <= r; j++ {
		for i <= mid && d.pts[i].b <= d.pts[j].b {
			d.bitAdd(int64(d.pts[i].c), d.pts[i].cnt)
			i++
		}
		d.pts[j].ans += d.bitSum(int64(d.pts[j].c))
	}
	for t := l; t < i; t++ { // undo
		d.bitAdd(int64(d.pts[t].c), -d.pts[t].cnt)
	}

	i, j = l, mid+1
	k := l
	for i <= mid && j <= r {
		if d.pts[i].b <= d.pts[j].b {
			d.buf[k] = d.pts[i]
			i++
		} else {
			d.buf[k] = d.pts[j]
			j++
		}
		k++
	}
	for ; i <= mid; i++ {
		d.buf[k] = d.pts[i]
		k++
	}
	for ; j <= r; j++ {
		d.buf[k] = d.pts[j]
		k++
	}
	copy(d.pts[l:r+1], d.buf[l:r+1])
}

func main() {
	in := newScanner()
	n := int(in.nextInt64())

	raw := make([]pt, n)
	for i := range raw {
		raw[i] = pt{
			a:   int32(in.nextUint64()),
			b:   int32(in.nextUint64()),
			c:   int32(in.nextUint64()),
			cnt: 1,
		}
	}
	sort.Slice(raw, func(i, j int) bool {
		switch {
		case raw[i].a != raw[j].a:
			return raw[i].a < raw[j].a
		case raw[i].b != raw[j].b:
			return raw[i].b < raw[j].b
		default:
			return raw[i].c < raw[j].c
		}
	})

	// Collapse runs of equal triples, counting multiplicities.
	d := &dominance{
		pts: make([]pt, 0, n),
		buf: make([]pt, n),
		bit: make([]int32, maxV+1),
	}
	for i := 0; i < n; {
		j := i + 1
		for j < n && raw[j].a == raw[i].a && raw[j].b == raw[i].b && raw[j].c == raw[i].c {
			j++
		}
		raw[i].cnt = int32(j - i)
		raw[i].ans = 0
		d.pts = append(d.pts, raw[i])
		i = j
	}
	d.buf = d.buf[:len(d.pts)]

	d.cdq(0, int64(len(d.pts))-1)

	var sum uint64
	var mx int64
	for i := range d.pts {
		ans := int64(d.pts[i].ans) + int64(d.pts[i].cnt) - 1
		sum += uint64(d.pts[i].cnt) * uint64(ans)
		if ans > mx {
			mx = ans
		}
	}
	fmt.Printf("sum=%d max=%d\n", sum, mx)
}
