#!/usr/bin/env bash
# boot-test-qemu.sh -- single-shot raw-disk QEMU boot launcher with explicit
# cold/warm firmware-NVRAM control.
#
# Owner: VM Automation Suite in
# todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md.
#
# This is the standardized raw-QEMU launcher primitive the boot-reliability
# gate (tools/boot-cert/boot_reliability.py) drives N cold + M warm times.
# It mirrors the --disk/--timeout/exit-code contract of the per-format
# launchers (scripts/release/boot-test-vhdx.sh, boot-test-vbox.sh) so the
# reliability gate's launcher registry can invoke any of them uniformly.
# It factors the previously-inline raw leg of scripts/ci/boot-matrix.sh into
# a reusable form; the OVMF + AHCI + serial-grep discipline matches
# scripts/test-smoke.sh.
#
# Cold vs warm (the reliability axis):
#   COLD  -- no --vars given: a FRESH OVMF_VARS copy is made from the system
#            template, so firmware NVRAM (boot entries, dbx, A/B counters held
#            in EFI variables) starts pristine. Use for the cold-boot leg.
#   WARM  -- --vars PATH points at an EXISTING vars file: it is reused in
#            place so a series of warm reboots inherits the firmware NVRAM the
#            previous iteration left behind. Use for the warm-reboot leg.
# Disk state is NOT mutated here; the gate owns disk pristine-vs-reuse (it
# resets boot.conf via scripts/patch-boot-conf.sh on cold legs).
#
# Exit codes (uniform launcher contract):
#   0 -- booted to userspace ("Boot complete in" + "C:\>" on serial)  (PASS)
#   1 -- boot timed out, QEMU crashed, or a fail pattern matched       (FAIL)
#   2 -- usage / preflight (missing disk / missing OVMF)
#   3 -- qemu-system-x86_64 not on PATH                                (SKIP)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$REPO_ROOT/build"

DISK="$BUILD/system-disk.img"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
TIMEOUT_SEC="${TIMEOUT_SEC:-30}"
ACCEL="auto"
VARS=""           # warm: reuse this OVMF_VARS file; cold (empty): fresh copy
SERIAL_OUT=""     # captured serial log path (default: mktemp)
SCREENSHOT=""     # optional PPM screendump path (best-effort via HMP monitor)
RUN_DIR=""        # per-invocation scratch (fresh vars + monitor socket)

err()  { printf '[ERROR] %s\n' "$*" >&2; }
note() { printf '[boot-test-qemu] %s\n' "$*" >&2; }

usage() {
    cat <<EOF
Usage: $0 [--disk PATH] [--timeout SEC] [--accel auto|kvm|tcg]
          [--vars PATH] [--serial-out PATH] [--screenshot PATH]

Cold boot:  omit --vars (a fresh OVMF_VARS copy is made each run).
Warm boot:  pass --vars PATH to an existing vars file to reuse firmware NVRAM.

Exit: 0 PASS | 1 FAIL | 2 preflight | 3 SKIP (qemu absent)
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --disk)       DISK="$2"; shift 2 ;;
        --timeout)    TIMEOUT_SEC="$2"; shift 2 ;;
        --accel)      ACCEL="$2"; shift 2 ;;
        --vars)       VARS="$2"; shift 2 ;;
        --serial-out) SERIAL_OUT="$2"; shift 2 ;;
        --screenshot) SCREENSHOT="$2"; shift 2 ;;
        -h|--help)    usage; exit 0 ;;
        *) err "unknown arg: $1"; usage >&2; exit 2 ;;
    esac
done

case "$ACCEL" in auto|kvm|tcg) ;; *) err "bad --accel: $ACCEL"; exit 2 ;; esac
case "$TIMEOUT_SEC" in ''|*[!0-9]*) err "bad --timeout: $TIMEOUT_SEC"; exit 2 ;; esac
[ "$TIMEOUT_SEC" -ge 1 ] || { err "--timeout must be >= 1"; exit 2; }

command -v qemu-system-x86_64 >/dev/null 2>&1 || { note "SKIP: qemu-system-x86_64 not on PATH"; exit 3; }
[ -f "$DISK" ]          || { err "missing disk: $DISK (run scripts/build.sh)"; exit 2; }
[ -f "$OVMF_CODE" ]     || { err "missing OVMF: $OVMF_CODE (apt install ovmf)"; exit 2; }
[ -f "$OVMF_VARS_SRC" ] || { err "missing OVMF VARS template: $OVMF_VARS_SRC (apt install ovmf)"; exit 2; }

RUN_DIR="$(mktemp -d --suffix=.boot-test-qemu)"
QEMU_PID=""
cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    [ -n "$RUN_DIR" ] && [ -d "$RUN_DIR" ] && rm -rf "$RUN_DIR"
}
trap cleanup EXIT INT TERM

# Firmware NVRAM:
#   cold (no --vars)       -- fresh ephemeral copy from template (discarded).
#   warm (--vars EXISTING) -- reuse the caller's vars in place so the reboot
#                             inherits the previous iteration's NVRAM.
#   warm bootstrap (--vars
#     MISSING)             -- seed it fresh from the template and PERSIST it, so
#                             the first warm reboot of a series starts clean and
#                             every later warm reboot reuses it. Without this the
#                             first warm iteration would error (exit 2) and be
#                             recorded as a false flake by the reliability gate.
if [ -n "$VARS" ]; then
    VARS_USE="$VARS"
    if [ -f "$VARS_USE" ]; then
        note "warm reboot: reusing firmware NVRAM at $VARS_USE"
    else
        cp "$OVMF_VARS_SRC" "$VARS_USE"
        note "warm series bootstrap: seeded fresh firmware NVRAM at $VARS_USE"
    fi
else
    VARS_USE="$RUN_DIR/OVMF_VARS.fd"
    cp "$OVMF_VARS_SRC" "$VARS_USE"
    note "cold boot: fresh firmware NVRAM"
fi

[ -n "$SERIAL_OUT" ] || SERIAL_OUT="$RUN_DIR/serial.log"
: > "$SERIAL_OUT"

# Accelerator selection: auto picks KVM only when /dev/kvm is writable.
ACCEL_USE="$ACCEL"
if [ "$ACCEL_USE" = "auto" ]; then
    if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL_USE="kvm"; else ACCEL_USE="tcg"; fi
fi

MON_SOCK="$RUN_DIR/monitor.sock"
QEMU_FLAGS=(
    -machine "q35,accel=$ACCEL_USE" -cpu max -m 2G
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$VARS_USE"
    -drive "id=disk0,if=none,format=raw,file=$DISK"
    -device "ich9-ahci,id=ahci0"
    -device "ide-hd,drive=disk0,bus=ahci0.0"
    -serial "file:$SERIAL_OUT"
    -display none
    -no-reboot
    -monitor "unix:$MON_SOCK,server,nowait"
)

note "booting $DISK (accel=$ACCEL_USE, timeout=${TIMEOUT_SEC}s)"
qemu-system-x86_64 "${QEMU_FLAGS[@]}" &
QEMU_PID=$!

# PASS requires BOTH markers; the real shell prompt is a SINGLE backslash
# "C:\>". Matching uses `grep -qF` (fixed string) against the ANSI-stripped
# serial -- the same discipline as scripts/test-smoke.sh -- to sidestep the
# case-glob backslash trap where a doubled-backslash literal never matches.
PASS_MARKER='Boot complete in'
PROMPT_MARKER='C:\>'
FAIL_RES=(
    "KERNEL PANIC" "ASSERT FAILED" "triple fault"
    "General Protection Fault" "Page Fault" "Double Fault"
    "ExitBootServices failed" "Kernel ELF corrupt" "BOOT HALT"
)

STRIPPED="$RUN_DIR/serial.stripped"
strip_ansi() { sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\x1b[=>]//g' "$SERIAL_OUT" > "$STRIPPED" 2>/dev/null || :; }
has() { grep -qF -- "$1" "$STRIPPED" 2>/dev/null; }
scan_fail() { local p; for p in "${FAIL_RES[@]}"; do has "$p" && { echo "$p"; return 0; }; done; return 1; }

# The verdict is decided ENTIRELY inside this loop while QEMU is alive. Only
# the explicit PASS branch (both markers observed on a live guest) returns 0.
# QEMU exiting on its own is a crash/shutdown -- a hard FAIL even if the log
# already shows the prompt (guest reached userspace then died), so it does NOT
# get a marker re-scan. Timeout exhaustion (loop falls through) is a hard FAIL
# too. There is no post-loop promotion path: a late PASS marker in the final
# read window can never turn a crash or an over-budget boot into success.
verdict=1
outcome="timeout"
for _ in $(seq 1 "$TIMEOUT_SEC"); do
    sleep 1
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        QEMU_PID=""; outcome="qemu-exited"; verdict=1; break
    fi
    strip_ansi
    if fp="$(scan_fail)"; then
        outcome="fail:$fp"; verdict=1; break
    fi
    if has "$PASS_MARKER" && has "$PROMPT_MARKER"; then
        outcome="pass"; verdict=0; break
    fi
done

# Best-effort screenshot via HMP monitor before teardown (only if still alive).
if [ -n "$SCREENSHOT" ] && [ -n "$QEMU_PID" ] && [ -S "$MON_SOCK" ] && command -v socat >/dev/null 2>&1; then
    printf 'screendump %s\n' "$SCREENSHOT" | socat - "UNIX-CONNECT:$MON_SOCK" >/dev/null 2>&1 || true
fi

case "$outcome" in
    pass)        note "PASS" ;;
    qemu-exited) note "FAIL (QEMU exited before reaching the prompt)" ;;
    fail:*)      note "FAIL (${outcome#fail:})" ;;
    *)           note "FAIL (timeout: no verdict within ${TIMEOUT_SEC}s)" ;;
esac
exit "$verdict"
