#!/usr/bin/env bash
# bench/gc —— GC 敏感形状横评。
# 口径见 SHAPES.md：时间 = best of N，RSS = 峰值；GC 语言跑"开/关 GC"两档，
# 差值就是**纯 GC 代价**（这比"某语言慢几倍"有意义）✓
#
# 用法： bash bench/gc/run.sh [形状名…]     例：bash bench/gc/run.sh churn rebuild
set -u
cd "$(dirname "$0")"
N=${N:-5}
B=build
shapes=${*:-"churn rebuild list tree queries hash"}

# row <标签> <命令…>   —— 命令里的空格用数组传，不要用字符串拼 ✗
row() {
    local label="$1"; shift
    local out ms rss rc
    out=$(N=$N python3 measure.py "$@" 2>/dev/null) || { printf '  %-24s ERROR\n' "$label"; return; }
    ms=$(echo "$out" | cut -f1); rss=$(echo "$out" | cut -f2); rc=$(echo "$out" | cut -f3)
    [ "$rc" = "0" ] || label="$label (rc=$rc)"
    printf '  %-24s %9s ms %9s MB\n' "$label" "$ms" "$rss"
}

for s in $shapes; do
    case $s in
    churn)
        echo "== 形状 1 · churn —— 3200 万次分配（两槽交替覆盖）=="
        row "C   malloc+free"      ./$B/churn_c
        row "C   bump arena（同模型）" ./$B/churn_c_arena
        row "Rust Box+drop"        ./$B/churn_rs
        row "Go  默认GC"           ./$B/churn_go
        row "Go  GOGC=off"         env GOGC=off ./$B/churn_go
        row "Java 默认GC"          java -cp $B Churn
        row "Java SerialGC"        java -XX:+UseSerialGC -cp $B Churn
        row "extC arena bump"      ./$B/churn_extc
        ;;
    rebuild)
        echo "== 形状 2 · rebuild —— 2 万轮 × 每轮 1000 节点链 =="
        row "C   malloc+free"      ./$B/rebuild_c
        row "Rust Box 链+drop"     ./$B/rebuild_rs
        row "Go  默认GC"           ./$B/rebuild_go
        row "Go  GOGC=off"         env GOGC=off ./$B/rebuild_go
        row "Java 默认GC"          java -cp $B Rebuild
        row "Java SerialGC"        java -XX:+UseSerialGC -cp $B Rebuild
        row "extC arena 出块即还"  ./$B/rebuild_extc
        ;;
    *) echo "== 形状 $s（未实现）==" ;;
    esac
    echo
done
