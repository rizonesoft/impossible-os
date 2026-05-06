#!/usr/bin/env bash
# boot-test-vbox.sh -- boot the VDI release artifact in VirtualBox and
# assert it reaches userspace via the serial pipe.
#
# Owner: VHD/VHDX/VDI conversion + validation feature in
# todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md.
#
# VirtualBox is OPTIONAL on dev hosts: this script auto-detects VBoxManage
# and emits a SKIP exit when the command is unavailable, so CI and dev
# workflows can call it unconditionally without forcing a VBox install.
# When VBoxManage IS present, the script creates a throwaway VM, attaches
# the VDI, boots with EFI firmware, captures serial via a pipe, and
# tears the VM down on exit.
#
# Exit codes:
#   0 -- VDI booted to userspace ("Boot complete in" on serial)  (PASS)
#   1 -- boot timed out, VBox crashed, or fail pattern matched  (FAIL)
#   2 -- usage / preflight (missing image only; VBoxManage absent is SKIP)
#   3 -- VBoxManage absent on host (SKIP, not a regression)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$REPO_ROOT/build"

DISK="$BUILD/release/disk.vdi"
TIMEOUT_SEC="${TIMEOUT_SEC:-60}"
VM_NAME="impossible-os-boot-test-$$"
# Per-invocation directory holds both the UNIX socket (VBox dial-target)
# and the serial log so two concurrent runs cannot cross-wire VMs to each
# other's listener or grep each other's log. Without this, a fixed
# build/boot-test-vbox.{sock,log} would let one run satisfy another's
# 'Boot complete in' assertion and let cleanup unlink the other's socket.
RUN_DIR=""

err()  { printf '[ERROR] %s\n' "$*" >&2; }
note() { printf '[boot-test-vbox] %s\n' "$*" >&2; }
skip() { printf '[boot-test-vbox] SKIP: %s\n' "$*" >&2; }

usage() {
    cat <<EOF
Usage: $0 [--disk PATH] [--timeout SEC]

Options:
  --disk PATH     VDI path (default: $DISK)
  --timeout SEC   Boot timeout (default: $TIMEOUT_SEC)
  -h, --help      Show this message

Exit code semantics:
  0 -- PASS (VDI booted to userspace under VirtualBox)
  1 -- FAIL (timeout / panic / VBox crash)
  2 -- preflight error (missing image)
  3 -- SKIP  (VBoxManage not installed on this host)
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

if ! command -v VBoxManage >/dev/null; then
    skip "VBoxManage not found on PATH; install VirtualBox to enable VDI boot validation"
    printf 'BOOT_TEST_VBOX=SKIP reason=VBoxManage_unavailable\n'
    exit 3
fi
[ -f "$DISK" ] || { err "missing disk: $DISK (run scripts/release/to-vdi.sh)"; exit 2; }

# Per-invocation run directory: holds the UNIX socket + serial log + the
# OVMF VARS-equivalent state. Created up-front so the cleanup trap below
# always has something to remove. The harness creates the socket FIRST
# and listens on it; VBox is configured in client mode (`--uartmode1
# client`) so it dials in once started. Server-mode would have VBox
# create the socket on `startvm`, but the listener (socat) would have
# already exited because the socket did not exist when it launched.
RUN_DIR="$(mktemp -d "$BUILD/boot-test-vbox.run.XXXXXX")"
PIPE_PATH="$RUN_DIR/serial.sock"
SERIAL_LOG="$RUN_DIR/serial.log"
: > "$SERIAL_LOG"

cleanup() {
    local ec=$?
    VBoxManage controlvm "$VM_NAME" poweroff >/dev/null 2>&1 || true
    sleep 1
    VBoxManage unregistervm "$VM_NAME" --delete >/dev/null 2>&1 || true
    if [ -n "${SERIAL_PID:-}" ] && kill -0 "$SERIAL_PID" 2>/dev/null; then
        kill "$SERIAL_PID" 2>/dev/null || true
    fi
    # On PASS keep the run dir so an operator can grep $SERIAL_LOG; on FAIL
    # tail it onto stderr first (best-effort) then remove.
    if [ "$ec" -ne 0 ] && [ -f "$SERIAL_LOG" ]; then
        printf '\n[boot-test-vbox] last 20 serial lines:\n' >&2
        tail -20 "$SERIAL_LOG" >&2 2>/dev/null || true
    fi
    rm -rf "$RUN_DIR"
    exit "$ec"
}
trap cleanup EXIT INT TERM

# Start the listener BEFORE creating the VM so the socket exists when VBox
# tries to dial. socat is required (nc -l -U has no portable in-place log
# pipeline); fall through to a clear error rather than racing.
if ! command -v socat >/dev/null; then
    err "socat is required for VBox serial capture (sudo apt install socat)"
    exit 2
fi
( socat -u "UNIX-LISTEN:$PIPE_PATH,fork" "OPEN:$SERIAL_LOG,creat,append" 2>/dev/null ) &
SERIAL_PID=$!

# Wait for the listener socket to actually appear, otherwise VBox client
# mode dials into nothing and the VM serial output is silently dropped.
for _ in $(seq 1 10); do
    [ -S "$PIPE_PATH" ] && break
    sleep 0.2
done
if [ ! -S "$PIPE_PATH" ]; then
    err "socat listener did not create $PIPE_PATH"
    exit 1
fi
# Self-check: listener still alive before we configure VBox to dial it.
if ! kill -0 "$SERIAL_PID" 2>/dev/null; then
    err "socat listener exited before VBox dial-in"
    exit 1
fi

note "creating throwaway VM $VM_NAME"
VBoxManage createvm --name "$VM_NAME" --ostype "Other_64" --register >/dev/null
VBoxManage modifyvm "$VM_NAME" \
    --memory 2048 \
    --firmware efi64 \
    --uart1 0x3F8 4 \
    --uartmode1 client "$PIPE_PATH" >/dev/null
VBoxManage storagectl "$VM_NAME" --name "AHCI" --add sata --controller IntelAhci >/dev/null
VBoxManage storageattach "$VM_NAME" \
    --storagectl "AHCI" --port 0 --device 0 \
    --type hdd --medium "$DISK" >/dev/null

note "starting VM (timeout ${TIMEOUT_SEC}s)..."
VBoxManage startvm "$VM_NAME" --type headless >/dev/null

# Match the boot-test-vhdx.sh PASS contract: kernel reached userspace.
# IXFS-mount-to-cmd.exe-prompt is owned by the kernel-fs domain.
PASS_ALL=("Boot complete in")
FAIL_PAT=("KERNEL PANIC" "BUG_CHECK" "TRIPLE_FAULT")

for i in $(seq 1 "$TIMEOUT_SEC"); do
    sleep 1
    state="$(VBoxManage showvminfo "$VM_NAME" --machinereadable 2>/dev/null | sed -n 's/^VMState="\(.*\)"/\1/p')"
    if [ "$state" = "aborted" ] || [ "$state" = "poweroff" ]; then
        err "VM ended in state=$state at ${i}s"
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
        printf 'BOOT_TEST_VBOX=PASS time_to_userspace_s=%d\n' "$i"
        exit 0
    fi
done

err "timeout: did not reach 'Boot complete in' within ${TIMEOUT_SEC}s"
tail -20 "$SERIAL_LOG" >&2 || true
exit 1
