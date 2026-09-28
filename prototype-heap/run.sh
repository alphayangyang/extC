#!/usr/bin/env bash
# prototype-heap/run.sh —— 编译并跑四个测量（编译契约与产物一致）
set -eu
cd "$(dirname "$0")"
CC=${CC:-gcc}
FLAGS="-O2 -std=c11 -fwrapv -Wall -Wextra -Werror"
$CC $FLAGS -fPIC -shared -o plugin.so plugin.c
$CC $FLAGS -o host host.c -ldl
./host
