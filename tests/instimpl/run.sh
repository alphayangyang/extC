#!/usr/bin/env bash
# 实例化类型上的 impl / dyn：编译 + gcc 零告警 + 运行输出比对
set -u
here=$(cd "$(dirname "$0")" && pwd)
EXTC="${EXTC:-$(cd "$here/../.." && pwd)/build/extc}"
src() { printf '%s/%s' "$here" "$1"; }   # 用例文件按脚本自身位置找，CWD 无关
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); printf '  ok   %-34s %s\n' "$1" "$2"; }
bad() { fail=$((fail+1)); printf '  FAIL %-34s %s\n' "$1" "$2"; }

one() { # name file expected
  local name="$1" src="$2" want="$3"
  if ! "$EXTC" -w --no-line-map -o "$tmp/a.c" "$src" 2>"$tmp/e1"; then
    bad "$name" "extC: $(head -1 "$tmp/e1")"; return
  fi
  if ! gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror -o "$tmp/a" "$tmp/a.c" 2>"$tmp/e2"; then
    bad "$name" "gcc: $(head -1 "$tmp/e2")"; return
  fi
  local got; got=$("$tmp/a"); local rc=$?
  [ "$got" = "$want" ] && [ $rc -eq 0 ] && ok "$name" "→ $got" || bad "$name" "期望 [$want] 实得 [$got] 退出码 $rc"
}

neg() { # name file expect-substring（必须被拒，且理由里含该串）
  local name="$1" src="$2" want="$3"
  if "$EXTC" -w --no-line-map -o "$tmp/n.c" "$src" 2>"$tmp/e3"; then
    bad "$name" "应当被拒却编过了"
  elif grep -q "$want" "$tmp/e3"; then ok "$name" "按预期被拒"
  else bad "$name" "拒绝理由不符: $(head -1 "$tmp/e3")"; fi
}

one "固有方法挂 slice<u8>"        "$(src t_inherent.extc)"  "half=4 trimmed=6"
one "trait impl 挂 slice<u8>"     "$(src t_trait.extc)"     "enc=4"
one "两实例各挂各的方法"           "$(src t_two.extc)"       "u8=1 i64=2"
one "dyn 打在实例与内建上"         "$(src t_dyn.extc)"       "4 42"
one "泛型 impl（impl<T> pair<T>）" "$(src t_gen_inherent.extc)" "get=5"
one "泛型 trait impl（impl<T> Tag for pair<T>）" "$(src t_gen_trait.extc)" "tag=1"
one "带参 trait（trait Codec<T>）" "$(src t_param_trait.extc)" "enc=7"
one "泛型方法（方法自己的 <U> 按实参推断）" "$(src t_gen_method.extc)" "5 2"
one "泛型函数返回 T 构造的实例（零值名与声明一致）" "$(src t_gen_return.extc)" "box=8 pair=3,4"
# 反向金丝雀：实现体不读 `self` 时，dyn 表保住的实现不会被"未用参数"那趟看到
# （markUnusedParams 只遍历 g.deadFuncs）⇒ 今天**必须**失败。修好那天它会变绿，这条就会报出来。
if "$EXTC" -w --no-line-map -o "$tmp/m.c" "$(src t_dyn_multi.extc)" 2>/dev/null &&
   gcc -O2 -std=c11 -fwrapv -Wall -Wextra -Werror -o "$tmp/m" "$tmp/m.c" 2>/dev/null; then
  bad "dyn 多方法（金丝雀）" "已经能编过了 ⇒ 该把这条改成 one 用例（L 债已修）"
else ok "dyn 多方法（金丝雀）" "如预期仍失败（L 债：未用 self 触发 -Werror）"; fi
neg "slice<i64> 上没有 u8 的方法" "$(src t_isolate.extc)"   "no method"
neg "未绑定参数仍被拒"             "$(src t_unbound.extc)"   "unknown type"
neg "块级参数名与声明不一致要报清楚" "$(src t_gen_badname.extc)" "type parameters must match"
neg "带参 trait 必须给实参" "$(src t_param_arity.extc)" "has 1 type parameter"
neg "带参 trait 的签名按实参替换后要比" "$(src t_param_sig.extc)" "parameter 2 of"
neg "泛型方法推断不出要报清楚" "$(src t_gen_method_bad.extc)" "cannot infer type parameter"
echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
