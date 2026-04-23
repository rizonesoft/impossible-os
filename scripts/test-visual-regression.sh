#!/usr/bin/env bash
# test-visual-regression.sh -- run every desktop UI scenario and compare
# the captured screenshot against the stored reference, or seed the
# reference set when --update-refs is passed.
#
# Implements TODO-05-desktop-ui-test-framework.md the visual-regression
# CI pipeline section. Wrapped by .github/workflows/visual-regression.yml
# for CI and by `make test-visual` / `make update-ui-refs` locally.
#
# Scenarios are a small name -> (capture-recipe, needs-fresh-boot) table;
# to add a new one, extend `scenario_capture_fresh()`, `scenario_setup()`,
# and `scenario_needs_fresh_boot()`.
# Each scenario is expected to leave a PNG at $BUILD_DIR/visual-<scenario>.png
# after running.
#
# Session model:
#   - Multi-scenario runs boot QEMU ONCE (shared session) when every
#     listed scenario is `needs_fresh_boot=0`, inject per-scenario setup
#     between captures via qemu-input.sh sendstring / sendkey, and tear
#     down the VM after the final scenario. Amortizes the boot cost.
#   - A scenario marked `needs_fresh_boot=1` boots its own VM via
#     test-desktop.sh. Any `needs_fresh_boot=1` scenario in the list
#     falls the entire run back to the per-scenario delegation path so
#     isolation guarantees are preserved.
#   - Single-scenario runs (default SCENARIOS=idle) preserve the
#     test-desktop.sh delegation so the baseline behavior is unchanged.
#
# Current scenarios:
#   idle -- fresh-boot desktop (wallpaper + taskbar, no user input).
#           Via test-desktop.sh in single-scenario mode; via shared
#           session (first capture after DESKTOP_READY) in multi mode.
#
# Usage:
#   bash scripts/test-visual-regression.sh [--accel kvm|tcg] [--timeout <s>]
#       [--scenarios "idle [other...]"] [--update-refs]
#
# Environment:
#   VR_THRESHOLD   percent-identical floor, default 95 (tolerance)
#   VR_FUZZ        per-channel fuzz, default 2
#   VR_FORCE_FRESH set to 1 to disable shared-session mode (every scenario
#                  boots fresh via test-desktop.sh). Useful when a bug in
#                  shared-session capture needs to be isolated.
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

# Scenarios marked needs_fresh_boot=1 get their own test-desktop.sh
# invocation (full boot). Everything else runs against the shared session
# described in the header session model block.
scenario_needs_fresh_boot() {
    local name="$1"
    case "$name" in
        idle) return 1 ;;   # shared-session-friendly
        *) return 1 ;;      # default: shared-session-friendly
    esac
}

# Per-scenario SETUP runs AFTER the shared session reaches DESKTOP_READY
# and BEFORE the screenshot. This is where keyboard / mouse sequences
# get injected via qemu-input.sh to drive the VM into the scenario's
# expected visual state. Idle needs no setup (it IS the first-paint
# state). Return non-zero from the setup hook to abort capture.
scenario_setup() {
    local name="$1"; local socket="$2"
    case "$name" in
        idle) return 0 ;;
        *) fail "no setup hook for scenario: $name"; return 1 ;;
    esac
}

# scenario_capture_fresh() always boots a fresh VM via test-desktop.sh.
# Used for:
#   - single-scenario runs (preserves the original fresh-boot baseline)
#   - scenarios marked needs_fresh_boot=1
#   - VR_FORCE_FRESH=1 debug override
scenario_capture_fresh() {
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
            fail "unknown scenario: $name (edit scenario_capture_fresh() / scenario_setup() to add it)"
            return 1 ;;
    esac
    return 0
}

# scenario_capture_shared() runs against an already-booted VM reachable
# via the UNIX socket at $SESSION_SOCKET. Setup hook runs first (typically
# a qemu-input.sh sendstring), then the screendump.
scenario_capture_shared() {
    local name="$1"
    local out="$BUILD_DIR/visual-${name}.png"

    scenario_setup "$name" "$SESSION_SOCKET" || return 2

    # Brief paint window after setup input so the compositor has a frame
    # to process before the capture. 300 ms covers one refresh cycle on
    # a slow TCG run with margin for small animations. Idle's setup is
    # a no-op, so this sleep is only paid when setup actually does work
    # (future scenarios with input sequences).
    if [ "$name" != "idle" ]; then
        sleep 0.3
    fi

    QEMU_MONITOR_SOCKET="$SESSION_SOCKET" \
    QEMU_SCREENSHOT_MIN=0 \
        bash "$SCRIPT_DIR/qemu-screenshot.sh" "$out" || return 2
    [ -s "$out" ] || return 2
    return 0
}

# --- Step 3b: shared session lifecycle -----------------------------------

SESSION_QEMU_PID=""
SESSION_SOCKET=""
SESSION_SERIAL_LOG=""
SESSION_OVMF_VARS=""

# session_start boots QEMU with a UNIX-socket HMP monitor, waits for
# DESKTOP_READY on the serial log, and leaves the VM running. Caller is
# responsible for calling session_stop() at end-of-run or on error.
# Returns non-zero on any failure (exits 2/3 style, surfaced to caller).
session_start() {
    local disk="$BUILD_DIR/system-disk.img"
    local ovmf_code="/usr/share/OVMF/OVMF_CODE_4M.fd"
    local ovmf_vars_src="/usr/share/OVMF/OVMF_VARS_4M.fd"

    for tool in qemu-system-x86_64 nc convert identify; do
        command -v "$tool" >/dev/null 2>&1 || { fail "missing required tool: $tool"; return 1; }
    done
    [ -f "$disk" ]           || { fail "system disk missing: $disk"; return 2; }
    [ -f "$ovmf_code" ]      || { fail "OVMF_CODE missing: $ovmf_code"; return 2; }
    [ -f "$ovmf_vars_src" ]  || { fail "OVMF_VARS missing: $ovmf_vars_src"; return 2; }

    # Shared UNIX socket reused by every scenario's setup (qemu-input.sh)
    # and capture (qemu-screenshot.sh) calls.
    SESSION_SOCKET="/tmp/qemu-mon-visual-reg-$$.sock"
    SESSION_SERIAL_LOG="$BUILD_DIR/visual-reg-session-$$.log"
    SESSION_OVMF_VARS="$BUILD_DIR/OVMF_VARS_visual-reg-$$.fd"

    rm -f "$SESSION_SOCKET" "$SESSION_SERIAL_LOG" "$SESSION_OVMF_VARS"
    cp "$ovmf_vars_src" "$SESSION_OVMF_VARS"

    local accel_args
    case "$ACCEL" in
        kvm)  accel_args="-accel kvm -cpu host" ;;
        tcg)  accel_args="-accel tcg -cpu qemu64" ;;
        auto)
            if [ -w /dev/kvm ] 2>/dev/null; then
                accel_args="-accel kvm -cpu host"
            else
                accel_args="-accel tcg -cpu qemu64"
            fi
            ;;
        *) fail "unknown --accel: $ACCEL"; return 1 ;;
    esac

    info "shared session: booting QEMU ($ACCEL, ${TIMEOUT}s timeout, monitor=unix)..."

    # shellcheck disable=SC2086 -- accel_args is pre-split intentionally
    qemu-system-x86_64 $accel_args \
        -smp 2 -m 2G \
        -drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
        -drive if=pflash,format=raw,file="$SESSION_OVMF_VARS" \
        -drive id=disk0,file="$disk",format=raw,if=none \
        -device ich9-ahci,id=ahci0 \
        -device ide-hd,drive=disk0,bus=ahci0.0 \
        -vga none \
        -device bochs-display,xres=1280,yres=720 \
        -display none \
        -serial file:"$SESSION_SERIAL_LOG" \
        -monitor "unix:${SESSION_SOCKET},server,nowait" \
        -netdev user,id=net0 \
        -device rtl8139,netdev=net0 \
        -rtc base=localtime \
        -no-reboot 2>/dev/null &
    SESSION_QEMU_PID=$!

    info "shared session: waiting for DESKTOP_READY..."
    local deadline=$((SECONDS + TIMEOUT))
    while [ $SECONDS -lt $deadline ]; do
        if [ -f "$SESSION_SERIAL_LOG" ] && grep -q 'DESKTOP_READY' "$SESSION_SERIAL_LOG" 2>/dev/null; then
            info "shared session: DESKTOP_READY seen after ${SECONDS}s"
            # One paint window before any scenario captures, matching
            # test-desktop.sh's pre-first-attempt sleep.
            sleep 1
            return 0
        fi
        if ! kill -0 "$SESSION_QEMU_PID" 2>/dev/null; then
            fail "shared session: QEMU exited before DESKTOP_READY"
            tail -20 "$SESSION_SERIAL_LOG" 2>/dev/null | sed 's/^/    /' >&2
            return 2
        fi
        sleep 1
    done
    fail "shared session: DESKTOP_READY not seen within ${TIMEOUT}s"
    tail -20 "$SESSION_SERIAL_LOG" 2>/dev/null | sed 's/^/    /' >&2
    return 2
}

session_stop() {
    if [ -n "$SESSION_QEMU_PID" ] && kill -0 "$SESSION_QEMU_PID" 2>/dev/null; then
        kill "$SESSION_QEMU_PID" 2>/dev/null || true
        wait "$SESSION_QEMU_PID" 2>/dev/null || true
    fi
    SESSION_QEMU_PID=""
    if [ -n "$SESSION_SOCKET" ] && [ -S "$SESSION_SOCKET" ]; then
        rm -f "$SESSION_SOCKET"
    fi
    if [ -n "$SESSION_OVMF_VARS" ] && [ -f "$SESSION_OVMF_VARS" ]; then
        rm -f "$SESSION_OVMF_VARS"
    fi
}

# Register session teardown on script exit. Must run BEFORE any other
# trap-sensitive state so a SIGINT mid-scenario kills the VM cleanly.
session_cleanup() {
    local ec=$?
    session_stop
    exit "$ec"
}
trap session_cleanup EXIT INT TERM

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

# Decide shared-session vs fresh-boot-per-scenario. Preserve baseline
# (`idle` single-scenario -> test-desktop.sh) for the common case so the
# current CI wall-time is unchanged. Multi-scenario or explicit override
# -> attempt shared session; any needs_fresh_boot=1 scenario in the list
# falls back to fresh-boot mode to honor isolation guarantees.
SHARED_SESSION=0
SCENARIO_COUNT=0
FRESH_REQUIRED=0
for s in $SCENARIOS_CSV; do
    SCENARIO_COUNT=$((SCENARIO_COUNT + 1))
    if scenario_needs_fresh_boot "$s"; then FRESH_REQUIRED=1; fi
done

if [ "${VR_FORCE_FRESH:-0}" = "1" ]; then
    info "session: fresh-boot per scenario (VR_FORCE_FRESH=1 override)"
elif [ "$UPDATE_REFS" = "1" ]; then
    # Reference seeding writes the baseline image set; do that from
    # fresh-boot so the committed PNGs are not tainted by any cross-
    # scenario state that survives in a shared session.
    info "session: fresh-boot per scenario (--update-refs active)"
elif [ "$SCENARIO_COUNT" -le 1 ]; then
    info "session: fresh-boot per scenario (single-scenario baseline)"
elif [ "$FRESH_REQUIRED" = "1" ]; then
    info "session: fresh-boot per scenario (a scenario requires kernel isolation)"
else
    info "session: shared across $SCENARIO_COUNT scenarios (amortizing boot)"
    if session_start; then
        SHARED_SESSION=1
    else
        warn "shared session failed to start; falling back to fresh-boot per scenario"
        session_stop
    fi
fi

for name in $SCENARIOS_CSV; do
    TOTAL=$((TOTAL + 1))
    info "--- scenario: $name ---"

    if [ "$SHARED_SESSION" = "1" ]; then
        scenario_capture_shared "$name"; cap_rc=$?
    else
        scenario_capture_fresh "$name"; cap_rc=$?
    fi
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
