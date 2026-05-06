#!/usr/bin/env bash
# boot-test-iso.sh -- boot the ISO release artifact in QEMU and assert it
# reaches userspace within a fixed timeout.
#
# Owner: Hybrid ISO / El Torito UEFI Boot feature in
# todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md.
#
# PASS = "Boot complete in" on serial. The C:\> prompt is NOT part of the
# PASS contract for ISO boot: ISO9660 carries only the ESP image; the
# kernel does not yet have an ISO9660 driver to mount the IXFS partition
# from CD-ROM media (kernel-fs domain owns the ISO9660 + media-aware
# C:\ mount work). Boot proves UEFI handoff + kernel reach userspace.
#
# Exit codes:
#   0 -- ISO booted to "Boot complete in"
#   1 -- boot timed out, QEMU crashed, or fail pattern matched
#   2 -- usage / preflight (missing tool / missing image / missing OVMF)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$REPO_ROOT/build"

ISO="$BUILD/release/disk.iso"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
TIMEOUT_SEC="${TIMEOUT_SEC:-30}"
RUN_DIR=""

err()  { printf '[ERROR] %s\n' "$*" >&2; }
note() { printf '[boot-test-iso] %s\n' "$*" >&2; }

usage() {
    cat <<EOF
Usage: $0 [--iso PATH] [--timeout SEC]

Options:
  --iso PATH     ISO path (default: $ISO)
  --timeout SEC  Boot timeout (default: $TIMEOUT_SEC)
  -h, --help     Show this message
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --iso)     ISO="$2"; shift 2 ;;
        --timeout) TIMEOUT_SEC="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) err "unknown arg: $1"; usage >&2; exit 2 ;;
    esac
done

[ -f "$ISO" ] || { err "missing ISO: $ISO (run scripts/release/build-iso.sh)"; exit 2; }
[ -f "$OVMF_CODE" ] || { err "missing OVMF: $OVMF_CODE (apt install ovmf)"; exit 2; }
[ -f "$OVMF_VARS_SRC" ] || { err "missing OVMF VARS template: $OVMF_VARS_SRC (apt install ovmf)"; exit 2; }
command -v qemu-system-x86_64 >/dev/null || { err "missing qemu-system-x86_64"; exit 2; }

mkdir -p "$BUILD"
RUN_DIR="$(mktemp -d "$BUILD/boot-test-iso.run.XXXXXX")"
cleanup() {
    local ec=$?
    if [ -n "${QEMU_PID:-}" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [ "$ec" -ne 0 ] && [ -f "${SERIAL_LOG:-/dev/null}" ]; then
        printf '\n[boot-test-iso] last 20 serial lines:\n' >&2
        tail -20 "$SERIAL_LOG" >&2 2>/dev/null || true
    fi
    [ -n "$RUN_DIR" ] && rm -rf "$RUN_DIR"
    exit "$ec"
}
trap cleanup EXIT INT TERM
OVMF_VARS_CP="$RUN_DIR/OVMF_VARS_4M.fd"
SERIAL_LOG="$RUN_DIR/serial.log"
cp "$OVMF_VARS_SRC" "$OVMF_VARS_CP"

QEMU_FLAGS=(
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$OVMF_VARS_CP"
    -cdrom "$ISO"
    -boot d
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

PASS_ALL=("Boot complete in")
FAIL_PAT=("KERNEL PANIC" "BUG_CHECK" "TRIPLE_FAULT")

for i in $(seq 1 "$TIMEOUT_SEC"); do
    sleep 1
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        err "QEMU exited unexpectedly at ${i}s"
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
        printf 'BOOT_TEST_ISO=PASS time_to_userspace_s=%d\n' "$i"
        exit 0
    fi
done

err "timeout: did not reach 'Boot complete in' within ${TIMEOUT_SEC}s"
exit 1
