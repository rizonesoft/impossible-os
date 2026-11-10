#!/usr/bin/env bash
# Build and run the host-side fuzzers for parsers of untrusted input.
#
# Each target is the kernel's or loader's own source compiled for the host with
# libFuzzer + AddressSanitizer + UndefinedBehaviorSanitizer. A crash, a read
# past the input or undefined behaviour stops the target and leaves the
# reproducer under the output directory.
#
#   bash tools/fuzz/run.sh                    every target, 20 s each
#   FUZZ_SECONDS=600 bash tools/fuzz/run.sh   a longer soak
#   bash tools/fuzz/run.sh boot_entries       one target
#   bash tools/fuzz/run.sh --repro <target> <file>   replay one input
#
# Exit: 0 every target ran clean, 1 a target found a defect, 2 setup failure.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$REPO_ROOT/tools/fuzz"
OUT="${FUZZ_OUT:-${TMPDIR:-/tmp}/impossible-os-fuzz}"
SECONDS_EACH="${FUZZ_SECONDS:-20}"
CLANG="${FUZZ_CC:-clang-19}"

command -v "$CLANG" >/dev/null 2>&1 || {
    echo "fuzz: no $CLANG on PATH (bash scripts/setup.sh)" >&2; exit 2; }

SAN="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
BASE="-std=gnu11 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -Werror -Wno-unused-function"

# target -> the sources that make it. Kernel sources take -I include with the
# host <stdint.h> forced in first: kernel/types.h guards each typedef for
# exactly this coexistence (tools/memmap-check uses the same route).
build_target() {
    case "$1" in
        boot_entries)
            # shellcheck disable=SC2086
            "$CLANG" $BASE $SAN -o "$OUT/$1" "$HERE/fuzz_boot_entries.c" ;;
        security_descriptor)
            # shellcheck disable=SC2086
            "$CLANG" $BASE $SAN -I "$REPO_ROOT/include" \
                -include stdint.h -include stddef.h \
                -o "$OUT/$1" "$HERE/fuzz_security_descriptor.c" \
                "$REPO_ROOT/src/kernel/security/acl.c" \
                "$REPO_ROOT/src/kernel/security/sid.c" ;;
        *) echo "fuzz: unknown target '$1'" >&2; exit 2 ;;
    esac
}
ALL_TARGETS="boot_entries security_descriptor"

mkdir -p "$OUT"

if [ "${1:-}" = "--repro" ]; then
    [ $# -eq 3 ] || { echo "usage: run.sh --repro <target> <file>" >&2; exit 2; }
    build_target "$2"
    exec "$OUT/$2" "$3"
fi

TARGETS="${*:-$ALL_TARGETS}"
FAILED=0
for t in $TARGETS; do
    build_target "$t"
    work="$OUT/corpus-$t"; art="$OUT/crash-$t-"
    mkdir -p "$work"
    log="$OUT/$t.log"
    # The checked-in corpus is read-only seed; discoveries stay in $work.
    if "$OUT/$t" -max_total_time="$SECONDS_EACH" -timeout=10 -rss_limit_mb=2048 \
            -artifact_prefix="$art" -print_final_stats=1 \
            "$work" "$HERE/corpus/$t" >"$log" 2>&1; then
        echo "  PASS  $t  ($(grep -E 'stat::number_of_executed_units' "$log" | awk '{print $2}') runs in ${SECONDS_EACH}s)"
    else
        FAILED=1
        echo "  FAIL  $t  (log: $log)"
        grep -E 'ERROR: |SUMMARY: |runtime error|Test unit written' "$log" | head -8 | sed 's/^/    | /'
    fi
done
exit "$FAILED"
