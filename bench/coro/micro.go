// Go 同形对照：① goroutine + join（每个 10 步）② 两个 goroutine 用 channel 交替
package main

import (
	"fmt"
	"time"
)

func main() {
	const K = 2000

	t0 := time.Now()
	for i := 0; i < 1000*K; i++ {
		done := make(chan struct{})
		go func() {
			s := 0
			for j := 0; j < 10; j++ {
				s += j
			}
			close(done)
		}()
		<-done
	}
	dt := time.Since(t0)
	fmt.Printf("spawn+join  : %7.2f ns/个   (1000×%d)\n", float64(dt.Nanoseconds())/(1000*K), K)

	t0 = time.Now()
	for k := 0; k < K; k++ {
		a := make(chan int)
		b := make(chan int)
		go func() { for i := 0; i < 5000; i++ { <-a; b <- i } }()
		go func() { for i := 0; i < 5000; i++ { <-b; a <- i } }()
		a <- 0
		<-a
	}
	dt = time.Since(t0)
	fmt.Printf("switch      : %7.2f ns/次   (10000×%d)\n", float64(dt.Nanoseconds())/(10000*K), K)
}
