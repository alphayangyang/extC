// Go 版 echo server：与 bench/echo/server.extc 同一份语义（同缓冲、同收尾、同 socket 选项）。
// 单线程列：GOMAXPROCS=1 跑；自然列：默认。
package main

import (
	"flag"
	"io"
	"log"
	"net"
)

func main() {
	port := flag.String("port", "7654", "listen port")
	flag.Parse()
	ln, err := net.Listen("tcp", "127.0.0.1:"+*port)
	if err != nil {
		log.Fatal(err)
	}
	for {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		if tc, ok := c.(*net.TCPConn); ok {
			tc.SetNoDelay(true)
		}
		go func(c net.Conn) {
			defer c.Close()
			buf := make([]byte, 16384)     /* 与 extC 侧同一个缓冲大小（压测的公平性要求）*/
			for {
				n, err := c.Read(buf)
				if n > 0 {
					if _, werr := c.Write(buf[:n]); werr != nil {
						return
					}
				}
				if err != nil {
					if err != io.EOF {
						return
					}
					return
				}
			}
		}(c)
	}
}
