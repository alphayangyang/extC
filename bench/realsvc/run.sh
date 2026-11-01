#!/usr/bin/env bash
#
# Four-axis cross-language benchmark: extC against C++, Go, and Rust on one
# shared dataset and one shared client.
#
# The workloads are documented in SPEC.md; every arm reads data/log.tsv and
# prints one line per workload, whose checksum must match across languages.
# This script builds the arms, runs each workload pinned to a fixed core set,
# and writes RESULTS.tsv (plus a per-run log in out/).
#
# Usage:
#   ./run.sh                     # build what is missing, run 5 repetitions
#   ./run.sh -n 3                # repetitions
#   ./run.sh -c 4                # core for single-threaded workloads
#   ./run.sh -m "etl parmix"     # subset of workloads
#   ./run.sh -l "extc cpp"       # subset of arms
#   ./run.sh -b                  # build only
#   ./run.sh -r                  # report from an existing RESULTS.tsv
#
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

reps=5
core=0
reqs=200000
conns=64
depth=8
port=9200
workloads="etl sessions parmix serve"
langs="cpp extc go rust"
do_build=1
do_run=1

while getopts "n:c:m:l:p:bn:r" opt; do
  case $opt in
    n) reps=$OPTARG ;;
    c) core=$OPTARG ;;
    m) workloads=$OPTARG ;;
    l) langs=$OPTARG ;;
    b) do_run=0 ;;
    r) do_build=0; do_run=0 ;;
    *) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
  esac
done

mkdir -p build out
data=data/log.tsv
rows=$((2000000))

say() { printf '%s\n' "$*"; }

# ---------------------------------------------------------------- dataset ----

if [ ! -f "$data" ]; then
  say "== generating $data ($rows rows)"
  cc -O2 -std=c11 -o build/gen gen/gen.c
  ./build/gen "$data" "$rows"
fi
say "== dataset $(wc -l < "$data") rows, $(stat -c %s "$data") bytes"

# ------------------------------------------------------------------ build ----

build_cpp() {
  g++ -O2 -std=c++17 -pthread -o build/realsvc_cpp cpp/main.cpp
}

build_client() {
  cc -O2 -std=c11 -o build/client client/client.c
}

build_extc() {
  ../../build/extc -w --no-line-map -o build/main.c extc/main.extc
  cc -O2 -std=c11 -fwrapv -pthread -o build/realsvc_extc build/main.c
}

build_go() {
  [ -x go/build.sh ] && (cd go && ./build.sh) || say "!! go/build.sh missing"
}

build_rust() {
  [ -x rust/build.sh ] && (cd rust && ./build.sh) || say "!! rust/build.sh missing"
}

if [ "$do_build" = 1 ]; then
  for l in $langs; do
    case $l in
      cpp)  say "== build cpp";  build_cpp ;;
      extc) say "== build extc"; build_extc ;;
      go)   say "== build go";   build_go ;;
      rust) say "== build rust"; build_rust ;;
    esac
  done
  build_client
fi

if [ "$do_run" = 0 ]; then
  say "== build only"
  exit 0
fi

# -------------------------------------------------------------- execution ----

bin_of() {
  case $1 in
    cpp)  echo build/realsvc_cpp ;;
    extc) echo build/realsvc_extc ;;
    go)   echo build/realsvc_go ;;
    rust) echo build/realsvc_rust ;;
  esac
}

# Peak resident set of one process, in kilobytes.
peak_kb() {
  /usr/bin/time -f %M -o "$1" "${@:2}" >/dev/null 2>>"$1.err" || true
  head -1 "$1"
}

# work_ms is the workload time the arm itself reports, in milliseconds with one
# decimal: the ingest is excluded so the numbers compare across languages.
run_case() {
  local lang=$1 workload=$2 extra=$3 label=${4:-$2} cpus=${5:-$core}
  local tag=${label//\//-}
  local bin; bin=$(bin_of "$lang")
  if [ ! -x "$bin" ]; then say "-- $lang/$workload: $bin not built"; return 0; fi
  local i line
  local log="out/$lang-$tag.log"
  : > "$log"
  for i in $(seq 1 "$reps"); do
    line=$(EXTC_PAR_THREADS="${extra:-1}" taskset -c "$cpus" "$bin" "$workload" "$data" $extra 2>/dev/null | tail -1)
    local wms
    wms=$(printf '%s\n' "$line" | grep -o 'work_ms=[0-9.]*' | head -1 | cut -d= -f2)
    [ -n "$wms" ] || { say "-- $lang/$workload: no work_ms in: $line"; continue; }
    printf '%s\t%s\t%s\t%s\t%s\n' "$label" "$lang" "$i" "$wms" "-" >> RESULTS.tsv
    printf '%s\t%s\t%s\n' "$label" "$lang" "$line" >> "$log"
  done
  local kb
  kb=$(peak_kb "out/$lang-$tag.rss" env EXTC_PAR_THREADS="${extra:-1}" taskset -c "$cpus" "$bin" "$workload" "$data" $extra)
  printf '%s\t%s\tpeak\t0\t%s\n' "$label" "$lang" "$kb" >> RESULTS.tsv
  say "-- $lang/$label: $(grep -o "work_ms=[0-9.]*" "$log" | head -1)"
}

# The serve workload is a server process plus the shared client: the number that
# compares across languages is the client's completion time for a fixed request
# count and the throughput it reports.
serve_once() {
  local lang=$1 rep=$2
  local bin; bin=$(bin_of "$lang")
  if [ ! -x "$bin" ]; then say "-- $lang/serve: $bin not built"; return 0; fi
  local line secs reqps bad srvline srvcs i
  port=$((port + 1))
  taskset -c "$core" "$bin" serve "$data" "$port" "$reqs" > "out/$lang-serve.server.log" 2>&1 &
  local srv=$!
  for i in $(seq 1 100); do
    grep -q serve_ready "out/$lang-serve.server.log" 2>/dev/null && break
    kill -0 "$srv" 2>/dev/null || break
    sleep 0.1
  done
  if ! grep -q serve_ready "out/$lang-serve.server.log" 2>/dev/null; then
    say "-- $lang/serve/$rep: server did not come up ($(tail -1 "out/$lang-serve.server.log" 2>/dev/null))"
    kill "$srv" 2>/dev/null || true
    wait "$srv" 2>/dev/null || true
    return 0
  fi
  taskset -c "$((core + 1))-$((core + 3))" ./build/client "$data" "$port" "$reqs" "$conns" "$depth" \
    > "out/$lang-serve.client.log" 2>&1 || true
  kill "$srv" 2>/dev/null || true
  wait "$srv" 2>/dev/null || true
  line=$(cat "out/$lang-serve.client.log")
  secs=$(printf '%s\n' "$line" | grep -o ' ms=[0-9.]*' | head -1 | cut -d= -f2)
  reqps=$(printf '%s\n' "$line" | grep -o 'reqps=[0-9.]*' | head -1 | cut -d= -f2)
  bad=$(printf '%s\n' "$line" | grep -o 'bad=[0-9]*' | head -1 | cut -d= -f2)
  srvline=$(grep '^serve ' "out/$lang-serve.server.log" || true)
  srvcs=$(printf '%s\n' "$srvline" | grep -oE '(^| )cs=[0-9]+' | head -1 | grep -oE '[0-9]+')
  [ -n "$secs" ] || { say "-- $lang/serve: no client line"; return 0; }
  printf 'serve\t%s\t%s\t%s\tbad=%s\treqps=%s\tserver_cs=%s\n' \
    "$lang" "$rep" "$secs" "$bad" "$reqps" "$srvcs" >> "out/serve-$lang.tsv"
  cp "out/$lang-serve.client.log" "out/$lang-serve-$rep.client.log"
  cp "out/$lang-serve.server.log" "out/$lang-serve-$rep.server.log"
  say "-- $lang/serve/$rep: reqps=$reqps bad=$bad"
}

# The serve arms are interleaved (one repetition of every arm, then the next
# repetition) so that machine drift over the run does not bias one arm's median.
run_serve_all() {
  local rep l
  for rep in $(seq 1 "$reps"); do
    for l in $langs; do serve_once "$l" "$rep"; done
  done
}

rm -f RESULTS.tsv out/serve-*.tsv
mkdir -p out
printf 'workload\tlanguage\trun\twork_ms\tpeak_rss_kb\n' > RESULTS.tsv

say "== running (${reps} repetitions, core $core, data $data)"
for w in $workloads; do
  case $w in
    etl)
      for l in $langs; do run_case "$l" etl ""; done ;;
    sessions)
      for l in $langs; do run_case "$l" sessions ""; done ;;
    parmix)
      for t in 1 8 24; do
        for l in $langs; do run_case "$l" parmix "$t" "parmix/$t" "0-$((t - 1))"; done
      done ;;
    serve)
      run_serve_all ;;
  esac
done

say "== wrote RESULTS.tsv"
