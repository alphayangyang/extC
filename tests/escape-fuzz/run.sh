#!/usr/bin/env bash
# tests/escape-fuzz/run.sh —— 「new 像 C++ 堆」误拒 fuzz 的基线跑法。
#
# 只读 cases/ 里提交好的语料（改语料跑 gen.py），逐个：
#   extC 编过 ⇒ gcc -O1 -fsanitize=address 编 + 跑 + 比输出
#   extC 被拒 ⇒ 抓完整报错、按判据聚类
#   C++ 对照  ⇒ g++ -O1 编 + 跑
# 输出一张分类表。基线性质：exit 0（语法/类型错会显式列出，便于清零）。
#
# 用法：bash tests/escape-fuzz/run.sh [--jobs N]
set -u
# LeakSanitizer 对这套内存模型没有意义（arena 到进程结束才释放），它在最小发行版里还会偶尔
# 在退出扫描时挂住 ⇒ 这里只抓越界与 use-after-free。
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
# gcc<=12 的 libasan 遇到 vm.mmap_rnd_bits=32 的新内核会**偶发**在启动阶段自己 SEGV
# （stderr 只有 DEADLYSIGNAL，然后挂住）；实测 32 路并发 9/32 次，关掉地址随机化后 0/32。
# 原因与三条出路见 tests/asan-probe.sh。这里只对本套件关随机化。
if [ -z "${EXTC_ASAN_NO_SETARCH:-}" ] && command -v setarch >/dev/null 2>&1; then
    EXTC_ASAN_NO_SETARCH=1 exec setarch "$(uname -m)" -R bash "$0" "$@"
fi
cd "$(dirname "$0")/../.."          # 仓库根，与 tests/arena*/run.sh 同约定
export EXTC=${EXTC:-./build/extc}
export CC=${CC:-cc}
export CXX=${CXX:-g++}
JOBS=8
if [ "${1:-}" = "--jobs" ] && [ -n "${2:-}" ]; then JOBS=$2; fi
export JOBS
exec python3 tests/escape-fuzz/harness.py
