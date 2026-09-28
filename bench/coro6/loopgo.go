package main
import "fmt"
func run(k, m int64) int64 { var s int64
	for id := int64(0); id < k; id++ { for i := int64(0); i < m; i++ { s += (i%65536)*65521 + id*40503 } }
	return s }
func main(){ t := run(8,2000); for r:=0;r<1;r++ { t += run(512,20000) }; fmt.Println(t) }
