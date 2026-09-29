#!/usr/bin/env bash
# bench/numpy-matmul/run.sh —— **同一份 numpy 干活**，量三根（四根）柱子的时间与峰值 RSS：
#   py-numpy    Python + numpy（`np.matmul(a, b, out=c)`）
#   extc-numpy  extC + numpy：乘法走 **numpy 的 C-API 表**（槽 280 = PyArray_MatrixProduct2）
#   c-numpy     C + numpy：和 extC 那条路一一对应（对照组 ⇒ 分辨"extC 的胶水"有没有额外代价）
#   extc-native extC 自己三重循环（不进 numpy；只在小 N 上跑 —— 与 BLAS 不是一个量级）
#
# 环境：numpy 必须在（否则跳过）；BLAS 线程钉成 1（`OPENBLAS_NUM_THREADS=1`）否则数字抖。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

if ! python3 -c "import numpy" 2>/dev/null; then
    echo "跳过：numpy 不在本机。拿到它不需要 root："
    echo "      pip install --target /tmp/npypip numpy && PYTHONPATH=/tmp/npypip $0"
    exit 0
fi

NPY_INC=$(python3 -c "import numpy; print(numpy.get_include())")
PY_LIBDIR=$(python3 -c "import sysconfig; print(sysconfig.get_config_var('LIBDIR'))")
PY_LINK=$(python3 -c "import sysconfig; v=sysconfig.get_config_var('LDLIBRARY'); print(v[3:-3] if v.startswith('lib') and v.endswith('.so') else v)")
PY_SO=$(python3 -c "import sysconfig; print(sysconfig.get_config_var('INSTSONAME'))")
mkdir -p build
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 MKL_NUM_THREADS=1

python3 tools/npytable.py "$NPY_INC/numpy/__multiarray_api.h" "$PY_SO" --need PyArray_MatrixProduct2 > build/npytable.extc
"$EXTC" -w --no-line-map -I build -o build/np_extc.c bench/numpy-matmul/extc_numpy.extc \
    && gcc -O2 -std=c11 -o build/np_extc build/np_extc.c -L"$PY_LIBDIR" -l"$PY_LINK" -Wl,-rpath,"$PY_LIBDIR" \
    || { echo "extC+numpy 编不过"; exit 1; }
gcc -O2 -std=c11 -o build/np_c bench/numpy-matmul/c_numpy.c -ldl -L"$PY_LIBDIR" -l"$PY_LINK" -Wl,-rpath,"$PY_LIBDIR" || exit 1
"$EXTC" -w --no-line-map -o build/np_native.c bench/numpy-matmul/extc_native.extc \
    && gcc -O2 -std=c11 -o build/np_native build/np_native.c || { echo "extC 裸算编不过"; exit 1; }

printf '%-14s %8s %12s %14s\n' 实现 N 时间ms 峰值RSS_MB
for n in 8 64 256 1024; do
    case $n in 8) reps=3000;; 64) reps=200;; 256) reps=30;; 1024) reps=5;; esac
    :
    for impl in "py-numpy:python3 bench/numpy-matmul/py_numpy.py" "extc-numpy:./build/np_extc" "c-numpy:./build/np_c"; do
        name=${impl%%:*}; cmd=${impl#*:}
        out=$($cmd "$n" "$reps" 2>/dev/null | tail -1)
        if [ -z "$out" ]; then printf '%-14s %8s %12s %14s\n' "$name" "$n" "FAIL" "-"; fail=1; continue; fi
        ms=$(echo "$out" | sed -n 's/.*best_ms=\([0-9.]*\).*/\1/p')
        rss=$(echo "$out" | sed -n 's/.*peak_rss_kb=\([0-9]*\).*/\1/p')
        printf '%-14s %8s %12s %14s\n' "$name" "$n" "$ms" "$(awk -v k="$rss" 'BEGIN{printf "%.1f", k/1024}')"
    done
    if [ "$n" -le 256 ]; then
        out=$(./build/np_native "$n" 3 2>/dev/null | tail -1)
        ms=$(echo "$out" | sed -n 's/.*best_ms=\([0-9.]*\).*/\1/p')
        rss=$(echo "$out" | sed -n 's/.*peak_rss_kb=\([0-9]*\).*/\1/p')
        printf '%-14s %8s %12s %14s\n' "extc-native" "$n" "$ms" "$(awk -v k="$rss" 'BEGIN{printf "%.1f", k/1024}')"
    fi
done
echo "（BLAS 线程钉成 1；数组创建不计时；每格取三轮里的最好一次）"
[ "$fail" = 0 ]
