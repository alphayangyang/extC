// bench/app/session.go —— **场景 A：会话 / 连接表**（session.extc 的 Go 对照）
//
// 用法：session <ops> <active>      （默认 2000000 / 65536）
// 输出：a.len=<活跃条目> a.hits=<命中查询数> a.acc=<校验和>
package main

import (
	"fmt"
	"os"
)

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

	m := make(map[int64]session, active)
	var acc, hits int64
	for i := int64(0); i < ops; i++ {
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
	fmt.Printf("a.len=%d a.hits=%d a.acc=%d\n", len(m), hits, acc%1000000007)
}
