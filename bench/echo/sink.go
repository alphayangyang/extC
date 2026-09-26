// 生成器天花板：一个**不做任何调度**的 Go echo（每连接一个 goroutine + 固定缓冲）。
// 生成器打不动它，就没有资格去比两个真 server。
package main

import (
	"flag"
	"log"
	"net"
)

func main() {
	port := flag.String("port", "7655", "listen port")
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
			buf := make([]byte, 65536)
			for {
				n, err := c.Read(buf)
				if n > 0 {
					if _, werr := c.Write(buf[:n]); werr != nil {
						return
					}
				}
				if err != nil {
					return
				}
			}
		}(c)
	}
}
