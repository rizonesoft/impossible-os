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

# Kernel sources build against the kernel headers with the host's fixed-width
# types forced in first, and link tools/fuzz/host_stubs.c for klog.
KINC="-I $REPO_ROOT/include -include stdint.h -include stddef.h"
STUBS="$HERE/host_stubs.c"

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
        pe)
            # shellcheck disable=SC2086
            "$CLANG" $BASE $SAN $KINC -o "$OUT/$1" "$HERE/fuzz_pe.c" "$STUBS" \
                "$REPO_ROOT/src/kernel/pe_validate.c" ;;
        elf)
            # shellcheck disable=SC2086
            "$CLANG" $BASE $SAN $KINC -o "$OUT/$1" "$HERE/fuzz_elf.c" "$STUBS" \
                "$REPO_ROOT/src/kernel/elf_parse.c" ;;
        eif)
            # shellcheck disable=SC2086
            "$CLANG" $BASE $SAN $KINC -I "$REPO_ROOT/src" -o "$OUT/$1" \
                "$HERE/fuzz_eif.c" "$STUBS" "$REPO_ROOT/src/kernel/eif_parse.c" \
                "$REPO_ROOT/src/kernel/lz4.c" "$REPO_ROOT/src/libs/lz4/lz4.c" ;;
        fat32)
            # shellcheck disable=SC2086
            "$CLANG" $BASE $SAN $KINC -I "$REPO_ROOT/src/kernel/fs/fat32" \
                -o "$OUT/$1" "$HERE/fuzz_fat32.c" "$STUBS" \
                "$REPO_ROOT/src/kernel/fs/fat32/fat32_parse.c" ;;
        registry_hive)
            # shellcheck disable=SC2086
            "$CLANG" $BASE $SAN $KINC -o "$OUT/$1" "$HERE/fuzz_registry_hive.c" \
                "$STUBS" "$REPO_ROOT/src/kernel/registry_hive_parse.c" ;;
        boot_conf)
            # shellcheck disable=SC2086
            "$CLANG" $BASE $SAN -o "$OUT/$1" "$HERE/fuzz_boot_conf.c" \
                "$REPO_ROOT/src/boot/uefi/boot_conf_lexer.c" ;;
        *) echo "fuzz: unknown target '$1'" >&2; exit 2 ;;
    esac
}
ALL_TARGETS="boot_entries boot_conf security_descriptor pe elf eif fat32 registry_hive"

mkdir -p "$OUT"

if [ "${1:-}" = "--repro" ]; then
    [ $# -eq 3 ] || { echo "usage: run.sh --repro <target> <file>" >&2; exit 2; }
    build_target "$2"
    exec "$OUT/$2" "$3"
fi

TARGETS="${*:-$ALL_TARGETS}"
FAILED=0
for t in $TARGETS; do
    if ! build_target "$t" >"$OUT/$t.build.log" 2>&1; then
        FAILED=1
        echo "  FAIL  $t  (did not build)"
        head -12 "$OUT/$t.build.log" | cut -c1-300 | sed 's/^/    | FUZZ-FAIL: /'
        continue
    fi
    # The seeds are part of the test: a fixed defect's reproducer lives there.
    # Checked against git, not the directory: a seed that an ignore rule kept
    # out of the commit exists here and nowhere else (2026-11-10, `*.bin`).
    if [ -z "$(git -C "$REPO_ROOT" ls-files "tools/fuzz/corpus/$t" 2>/dev/null)" ]; then
        FAILED=1
        echo "  FAIL  $t  (no tracked seed under tools/fuzz/corpus/$t)"
        echo "    | FUZZ-FAIL: add at least one seed and check 'git status' shows it (not ignored)"
        continue
    fi
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
        # Tagged FUZZ-FAIL so a wrapper that filters on "FAIL" keeps them, and
        # the log's tail as well: a build error or a killed process prints no
        # sanitizer line, and "FAIL" with no reason cannot be triaged from CI.
        # `|| true` on the grep: under `set -e -o pipefail` a grep with no match
        # ended this block, and the script, before the tail was printed, which
        # is exactly the case (no sanitizer line) the tail exists for.
        { { grep -E 'ERROR: |SUMMARY: |runtime error|Test unit written|error: |ABORTING' "$log" || true; } | head -8
          echo "--- first and last lines of $log ---"; head -6 "$log"; tail -14 "$log"; } \
            | cut -c1-300 | sed 's/^/    | FUZZ-FAIL: /' || true
        # The reproducer, as text, so a CI failure can be replayed locally.
        for a in "$art"*; do
            if [ -f "$a" ]; then
                echo "    | FUZZ-FAIL: reproducer $(basename "$a") base64: $(head -c 2048 "$a" | base64 -w0)"
            fi
        done
    fi
done
exit "$FAILED"
