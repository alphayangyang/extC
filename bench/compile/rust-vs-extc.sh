#!/usr/bin/env bash
# bench/compile/rust-vs-extc.sh —— 同规模代码：**extC vs Rust（vs C）编译时长**
#
# 程序形状（三条线**一模一样**）：N 个 struct（各带一个方法 get）+ N 个函数 g0..gN-1
#   + main 全调一遍 + **5 个泛型容器实例**（i32/i64/u64/u8/bool/f64 各 push 一次）
#   · extC：`varArray<T>`（prelude 里用 extC 写的泛型 + 单态化）
#   · Rust：`Vec<T>`（std 泛型 + 单态化）
#   · C   ：能手写对照（没有泛型/方法 ⇒ 只作"手写 C"参照）
#
# 量两件事：
#   ① **前端/类型检查**（extC: 只吐 C · Rust: --emit=metadata · C 无此阶段）
#   ② **端到端**（extC+gcc · rustc -O · gcc -O2）
# ⚠️ rustc 默认单线程；`-C codegen-units` 保持默认（跟用户实际一样）✓
set -u
cd "$(dirname "$0")/../.."
EXTC=${EXTC:-./build/extc}; CC=${CC:-cc}; RUSTC=${RUSTC:-rustc}
OUT=build/compile-bench; mkdir -p "$OUT"; RUNS=${RUNS:-3}

gen_extc() { local n=$1 f=$2 i; {
  for ((i=0;i<n;i++)); do echo "struct s$i { a: i32  b: i64  fn get(self: ref s$i) -> i32 { return self.a } }"; done
  for ((i=0;i<n;i++)); do echo "fn g$i(x: i32) -> i32 {
    var t: s$i = { a: x, b: 1 }
    var v: varArray<i32> = varArray<i32>::withCap(2)
    v.push(x)
    return t.get() + (v.get(0) ?? 0)
}"; done
  echo "fn main() {"
  echo "    var acc: i32 = 0"
  for ((i=0;i<n;i++)); do echo "    acc = acc + g$i($i)"; done
  echo "    println(\"acc = \", acc)"
  echo "    var vi: varArray<i64> = varArray<i64>::withCap(1)"
  echo "    var vu: varArray<u64> = varArray<u64>::withCap(1)"
  echo "    var vb: varArray<bool> = varArray<bool>::withCap(1)"
  echo "    var vf: varArray<f64> = varArray<f64>::withCap(1)"
  echo "    var vq: varArray<u8> = varArray<u8>::withCap(1)"
  echo "    vi.push(1)  vu.push(2)  vb.push(true)  vf.push(0.5)  vq.push(3)"
  echo "    println(\"insts = \", vi.size(), vu.size(), vb.size(), vf.size(), vq.size())"
  echo "}"; } > "$f"; }

gen_rust() { local n=$1 f=$2 i; {
  for ((i=0;i<n;i++)); do echo "struct S$i { a: i32, b: i64 }
impl S$i { fn get(&self) -> i32 { self.a } }"; done
  for ((i=0;i<n;i++)); do echo "fn g$i(x: i32) -> i32 { let t = S$i { a: x, b: 1 }; let mut v: Vec<i32> = Vec::with_capacity(2); v.push(x); t.get() + v.get(0).copied().unwrap_or(0) }"; done
  echo "fn main() {"
  echo "    let mut acc: i32 = 0;"
  for ((i=0;i<n;i++)); do echo "    acc += g$i($i);"; done
  echo "    println!(\"acc = {}\", acc);"
  echo "    let mut vi: Vec<i64> = Vec::with_capacity(1);"
  echo "    let mut vu: Vec<u64> = Vec::with_capacity(1);"
  echo "    let mut vb: Vec<bool> = Vec::with_capacity(1);"
  echo "    let mut vf: Vec<f64> = Vec::with_capacity(1);"
  echo "    let mut vq: Vec<u8> = Vec::with_capacity(1);"
  echo "    vi.push(1); vu.push(2); vb.push(true); vf.push(0.5); vq.push(3);"
  echo "    println!(\"insts = {} {} {} {} {}\", vi.len(), vu.len(), vb.len(), vf.len(), vq.len());"
  echo "}"; } > "$f"; }

ms() { local best=999999999 t0 t1 dt i
  for ((i=0;i<RUNS;i++)); do t0=$(date +%s%N)
    if ! "$@" >/dev/null 2>&1; then echo "ERR"; return; fi
    t1=$(date +%s%N); dt=$(( (t1-t0)/1000000 )); (( dt<best )) && best=$dt; done
  echo "$best"; }

printf '%-6s %13s %14s | %13s %14s | %12s\n' N "extC->C" "extC+gcc" "rustc --emit=md" "rustc -O" "gcc(C 同形)"
printf '%s\n' "-------------------------------------------------------------------------------------------"
for n in ${NS:-300 1000 3000}; do
  gen_extc "$n" "$OUT/r$n.extc"; gen_rust "$n" "$OUT/r$n.rs"
  a=$(ms $EXTC "$OUT/r$n.extc" -o "$OUT/r$n.extc.c")
  b=$(ms $EXTC --run "$OUT/r$n.extc")
  r1=$(ms $RUSTC --edition 2021 --emit=metadata -o "$OUT/r$n.rmeta" "$OUT/r$n.rs")
  r2=$(ms $RUSTC --edition 2021 -O -o "$OUT/r$n.bin" "$OUT/r$n.rs")
  printf '%-6s %10s ms %11s ms | %10s ms %11s ms | %9s ms\n' "$n" "$a" "$b" "$r1" "$r2" "-"
done
echo
echo "（各列 3 次取最快；'extC->C' = 只做前端；'rustc --emit=metadata' = 只做前端+类型检查；"
echo "  'rustc -O' 与 'extC+gcc' 都是端到端（含后端优化）✓）"
