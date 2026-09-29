#!/usr/bin/env bash
# tests/real-lib/run.sh —— **真的第三方库**跑一遍（C-ABI.md §9.11 / 第 5 步 ③）
#
# 为什么要有这一套：前面所有 dl 测试用的都是我们自己编的 .so —— 签名是我们写的、布局是我们定的、
# 行为我们知道。这里换成 libcairo.so.2：我们没参与过它，没有它的头文件，也**不链接**它。
#
# 判据：
#   ① `dlopen` + 20 个 `dlsym` + 显式转换 + 经**签名的表槽**调用：真画出东西；
#   ② 画布内存来自**板**（4 GiB 保留 / 按需 commit），cairo 留着那个指针（槽签 `Addr=1`）；
#   ③ "画对了"由**数字**判：绿色（边框+大括号+基线）与白色（三个字母）的像素数、四条边各有绿、
#      中间横带有白 —— 空图 / 纯色图 / 画错位置都过不了；
#   ④ PNG 真落盘（cairo 自己写的，8-bit RGB 512×512）；
#   ⑤ 没有 cairo 的机器上**显式跳过**（不是静默通过）。
set -u
cd "$(dirname "$0")/../.."
EXTC=./build/extc
fail=0

echo "== 绑定生成器（tools/cbindgen.py）：生成物与头文件一致吗 =="
# 头文件目录：$CAIRO_INC > pkg-config > /usr/include/cairo > 本机解包的 deb（见下面的命令）
CAIRO_INC=${CAIRO_INC:-}
if [ -z "$CAIRO_INC" ]; then
    for d in "$(pkg-config --variable=includedir cairo 2>/dev/null)/cairo" /usr/include/cairo; do
        if [ -f "$d/cairo.h" ]; then CAIRO_INC="$d"; break; fi
    done
fi
if [ -z "$CAIRO_INC" ]; then
    echo "  跳过  cairo 的头文件不在本机（生成物的 --check 需要它）。拿到它不需要 root："
    echo "        apt-get download libcairo2-dev && dpkg-deb -x libcairo2-dev_*.deb /tmp/cairohdr"
    echo "        CAIRO_INC=/tmp/cairohdr/usr/include/cairo ./tests/real-lib/run.sh"
else
    if gen=$(python3 tools/cbindgen.py @tools/cbindgen-cairo.args --include cairo.h -I "$CAIRO_INC" --check 2>&1); then
        echo "  ok   cbindgen  ->  $(echo "$gen" | tail -1)（头文件目录 $CAIRO_INC）"
    else
        echo "  FAIL cbindgen  ->  $gen"; fail=1
    fi
fi

echo "== 真库功能测试：用 cairo 画一张 extC 的 logo（绑定由生成器产出）=="
if ! ldconfig -p 2>/dev/null | grep -q 'libcairo\.so\.2'; then
    echo "  跳过  libcairo.so.2 不在本机（ldconfig -p 没找到）—— 这不是失败，是环境没有"
    echo "失败 0 个（0 = 全过，1 项跳过）"
    exit 0
fi

rm -f build/extc-logo.png
if out=$("$EXTC" --run tests/real-lib/cairo-logo.extc 2>&1); then
    ok=1
    # `// expect:` 只列**稳定**的部分：具体像素数与 cairo 版本/字体有关，不当判据用。
    want=$(grep -o '// expect:.*' tests/real-lib/cairo-logo.extc | sed 's|// expect: *||' | head -1)
    IFS=' ' read -ra parts <<< "$want"
    for p in "${parts[@]}"; do echo "$out" | grep -qF -- "$p" || { ok=0; echo "  FAIL 输出里缺「$p」"; }; done
    # 数字判据（阈值才是判据）
    green=$(echo "$out" | grep -o 'green=[0-9]*' | cut -d= -f2)
    white=$(echo "$out" | grep -o 'white=[0-9]*' | cut -d= -f2)
    edges=$(echo "$out" | grep -o 'edges=[0-9]*' | cut -d= -f2)
    if [ "${green:-0}" -lt 5000 ]; then echo "  FAIL 绿色像素只有 ${green}（边框+大括号+基线应当上万）"; ok=0; fi
    if [ "${white:-0}" -lt 1000 ]; then echo "  FAIL 白色像素只有 ${white}（三个字母应当上千）"; ok=0; fi
    if [ "${edges:-0}" != 1111 ]; then echo "  FAIL 四条边不齐（edges=$edges，应为 1111）"; ok=0; fi
    if [ ! -s build/extc-logo.png ]; then echo "  FAIL PNG 没落盘（或空文件）"; ok=0; fi
    if [ "$ok" = 1 ]; then
        echo "  ok   cairo  ->  $(echo "$out" | tr '\n' '|')"
        echo "        （$(du -h build/extc-logo.png | cut -f1) PNG：绿色 ${green} 像素 · 白色 ${white} 像素 · 四边齐）"
    else
        echo "  FAIL cairo  ->  判据不过"; fail=1
    fi
else
    echo "  FAIL cairo  ->  跑不起来"; echo "$out" | sed 's/^/        /' | head -6; fail=1
fi

echo "== 真库：通过 ABI 接 numpy（Python C API + numpy 的 C-API 函数指针表）=="
if ! python3 -c "import numpy" 2>/dev/null; then
    echo "  跳过  numpy 不在本机（这一步需要它）。拿到它不需要 root："
    echo "        pip install --target /tmp/npypip numpy && PYTHONPATH=/tmp/npypip ./tests/real-lib/run.sh"
else
    NPY_INC=$(python3 -c "import numpy; print(numpy.get_include())")
    PY_LIBDIR=$(python3 -c "import sysconfig; print(sysconfig.get_config_var('LIBDIR'))")
    # `LIBRARY` 是**静态**库名（libpython3.14.a ✗ 没有它）⇒ 要 `LDLIBRARY`（.so ✓）
    PY_LINK=$(python3 -c "import sysconfig; v=sysconfig.get_config_var('LDLIBRARY'); print(v[3:-3] if v.startswith('lib') and v.endswith('.so') else v)")
    PY_SO=$(python3 -c "import sysconfig; print(sysconfig.get_config_var('INSTSONAME'))")
    mkdir -p build
    # 表镜像：编号与签名**从装着的头文件里读**（猜就是 SIGSEGV —— 实测过 ✗）
    if python3 tools/npytable.py "$NPY_INC/numpy/__multiarray_api.h" "$PY_SO" > build/npytable.extc; then
        if "$EXTC" -w --no-line-map -I build -o build/numpy.c tests/real-lib/numpy.extc 2>build/numpy.err \
           && gcc -std=c11 -o build/numpy build/numpy.c -L"$PY_LIBDIR" -l"${PY_LINK#lib}" -Wl,-rpath,"$PY_LIBDIR" 2>>build/numpy.err; then
            if out=$(PYTHONPATH="${PYTHONPATH:-}" ./build/numpy 2>/dev/null) \
               && echo "$out" | grep -qF "c-api-version=33554432 size-via-capi=5 sum=10"; then
                echo "  ok   numpy  ->  $out"
                echo "        （表由 ${NPY_INC##*/} 的 __multiarray_api.h 现场生成：槽 0 版本 · 槽 59 PyArray_Size）"
            else
                echo "  FAIL numpy  ->  输出对不上：$(echo "$out" | tr '\n' '|')"; fail=1
            fi
        else
            echo "  FAIL numpy  ->  编/链不过：$(head -2 build/numpy.err | tr '\n' '|')"; fail=1
        fi
    else
        echo "  FAIL numpy  ->  表镜像生成失败"; fail=1
    fi
fi

echo "失败 $fail 个（0 = 全过）"
[ "$fail" = 0 ]
