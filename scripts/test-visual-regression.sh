#!/usr/bin/env bash
# test-visual-regression.sh -- run every desktop UI scenario and compare
# the captured screenshot against the stored reference, or seed the
# reference set when --update-refs is passed.
#
# Implements TODO-05-desktop-ui-test-framework.md the visual-regression
# CI pipeline section. Wrapped by .github/workflows/visual-regression.yml
# for CI and by `make test-visual` / `make update-ui-refs` locally.
#
# Scenarios are a small name -> capture-recipe table; to add a new one,
# extend `scenario_capture()`. Each scenario is expected to leave a PNG
# at $BUILD_DIR/visual-<scenario>.png after running.
#
# Current scenarios:
#   idle -- fresh-boot desktop (wallpaper + taskbar, no user input).
#           Delegates to test-desktop.sh for boot+capture, then copies
#           its build/desktop-smoke.png to build/visual-idle.png.
#
# Usage:
#   bash scripts/test-visual-regression.sh [--accel kvm|tcg] [--timeout <s>]
#       [--scenarios "idle [other...]"] [--update-refs]
#
# Environment:
#   VR_THRESHOLD   percent-identical floor, default 95 (per §6 tolerance)
#   VR_FUZZ        per-channel fuzz, default 2
#   GITHUB_STEP_SUMMARY  when set, the markdown summary is appended here
#
# Exit codes:
#   0  all scenarios passed (or --update-refs wrote every reference)
#   1  precondition failure (missing tool, bad arg)
#   2  at least one scenario failed to boot / capture (infrastructure error)
#   3  at least one scenario compared below the threshold (regression)
#   4  compare-screenshot.sh reported an internal error (decode, dims)
#
# Note: a pristine run with NO `tests/references/*.png` committed yet
# prints an advisory line and exits 0. CI is expected to seed the
# reference set via a dispatched `make update-ui-refs` run before the
# visual-regression workflow can actually gate PRs.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
REF_DIR="$PROJECT_ROOT/tests/references"

ACCEL="auto"
TIMEOUT="${DESKTOP_BOOT_TIMEOUT:-60}"
SCENARIOS_CSV="idle"
UPDATE_REFS=0
THRESHOLD="${VR_THRESHOLD:-95}"
FUZZ="${VR_FUZZ:-2}"

RED=$'\033[0;31m'
GREEN=$'\033[0;32m'
YELLOW=$'\033[1;33m'
CYAN=$'\033[0;36m'
RESET=$'\033[0m'

info() { printf '%s[visual-reg]%s %s\n' "$CYAN"   "$RESET" "$*"; }
warn() { printf '%s[visual-reg]%s %s\n' "$YELLOW" "$RESET" "$*" >&2; }
fail() { printf '%s[visual-reg]%s %s\n' "$RED"    "$RESET" "$*" >&2; }
pass() { printf '%s[visual-reg]%s %s\n' "$GREEN"  "$RESET" "$*"; }

die() { local code=$1; shift; fail "$*"; exit "$code"; }

# --- Step 1: parse args --------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        --accel)     [ $# -ge 2 ] || die 1 "--accel needs a value"; ACCEL="$2"; shift 2 ;;
        --timeout)   [ $# -ge 2 ] || die 1 "--timeout needs a value"; TIMEOUT="$2"; shift 2 ;;
        --scenarios) [ $# -ge 2 ] || die 1 "--scenarios needs a value"; SCENARIOS_CSV="$2"; shift 2 ;;
        --update-refs) UPDATE_REFS=1; shift ;;
        -h|--help)
            sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) die 1 "unknown flag: $1 (try --help)" ;;
    esac
done

# --- Step 2: preflight ---------------------------------------------------

[ -x "$SCRIPT_DIR/test-desktop.sh" ]       || die 1 "missing: $SCRIPT_DIR/test-desktop.sh"
[ -x "$SCRIPT_DIR/compare-screenshot.sh" ] || die 1 "missing: $SCRIPT_DIR/compare-screenshot.sh"
command -v identify >/dev/null 2>&1        || die 1 "missing: ImageMagick 'identify'"

mkdir -p "$BUILD_DIR" "$REF_DIR"

# --- Step 3: scenario dispatch -------------------------------------------

scenario_capture() {
    local name="$1"
    local out="$BUILD_DIR/visual-${name}.png"
    case "$name" in
        idle)
            # Delegate boot + first-paint wait + screendump to test-desktop.sh.
            # Its exit codes: 0 pass, 1 preflight, 2 boot-timeout, 3 screendump,
            # 4 black, 5 taskbar-indistinct -- map anything non-zero to a
            # capture failure so the caller reports infrastructure vs regression
            # distinctly.
            if ! bash "$SCRIPT_DIR/test-desktop.sh" --accel "$ACCEL" --timeout "$TIMEOUT"; then
                return 2
            fi
            if [ ! -s "$BUILD_DIR/desktop-smoke.png" ]; then
                fail "scenario '$name': test-desktop.sh returned 0 but build/desktop-smoke.png is missing/empty"
                return 2
            fi
            cp "$BUILD_DIR/desktop-smoke.png" "$out" || return 2
            ;;
        *)
            fail "unknown scenario: $name (edit scenario_capture() to add it)"
            return 1 ;;
    esac
    return 0
}

# Resolve a capture PNG into the tests/references/ path format
# <scenario>-<WxH>.png so different resolutions do not collide when the
# default framebuffer mode changes.
resolve_ref_path() {
    local name="$1" current="$2"
    local dims w h
    dims=$(identify -format '%w %h' "png:$current" 2>/dev/null) || return 1
    w=${dims% *}; h=${dims#* }
    printf '%s/%s-%sx%s.png\n' "$REF_DIR" "$name" "$w" "$h"
}

# --- Step 4: run every scenario ------------------------------------------

TOTAL=0
PASSED=0
FAILED=0
CAPTURE_ERRORS=0
CONFIG_ERRORS=0
COMPARE_INTERNAL=0
MISSING_REFS=0
SUMMARY_ROWS=()

for name in $SCENARIOS_CSV; do
    TOTAL=$((TOTAL + 1))
    info "--- scenario: $name ---"

    scenario_capture "$name"; cap_rc=$?
    if [ "$cap_rc" -ne 0 ]; then
        # Distinguish config errors (rc=1: bad scenario name / dispatcher
        # rejection) from real boot/capture failures (rc=2). Codex [M]
        # quality review of section 7: collapsing both into CAPTURE_ERRORS
        # sent maintainers debugging QEMU when the real fix was a typo.
        if [ "$cap_rc" = "1" ]; then
            CONFIG_ERRORS=$((CONFIG_ERRORS + 1))
            SUMMARY_ROWS+=("| $name | ❌ CONFIG | -- | unknown scenario / dispatcher rejection |")
        else
            CAPTURE_ERRORS=$((CAPTURE_ERRORS + 1))
            SUMMARY_ROWS+=("| $name | ❌ CAPTURE | -- | rc=$cap_rc |")
        fi
        continue
    fi

    current="$BUILD_DIR/visual-${name}.png"
    ref=$(resolve_ref_path "$name" "$current") \
        || { CAPTURE_ERRORS=$((CAPTURE_ERRORS + 1));
             SUMMARY_ROWS+=("| $name | ❌ DIMS | -- | -- |"); continue; }

    if [ "$UPDATE_REFS" = "1" ]; then
        cp -f "$current" "$ref" || die 2 "failed to write reference: $ref"
        pass "updated reference: $ref"
        PASSED=$((PASSED + 1))
        SUMMARY_ROWS+=("| $name | 🆕 UPDATED | -- | \`$(basename "$ref")\` |")
        continue
    fi

    if [ ! -f "$ref" ]; then
        warn "no reference yet for '$name' (expected: $ref). Run: make update-ui-refs"
        MISSING_REFS=$((MISSING_REFS + 1))
        SUMMARY_ROWS+=("| $name | ⚠️ NO REF | -- | run \`make update-ui-refs\` |")
        continue
    fi

    diff_path="$BUILD_DIR/visual-${name}.diff.png"
    # compare-screenshot.sh emits its own PASS/FAIL line + exit code (0 pass,
    # 5 regression, 3 dim mismatch, 4 decode failure, 1/2 preflight). Capture
    # combined stdout+stderr so we can parse the percent-identical token from
    # either the "Visual match" (PASS) or "Visual MISMATCH" (FAIL) line.
    compare_out=$(bash "$SCRIPT_DIR/compare-screenshot.sh" \
            --threshold "$THRESHOLD" --fuzz "$FUZZ" \
            "$ref" "$current" "$diff_path" 2>&1); rc=$?
    echo "$compare_out"
    # Extract the first percentage token (e.g. "99.27%") from whichever
    # status line was printed. awk is locale-agnostic here since the value
    # is a plain decimal with a literal percent sign.
    pct=$(printf '%s\n' "$compare_out" \
            | awk '/Visual (match|MISMATCH)/ { for (i=1;i<=NF;i++) if ($i ~ /^[0-9]+(\.[0-9]+)?%$/) { gsub("%","",$i); print $i; exit } }')
    [ -z "$pct" ] && pct="?"

    if [ "$rc" = "0" ]; then
        PASSED=$((PASSED + 1))
        SUMMARY_ROWS+=("| $name | ✅ PASS | ${pct}% | \`$(basename "$ref")\` |")
    else
        # Distinguish compare-screenshot INTERNAL failures (harness broken,
        # dimension mismatch vs reference, decode error) from a real
        # screenshot regression. The former means "CI infrastructure is
        # broken, not a product regression" and deserves its own exit code
        # so the workflow / maintainer can route correctly. Codex [H].
        case $rc in
            3|4)
                COMPARE_INTERNAL=$((COMPARE_INTERNAL + 1))
                if [ "$rc" = "3" ]; then
                    SUMMARY_ROWS+=("| $name | ⚠️ HARNESS | -- | reference vs current size mismatch (not a regression) |")
                else
                    SUMMARY_ROWS+=("| $name | ⚠️ HARNESS | -- | ImageMagick compare/identify failed (not a regression) |")
                fi
                ;;
            5)
                FAILED=$((FAILED + 1))
                SUMMARY_ROWS+=("| $name | ❌ MISMATCH | ${pct}% | \`$(basename "$diff_path")\` |")
                ;;
            *)
                COMPARE_INTERNAL=$((COMPARE_INTERNAL + 1))
                SUMMARY_ROWS+=("| $name | ⚠️ HARNESS | -- | compare-screenshot exit $rc |")
                ;;
        esac
    fi
done

# --- Step 5: emit summary -------------------------------------------------

# Headline always uses TOTAL as the denominator so a partially-gated
# run (some scenarios missing baselines) cannot read as a full pass.
# Codex [H] quality review of section 7: previously we computed
# EFFECTIVE_TOTAL = TOTAL - MISSING_REFS and reported `1/1 visual
# checks passed` even when 1 of 2 scenarios was never gated.
STATUS_ICON="✅"
STATUS_LINE="UI Tests: ${PASSED}/${TOTAL} visual checks passed"
FINAL_RC=0

# Ordering: config errors (typo / dispatcher rejection) outrank capture
# (boot infra), which outrank compare-internal (harness), which outrank
# product regressions. Missing-refs is advisory but still surfaced in
# the headline so partial coverage is visible without opening the
# Summary tab.
if [ "$CONFIG_ERRORS" -gt 0 ]; then
    STATUS_ICON="❌"
    STATUS_LINE="UI Tests: CONFIG error in ${CONFIG_ERRORS}/${TOTAL} scenario(s) -- check scenario names"
    FINAL_RC=1
elif [ "$CAPTURE_ERRORS" -gt 0 ]; then
    STATUS_ICON="❌"
    STATUS_LINE="UI Tests: capture failed for ${CAPTURE_ERRORS}/${TOTAL} scenario(s)"
    FINAL_RC=2
elif [ "$COMPARE_INTERNAL" -gt 0 ]; then
    STATUS_ICON="❌"
    STATUS_LINE="UI Tests: HARNESS error in ${COMPARE_INTERNAL}/${TOTAL} scenario(s) -- not a product regression"
    FINAL_RC=4
elif [ "$FAILED" -gt 0 ]; then
    STATUS_ICON="❌"
    STATUS_LINE="UI Tests: MISMATCH -- ${FAILED}/${TOTAL} scenario(s) regressed"
    FINAL_RC=3
elif [ "$MISSING_REFS" -gt 0 ] && [ "$UPDATE_REFS" != "1" ]; then
    # Partial coverage: some scenarios passed, some have no baseline.
    # Headline reports the gating gap so it's visible at a glance.
    STATUS_ICON="⚠️"
    STATUS_LINE="UI Tests: ${PASSED}/${TOTAL} passed -- ${MISSING_REFS} scenario(s) ungated (no reference yet)"
    FINAL_RC=0
fi

# Emit a GitHub Actions warning annotation on advisory / partial states so
# the Checks tab surfaces that visual gating is not fully active. Codex [M]
# adversarial review: without this, an empty reference set silently passes
# and maintainers only discover the gap when a real regression ships.
if [ -n "${GITHUB_ACTIONS:-}" ]; then
    if [ "$MISSING_REFS" -gt 0 ] && [ "$UPDATE_REFS" != "1" ]; then
        printf '::warning title=Visual regression gate advisory::%d of %d scenarios have no reference image; run `make update-ui-refs` and commit the PNGs in tests/references/ to enable gating\n' \
            "$MISSING_REFS" "$TOTAL"
    fi
fi

echo

# GitHub Actions step summary. The runner redirects this to the job's
# Summary tab; locally it is just stdout.
{
    printf '### %s %s\n\n' "$STATUS_ICON" "$STATUS_LINE"
    printf '| Scenario | Result | Identical | Artifact |\n'
    printf '|---|---|---|---|\n'
    for row in "${SUMMARY_ROWS[@]}"; do printf '%s\n' "$row"; done
    printf '\n_Threshold %s%% / fuzz %s%% / accel %s / scenarios %s_\n' \
        "$THRESHOLD" "$FUZZ" "$ACCEL" "$SCENARIOS_CSV"
} | if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
        tee -a "$GITHUB_STEP_SUMMARY"
    else
        cat
    fi

if [ "$FINAL_RC" -eq 0 ]; then
    pass "$STATUS_LINE"
else
    fail "$STATUS_LINE"
fi
exit "$FINAL_RC"
