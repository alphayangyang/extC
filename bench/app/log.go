// bench/app/log.go —— **场景 C：请求日志 / 动态缓冲**（log.extc 的 Go 对照）
//
// 用法：log <ops>            （默认 2000000）
// 输出：c.appended=<累计追加字节> c.trunc=<截断次数> c.find=<find 偏移之和> c.acc=<校验和> c.len=<最终长度>
package main

import (
	"bytes"
	"fmt"
	"os"
)

const (
	CAP  int64 = 16384
	KEEP int64 = 8192
	REC  int64 = 24
)

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

// recByte：第 i 条记录的第 j 个字节：33..122，可打印、逐字节与 extC 相同。
func recByte(i, j int64) byte {
	return byte(33 + ((i + j*3) % 90))
}

func main() {
	var ops int64 = 2000000
	if len(os.Args) > 1 {
		ops = toInt(os.Args[1], ops)
	}

	buf := make([]byte, 0, CAP)
	var trunc, findSum, acc int64
	for i := int64(0); i < ops; i++ {
		for j := int64(0); j < REC; j++ {
			buf = append(buf, recByte(i, j))
		}
		// 找“上一条记录的前 4 个字节”：它只可能出现在缓冲末尾附近 ⇒ find 扫满整条缓冲。
		if i%512 == 0 && i > 0 {
			needle := []byte{recByte(i-1, 0), recByte(i-1, 1), recByte(i-1, 2), recByte(i-1, 3)}
			at := bytes.Index(buf, needle)
			findSum += int64(at)
		}
		if int64(len(buf)) >= CAP {
			acc += int64(buf[0]) + int64(buf[len(buf)-1])
			n := int64(len(buf)) - KEEP
			copy(buf, buf[n:]) // 原地丢掉最老的 n 个字节（memmove，不重新分配）
			buf = buf[:KEEP]
			trunc++
		}
	}
	fmt.Printf("c.appended=%d c.trunc=%d c.find=%d c.acc=%d c.len=%d\n",
		ops*REC, trunc, findSum%1000000007, acc, int64(len(buf)))
}
