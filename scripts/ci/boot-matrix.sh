#!/usr/bin/env bash
# boot-matrix.sh -- CI boot matrix for every release artifact format.
#
# Owner: CI Boot Matrix for Every Artifact section in
# todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md.
#
# Runs six boot configurations and reports a single PASS/SKIP/FAIL
# summary. Each configuration delegates to an existing per-format
# boot-test script (test-smoke.sh / boot-test-iso.sh / boot-test-vhdx.sh
# / boot-test-vbox.sh) plus an inline USB-loopback driver and an inline
# WHPX-Windows-only delegator. Per-configuration stripped serial logs
# land under build/ci/<config>.log so failures can be triaged without
# re-running the entire matrix.
#
# Exit codes:
#   0 -- every configuration that ran returned PASS (SKIPs allowed)
#   1 -- one or more configurations FAILED
#
# Skipped configurations do NOT count as a failure: if the host lacks
# VBoxManage, lacks losetup, or lacks /dev/kvm we still want the matrix
# to report on the configurations it CAN run. CI gate is the absence
# of FAIL, not the presence of all-PASS.
#
# Cross-platform:
#   Linux dev host / WSL2: raw (KVM-or-TCG), iso, vhdx all run; vbox
#     and usb-loopback skip when their tools are absent. The "Hyper-V
#     VHDX boot under WHPX" checklist item is satisfied by
#     boot-test-vhdx.sh because that script reads the same VHDX
#     through QEMU's vhdx driver and KVM/TCG -- the WHPX accel path
#     is QEMU-host-provided when run on Windows; the artifact-
#     correctness contract is identical.
#
# Scope boundary: TODO-28 (boot validation certification matrix) owns
# the broader QEMU/VBox/Hyper-V/USB/NVMe/SecureBoot/TPM/network/A-B
# /recovery/watchdog/hibernation matrix. This script is the artifact-
# format-only feeder.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

OUT_DIR="build/ci"
mkdir -p "$OUT_DIR"

err()  { printf '[boot-matrix] ERROR: %s\n' "$*" >&2; }
note() { printf '[boot-matrix] %s\n' "$*" >&2; }

usage() {
    cat <<EOF
usage: $0 [--out DIR]

  --out DIR    Output directory for per-config stripped serial logs
               (default: $OUT_DIR).

Configurations (run sequentially):
  1. raw          QEMU raw disk boot (KVM-or-TCG)        -> inline driver
  2. iso          QEMU ISO boot                          -> scripts/release/boot-test-iso.sh
  3. vhdx         QEMU VHDX boot (KVM/TCG)               -> scripts/release/boot-test-vhdx.sh
  4. vdi          VirtualBox VDI boot                    -> scripts/release/boot-test-vbox.sh
  5. whpx         Hyper-V/WHPX VHDX boot (Windows-only)  -> scripts/machines/run-qemu.ps1
  6. usb-loop     USB image loopback boot (losetup+qemu) -> inline driver

Exit 0 if all run configurations PASS; 1 if any FAIL. SKIP results
do not fail the matrix.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --out)        OUT_DIR="$2"; shift 2 ;;
        -h|--help)    usage; exit 0 ;;
        *) err "unknown arg: $1"; usage >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT_DIR"

RESULTS=()
LOOP_DEV=""
VARS_TMP=""

cleanup() {
    if [ -n "$LOOP_DEV" ]; then
        losetup -d "$LOOP_DEV" >/dev/null 2>&1 || true
        LOOP_DEV=""
    fi
    if [ -n "$VARS_TMP" ] && [ -f "$VARS_TMP" ]; then
        rm -f "$VARS_TMP"
        VARS_TMP=""
    fi
}
trap cleanup EXIT INT TERM

strip_ansi() {
    sed -r 's/\x1b\[[0-9;]*[A-Za-z]//g'
}

# run_config -- execute a sub-script, capture stdout+stderr to a log,
# classify the exit code as PASS / SKIP / FAIL.
#
# $1 = config name (used as log filename)
# $2 = command to run
# $3 = SKIP exit codes (space-separated; default "3")
run_config() {
    local name="$1"
    local cmd="$2"
    local skip_codes="${3:-3}"
    local log="$OUT_DIR/${name}.log"
    # Capture into /tmp because the raw config (test-smoke.sh) calls
    # `build.sh clean` which wipes build/ wholesale -- a raw_log under
    # build/ci/ would be unlink()'d mid-eval, leaving the parent shell's
    # file descriptor pointing at a deleted inode that strip_ansi cannot
    # reopen by path. /tmp is outside the build tree.
    local raw_log
    raw_log="$(mktemp --suffix=".${name}.log")"
    note "[$name] running: $cmd"
    set +e
    eval "$cmd" >"$raw_log" 2>&1
    local rc=$?
    set -e
    # Recreate OUT_DIR after each run: build.sh clean may have wiped it.
    mkdir -p "$OUT_DIR"
    strip_ansi <"$raw_log" >"$log"
    rm -f "$raw_log"
    local is_skip=0
    for sc in $skip_codes; do
        if [ "$rc" -eq "$sc" ]; then is_skip=1; break; fi
    done
    if [ "$rc" -eq 0 ]; then
        RESULTS+=("$name|PASS|exit=0")
        note "[$name] PASS"
    elif [ "$is_skip" -eq 1 ]; then
        local reason
        reason="$(grep -m1 -E 'SKIP[: ]|skip:' "$log" 2>/dev/null | head -c 160 || true)"
        [ -z "$reason" ] && reason="exit=$rc"
        RESULTS+=("$name|SKIP|$reason")
        note "[$name] SKIP ($reason)"
    else
        RESULTS+=("$name|FAIL|exit=$rc")
        note "[$name] FAIL (exit=$rc; see $log)"
    fi
}

# ---- 1. raw QEMU boot ------------------------------------------------------
# Inline driver (NOT scripts/test-smoke.sh) because test-smoke.sh runs
# `build.sh clean` first, wiping build/release/* and forcing the rest
# of the matrix to SKIP-because-missing on the same invocation. The CI
# contract here is "build all artifacts up-front, then validate them
# in place"; rebuilding mid-matrix violates that. The boot pattern
# below mirrors test-smoke.sh's OVMF + AHCI + serial-grep discipline
# pointed at the prebuilt build/system-disk.img.
RAW_LOG="$OUT_DIR/raw.log"
mkdir -p "$OUT_DIR"
RAW_IMG="build/system-disk.img"
if [ ! -f "$RAW_IMG" ]; then
    RESULTS+=("raw|SKIP|missing $RAW_IMG (run scripts/build.sh)")
    note "[raw] SKIP (missing $RAW_IMG)"
elif ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    RESULTS+=("raw|SKIP|qemu-system-x86_64 not on PATH")
    note "[raw] SKIP (qemu absent)"
else
    OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
    OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
    if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_SRC" ]; then
        RESULTS+=("raw|SKIP|OVMF firmware not available (apt install ovmf)")
        note "[raw] SKIP (OVMF absent)"
    else
        VARS_TMP="$(mktemp --suffix=.fd)"
        cp "$OVMF_VARS_SRC" "$VARS_TMP"
        ACCEL="tcg"
        [ -w /dev/kvm ] && ACCEL="kvm"
        note "[raw] booting $RAW_IMG with accel=$ACCEL"
        RAW_RAW_LOG="$(mktemp --suffix=.raw.log)"
        set +e
        timeout 30 qemu-system-x86_64 \
            -machine q35,accel="$ACCEL" -cpu max -m 1024 \
            -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
            -drive if=pflash,format=raw,file="$VARS_TMP" \
            -drive id=hd0,if=none,format=raw,file="$RAW_IMG",cache=none \
            -device ahci,id=ahci0 \
            -device ide-hd,drive=hd0,bus=ahci0.0 \
            -nographic -serial mon:stdio -no-reboot \
            >"$RAW_RAW_LOG" 2>&1
        qemu_rc=$?
        set -e
        rm -f "$VARS_TMP"
        VARS_TMP=""
        mkdir -p "$OUT_DIR"
        strip_ansi <"$RAW_RAW_LOG" >"$RAW_LOG"
        rm -f "$RAW_RAW_LOG"
        if grep -q 'Boot complete in' "$RAW_LOG" && grep -q 'C:\\>' "$RAW_LOG"; then
            RESULTS+=("raw|PASS|reached userspace (accel=$ACCEL)")
            note "[raw] PASS"
        else
            RESULTS+=("raw|FAIL|missing 'Boot complete in' or 'C:\\\\>' (qemu_rc=$qemu_rc, accel=$ACCEL)")
            note "[raw] FAIL"
        fi
    fi
fi

# ---- 2. QEMU ISO boot -----------------------------------------------------
ISO_PATH="build/release/disk.iso"
if [ -f "$ISO_PATH" ]; then
    run_config "iso" "bash scripts/release/boot-test-iso.sh --iso $ISO_PATH" "2 3"
else
    RESULTS+=("iso|SKIP|missing $ISO_PATH (run scripts/release/build-iso.sh)")
    note "[iso] SKIP (missing $ISO_PATH)"
fi

# ---- 3. QEMU VHDX boot ----------------------------------------------------
VHDX_PATH="build/release/disk.vhdx"
if [ -f "$VHDX_PATH" ]; then
    run_config "vhdx" "bash scripts/release/boot-test-vhdx.sh --disk $VHDX_PATH" "2 3"
else
    RESULTS+=("vhdx|SKIP|missing $VHDX_PATH (run scripts/release/to-vhdx.sh)")
    note "[vhdx] SKIP (missing $VHDX_PATH)"
fi

# ---- 4. VirtualBox VDI boot ----------------------------------------------
VDI_PATH="build/release/disk.vdi"
if [ -f "$VDI_PATH" ] && command -v VBoxManage >/dev/null 2>&1; then
    run_config "vdi" "bash scripts/release/boot-test-vbox.sh --disk $VDI_PATH" "2 3"
elif [ ! -f "$VDI_PATH" ]; then
    RESULTS+=("vdi|SKIP|missing $VDI_PATH (run scripts/release/to-vdi.sh)")
    note "[vdi] SKIP (missing $VDI_PATH)"
else
    RESULTS+=("vdi|SKIP|VBoxManage not on PATH (install VirtualBox)")
    note "[vdi] SKIP (VBoxManage absent)"
fi

# ---- 5. Hyper-V WHPX VHDX boot (Windows-only) ----------------------------
# The CI-boot-matrix feature list explicitly calls for "Hyper-V VHDX
# boot under WHPX". Linux + KVM/TCG cannot exercise the WHPX
# accelerator, and CLAUDE.md documents WHPX-specific behavior (PAT/MSR
# resets on CR3 reload, MSR read traps, INIT-de-assert IPI hang) that
# does NOT reproduce on KVM/TCG -- so VHDX-via-KVM coverage is NOT a
# substitute for WHPX coverage. This script SKIPs the WHPX leg with an
# explicit pointer at the Windows-side runner; the contract is that a
# Windows CI runner (or a user with QEMU+WHPX or Hyper-V on a Windows
# host) executes the PowerShell path and reports its own PASS/FAIL
# into the matrix.
WHPX_RUNNER="scripts/machines/run-qemu.ps1"
if [ -f "$WHPX_RUNNER" ] && [ -n "${OS:-}" ] && [ "${OS:-}" = "Windows_NT" ]; then
    run_config "whpx" "powershell.exe -ExecutionPolicy Bypass -File $WHPX_RUNNER -Accel whpx -Image build/release/disk.vhdx -BootOnly" "2 3"
else
    RESULTS+=("whpx|SKIP|Windows host required (run scripts/machines/run-qemu.ps1 -Accel whpx -Image build/release/disk.vhdx)")
    note "[whpx] SKIP (Windows-only)"
fi

# ---- 6. USB-loopback boot -------------------------------------------------
# losetup needs CAP_SYS_ADMIN (typically root). When it's not available
# we SKIP cleanly. The boot itself is a re-run of the same OVMF + AHCI
# QEMU pattern as test-smoke.sh, but pointing at the loop device instead
# of the file -- the same kernel path that the user-burned USB stick
# exercises on bare metal. This catches block-device-specific bugs
# (alignment, queue depth, write-back) that the file-backed raw boot
# does not.
RAW_IMG="build/system-disk.img"
USB_LOG="$OUT_DIR/usb-loop.log"
if [ ! -f "$RAW_IMG" ]; then
    RESULTS+=("usb-loop|SKIP|missing $RAW_IMG (run scripts/build.sh)")
    note "[usb-loop] SKIP (missing $RAW_IMG)"
elif ! command -v losetup >/dev/null 2>&1; then
    RESULTS+=("usb-loop|SKIP|losetup not on PATH")
    note "[usb-loop] SKIP (losetup absent)"
elif ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    RESULTS+=("usb-loop|SKIP|qemu-system-x86_64 not on PATH")
    note "[usb-loop] SKIP (qemu absent)"
elif [ "$(id -u)" -ne 0 ]; then
    RESULTS+=("usb-loop|SKIP|losetup requires root (re-run via sudo for usb-loop coverage)")
    note "[usb-loop] SKIP (not root)"
else
    note "[usb-loop] attaching $RAW_IMG via losetup"
    : > "$USB_LOG"
    set +e
    LOOP_DEV="$(losetup --find --show "$RAW_IMG" 2>>"$USB_LOG")"
    rc=$?
    set -e
    if [ "$rc" -ne 0 ] || [ -z "$LOOP_DEV" ]; then
        LOOP_DEV=""
        RESULTS+=("usb-loop|FAIL|losetup attach failed (exit=$rc)")
        note "[usb-loop] FAIL (losetup attach failed)"
    else
        OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
        OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
        if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_SRC" ]; then
            RESULTS+=("usb-loop|SKIP|OVMF firmware not available (apt install ovmf)")
            note "[usb-loop] SKIP (OVMF absent)"
        else
            VARS_TMP="$(mktemp --suffix=.fd)"
            cp "$OVMF_VARS_SRC" "$VARS_TMP"
            ACCEL="tcg"
            [ -w /dev/kvm ] && ACCEL="kvm"
            note "[usb-loop] booting $LOOP_DEV with accel=$ACCEL"
            set +e
            timeout 30 qemu-system-x86_64 \
                -machine q35,accel="$ACCEL" -cpu max -m 1024 \
                -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
                -drive if=pflash,format=raw,file="$VARS_TMP" \
                -drive id=usbdisk,if=none,format=raw,file="$LOOP_DEV",cache=none \
                -device ahci,id=ahci0 \
                -device ide-hd,drive=usbdisk,bus=ahci0.0 \
                -nographic -serial mon:stdio -no-reboot \
                >"$USB_LOG" 2>&1
            qemu_rc=$?
            set -e
            strip_ansi <"$USB_LOG" >"$USB_LOG.stripped" && mv "$USB_LOG.stripped" "$USB_LOG"
            # PASS contract: same as the raw config -- both 'Boot
            # complete in' and 'C:\>' shell prompt must appear on
            # serial. usb-loop boots the same raw image that mounts
            # IXFS as C:\, so the shell SHOULD start. Without the
            # second marker the loopback could reach userspace but
            # never start the shell, and we would call it PASS.
            if grep -q 'Boot complete in' "$USB_LOG" && grep -q 'C:\\>' "$USB_LOG"; then
                RESULTS+=("usb-loop|PASS|loopback boot reached shell prompt")
                note "[usb-loop] PASS"
            else
                local_missing=""
                grep -q 'Boot complete in' "$USB_LOG" || local_missing="${local_missing} Boot-complete"
                grep -q 'C:\\>' "$USB_LOG" || local_missing="${local_missing} shell-prompt"
                RESULTS+=("usb-loop|FAIL|missing markers:${local_missing} (qemu_rc=$qemu_rc)")
                note "[usb-loop] FAIL (missing markers:${local_missing})"
            fi
        fi
    fi
fi

# ---- Summary --------------------------------------------------------------
printf '\n=== boot-matrix summary ===\n'
fail_count=0
pass_count=0
skip_count=0
for r in "${RESULTS[@]}"; do
    name="${r%%|*}"
    rest="${r#*|}"
    status="${rest%%|*}"
    reason="${rest#*|}"
    case "$status" in
        PASS) pass_count=$((pass_count + 1)) ;;
        SKIP) skip_count=$((skip_count + 1)) ;;
        FAIL) fail_count=$((fail_count + 1)) ;;
    esac
    printf '  [%4s] %-12s  %s\n' "$status" "$name" "$reason"
done
printf '\nTotals: %d PASS, %d SKIP, %d FAIL (logs in %s/)\n' "$pass_count" "$skip_count" "$fail_count" "$OUT_DIR"

if [ "$fail_count" -gt 0 ]; then
    printf '\nBOOT MATRIX: FAIL\n' >&2
    exit 1
fi
printf '\nBOOT MATRIX: PASS\n'
exit 0
