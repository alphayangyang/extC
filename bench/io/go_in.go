// Go · 输入：`bufio.Scanner` + `ScanWords` + `strconv.ParseInt`（惯用快写法 ✓）
// 契约：`in <文件|->` ⇒ 打印总和 ✓
package main

import (
"bufio"
"os"
"strconv"
)

func main() {
path := os.Args[1]
var in *os.File
if path == "-" {
in = os.Stdin
} else {
f, err := os.Open(path)
if err != nil { panic(err) }
in = f
}
sc := bufio.NewScanner(in)
sc.Buffer(make([]byte, 1<<20), 1<<20)
sc.Split(bufio.ScanWords)
var total int64
for sc.Scan() {
v, err := strconv.ParseInt(sc.Text(), 10, 64)
if err != nil { panic(err) }
total += v
}
os.Stdout.WriteString(strconv.FormatInt(total, 10) + "\n")
}
