#!/usr/bin/env bash
# build-sb-test-disk.sh — Build a Secure Boot test disk image.
#
# Produces: build/system-disk-secureboot.img
#
# Boot chain verified by this disk:
#   OVMF (OVMF_CODE_4M.secboot.fd + OVMF_VARS_4M.snakeoil.fd)
#     → shimx64.efi  (signed with OVMF snakeoil key  → OVMF trusts it)
#         → grubx64.efi (signed with MOK.key → shim trusts via VENDOR_CERT_FILE=MOK.cer)
#             → kernel
#
# The snakeoil key is the OVMF test key at /usr/share/ovmf/PkKek-1-snakeoil.key
# (passphrase: snakeoil). The snakeoil cert is pre-enrolled in OVMF_VARS_4M.snakeoil.fd.
#
# Prerequisites:
#   apt install ovmf sbsigntool mtools
#   bash scripts/build.sh        (produces system-disk.img + signed grubx64.efi)
#   keys/MOK.key must exist

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$REPO_ROOT/build"
SRC_DISK="$BUILD/system-disk.img"
DST_DISK="$BUILD/system-disk-secureboot.img"
SHIM_SRC="$REPO_ROOT/shim/shimx64.efi"
SIGNED_SHIM="$BUILD/shimx64-sb-test.efi"
SNAKEOIL_KEY="/usr/share/ovmf/PkKek-1-snakeoil.key"
SNAKEOIL_CER="/usr/share/ovmf/PkKek-1-snakeoil.pem"
# ESP starts at sector 2048 (GPT layout from make-system-disk); offset = 2048 * 512
ESP_OFFSET=1048576

echo "[SB-TEST] Building Secure Boot test disk..."

# --- Prerequisite checks ---
for f in "$SRC_DISK" "$SHIM_SRC"; do
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

# --- Sign shimx64.efi with snakeoil key ---
echo "[SB-TEST] Signing shimx64.efi with OVMF snakeoil key..."
sbsign \
    --key "$SNAKEOIL_KEY_NOPASS" \
    --cert "$SNAKEOIL_CER" \
    --output "$SIGNED_SHIM" \
    "$SHIM_SRC" 2>/dev/null
sbverify --cert "$SNAKEOIL_CER" "$SIGNED_SHIM" \
    && echo "[SB-TEST] shim signature OK (snakeoil)"

# --- Copy system disk ---
echo "[SB-TEST] Copying $SRC_DISK -> $DST_DISK ..."
cp "$SRC_DISK" "$DST_DISK"

# --- Inject snakeoil-signed shim into ESP using mtools ---
# Drive letter 's:' with offset to the FAT32 EFI System Partition
MTOOLSRC_FILE="$BUILD/mtoolsrc-sb-test"
printf 'drive s: file="%s" offset=%d\n' "$DST_DISK" "$ESP_OFFSET" > "$MTOOLSRC_FILE"

echo "[SB-TEST] Injecting signed shim into ESP..."
MTOOLSRC="$MTOOLSRC_FILE" mcopy -o "$SIGNED_SHIM" "s:EFI/BOOT/BOOTX64.EFI"

echo "[SB-TEST] ESP contents after injection:"
MTOOLSRC="$MTOOLSRC_FILE" mdir "s:EFI/BOOT/"

# --- Copy OVMF secboot firmware to build/ for PowerShell scripts ---
if [ ! -f "$BUILD/OVMF_CODE_4M.secboot.fd" ]; then
    echo "[SB-TEST] Copying OVMF secboot firmware to $BUILD/ ..."
    cp /usr/share/OVMF/OVMF_CODE_4M.secboot.fd "$BUILD/"
    cp /usr/share/OVMF/OVMF_VARS_4M.snakeoil.fd "$BUILD/"
fi

# --- Cleanup temp files ---
rm -f "$SNAKEOIL_KEY_NOPASS" "$MTOOLSRC_FILE"

echo ""
echo "[SB-TEST] Done: $DST_DISK"
echo "[SB-TEST] OVMF firmware: $BUILD/OVMF_CODE_4M.secboot.fd"
echo "[SB-TEST] OVMF VARS:     $BUILD/OVMF_VARS_4M.snakeoil.fd (snakeoil PK/KEK/db enrolled)"
echo ""
echo "[SB-TEST] Boot chain:"
echo "  OVMF (secboot, snakeoil VARS)"
echo "    -> EFI/BOOT/BOOTX64.EFI  [shimx64.efi, snakeoil-signed]"
echo "    -> EFI/BOOT/grubx64.efi  [our bootloader, MOK-signed, trusted via VENDOR_CERT_FILE]"
echo "    -> kernel"
