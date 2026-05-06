#!/usr/bin/env bash
# boot-test-vhdx.sh -- boot the VHDX release artifact in QEMU and assert it
# reaches userspace within a fixed timeout.
#
# Owner: VHD/VHDX/VDI conversion + validation feature in
# todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md.
#
# Pattern follows scripts/test-smoke.sh (same OVMF + AHCI + serial-grep
# discipline) but reads the VHDX directly via QEMU's vhdx driver. Boot
# correctness was already proven by the raw-image smoke test; this gate
# proves the conversion-pipeline VHDX is bootable end-to-end.
#
# Exit codes:
#   0 -- VHDX booted to userspace ("Boot complete in" on serial)
#   1 -- boot timed out, QEMU crashed, or fail pattern matched
#   2 -- usage / preflight (missing tool / missing image / missing OVMF)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$REPO_ROOT/build"

DISK="$BUILD/release/disk.vhdx"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
TIMEOUT_SEC="${TIMEOUT_SEC:-30}"
# Per-invocation run directory holds the OVMF VARS copy + serial log so two
# concurrent runs cannot copy/write the same vars file or truncate/read each
# other's log -- the same race the VBox harness avoids with mktemp -d.
RUN_DIR=""

err()  { printf '[ERROR] %s\n' "$*" >&2; }
note() { printf '[boot-test-vhdx] %s\n' "$*" >&2; }

usage() {
    cat <<EOF
Usage: $0 [--disk PATH] [--timeout SEC]

Options:
  --disk PATH     VHDX path (default: $DISK)
  --timeout SEC   Boot timeout (default: $TIMEOUT_SEC)
  -h, --help      Show this message
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --disk)    DISK="$2"; shift 2 ;;
        --timeout) TIMEOUT_SEC="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) err "unknown arg: $1"; usage >&2; exit 2 ;;
    esac
done

[ -f "$DISK" ] || { err "missing disk: $DISK (run scripts/release/to-vhdx.sh)"; exit 2; }
[ -f "$OVMF_CODE" ] || { err "missing OVMF: $OVMF_CODE (apt install ovmf)"; exit 2; }
[ -f "$OVMF_VARS_SRC" ] || { err "missing OVMF VARS template: $OVMF_VARS_SRC (apt install ovmf)"; exit 2; }
command -v qemu-system-x86_64 >/dev/null || { err "missing qemu-system-x86_64"; exit 2; }

mkdir -p "$BUILD"
RUN_DIR="$(mktemp -d "$BUILD/boot-test-vhdx.run.XXXXXX")"
# Arm cleanup BEFORE any further work so a failure between mktemp and
# QEMU launch (e.g. cp OVMF VARS hits ENOSPC, set -e fires) still removes
# RUN_DIR. Defining cleanup inline so the trap can reference QEMU_PID
# even before it is set (kill -0 on empty pid is a no-op).
cleanup() {
    local ec=$?
    if [ -n "${QEMU_PID:-}" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [ "$ec" -ne 0 ] && [ -f "${SERIAL_LOG:-/dev/null}" ]; then
        printf '\n[boot-test-vhdx] last 20 serial lines:\n' >&2
        tail -20 "$SERIAL_LOG" >&2 2>/dev/null || true
    fi
    [ -n "$RUN_DIR" ] && rm -rf "$RUN_DIR"
    exit "$ec"
}
trap cleanup EXIT INT TERM
OVMF_VARS_CP="$RUN_DIR/OVMF_VARS_4M.fd"
SERIAL_LOG="$RUN_DIR/serial.log"
cp "$OVMF_VARS_SRC" "$OVMF_VARS_CP"

# Declare the VHDX format explicitly. Auto-detect can be brittle on
# stripped containers; the AHCI bus matches the legacy smoke test so the
# kernel sees an identical hardware topology.
QEMU_FLAGS=(
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$OVMF_VARS_CP"
    -drive "id=disk0,file=$DISK,format=vhdx,if=none"
    -device "ich9-ahci,id=ahci0"
    -device "ide-hd,drive=disk0,bus=ahci0.0"
    -m 2G
    -serial file:"$SERIAL_LOG"
    -no-reboot
    -no-shutdown
    -display none
)

if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then
    QEMU_FLAGS+=(-enable-kvm -cpu host)
    note "KVM acceleration enabled"
fi

> "$SERIAL_LOG"

note "starting QEMU (timeout ${TIMEOUT_SEC}s)..."
qemu-system-x86_64 "${QEMU_FLAGS[@]}" &
QEMU_PID=$!

# PASS = kernel reached userspace AND cmd.exe printed its prompt. The
# raw-image producer populates IXFS via mkfs-ixfs --populate, so C:\ mounts
# at boot and cmd.exe reaches the prompt; both markers must appear before
# the test calls PASS.
PASS_ALL=("Boot complete in" 'C:\>')
FAIL_PAT=("KERNEL PANIC" "BUG_CHECK" "TRIPLE_FAULT")

for i in $(seq 1 "$TIMEOUT_SEC"); do
    sleep 1
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        err "QEMU exited unexpectedly at ${i}s"
        tail -20 "$SERIAL_LOG" >&2 || true
        exit 1
    fi
    for p in "${FAIL_PAT[@]}"; do
        if grep -qF -- "$p" "$SERIAL_LOG" 2>/dev/null; then
            err "fail pattern matched: $p"
            exit 1
        fi
    done
    all=1
    for p in "${PASS_ALL[@]}"; do
        if ! grep -qF -- "$p" "$SERIAL_LOG" 2>/dev/null; then
            all=0
            break
        fi
    done
    if [ "$all" = 1 ]; then
        note "boot reached userspace at ${i}s"
        printf 'BOOT_TEST_VHDX=PASS time_to_userspace_s=%d\n' "$i"
        exit 0
    fi
done

err "timeout: did not reach 'Boot complete in' within ${TIMEOUT_SEC}s"
tail -20 "$SERIAL_LOG" >&2 || true
exit 1
