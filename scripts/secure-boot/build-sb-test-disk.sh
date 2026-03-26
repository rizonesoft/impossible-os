#!/usr/bin/env bash
# build-sb-test-disk.sh -- Build a Secure Boot test disk image.
#
# Produces: build/system-disk-secureboot.img
#
# Boot chain verified by this disk:
#   OVMF (OVMF_CODE_4M.snakeoil.fd + OVMF_VARS_4M.snakeoil.fd, q35+SMM)
#     -> BOOTX64.EFI (our bootloader, snakeoil-signed -> OVMF trusts it)
#         -> kernel
#
# Why we sign BOOTX64.EFI directly instead of going through the shim:
#   The shim binary (shim/shimx64.efi) has PE/COFF section gaps that cause
#   OVMF's Authenticode hash to differ from the hash sbsign computed, so OVMF
#   always rejects it with "Security Violation (0x1A)" even though sbverify
#   reports OK.  Signing our own bootloader (which has no such gaps) works
#   correctly.  The shim is still needed for REAL hardware (where we cannot
#   pre-enroll our key into the firmware) but is not required for QEMU testing.
#
# IMPORTANT: OVMF SB firmware requires Q35 (-machine q35).
#   The default pc-i440fx machine hangs before initialising the display.
#   smm=on is NOT required -- it adds runtime VARS protection but is
#   unsupported by WHPX; boot-time SB enforcement works without it.
#
# IMPORTANT: CODE and VARS must be the matched snakeoil pair.
#   OVMF_CODE_4M.secboot.fd + OVMF_VARS_4M.snakeoil.fd is a MISMATCHED pair.
#
# The snakeoil key is the OVMF test key at /usr/share/ovmf/PkKek-1-snakeoil.key
# (passphrase: snakeoil). The snakeoil cert is pre-enrolled in OVMF_VARS_4M.snakeoil.fd
# as PK, KEK, and db -- so OVMF trusts snakeoil-signed binaries.
#
# Prerequisites:
#   apt install ovmf sbsigntool mtools
#   bash scripts/build.sh   (produces system-disk.img and build/tools/BOOTX64.EFI)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$REPO_ROOT/build"
SRC_DISK="$BUILD/system-disk.img"
DST_DISK="$BUILD/system-disk-secureboot.img"
BOOTX64_SRC="$BUILD/tools/BOOTX64.EFI"
SIGNED_BOOTX64="$BUILD/bootx64-sb-test.efi"
SNAKEOIL_KEY="/usr/share/ovmf/PkKek-1-snakeoil.key"
SNAKEOIL_CER="/usr/share/ovmf/PkKek-1-snakeoil.pem"
# ESP starts at sector 2048 (GPT layout from make-system-disk); offset = 2048 * 512
ESP_OFFSET=1048576

echo "[SB-TEST] Building Secure Boot test disk..."

# --- Prerequisite checks ---
for f in "$SRC_DISK" "$BOOTX64_SRC"; do
    if [ ! -f "$f" ]; then
        echo "[ERROR] Not found: $f"
        echo "        Run: bash scripts/build.sh"
        exit 1
    fi
done
for tool in sbsign sbverify openssl mcopy; do
    command -v "$tool" >/dev/null || {
        echo "[ERROR] Missing tool: $tool  (apt install sbsigntool mtools)"
        exit 1
    }
done
if [ ! -f "$SNAKEOIL_KEY" ]; then
    echo "[ERROR] OVMF snakeoil key not found: $SNAKEOIL_KEY"
    echo "        Install: sudo apt install ovmf"
    exit 1
fi

# --- Extract snakeoil key (no passphrase) ---
SNAKEOIL_KEY_NOPASS="$BUILD/snakeoil-nopass.key"
echo "[SB-TEST] Extracting OVMF snakeoil key (passphrase: snakeoil)..."
openssl rsa \
    -in "$SNAKEOIL_KEY" \
    -passin pass:snakeoil \
    -out "$SNAKEOIL_KEY_NOPASS" 2>/dev/null

# --- Sign BOOTX64.EFI with snakeoil key ---
# BOOTX64.EFI may already carry a MOK signature from 'make sign-efi'; sbsign
# appends the snakeoil signature alongside it.  OVMF accepts the binary when
# ANY embedded signing cert matches an enrolled db entry.
echo "[SB-TEST] Signing BOOTX64.EFI with OVMF snakeoil key..."
cp "$BOOTX64_SRC" "$SIGNED_BOOTX64"
sbsign \
    --key "$SNAKEOIL_KEY_NOPASS" \
    --cert "$SNAKEOIL_CER" \
    --output "$SIGNED_BOOTX64" \
    "$SIGNED_BOOTX64" 2>/dev/null
sbverify --cert "$SNAKEOIL_CER" "$SIGNED_BOOTX64" \
    && echo "[SB-TEST] BOOTX64.EFI signature OK (snakeoil)"

# --- Copy system disk ---
echo "[SB-TEST] Copying $SRC_DISK -> $DST_DISK ..."
cp "$SRC_DISK" "$DST_DISK"

# --- Inject snakeoil-signed BOOTX64.EFI into ESP using mtools ---
# Drive letter 's:' with offset to the FAT32 EFI System Partition
MTOOLSRC_FILE="$BUILD/mtoolsrc-sb-test"
printf 'drive s: file="%s" offset=%d\n' "$DST_DISK" "$ESP_OFFSET" > "$MTOOLSRC_FILE"

echo "[SB-TEST] Injecting signed BOOTX64.EFI into ESP..."
MTOOLSRC="$MTOOLSRC_FILE" mcopy -o "$SIGNED_BOOTX64" "s:EFI/BOOT/BOOTX64.EFI"

echo "[SB-TEST] ESP contents after injection:"
MTOOLSRC="$MTOOLSRC_FILE" mdir "s:EFI/BOOT/"

# --- Copy matched snakeoil firmware pair to build/ for PowerShell scripts ---
# Must use the matched pair: OVMF_CODE_4M.snakeoil.fd + OVMF_VARS_4M.snakeoil.fd
# Using OVMF_CODE_4M.secboot.fd with snakeoil VARS causes a firmware hang.
echo "[SB-TEST] Copying OVMF snakeoil firmware pair to $BUILD/ ..."
cp /usr/share/OVMF/OVMF_CODE_4M.snakeoil.fd "$BUILD/"
cp /usr/share/OVMF/OVMF_VARS_4M.snakeoil.fd "$BUILD/"

# --- Cleanup temp files ---
rm -f "$SNAKEOIL_KEY_NOPASS" "$MTOOLSRC_FILE"

echo ""
echo "[SB-TEST] Done: $DST_DISK"
echo "[SB-TEST] OVMF firmware: $BUILD/OVMF_CODE_4M.snakeoil.fd"
echo "[SB-TEST] OVMF VARS:     $BUILD/OVMF_VARS_4M.snakeoil.fd (snakeoil PK/KEK/db enrolled)"
echo ""
echo "[SB-TEST] Boot chain:"
echo "  OVMF (snakeoil CODE + snakeoil VARS -- matched pair, q35)"
echo "    -> EFI/BOOT/BOOTX64.EFI  [our bootloader, snakeoil-signed]"
echo "    -> kernel"
