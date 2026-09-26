// Go 的各路"最快写法"：spawn（纯 go / WaitGroup / chan close）与切换（chan 交接 / mutex 交接 /
// atomic 自旋）。只看最快的那一格，不问是否等价。
package main

import (
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"time"
)

var sink int64

func main() {
	const K = 2000
	out := make([]int64, 1000*K)

	// ---- spawn ----
	t0 := time.Now()
	for i := 0; i < 1000*K; i++ {
		go func() {}()
	}
	dt := time.Since(t0)
	runtime.Gosched()
	time.Sleep(200 * time.Millisecond) // 等它们跑完，免得污染后面的测量
	fmt.Printf("spawn  纯 go          : %8.2f ns/个\n", float64(dt.Nanoseconds())/(1000*K))

	t0 = time.Now()
	var wg sync.WaitGroup
	for i := 0; i < 1000*K; i++ {
		wg.Add(1)
		go func(i int) { out[i] = int64(i); wg.Done() }(i)
	}
	wg.Wait()
	dt = time.Since(t0)
	fmt.Printf("spawn  等待组 join    : %8.2f ns/个\n", float64(dt.Nanoseconds())/(1000*K))

	t0 = time.Now()
	for i := 0; i < 1000*K; i++ {
		done := make(chan struct{})
		go func(i int) { out[i] = int64(i); close(done) }(i)
		<-done
	}
	dt = time.Since(t0)
	fmt.Printf("spawn  chan close/recv: %8.2f ns/个\n", float64(dt.Nanoseconds())/(1000*K))

	// ---- 切换 ----
	t0 = time.Now()
	for k := 0; k < K; k++ {
		a := make(chan int64)
		b := make(chan int64)
		go func() { for i := int64(0); i < 5000; i++ { sink += <-a; b <- i } }()
		go func() { for i := int64(0); i < 5000; i++ { sink += <-b; a <- i } }()
		a <- 0
		<-a
	}
	dt = time.Since(t0)
	fmt.Printf("switch chan 交接      : %8.2f ns/次\n", float64(dt.Nanoseconds())/(10000*K))

	t0 = time.Now()
	for k := 0; k < K; k++ {
		var m1, m2 sync.Mutex
		m2.Lock()
		done := make(chan struct{})
		go func() {
			for i := 0; i < 5000; i++ { m1.Lock(); sink++; m2.Unlock() }
			close(done)
		}()
		for i := 0; i < 5000; i++ { m2.Lock(); sink++; m1.Unlock() }
		<-done
	}
	dt = time.Since(t0)
	fmt.Printf("switch mutex 交接     : %8.2f ns/次\n", float64(dt.Nanoseconds())/(10000*K))

	runtime.GOMAXPROCS(2) // 自旋需要两个 P 并行
	var f1, f2 int64 = 0, 0
	t0 = time.Now()
	for k := 0; k < K; k++ {
		done := make(chan struct{})
		go func() {
			for i := 0; i < 5000; i++ {
				for atomic.LoadInt64(&f1) == 0 {}
				atomic.StoreInt64(&f1, 0)
				sink++
				atomic.StoreInt64(&f2, 1)
			}
			close(done)
		}()
		for i := 0; i < 5000; i++ {
			for atomic.LoadInt64(&f2) == 0 {}
			atomic.StoreInt64(&f2, 0)
			sink++
			atomic.StoreInt64(&f1, 1)
		}
		<-done
	}
	dt = time.Since(t0)
	fmt.Printf("switch atomic 自旋    : %8.2f ns/次（GOMAXPROCS=2，烧一个核）\n", float64(dt.Nanoseconds())/(10000*K))
	fmt.Printf("sink=%d\n", sink)
}
