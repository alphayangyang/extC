// bench/app/session.go —— **场景 A：会话 / 连接表**（session.extc 的 Go 对照）
//
// 用法：session <ops> <active>      （默认 2000000 / 65536）
// 输出：a.len=<活跃条目> a.hits=<命中查询数> a.acc=<校验和>
package main

import (
	"fmt"
	"os"
	"time"
)


// ================ 应用侧逐单位时间戳（USER-STALL / bench/app） ================
// 只写**戳文件**（路径 = os.Args 最后一项）；stdout 一个字节都不动。
// 40 字节头 + (nstamps-1) 个 unsigned-LEB128 增量（小端）。
// 戳数 = 单位数 + 1：第一次 us() 记 t0，其后每次记一个增量，循环后补最后一次。
// ============================================================================
var (
	usBuf   []byte
	usCap   int64
	usN     int64
	usT0    int64
	usPrev  int64
	usNeg   int64
	usOvf   int64
	usStart bool
	usBase  time.Time
)

func usReserve(units int64) {
	usCap = units*5 + 64
	usBuf = make([]byte, 0, usCap)
	usBase = time.Now()
}

func us() {
	t := time.Since(usBase).Nanoseconds()
	if !usStart {
		usStart = true
		usT0 = t
		usPrev = t
		usN = 1
		return
	}
	d := t - usPrev
	if d < 0 {
		usNeg++
		d = 0
	}
	usPrev = t
	usN++
	v := uint64(d)
	for v >= 128 {
		if int64(len(usBuf)) < usCap {
			usBuf = append(usBuf, byte(v&127|128))
		} else {
			usOvf++
		}
		v >>= 7
	}
	if int64(len(usBuf)) < usCap {
		usBuf = append(usBuf, byte(v))
	} else {
		usOvf++
	}
}

func usDump(path string) {
	hdr := make([]byte, 40)
	copy(hdr, "USAPPST1")
	put := func(off int, val uint64) {
		for i := 0; i < 8; i++ {
			hdr[off+i] = byte(val >> (8 * uint(i)))
		}
	}
	put(8, uint64(usN))
	put(16, uint64(usT0))
	put(24, uint64(usNeg))
	put(32, uint64(usOvf))
	f, err := os.Create(path)
	if err != nil {
		fmt.Fprintln(os.Stderr, "USAPP: create stamp file failed:", err)
		return
	}
	f.Write(hdr)
	f.Write(usBuf)
	f.Close()
}
type session struct {
	user  int64
	state int32
	bytes int64
	last  int64
}

// toInt 与 extC 版 toInt 语义一致：空串或含非数字字符 => dflt。
func toInt(s string, dflt int64) int64 {
	if len(s) == 0 {
		return dflt
	}
	var v int64 = 0
	for i := 0; i < len(s); i++ {
		b := int64(s[i])
		if b < 48 || b > 57 {
			return dflt
		}
		v = v*10 + (b - 48)
	}
	return v
}

func main() {
	var ops int64 = 2000000
	var active int64 = 65536
	if len(os.Args) > 1 {
		ops = toInt(os.Args[1], ops)
	}
	if len(os.Args) > 2 {
		active = toInt(os.Args[2], active)
	}
	usPath := ""
	if len(os.Args) > 3 {
		usPath = os.Args[len(os.Args)-1]
	}
	usReserve(ops)

	m := make(map[int64]session, active)
	var acc, hits int64
	for i := int64(0); i < ops; i++ {
		us()
		k := i % active
		op := i % 977
		ph := op % 5
		if ph < 3 {
			// 命中就取该条目；缺席时相当于 extC 的 ?? 零值结构（bytes == 0）。
			if s, ok := m[k]; ok {
				acc += s.bytes
				if s.bytes > 0 {
					hits++
				}
			}
		} else if ph == 3 {
			m[k] = session{user: k % 1024, state: 1, bytes: i + k, last: i}
		} else {
			if _, ok := m[k]; ok {
				delete(m, k)
				acc++
			}
		}
	}
	us()
	usDump(usPath)
	fmt.Printf("a.len=%d a.hits=%d a.acc=%d\n", len(m), hits, acc%1000000007)
}
