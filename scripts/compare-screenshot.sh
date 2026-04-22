#!/usr/bin/env bash
# compare-screenshot.sh -- pixel diff two PNGs with tolerance + diff image
#
# Implements TODO-05-desktop-ui-test-framework.md the reference screenshot
# comparison section. Host-side script: consumes a reference PNG and a
# current PNG (typically from scripts/qemu-screenshot.sh), computes the
# percentage of identical pixels with a per-channel fuzz tolerance, and
# emits a highlighted diff image showing the changed pixels.
#
# Pass / fail uses a percent-identical threshold (default 95 percent), so
# routine anti-aliasing and timing wobble does not trip the gate while a
# wallpaper change or a UI regression does.
#
# Usage:
#   bash scripts/compare-screenshot.sh <reference.png> <current.png> [diff.png]
#       [--threshold <pct>] [--fuzz <pct>]
#
# Defaults:
#   diff.png     <current>.diff.png  (sibling of current)
#   --threshold  95   (require >= 95 percent identical for pass)
#   --fuzz       2    (anti-alias tolerance: per-pixel channel delta budget)
#
# Output:
#   PASS line: "Visual match: <pct>% identical (threshold: <T>%)"
#   FAIL line: "Visual MISMATCH: <pct>% identical (expected <T>%+) -- diff: <path>"
#
# Exit codes:
#   0  pass: percent identical >= threshold
#   1  precondition failure: tooling missing (ImageMagick compare/identify),
#      bad args, unparseable threshold/fuzz
#   2  input file missing or unreadable
#   3  reference and current have different dimensions (cannot diff)
#   4  PNG decode / compare run failed (ImageMagick reported error other
#      than just "pixels differ")
#   5  fail: percent identical < threshold (visual regression detected;
#      diff image was still written for inspection)

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

RED=$'\033[0;31m'
GREEN=$'\033[0;32m'
YELLOW=$'\033[1;33m'
CYAN=$'\033[0;36m'
RESET=$'\033[0m'

die() {
    local code=$1; shift
    printf '%s[compare-screenshot]%s %s\n' "$RED" "$RESET" "$*" >&2
    exit "$code"
}

info() {
    printf '%s[compare-screenshot]%s %s\n' "$CYAN" "$RESET" "$*"
}

ok() {
    printf '%s[compare-screenshot]%s %s\n' "$GREEN" "$RESET" "$*"
}

warn() {
    printf '%s[compare-screenshot]%s %s\n' "$YELLOW" "$RESET" "$*" >&2
}

usage() {
    sed -n '3,35p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-1}"
}

# --- Step 1: parse args --------------------------------------------------

REFERENCE=""
CURRENT=""
DIFF_OUT=""
THRESHOLD="95"
FUZZ="2"

while [ $# -gt 0 ]; do
    case "$1" in
        --threshold)
            [ $# -ge 2 ] || die 1 "--threshold needs a value"
            THRESHOLD="$2"; shift 2 ;;
        --threshold=*)
            THRESHOLD="${1#*=}"; shift ;;
        --fuzz)
            [ $# -ge 2 ] || die 1 "--fuzz needs a value"
            FUZZ="$2"; shift 2 ;;
        --fuzz=*)
            FUZZ="${1#*=}"; shift ;;
        -h|--help)
            usage 0 ;;
        -*)
            die 1 "unknown flag: $1 (try --help)" ;;
        *)
            if [ -z "$REFERENCE" ]; then
                REFERENCE="$1"
            elif [ -z "$CURRENT" ]; then
                CURRENT="$1"
            elif [ -z "$DIFF_OUT" ]; then
                DIFF_OUT="$1"
            else
                die 1 "too many positional args (got '$1' after reference, current, diff)"
            fi
            shift ;;
    esac
done

[ -n "$REFERENCE" ] || die 1 "missing <reference.png> argument (try --help)"
[ -n "$CURRENT" ]   || die 1 "missing <current.png> argument (try --help)"

# Validate numeric inputs. The values flow into ImageMagick command lines
# and into awk numeric coercion, so reject anything that is not a strict
# non-negative decimal (e.g. "95", "95.5", "0"). A sloppy character-class
# check accepts ".", "1.2.3", or "" which awk silently coerces to 0 and
# disables the gate. Range-check to [0, 100] for both threshold and fuzz
# since they are percentages -- outside that range ImageMagick either
# errors or produces meaningless behavior. Codex [M] adversarial review of
# the reference-comparison section.
validate_percent() {
    local name="$1" val="$2"
    if ! printf '%s' "$val" | grep -Eq '^[0-9]+(\.[0-9]+)?$'; then
        die 1 "--${name} must be a non-negative decimal (got: '${val}')"
    fi
    if [ "$(awk -v v="$val" 'BEGIN { print (v + 0 < 0 || v + 0 > 100) ? "1" : "0" }')" = "1" ]; then
        die 1 "--${name} must be in [0, 100], got: ${val}"
    fi
}
validate_percent threshold "$THRESHOLD"
validate_percent fuzz "$FUZZ"

[ -z "$DIFF_OUT" ] && DIFF_OUT="${CURRENT%.png}.diff.png"

# --- Step 2: preflight ---------------------------------------------------

for tool in compare identify; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        die 1 "required tool '$tool' not found in PATH (install: ImageMagick)"
    fi
done

[ -r "$REFERENCE" ] || die 2 "reference image not readable: $REFERENCE"
[ -r "$CURRENT" ]   || die 2 "current image not readable: $CURRENT"
[ -s "$REFERENCE" ] || die 2 "reference image is zero bytes: $REFERENCE"
[ -s "$CURRENT" ]   || die 2 "current image is zero bytes: $CURRENT"

mkdir -p "$(dirname "$DIFF_OUT")" || die 1 "cannot create diff output directory: $(dirname "$DIFF_OUT")"

# Atomic diff-artifact strategy:
#   1. Remove any pre-existing target so an aborted run never leaves a
#      stale file that looks like this run's diff.
#   2. Write to a unique temp path in the same directory.
#   3. Rename the temp into place only after compare reports a success-
#      or expected-mismatch exit; on any other exit, the trap erases it.
# Codex [H] adversarial review of the reference-comparison section: prior
# draft could report DIFF_OUT as "this run's artifact" while the file on
# disk came from a previous run or was partial from an aborted compare.
DIFF_TMP="${DIFF_OUT}.tmp.$$"
cleanup() {
    rm -f "$DIFF_TMP" 2>/dev/null || true
}
trap cleanup EXIT

rm -f "$DIFF_OUT"

# --- Step 3: dimension check --------------------------------------------

# `identify -format "%w %h"` works for the first frame only; both inputs
# are single-frame PNGs from screendump so this is exact. Pin to the
# explicit PNG decoder to avoid identify guessing wrong on a renamed file.
REF_DIMS=$(identify -format '%w %h' "png:$REFERENCE" 2>/dev/null) \
    || die 4 "identify failed on reference: $REFERENCE (not a valid PNG?)"
CUR_DIMS=$(identify -format '%w %h' "png:$CURRENT" 2>/dev/null) \
    || die 4 "identify failed on current: $CURRENT (not a valid PNG?)"

REF_W=${REF_DIMS% *}; REF_H=${REF_DIMS#* }
CUR_W=${CUR_DIMS% *}; CUR_H=${CUR_DIMS#* }

if [ "$REF_W" != "$CUR_W" ] || [ "$REF_H" != "$CUR_H" ]; then
    die 3 "dimension mismatch: reference is ${REF_W}x${REF_H}, current is ${CUR_W}x${CUR_H}"
fi

if [ "$REF_W" -le 0 ] || [ "$REF_H" -le 0 ]; then
    die 4 "reference reports zero pixels: ${REF_W}x${REF_H}"
fi

TOTAL_PIXELS=$((REF_W * REF_H))

info "comparing ${REF_W}x${REF_H} (${TOTAL_PIXELS} px) with fuzz=${FUZZ}% threshold=${THRESHOLD}%"

# --- Step 4: pixel diff via ImageMagick compare -------------------------

# `compare -metric AE -fuzz <pct>%` returns the count of pixels that differ
# beyond the fuzz tolerance on stderr, and a pass/fail status:
#   exit 0  -> images are identical (within fuzz)
#   exit 1  -> images differ; AE on stderr is the differing-pixel count
#   exit 2+ -> compare itself failed (decode error, missing file, etc.)
# We always want the AE count, so we DO NOT exit on `compare` failure --
# we look at the count and the exit code separately.
DIFF_COUNT=$(compare -metric AE -fuzz "${FUZZ}%" \
    "png:$REFERENCE" "png:$CURRENT" "png:$DIFF_TMP" 2>&1)
COMPARE_RC=$?

# `compare` writes the metric to stderr and on some builds appends extra
# diagnostic text on warnings. Strip to the leading integer.
DIFF_COUNT_NUM=$(printf '%s\n' "$DIFF_COUNT" | head -n1 | awk '{print $1}')

case "$DIFF_COUNT_NUM" in
    ''|*[!0-9]*)
        die 4 "compare did not return a pixel count (rc=$COMPARE_RC, output: ${DIFF_COUNT})"
        ;;
esac

if [ "$COMPARE_RC" -ge 2 ]; then
    die 4 "compare failed with exit $COMPARE_RC (output: ${DIFF_COUNT})"
fi

# --- Step 5: compute percent identical ----------------------------------

# Bash has no float math; use awk for the percentage. Print to two decimals
# so PASS / FAIL lines read as "99.27% identical" not "99.273456%".
SAME_PIXELS=$((TOTAL_PIXELS - DIFF_COUNT_NUM))
PCT=$(awk -v same="$SAME_PIXELS" -v total="$TOTAL_PIXELS" \
    'BEGIN { if (total == 0) { print "0.00"; exit } printf "%.2f", (same / total) * 100 }')

# Threshold compare is also awk (THRESHOLD may be "95" or "95.5").
PASS=$(awk -v p="$PCT" -v t="$THRESHOLD" 'BEGIN { print (p + 0 >= t + 0) ? "1" : "0" }')

# Promote the temp artifact to the final path. `compare` always writes a
# diff image (all-white on identical input beyond fuzz); a missing temp is
# a compare-side failure we already would have caught via COMPARE_RC, but
# we guard anyway so a silent ImageMagick regression does not leave the
# caller staring at a stale path.
if [ -s "$DIFF_TMP" ]; then
    mv -f "$DIFF_TMP" "$DIFF_OUT" || die 4 "failed to move diff artifact into place: $DIFF_OUT"
else
    warn "diff image was not written by compare (rc=${COMPARE_RC}); leaving $DIFF_OUT absent"
fi

if [ "$PASS" = "1" ]; then
    ok "Visual match: ${PCT}% identical (threshold: ${THRESHOLD}%)"
    info "diff image: $DIFF_OUT (${DIFF_COUNT_NUM} pixels differed beyond ${FUZZ}% fuzz)"
    exit 0
else
    printf '%s[compare-screenshot]%s Visual MISMATCH: %s%% identical (expected %s%%+) -- diff: %s\n' \
        "$RED" "$RESET" "$PCT" "$THRESHOLD" "$DIFF_OUT" >&2
    info "${DIFF_COUNT_NUM} of ${TOTAL_PIXELS} pixels differed beyond ${FUZZ}% per-channel fuzz"
    exit 5
fi
