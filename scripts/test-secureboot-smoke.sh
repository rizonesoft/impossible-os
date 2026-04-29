#!/usr/bin/env bash
# test-secureboot-smoke.sh -- non-interactive Secure Boot smoke test
# for the Unified Kernel Image (UKI) and the split-path BOOTX64.EFI
# signing pipeline. Verifies signature + UKI section structure offline
# without launching QEMU (avoids the interactive MokManager step that
# scripts/debug/kernel/run-secureboot.bat requires).
#
# What this checks (offline; no actual QEMU boot needed):
#   1. build/tools/BOOTX64.EFI exists, has a sbsign signature, and
#      verifies against keys/MOK.cer.
#   2. build/tools/BOOTX64.UKI.efi exists, has a sbsign signature, and
#      verifies against keys/MOK.cer.
#   3. The UKI artifact's PE section table contains .linux, .cmdline,
#      .osrel sections (objdump -h grep). All three must be present
#      for the bootloader's detect_uki_sections() to fire the UKI
#      fast path.
#   4. shim/shimx64.efi (when present) is recognized as signed by a
#      Microsoft Corporation UEFI CA generation that the build pipeline
#      knows how to track.
#
# What this does NOT do:
#   - Actually boot QEMU with Secure Boot enabled (interactive MOK
#     enrollment via MokManager). Use scripts/debug/kernel/run-secureboot.bat
#     for the manual end-to-end flow.
#   - Re-run sbsign. The smoke trusts the build pipeline's output.
#
# Exit 0: all checks pass. Exit 1: any check fails. Exit 2: required
# tooling missing (sbverify, llvm-objdump-19).

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EFI="$REPO_ROOT/build/tools/BOOTX64.EFI"
UKI="$REPO_ROOT/build/tools/BOOTX64.UKI.efi"
MOK_CRT="${MOK_CRT:-$REPO_ROOT/keys/MOK.cer}"

PASS=0
FAIL=0
FAILURES=()

t_ok()   { PASS=$((PASS + 1)); echo "  PASS  $1"; }
t_fail() { FAIL=$((FAIL + 1)); FAILURES+=("$1"); echo "  FAIL  $1${2:+ -- $2}"; }

echo "=================================================================="
echo " Secure Boot smoke -- BOOTX64.EFI + BOOTX64.UKI.efi signatures"
echo "=================================================================="

# Tooling check
for cmd in sbverify llvm-objdump-19; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "[ERROR] required tool '$cmd' not found on PATH" >&2
        echo "  install: sudo apt install sbsigntools llvm-19" >&2
        exit 2
    fi
done

if [ ! -f "$EFI" ]; then
    t_fail "BOOTX64.EFI missing" "run: bash scripts/build.sh"
fi
if [ ! -f "$UKI" ]; then
    t_fail "BOOTX64.UKI.efi missing" "run: bash scripts/build.sh"
fi
if [ ! -f "$MOK_CRT" ]; then
    echo "[skip] keys/MOK.cer not found -- dev build without local signing keys"
    echo "  signature verification skipped; UKI structural checks still run"
fi

# 1. BOOTX64.EFI signature verifies against MOK
if [ -f "$EFI" ] && [ -f "$MOK_CRT" ]; then
    if sbverify --cert "$MOK_CRT" "$EFI" 2>&1 | grep -q "Signature verification OK"; then
        t_ok "BOOTX64.EFI signature verifies against keys/MOK.cer"
    else
        t_fail "BOOTX64.EFI signature does NOT verify against MOK.cer"
    fi
fi

# 2. BOOTX64.UKI.efi signature verifies against MOK
if [ -f "$UKI" ] && [ -f "$MOK_CRT" ]; then
    if sbverify --cert "$MOK_CRT" "$UKI" 2>&1 | grep -q "Signature verification OK"; then
        t_ok "BOOTX64.UKI.efi signature verifies against keys/MOK.cer"
    else
        t_fail "BOOTX64.UKI.efi signature does NOT verify against MOK.cer"
    fi
fi

# 3. UKI section table carries .linux + .cmdline + .osrel
if [ -f "$UKI" ]; then
    SEC_DUMP="$(llvm-objdump-19 -h "$UKI" 2>&1)"
    for sec in .linux .cmdline .osrel; do
        if echo "$SEC_DUMP" | grep -qE "[[:space:]]${sec}[[:space:]]"; then
            t_ok "UKI section '$sec' present"
        else
            t_fail "UKI section '$sec' MISSING from PE table"
        fi
    done
    LINUX_SIZE_HEX="$(echo "$SEC_DUMP" | awk '/[[:space:]]\.linux[[:space:]]/ {print $3}')"
    LINUX_SIZE_DEC=$((16#$LINUX_SIZE_HEX))
    if [ "$LINUX_SIZE_DEC" -gt 1048576 ]; then
        t_ok "UKI .linux section size ${LINUX_SIZE_DEC} bytes (>1 MiB; plausible kernel)"
    else
        t_fail "UKI .linux section size ${LINUX_SIZE_DEC} bytes is implausibly small"
    fi
fi

# 4. Shim presence + CA generation recognition
SHIM="$REPO_ROOT/shim/shimx64.efi"
if [ -f "$SHIM" ]; then
    if sbverify --list "$SHIM" 2>&1 | grep -qE "Microsoft Corporation UEFI CA 20[12][0-9]"; then
        SHIM_CA_YEAR="$(sbverify --list "$SHIM" 2>&1 | grep -oE 'Microsoft Corporation UEFI CA 20[12][0-9]' | grep -oE '20[12][0-9]' | sort -u | tail -1)"
        t_ok "shim is signed by Microsoft Corporation UEFI CA $SHIM_CA_YEAR"
    else
        t_fail "shim/shimx64.efi present but no recognized MS UEFI CA generation"
    fi
fi

echo "=================================================================="
TOTAL=$((PASS + FAIL))
if [ "$FAIL" -eq 0 ]; then
    echo "  PASS  $TOTAL/$TOTAL Secure Boot smoke checks"
    echo "=================================================================="
    exit 0
else
    echo "  FAIL  $FAIL/$TOTAL Secure Boot smoke checks failed"
    for f in "${FAILURES[@]}"; do
        echo "        - $f"
    done
    echo "=================================================================="
    exit 1
fi
