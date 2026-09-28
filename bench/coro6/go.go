// Go：goroutine + 无缓冲 channel（一次收发 = 一次会合 = 一次"推进"）
package main
import "fmt"
func gen(id, n, salt int64, ch chan int64) {
	for i := int64(0); i < n; i++ { ch <- (i%65536)*65521 + id*40503 + salt }
	close(ch)
}
func run(k, m, salt int64) int64 {
	var s int64
	for id := int64(0); id < k; id++ {
		ch := make(chan int64)
		go gen(id, m, salt, ch)
		for v := range ch { s += v }
	}
	return s
}
func main() {
	t := run(8, 2000, 0)
	for r := 0; r < R_ROUNDS; r++ { t += run(512, 20000, int64(r)) }
	fmt.Println(t)
}
