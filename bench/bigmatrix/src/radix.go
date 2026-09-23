// Shape 1: radix -- Go port of radix.c.
//
// Read n and n u32 values from stdin, LSD radix sort (4 passes x 8 bits),
// print sorted/first/last.  Same shape as bench/heavy/rs, data from stdin.
package main

import (
	"fmt"
	"os"
)

// ---------------------------------------------------------------------------
// Fast stdin reader.
//
// The shapes feed up to 10^7 numbers, so fmt.Fscan -- reflection plus an
// interface box per item -- would dominate the run and turn the benchmark into
// a measurement of the standard library.  This is the hand-rolled equivalent of
// the C reference's fastio.h: one large block buffered from os.Stdin, and an
// integer parser that walks the bytes itself.
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

// fill replaces the block.  It reports whether any byte is available.
func (s *scanner) fill() bool {
	n, _ := os.Stdin.Read(s.buf)
	s.pos, s.end = 0, n
	return n > 0
}

// nextByte returns the next input byte, or -1 at end of input.
func (s *scanner) nextByte() int {
	if s.pos == s.end && !s.fill() {
		return -1
	}
	c := s.buf[s.pos]
	s.pos++
	return int(c)
}

// nextInt64 mirrors fastio.h's bm_i64: skip blanks, then an optional '-' and
// the digits that follow.  Only the header numbers are read this way, so the
// per-byte call is not worth optimising.
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

// nextUint64 mirrors fastio.h's bm_u64: skip everything that is not a digit,
// accumulate the digits, and return 0 at end of input.  The digit loop runs
// straight over the block, so only a number that straddles a block boundary
// pays for a refill.
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

func main() {
	in := newScanner()
	n := int(in.nextInt64())

	a := make([]uint32, n)
	b := make([]uint32, n)
	for i := range a {
		a[i] = uint32(in.nextUint64())
	}

	var cnt [256]int64
	for pass := uint(0); pass < 4; pass++ {
		sh := pass * 8
		for k := range cnt {
			cnt[k] = 0
		}
		for _, v := range a {
			cnt[(v>>sh)&255]++
		}
		var sum int64
		for k := range cnt {
			c := cnt[k]
			cnt[k] = sum
			sum += c
		}
		for _, v := range a {
			k := (v >> sh) & 255
			b[cnt[k]] = v
			cnt[k]++
		}
		copy(a, b)
	}

	ok := 1
	for i := 1; i < n; i++ {
		if a[i-1] > a[i] {
			ok = 0
			break
		}
	}
	fmt.Printf("sorted=%d first=%d last=%d\n", ok, a[0], a[n-1])
}
