#!/usr/bin/env bash
# test-desktop.sh -- desktop smoke test: boot QEMU, wait for DESKTOP_READY,
# screendump the framebuffer via the HMP monitor, analyze the PNG for
# non-black content + a distinct taskbar region, report PASS / FAIL.
#
# Implements TODO-05-desktop-ui-test-framework.md the desktop smoke test
# section. Consumes scripts/qemu-screenshot.sh for the capture step.
#
# Usage:
#   bash scripts/test-desktop.sh [--accel kvm|tcg] [--timeout <seconds>]
#
# Environment:
#   DESKTOP_MIN_NONBLACK_PCT   non-black floor, default 10 (per TODO)
#   DESKTOP_TASKBAR_DISTINCT   min mean-delta between top-left and full frame, default 3.0
#   DESKTOP_BOOT_TIMEOUT       seconds to wait for DESKTOP_READY, default 60
#   DESKTOP_MONITOR_PORT       HMP port; default is PID-derived in the
#                              44000-44999 range so concurrent invocations
#                              do not collide
#
# Security note: the HMP monitor binds to 127.0.0.1 with `server,nowait`.
# On a multi-tenant Linux host (shared WSL, build farm), any other local
# user can connect and send monitor commands. This harness is intended for
# local single-user development and CI runners; on shared infrastructure,
# prefer a UNIX-domain monitor socket (qemu-screenshot.sh would need `nc
# -U` support), not covered here.
#
# Exit codes:
#   0  PASS (desktop rendered, taskbar distinct)
#   1  precondition failure (missing tool, disk image, OVMF)
#   2  QEMU boot failed / DESKTOP_READY not seen within timeout
#   3  screendump failed (delegates to qemu-screenshot.sh exit code)
#   4  FAIL: screen black or >90% single color
#   5  FAIL: taskbar region not distinct from wallpaper

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

# --- CLI parsing ---------------------------------------------------------

ACCEL="auto"
TIMEOUT="${DESKTOP_BOOT_TIMEOUT:-60}"

while [ $# -gt 0 ]; do
    case "$1" in
        --accel) ACCEL="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0"
            exit 0
            ;;
        *) echo "unknown flag: $1" >&2; exit 1 ;;
    esac
done

# --- Environment defaults ------------------------------------------------

MIN_NONBLACK_PCT="${DESKTOP_MIN_NONBLACK_PCT:-10}"
TASKBAR_DISTINCT_MIN="${DESKTOP_TASKBAR_DISTINCT:-3.0}"

# Per-run monitor port so two concurrent invocations (manual rerun after
# SIGKILL, parallel CI jobs on one host) cannot collide on one TCP
# listener. 44000 + (pid % 1000) gives ~1000 distinct slots before
# collision, enough for any realistic concurrency on a dev box.
MONITOR_PORT="${DESKTOP_MONITOR_PORT:-$((44000 + ($$ % 1000)))}"

DISK="$BUILD_DIR/system-disk.img"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
# Per-PID OVMF vars + serial log prevent the second runner from overwriting
# the first's boot state. User-facing PNG keeps the stable path for easy
# inspection; it is the output the test actually cares about. Codex [H].
OVMF_VARS_CP="$BUILD_DIR/OVMF_VARS_desktop-smoke-$$.fd"
SERIAL_LOG="$BUILD_DIR/desktop-smoke-$$.log"
PNG="$BUILD_DIR/desktop-smoke.png"

RED=$'\033[0;31m'
GREEN=$'\033[0;32m'
YELLOW=$'\033[1;33m'
CYAN=$'\033[0;36m'
RESET=$'\033[0m'

info()  { printf '%s[desktop-smoke]%s %s\n' "$CYAN"    "$RESET" "$*"; }
warn()  { printf '%s[desktop-smoke]%s %s\n' "$YELLOW"  "$RESET" "$*"; }
fail()  { printf '%s[desktop-smoke]%s %s\n' "$RED"     "$RESET" "$*" >&2; }
pass()  { printf '%s[desktop-smoke]%s %s\n' "$GREEN"   "$RESET" "$*"; }

QEMU_PID=""
cleanup() {
    local ec=$?
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    # Remove per-run temp files but keep the user-facing PNG + the stable
    # build/desktop-smoke.log symlink (if we created one).
    rm -f "$OVMF_VARS_CP"
    exit "$ec"
}
trap cleanup EXIT INT TERM

# --- Step 1: preflight ---------------------------------------------------

for tool in qemu-system-x86_64 nc convert identify; do
    command -v "$tool" >/dev/null 2>&1 || { fail "missing required tool: $tool"; exit 1; }
done

[ -f "$DISK" ]      || { fail "system disk missing: $DISK (run: bash scripts/build.sh)"; exit 1; }
[ -f "$OVMF_CODE" ] || { fail "OVMF_CODE missing: $OVMF_CODE (apt install ovmf)"; exit 1; }
[ -f "$OVMF_VARS_SRC" ] || { fail "OVMF_VARS missing: $OVMF_VARS_SRC"; exit 1; }

mkdir -p "$BUILD_DIR"
rm -f "$SERIAL_LOG" "$PNG" "$OVMF_VARS_CP"
cp "$OVMF_VARS_SRC" "$OVMF_VARS_CP"

# --- Step 2: pick accelerator --------------------------------------------

ACCEL_ARGS=""
case "$ACCEL" in
    kvm)  ACCEL_ARGS="-accel kvm -cpu host" ;;
    tcg)  ACCEL_ARGS="-accel tcg -cpu qemu64" ;;
    auto)
        if [ -w /dev/kvm ] 2>/dev/null; then
            ACCEL_ARGS="-accel kvm -cpu host"
            ACCEL="kvm"
        else
            ACCEL_ARGS="-accel tcg -cpu qemu64"
            ACCEL="tcg"
        fi
        ;;
    *) fail "unknown --accel: $ACCEL (expected: kvm, tcg, auto)"; exit 1 ;;
esac

# --- Step 3: launch QEMU -------------------------------------------------

info "booting QEMU ($ACCEL, ${TIMEOUT}s timeout)..."

# shellcheck disable=SC2086 -- ACCEL_ARGS is pre-split intentionally
qemu-system-x86_64 $ACCEL_ARGS \
    -smp 2 -m 2G \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS_CP" \
    -drive id=disk0,file="$DISK",format=raw,if=none \
    -device ich9-ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -vga none \
    -device bochs-display,xres=1280,yres=720 \
    -display none \
    -serial file:"$SERIAL_LOG" \
    -monitor "telnet:127.0.0.1:${MONITOR_PORT},server,nowait" \
    -netdev user,id=net0 \
    -device rtl8139,netdev=net0 \
    -rtc base=localtime \
    -no-reboot 2>/dev/null &
QEMU_PID=$!

# --- Step 4: wait for DESKTOP_READY --------------------------------------

info "waiting for DESKTOP_READY on $SERIAL_LOG..."
deadline=$((SECONDS + TIMEOUT))
while [ $SECONDS -lt $deadline ]; do
    if [ -f "$SERIAL_LOG" ] && grep -q 'DESKTOP_READY' "$SERIAL_LOG" 2>/dev/null; then
        info "DESKTOP_READY seen after ${SECONDS}s"
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        fail "QEMU exited before DESKTOP_READY -- serial log tail:"
        tail -20 "$SERIAL_LOG" 2>/dev/null | sed 's/^/    /' >&2
        exit 2
    fi
    sleep 1
done

if ! grep -q 'DESKTOP_READY' "$SERIAL_LOG" 2>/dev/null; then
    fail "DESKTOP_READY not seen within ${TIMEOUT}s (boot timeout)"
    tail -20 "$SERIAL_LOG" 2>/dev/null | sed 's/^/    /' >&2
    exit 2
fi

# --- Step 5: capture + analyze (retry loop) ------------------------------
#
# DESKTOP_READY fires BEFORE the compositor finishes the first full paint
# (wallpaper, taskbar, windows). A fixed sleep is timing-dependent on
# slower accelerators: 2 seconds covers KVM easily but TCG can still be
# mid-paint at 2 s. Retry capture+analyze up to MAX_ATTEMPTS times with a
# short sleep between, passing on the first attempt that meets both
# thresholds. Failing all attempts emits the final diagnostics.
# Codex [H] adversarial review.
if [ "$ACCEL" = "tcg" ]; then
    MAX_ATTEMPTS=8
    ATTEMPT_SLEEP=3
else
    MAX_ATTEMPTS=4
    ATTEMPT_SLEEP=1
fi

# Initial paint window: always wait at least one frame interval before the
# first capture regardless of accel, so we do not grab mid-wallpaper-blit.
sleep 1

NONBLACK_PCT=""
TOP_NONBLACK=""
TASKBAR_DELTA=""

LAST_CAPTURE_RC=0
for attempt in $(seq 1 "$MAX_ATTEMPTS"); do
    info "capturing screenshot (attempt $attempt/$MAX_ATTEMPTS)..."
    QEMU_MONITOR_PORT="$MONITOR_PORT" \
    QEMU_SCREENSHOT_MIN=0 \
        bash "$SCRIPT_DIR/qemu-screenshot.sh" "$PNG"
    LAST_CAPTURE_RC=$?
    if [ "$LAST_CAPTURE_RC" -ne 0 ]; then
        # Precondition failures (exit 1 = tool missing, invalid path) are
        # not retriable -- surface immediately. Transient monitor/PPM/PNG
        # errors (exit 2/3/4/5) can legitimately recover on a later attempt
        # if the VM is still in the paint window; keep retrying until the
        # last attempt, then surface the final error code.
        # Codex [M] quality review.
        if [ "$LAST_CAPTURE_RC" = "1" ] || [ "$attempt" -ge "$MAX_ATTEMPTS" ]; then
            fail "screenshot capture failed (attempt $attempt, exit $LAST_CAPTURE_RC)"
            exit 3
        fi
        warn "capture failed with exit $LAST_CAPTURE_RC -- retrying in ${ATTEMPT_SLEEP}s..."
        sleep "$ATTEMPT_SLEEP"
        continue
    fi

    # Full-frame "non-black" percentage. ImageMagick -threshold 6%
    # binarizes (pixels > 6% of max brightness -> 1, rest -> 0) and the
    # mean of the resulting 0/1 image is the fraction that is "lit",
    # returned as a percentage.
    NONBLACK_PCT=$(convert "$PNG" -threshold 6% -format '%[fx:100*mean]' info: 2>/dev/null)
    if [ -z "$NONBLACK_PCT" ] || ! [[ "$NONBLACK_PCT" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
        fail "could not compute non-black percentage from $PNG"
        exit 4
    fi

    # Top-left 400x40 region. Cropped mean differs from full-frame mean
    # when the taskbar paints a distinct color over the wallpaper.
    TOP_NONBLACK=$(convert "$PNG" -crop 400x40+0+0 +repage -threshold 6% \
        -format '%[fx:100*mean]' info: 2>/dev/null)
    [ -z "$TOP_NONBLACK" ] && TOP_NONBLACK=0

    TASKBAR_DELTA=$(awk -v t="$TOP_NONBLACK" -v f="$NONBLACK_PCT" \
        'BEGIN{ d = t - f; if (d < 0) d = -d; printf "%.2f", d }')

    info "  full-frame: ${NONBLACK_PCT}% | top-left 400x40: ${TOP_NONBLACK}% | delta: ${TASKBAR_DELTA}%"

    # PASS condition: non-black above floor AND taskbar distinct.
    PASS_NONBLACK=$(awk -v v="$NONBLACK_PCT" -v m="$MIN_NONBLACK_PCT" \
        'BEGIN{ print (v >= m) ? 1 : 0 }')
    PASS_TASKBAR=$(awk -v d="$TASKBAR_DELTA" -v m="$TASKBAR_DISTINCT_MIN" \
        'BEGIN{ print (d >= m) ? 1 : 0 }')

    if [ "$PASS_NONBLACK" = "1" ] && [ "$PASS_TASKBAR" = "1" ]; then
        NONBLACK_INT=$(printf '%.0f' "$NONBLACK_PCT")
        pass "$(printf 'Desktop smoke test: PASS (rendered, %u%% non-black)' "$NONBLACK_INT")"
        exit 0
    fi

    # Last attempt -- fall through to the FAIL branch.
    if [ "$attempt" -lt "$MAX_ATTEMPTS" ]; then
        info "  thresholds not met; retrying in ${ATTEMPT_SLEEP}s..."
        sleep "$ATTEMPT_SLEEP"
    fi
done

# --- Step 6: final verdict (all attempts exhausted) ----------------------

if awk -v v="$NONBLACK_PCT" -v m="$MIN_NONBLACK_PCT" 'BEGIN{ exit (v < m) ? 0 : 1 }'; then
    fail "Desktop smoke test: FAIL (screen is black or >90% single color)"
    fail "  non-black ${NONBLACK_PCT}% < floor ${MIN_NONBLACK_PCT}% after ${MAX_ATTEMPTS} attempts"
    exit 4
fi

fail "Desktop smoke test: FAIL (taskbar region indistinguishable from wallpaper)"
fail "  top-left vs full-frame delta ${TASKBAR_DELTA}% < min ${TASKBAR_DISTINCT_MIN}% after ${MAX_ATTEMPTS} attempts"
exit 5
