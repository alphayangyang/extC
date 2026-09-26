// N 个活任务：同 liveN.extc 的形状 —— spawn N 个 goroutine，每个阻塞在**无缓冲** channel 的 send
// 上（= 挂起，任务活着）。两批：冷（堆/页/栈都新）与热。
//
// 关键：spawn 之后要让每个 goroutine 真的跑起来并挂到 channel 上（sleep 一下），否则量到的是
// "还没分配栈的 goroutine"，数字会好看得没意义 —— 这正是 RESULTS.md 里那条冷/热规矩。
package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"time"
)

func worker(id int64, ch chan int64) {
	var i int64 = 0
	for i < 3 {
		ch <- id + i // 阻塞在这里 = 挂起
		i++
	}
}

func rssKB() int64 {
	b, err := os.ReadFile("/proc/self/statm")
	if err != nil {
		return -1
	}
	var size, res int64
	fmt.Sscanf(string(b), "%d %d", &size, &res)
	return res * 4 // 页 = 4 KB ⇒ KB
}

// 只计时 **spawn 循环**：等它们挂上去的时间不算（那是"跑到挂起点"，与 extC 的"驱动一次"对应，
// 不是 spawn 成本）。不等的话量到的是"还没分配栈的 goroutine"，内存数字会假。
func batch(n int, ch chan int64) time.Duration {
	t0 := time.Now()
	for i := 0; i < n; i++ {
		go worker(int64(i), ch)
	}
	dt := time.Since(t0)
	runtime.Gosched()
	time.Sleep(500 * time.Millisecond)
	return dt
}

func main() {
	n, _ := strconv.Atoi(os.Args[1])
	ch := make(chan int64) // 无缓冲：send 挂起

	r0 := rssKB()
	cold := batch(n, ch)
	r1 := rssKB()

	warm := batch(n, ch)
	r2 := rssKB()

	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	fmt.Printf("Go   n=%d live=%d cold_ns=%d warm_ns=%d cold_B=%d warm_B=%d heap=%d KB\n",
		n, runtime.NumGoroutine(),
		cold.Nanoseconds()/int64(n), warm.Nanoseconds()/int64(n),
		(r1-r0)*1024/int64(n), (r2-r1)*1024/int64(n), m.HeapAlloc/1024)
}
