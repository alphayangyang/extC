// bench/gc/stamped/cfg.go —— 只打印 Go 运行期自己选择的配置（本程序不计时、不参与任何测量）。
// 用途：满足"必须把每个运行时自动选择的配置打印出来核对"这条硬要求。
// 读 GOGC 的正确姿势：debug.SetGCPercent(-1) 返回旧值但**会关掉 GC** ⇒ 读完立刻还原。
package main

import (
	"fmt"
	"os"
	"runtime"
	"runtime/debug"
)

func main() {
	old := debug.SetGCPercent(-1)
	debug.SetGCPercent(old)
	env := func(k string) string {
		if v, ok := os.LookupEnv(k); ok {
			return v
		}
		return "<unset>"
	}
	fmt.Printf("version=%s GOMAXPROCS=%d NumCPU=%d GOGC=%d GOMEMLIMIT=%d env_GOGC=%s env_GOMEMLIMIT=%s env_GOMAXPROCS=%s\n",
		runtime.Version(), runtime.GOMAXPROCS(0), runtime.NumCPU(),
		old, debug.SetMemoryLimit(-1),
		env("GOGC"), env("GOMEMLIMIT"), env("GOMAXPROCS"))
}
