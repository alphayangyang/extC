#!/usr/bin/env bash
# Regression suite for four bugs in the emitted C runtime text.
#
# Every case compiles a runtime emitter out of `src/` and exercises the text it produces, so
# the assertions are about the code that really runs in generated files -- not about a
# reimplementation kept in this directory.
#
#   bug 1  `reclaimEmitRuntime` must report malformed `#if`/`#else`/`#endif` text as a
#          generator bug instead of silently mis-emitting, and must not write past its
#          16-layer table on deeper nesting.
#   bug 2  every slot-store helper must treat a NULL slot as "no slot to update" (no-op);
#          before the fix each one dereferenced the NULL through its fallback.
#   bug 3  `extc_unix_listen` / `extc_unix_connect` must close their fd on every error path.
#   bug 4  `extc_sock_pair` must reject a NULL `out` with -1 instead of dereferencing it.
#
# Wired into tests/run.sh; the whole suite is about 2 s.
set -u
cd "$(dirname "$0")/../.."
CC=${CC:-cc}
CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wpedantic"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pass=0; fail=0
ok()  { printf '  ok   %s\n' "$1"; pass=$((pass+1)); }
bad() { printf '  FAIL %s\n' "$1"; fail=$((fail+1)); }

echo "== bug 2: a NULL slot is a no-op for every slot-store entry =="
if "$CC" $CFLAGS -Isrc/base -Isrc/back tests/reclaim/emit_main.c src/back/reclaim.c \
     src/base/base.c -o "$work/reclaim_emit" 2>"$work/emit.cc"; then
    ok "reclaim runtime emitter compiles"
    emit() {  # emit <outfile> [ENV=VAL ...]
        local out=$1; shift
        env "$@" "$work/reclaim_emit" >"$out"
    }
    for tier in "default:" "counting:EXTC_RC_NOCOUNT=0" "localstore:EXTC_RC_LOCALSTORE=1"; do
        name=${tier%%:*}
        envs=${tier#*:}
        dir=$work/probe_$name
        mkdir -p "$dir"
        if emit "$dir/extc_reclaim_runtime.inc" $envs; then
            cp tests/runtime-bugs/null_slot_probe.c "$dir/"
            defs=""
            [ "$name" = localstore ] && defs="-DPROBE_LOCAL=1"
            if "$CC" -std=c11 -O1 -g -Wall -Wextra -Werror $defs -I"$dir" \
                 "$dir/null_slot_probe.c" -o "$dir/probe" 2>"$work/$name.cc"; then
                if out=$("$dir/probe" 2>&1); then
                    ok "$name tier: every slot-store entry survives a NULL slot ($(echo "$out" | grep -c 'returned') entries)"
                else
                    bad "$name tier: NULL-slot probe crashed -- $(echo "$out" | head -2 | tr '\n' ' ')"
                fi
            else
                bad "$name tier: probe does not compile -- $(head -2 "$work/$name.cc" | tr '\n' ' ')"
            fi
        else
            bad "$name tier: emitter failed"
        fi
    done
else
    bad "reclaim runtime emitter does not compile -- $(head -2 "$work/emit.cc" | tr '\n' ' ')"
fi

echo "== bug 1: malformed directives are reported as a generator bug (not silently emitted) =="
if command -v python3 >/dev/null 2>&1; then
    for mode in deep stray_endif stray_else unterminated; do
        src=$work/reclaim_$mode.c
        if ! python3 tests/runtime-bugs/inject_rt_directives.py src/back/reclaim.c "$mode" "$src"; then
            bad "$mode: cannot inject a malformed directive into RECLAIM_RT"; continue
        fi
        if "$CC" $CFLAGS -Isrc/base -Isrc/back tests/reclaim/emit_main.c "$src" \
             src/base/base.c -o "$work/emit_$mode" 2>"$work/$mode.cc"; then
            "$work/emit_$mode" >/dev/null 2>"$work/$mode.err"; rc=$?
            if [ "$rc" -ne 0 ] && grep -q "extc: internal error -- reclaim runtime text:" "$work/$mode.err"; then
                ok "$mode: reported as a generator bug and exited ($(head -1 "$work/$mode.err"))"
            else
                bad "$mode: exit $rc with no generator-bug report (silent emission = the old bug)"
            fi
        else
            bad "$mode: the injected emitter does not compile -- $(head -2 "$work/$mode.cc" | tr '\n' ' ')"
        fi
    done
else
    bad "python3 is missing, skipping the bug-1 injection cases"
fi

echo "== bug 3/4: the event layer's socket helpers =="
if "$CC" $CFLAGS -Isrc/base -Isrc/back tests/runtime-bugs/coro_emit_main.c \
     src/back/coroutine.c src/base/base.c -o "$work/coro_emit" 2>"$work/coro.cc"; then
    dir=$work/event
    mkdir -p "$dir"
    if "$work/coro_emit" >"$dir/extc_event_runtime.inc"; then
        ok "event-layer emitter compiles and writes the runtime text"
        cp tests/runtime-bugs/sock_null_probe.c "$dir/"
        if "$CC" -std=c11 -O1 -g -Wall -Wextra -Werror -I"$dir" \
             "$dir/sock_null_probe.c" -o "$dir/sock_probe" 2>"$work/sock.cc"; then
            if out=$("$dir/sock_probe" 2>&1); then
                ok "bug 4: extc_sock_pair(NULL) returns -1, success path intact ($(echo "$out" | tail -1))"
            else
                bad "bug 4: extc_sock_pair(NULL) crashed or misbehaved -- $(echo "$out" | head -2 | tr '\n' ' ')"
            fi
        else
            bad "bug 4: probe does not compile -- $(head -2 "$work/sock.cc" | tr '\n' ' ')"
        fi
        cp tests/runtime-bugs/unix_bad_path_probe.c "$dir/"
        if "$CC" -std=c11 -O1 -g -Wall -Wextra -Werror -I"$dir" \
             "$dir/unix_bad_path_probe.c" -o "$dir/unix_probe" 2>"$work/unix.cc"; then
            if out=$("$dir/unix_probe" 2>&1); then
                ok "bug 3: 10400 rejected calls leave the fd count unchanged ($(echo "$out" | tail -1))"
            else
                bad "bug 3: the invalid-path loop leaks fds -- $(echo "$out" | head -3 | tr '\n' ' ')"
            fi
        else
            bad "bug 3: probe does not compile -- $(head -2 "$work/unix.cc" | tr '\n' ' ')"
        fi
    else
        bad "event-layer emitter wrote no text"
    fi
else
    bad "event-layer emitter does not compile -- $(head -2 "$work/coro.cc" | tr '\n' ' ')"
fi

echo
echo "passed $pass, failed $fail"
exit "$fail"
