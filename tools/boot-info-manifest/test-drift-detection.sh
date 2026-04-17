#!/usr/bin/env bash
# ============================================================================
# test-drift-detection.sh -- regress-test compare.sh's failure behavior.
#
# The manifest compare step (tools/boot-info-manifest/compare.sh) protects the
# kernel <-> bootloader ABI by reporting the first field that diverges between
# build/boot-info-abi.kernel.json and build/boot-info-abi.mirror.json. If
# compare.sh itself regresses (typo in the mismatch check, JSON parser bug,
# wrong field ordering), real drift would ship silently. This script builds
# intentionally-broken mirror fixtures from the real boot_info_mirror.h and
# verifies compare.sh reports each expected first-mismatch field by name.
#
# Each case mutates the real mirror header via sed into a tempfile, compiles
# a one-off mirror dumper against the mutated fixture, runs compare.sh against
# the real kernel JSON, and asserts:
#   1. compare.sh exits non-zero.
#   2. Output contains "FAIL boot_info ABI manifest: field #N 'NAME' diverges".
#   3. NAME matches the case's expected field (so a compare.sh bug that always
#      reports the wrong field would be caught).
#
# Usage:
#   bash tools/boot-info-manifest/test-drift-detection.sh              Full run
#   bash tools/boot-info-manifest/test-drift-detection.sh --quiet      Summary only
#   bash tools/boot-info-manifest/test-drift-detection.sh --help
#
# Exit codes:
#   0 = all drift scenarios detected as expected
#   1 = one or more scenarios failed (drift slipped through, or wrong field)
#   2 = prerequisite missing (kernel JSON not built, host compiler absent)
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
MIRROR_HDR="$REPO_ROOT/src/boot/uefi/boot_info_mirror.h"
DUMPER_SRC="$REPO_ROOT/tools/boot-info-manifest/dump-mirror.c"
COMPARE_SH="$REPO_ROOT/tools/boot-info-manifest/compare.sh"
KERNEL_JSON="$BUILD_DIR/boot-info-abi.kernel.json"

QUIET=0
for arg in "$@"; do
    case "$arg" in
        -h|--help)
            sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        -q|--quiet) QUIET=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# ---- Colors ----
if [ "$QUIET" = "0" ] && [ -t 1 ]; then
    RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; CYAN=$'\033[0;36m'; DIM=$'\033[0;90m'; NC=$'\033[0m'
else
    RED=''; GREEN=''; CYAN=''; DIM=''; NC=''
fi

# ---- Prereqs ----
HOST_CC="${HOST_CC:-gcc}"
if ! command -v "$HOST_CC" >/dev/null 2>&1; then
    echo "error: host compiler '$HOST_CC' not found; set HOST_CC or install gcc" >&2
    exit 2
fi
if [ ! -f "$KERNEL_JSON" ]; then
    echo "error: $KERNEL_JSON missing -- run 'bash scripts/build.sh' first" >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 required for compare.sh" >&2
    exit 2
fi

if ! TMPDIR="$(mktemp -d -t boot-info-drift.XXXXXX 2>/dev/null)"; then
    echo "error: mktemp -d failed under \$TMPDIR=${TMPDIR:-/tmp}; read-only filesystem or quota?" >&2
    exit 2
fi
if [ -z "$TMPDIR" ] || [ ! -d "$TMPDIR" ]; then
    echo "error: mktemp returned '$TMPDIR' but the directory does not exist" >&2
    exit 2
fi
# Only install the cleanup trap after TMPDIR is proven valid so rm -rf
# never runs against an empty variable if mktemp partially succeeded.
trap 'rm -rf "$TMPDIR"' EXIT

PASS=0
FAIL=0
FAILURES=()

t_pass() { PASS=$((PASS + 1)); [ "$QUIET" = "0" ] && echo "  ${GREEN}PASS${NC}  $1"; }
t_fail() {
    FAIL=$((FAIL + 1))
    FAILURES+=("$1")
    [ "$QUIET" = "0" ] && echo "  ${RED}FAIL${NC}  $1"
    [ -n "${2:-}" ] && [ "$QUIET" = "0" ] && echo "        ${DIM}${2}${NC}"
}

# ---- run_case ----
# $1    = case name
# $2    = sed expression to apply to the mirror header (single -e argument)
# $3    = expected first-mismatch field name (string inside '...' quotes in
#         compare.sh's FAIL line; grep -E regex)
# $4... = one or more preimage patterns (grep -E), each of which MUST match
#         exactly once in the real mirror header. The harness checks every
#         listed preimage -- cases with multi-substitution sed expressions
#         MUST list one preimage per distinct declaration the sed rewrites,
#         so a future mirror edit that duplicates any target trips the guard
#         and fails the case rather than silently mutating multiple lines.
run_case() {
    local name="$1" sed_expr="$2" expected_field="$3"
    shift 3
    local work="$TMPDIR/$name"
    if ! mkdir -p "$work"; then
        t_fail "$name" "mkdir -p $work failed -- temp filesystem issue?"
        return
    fi

    # Pre-flight: every mutation target must exist in the real mirror
    # exactly once, or the case no longer tests what its comment claims.
    # Cases with two or three sed substitutions (swaps) list one preimage
    # per distinct declaration the sed rewrites.
    local preimage hit_count
    local preimage_count=$#
    for preimage in "$@"; do
        hit_count="$(grep -Ec "$preimage" "$MIRROR_HDR" || true)"
        if [ "$hit_count" != "1" ]; then
            t_fail "$name" \
                "preimage pattern /$preimage/ matched $hit_count lines in boot_info_mirror.h; expected exactly 1. Update the case so every sed target pin-points one declaration."
            return
        fi
    done

    # Copy and mutate the mirror header. The dumper sees a directory where
    # boot_info_mirror.h is the mutated fixture but dump-common.h etc. still
    # come from the real tools directory.
    if ! sed -E "$sed_expr" "$MIRROR_HDR" > "$work/boot_info_mirror.h"; then
        t_fail "$name" "sed failed while writing fixture header"
        return
    fi
    if cmp -s "$MIRROR_HDR" "$work/boot_info_mirror.h"; then
        t_fail "$name" "sed produced identical output -- mutation pattern did not match; update the case"
        return
    fi

    # Post-flight: the sed expression must have rewritten EXACTLY
    # preimage_count lines. The preimage guard above checks what the case
    # CLAIMS to mutate (anchored regex); this guard checks what sed
    # ACTUALLY mutated by counting position-wise line differences (not
    # diff -- diff's alignment heuristic hides position swaps between
    # otherwise-identical lines, so this uses awk to compare line N of
    # each file directly). Without this guard, a case could list a strict
    # preimage while the sed expression uses a looser match and silently
    # rewrites extra lines -- recreating the compound-fixture failure mode.
    local changed_lines
    changed_lines="$(awk 'NR==FNR { a[NR]=$0; next } { if (a[FNR] != $0) n++ } END { print n+0 }' \
        "$MIRROR_HDR" "$work/boot_info_mirror.h")"
    if [ "$changed_lines" != "$preimage_count" ]; then
        t_fail "$name" \
            "sed rewrote $changed_lines line(s) but preimage list claimed $preimage_count target(s); mutation scope exceeded the guarded preimages (or the mutation's replacement text changed line count)"
        return
    fi

    local dumper="$work/dump-fixture"
    # Compile against the mutated header by putting $work first on the
    # include path. -I tools/boot-info-manifest picks up dump-common.h +
    # dump-fields.inc unmodified.
    if ! "$HOST_CC" -O2 \
            -I "$work" \
            -I "$REPO_ROOT/tools/boot-info-manifest" \
            -o "$dumper" "$DUMPER_SRC" 2> "$work/cc.log"; then
        t_fail "$name" "fixture compile failed: $(tail -3 "$work/cc.log" | tr '\n' ' ')"
        return
    fi

    local mirror_json="$work/mirror.json"
    if ! "$dumper" > "$mirror_json" 2> "$work/dumper.log"; then
        t_fail "$name" "fixture dumper failed: $(tail -1 "$work/dumper.log")"
        return
    fi

    # Run compare.sh; expect non-zero exit.
    local cmp_out cmp_rc
    cmp_out="$(bash "$COMPARE_SH" "$KERNEL_JSON" "$mirror_json" 2>&1)" && cmp_rc=$? || cmp_rc=$?
    if [ "$cmp_rc" = "0" ]; then
        t_fail "$name" "compare.sh passed (exit 0) on a mutated fixture -- drift detection regressed"
        return
    fi

    # Assert: FAIL line contains the expected field name.
    if ! printf '%s\n' "$cmp_out" | grep -qE "^FAIL boot_info ABI manifest: field #[0-9]+ '${expected_field}' diverges"; then
        t_fail "$name" \
            "compare.sh failed but did not name '$expected_field'; output: $(printf '%s\n' "$cmp_out" | head -1)"
        return
    fi

    t_pass "$name (compare.sh flagged '$expected_field' on mutated mirror)"
}

# ---- Cases ----
[ "$QUIET" = "0" ] && echo "${CYAN}[boot_info ABI drift detection]${NC}"

# Case 1: element-struct same-size swap -- swap `type` and `uefi_memory_type`
# inside struct boot_mmap_entry. Both UINT32, same total struct size; the
# whole-array row F(mmap) passes, but F(mmap[0].type) catches it.
# TWO sed substitutions -> TWO preimages (both sides of the swap) anchored
# by their trailing comments so a future additional declaration on either
# side trips the guard and fails the case.
run_case "mmap-type-swap" \
    's|UINT32 type;\s+/\* simplified.*\*/\s*$|UINT32 uefi_memory_type; /* MUTATED: swapped with type */|; s|UINT32 uefi_memory_type;\s+/\* original EFI_MEMORY_TYPE.*\*/\s*$|UINT32 type; /* MUTATED: swapped with uefi_memory_type */|' \
    "mmap\[0\]\.type" \
    '^\s*UINT32 type;\s+/\* simplified' \
    '^\s*UINT32 uefi_memory_type;\s+/\* original EFI_MEMORY_TYPE'

# Case 2: scalar-array element-type change -- dma_pages[16] UINT64 -> UINT32[32].
# Same total 128 bytes; whole-array row F(usb_controller.dma_pages) passes;
# F(usb_controller.dma_pages[0]) sentinel catches it by size (8 vs 4).
# Preimage pins the full array declaration including the macro bound.
run_case "dma-pages-element-shrink" \
    's|UINT64  dma_pages\[BOOT_USB_MAX_DMA_PAGES\];|UINT32  dma_pages[BOOT_USB_MAX_DMA_PAGES * 2]; /* MUTATED: u64[16] -> u32[32] */|' \
    "usb_controller\.dma_pages\[0\]" \
    '^\s*UINT64  dma_pages\[BOOT_USB_MAX_DMA_PAGES\];$'

# Case 3: element-struct same-size swap inside a nested nested -- swap
# `address` and `attributes` in boot_usb_endpoint (both UINT8). The mutation
# uses a three-step intermediate-name rename. TWO distinct preimages (the
# third sed step rewrites the intermediate name which does not exist in the
# real mirror) -- if either the `address` or `attributes` declaration gains
# a duplicate in the future, the guard fails the case.
run_case "endpoint-address-swap" \
    's|UINT8   address;|UINT8   attributes_MUT;|; s|UINT8   attributes;|UINT8   address;|; s|UINT8   attributes_MUT;|UINT8   attributes;|' \
    "usb_devices\[0\]\.endpoints\[0\]\.address" \
    '^\s*UINT8   address;$' \
    '^\s*UINT8   attributes;$'

# Case 4: nested-struct element-type shrink in boot_gop_mode -- replace
# `UINT32  pixels_per_scanline` with `UINT16 pixels_per_scanline; UINT16 pad`.
# boot_gop_mode stays 16 bytes (4 becomes 2+2); subsequent field offsets
# unchanged so no pinned count offset shifts; but the size of
# gop_modes[0].pixels_per_scanline goes 4 -> 2 and the manifest catches it.
# Field name is unique across the whole struct, so the plain declaration
# pattern suffices.
run_case "gop-mode-pixels-per-scanline-shrink" \
    's|UINT32  pixels_per_scanline;|UINT16  pixels_per_scanline; UINT16  _mut_pad;|' \
    "gop_modes\[0\]\.pixels_per_scanline" \
    '^\s*UINT32  pixels_per_scanline;$'

# Case 5: nested-struct element-type shrink in boot_rt_mem_entry -- replace
# `UINT64 num_pages` with `UINT32 num_pages; UINT32 _mut_pad`. Struct stays
# 32 bytes, subsequent field offsets unchanged, but
# size(rt_mmap[0].num_pages) goes 8 -> 4. Preimage pins the declaration so a
# future runtime-memory struct that reuses num_pages cannot silently expand
# the mutation target.
run_case "rt-mmap-num-pages-shrink" \
    's|UINT64 num_pages;|UINT32 num_pages; UINT32 _mut_pad;|' \
    "rt_mmap\[0\]\.num_pages" \
    '^\s*UINT64 num_pages;$'

# ---- Negative control ----
# Identical copy of the real mirror must PASS compare.sh. This protects
# against a silly bug where every case incorrectly fails regardless of the
# mutation.
control_dir="$TMPDIR/control"
mkdir -p "$control_dir"
cp "$MIRROR_HDR" "$control_dir/boot_info_mirror.h"
if ! "$HOST_CC" -O2 \
        -I "$control_dir" \
        -I "$REPO_ROOT/tools/boot-info-manifest" \
        -o "$control_dir/dump-fixture" "$DUMPER_SRC" 2> "$control_dir/cc.log"; then
    t_fail "control (unmutated mirror)" "compile failed: $(tail -1 "$control_dir/cc.log")"
else
    "$control_dir/dump-fixture" > "$control_dir/mirror.json"
    if bash "$COMPARE_SH" "$KERNEL_JSON" "$control_dir/mirror.json" >/dev/null 2>&1; then
        t_pass "control (unmutated mirror passes compare.sh)"
    else
        t_fail "control (unmutated mirror)" \
            "unmutated mirror disagrees with kernel JSON -- either the real mirror drifted or test harness broke"
    fi
fi

# ---- Summary ----
total=$((PASS + FAIL))
if [ "$FAIL" = "0" ]; then
    echo "${GREEN}PASS${NC} boot_info ABI drift detection: $total/$total scenarios produced the expected first-mismatch field"
    exit 0
else
    echo "${RED}FAIL${NC} boot_info ABI drift detection: $FAIL/$total scenarios regressed"
    for failure in "${FAILURES[@]}"; do
        echo "  - $failure"
    done
    exit 1
fi
