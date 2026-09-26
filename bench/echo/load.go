// 唯一的负载生成器：extC 与 Go 两个 echo server 都用它 —— 公平性的硬约束。
//
// 两种形态：
//   pingpong  发一批 → 等回声 → 再发下一批（量往返延迟）
//   pipeline  一次性灌 depth 批、不等（量吞吐）
//
// 每个回声都**逐个字节校验**（快的服务器要是把数据搞错了就是失败）。还报 p50/p99。
package main

import (
	"flag"
	"fmt"
	"net"
	"os"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

func fill(buf []byte, seed byte) {
	for i := range buf {
		buf[i] = seed ^ byte(i*31+i>>3)
	}
}

func main() {
	addr := flag.String("addr", "127.0.0.1:7654", "server address")
	conns := flag.Int("conns", 1, "concurrent connections")
	payload := flag.Int("payload", 64, "bytes per request")
	iters := flag.Int("iters", 1000, "requests per connection")
	mode := flag.String("mode", "pingpong", "pingpong | pipeline")
	depth := flag.Int("depth", 16, "pipeline depth (pipeline mode)")
	warmup := flag.Int("warmup", 0, "warm-up iterations per connection (not measured)")
	flag.Parse()

	// 先建好全部连接（避免把 connect 时间算进来）
	cs := make([]net.Conn, *conns)
	for i := range cs {
		c, err := net.Dial("tcp", *addr)
		if err != nil {
			fmt.Println("dial:", err)
			os.Exit(1)
		}
		if tc, ok := c.(*net.TCPConn); ok {
			tc.SetNoDelay(true)
		}
		cs[i] = c
	}

	rttSample := make([]int64, 0, 4096)
	var mu sync.Mutex
	var bad atomic.Int64

	work := func(c net.Conn, seed byte, n int, rec bool) {
		buf := make([]byte, *payload)
		fill(buf, seed)
		got := make([]byte, *payload)
		for i := 0; i < n; i++ {
			t0 := time.Now()
			if _, err := c.Write(buf); err != nil {
				bad.Add(1)
				return
			}
			if _, err := readFull(c, got); err != nil {
				bad.Add(1)
				return
			}
			for k := range got {
				if got[k] != buf[k] {
					bad.Add(1)
					return
				}
			}
			if rec {
				mu.Lock()
				rttSample = append(rttSample, time.Since(t0).Nanoseconds())
				mu.Unlock()
			}
		}
	}

	pipeWork := func(c net.Conn, seed byte, n int, rec bool) {
		buf := make([]byte, *payload)
		fill(buf, seed)
		got := make([]byte, *payload)
		full := (*depth) * (*payload)
		out := make([]byte, full)
		for i := 0; i < *depth; i++ {
			copy(out[i*(*payload):], buf)
		}
		for i := 0; i < n; i += *depth {
			t0 := time.Now()
			if _, err := c.Write(out); err != nil {
				bad.Add(1)
				return
			}
			for k := 0; k < *depth; k++ {
				if _, err := readFull(c, got); err != nil {
					bad.Add(1)
					return
				}
				for m := range got {
					if got[m] != buf[m] {
						bad.Add(1)
						return
					}
				}
			}
			if rec && i == 0 {
				mu.Lock()
				rttSample = append(rttSample, time.Since(t0).Nanoseconds())
				mu.Unlock()
			}
		}
	}

	per := *iters
	if *mode == "pipeline" {
		per = (*iters / *depth) * *depth
		if per == 0 {
			per = *depth
		}
	}

	if *warmup > 0 {
		var wg sync.WaitGroup
		for i := range cs {
			wg.Add(1)
			go func(c net.Conn, seed byte) { defer wg.Done(); work(c, seed, *warmup, false) }(cs[i], byte(i+1))
		}
		wg.Wait()
	}

	var wg sync.WaitGroup
	t0 := time.Now()
	for i := range cs {
		wg.Add(1)
		if *mode == "pipeline" {
			go func(c net.Conn, seed byte) { defer wg.Done(); pipeWork(c, seed, per, true) }(cs[i], byte(i+1))
		} else {
			go func(c net.Conn, seed byte) { defer wg.Done(); work(c, seed, per, true) }(cs[i], byte(i+1))
		}
	}
	wg.Wait()
	dt := time.Since(t0)
	for _, c := range cs {
		c.Close()
	}

	total := int64(*conns * per)
	ok := bad.Load() == 0
	sort.Slice(rttSample, func(a, b int) bool { return rttSample[a] < rttSample[b] })
	pct := func(p float64) int64 {
		if len(rttSample) == 0 {
			return 0
		}
		return rttSample[int(float64(len(rttSample)-1)*p)]
	}
	fmt.Printf("%-9s conns=%-5d payload=%-5d iters=%-8d  %8.0f req/s  %7.1f MB/s  p50=%6.1fus p99=%6.1fus  %s\n",
		*mode, *conns, *payload, per,
		float64(total)/dt.Seconds(),
		float64(total*int64(*payload))/dt.Seconds()/1e6,
		float64(pct(0.50))/1e3, float64(pct(0.99))/1e3,
		map[bool]string{true: "ok", false: "*** 数据校验失败 ***"}[ok])
	if !ok {
		os.Exit(2)
	}
}

func readFull(c net.Conn, buf []byte) (int, error) {
	n := 0
	for n < len(buf) {
		m, err := c.Read(buf[n:])
		if m > 0 {
			n += m
		}
		if err != nil {
			return n, err
		}
	}
	return n, nil
}
