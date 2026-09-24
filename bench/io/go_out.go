// Go · 输出：`bufio.Writer` + `strconv.AppendInt`（避开 fmt 的反射开销 ✓）
package main

import (
"bufio"
"os"
"strconv"
)

func main() {
path := os.Args[1]
n, _ := strconv.ParseInt(os.Args[2], 10, 64)
var w *bufio.Writer
if path == "-" {
w = bufio.NewWriterSize(os.Stdout, 1<<16)
} else {
f, err := os.Create(path)
if err != nil { panic(err) }
w = bufio.NewWriterSize(f, 1<<16)
}
buf := make([]byte, 0, 32)
for i := int64(0); i < n; i++ {
buf = strconv.AppendInt(buf[:0], i, 10)
buf = append(buf, ' ')
buf = strconv.AppendInt(buf, i*i, 10)
buf = append(buf, '\n')
w.Write(buf)
}
w.Flush()
}
