// Shape 4: mandel -- Go port of mandel.c.
//
// Read `w h maxiter`; same loop as bench/heavy/mb (float64, escape at |z|>2).
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

func main() {
	in := newScanner()
	w := in.nextInt64()
	h := in.nextInt64()
	maxiter := in.nextInt64()

	img := make([]uint8, w*h)
	var acc int64
	for y := int64(0); y < h; y++ {
		for x := int64(0); x < w; x++ {
			cr := float64(x)/float64(w)*3.5 - 2.5
			ci := float64(y)/float64(h)*2.0 - 1.0
			zr, zi := 0.0, 0.0
			// The C loop breaks out before it increments, so the escape
			// iteration is not counted; a for-post statement does the same.
			it := int64(0)
			for ; it < maxiter; it++ {
				t := zr*zr - zi*zi + cr
				zi = 2.0*zr*zi + ci
				zr = t
				if zr*zr+zi*zi > 4.0 {
					break
				}
			}
			img[y*w+x] = uint8(it)
			acc += it
		}
	}
	fmt.Printf("mandel=%d img0=%d\n", acc, img[0])
}
