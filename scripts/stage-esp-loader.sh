#!/usr/bin/env bash
# Stage the EFI loader chain into an ESP staging tree (called by the Makefile
# system-disk recipe; kept here so the policy is testable without a full build).
#
# Usage: stage-esp-loader.sh <shim-dir> <mok-key> <mok-crt> <unsigned-efi> \
#                            <signed-efi> <staging-dir>
#
# Layouts:
#   shim chain   shim + MokManager + MOK key, both binaries approved in
#                shim/SHA256SUMS: EFI/BOOT/BOOTX64.EFI = shim,
#                grubx64.efi = the SIGNED loader, MokManager in both places,
#                MOK.cer (DER) at the ESP root when the certificate exists.
#   direct boot  BOOTX64.EFI = the loader (signed when a MOK key exists).
#
# REQUIRE_SHIM=1 makes direct boot a refusal: a build that is meant to boot
# under Secure Boot must not quietly fall back to an image that only boots
# with Secure Boot off. It is opt-in because dev and CI builds legitimately
# ship no shim; the same switch turns an uncovered chain into a failure in
# scripts/test-secureboot-smoke.sh.
#
# Exit: 0 staged, 1 refused or failed, 2 usage (including REQUIRE_SHIM other
# than 0 or 1).
set -euo pipefail

if [ "$#" -ne 6 ]; then
    echo "usage: $0 <shim-dir> <mok-key> <mok-crt> <unsigned-efi> <signed-efi> <staging-dir>" >&2
    exit 2
fi
SHIM_DIR="$1"; MOK_KEY="$2"; MOK_CRT="$3"; UEFI_EFI="$4"; UEFI_EFI_SIGNED="$5"; STAGE="$6"
REQUIRE_SHIM="${REQUIRE_SHIM:-0}"
case "$REQUIRE_SHIM" in
    0|1) ;;
    *) echo "[ERROR] REQUIRE_SHIM must be 0 or 1, got '$REQUIRE_SHIM' (refusing to guess: any other value would silently disable the shim requirement)" >&2
       exit 2 ;;
esac
SHIM="$SHIM_DIR/shimx64.efi"; MM="$SHIM_DIR/mmx64.efi"

refuse_direct() {
    echo "[ERROR] REQUIRE_SHIM=1 but $1 -- refusing to stage a direct-boot image for a Secure Boot build" >&2
    exit 1
}

mkdir -p "$STAGE/EFI/BOOT"

EFI_TO_SHIP="$UEFI_EFI"
if [ -f "$MOK_KEY" ]; then
    if [ -f "$UEFI_EFI_SIGNED" ]; then
        EFI_TO_SHIP="$UEFI_EFI_SIGNED"
    else
        echo "[ERROR] MOK key present but signed loader missing ($UEFI_EFI_SIGNED) -- refusing to stage an unsigned loader for a signed build" >&2
        exit 1
    fi
fi

if [ -f "$SHIM" ] && [ -f "$MM" ] && [ -f "$MOK_KEY" ]; then
    echo "[DISK] Shim + MOK key found -- using Secure Boot chain-load layout (signed grubx64.efi)"
    # Both binaries need an approved entry: `sha256sum -c` would pass a file
    # the list does not name, and an absent list would skip the check entirely.
    bash "$(dirname "$0")/secure-boot/shim-hash-check.sh" "$SHIM_DIR/SHA256SUMS" "$SHIM" "$MM" \
        || { echo "[ERROR] Shim hash verification FAILED" >&2; exit 1; }
    cp "$SHIM" "$STAGE/EFI/BOOT/BOOTX64.EFI"
    cp "$EFI_TO_SHIP" "$STAGE/EFI/BOOT/grubx64.efi"
    cp "$MM" "$STAGE/EFI/BOOT/mmx64.efi"
    cp "$MM" "$STAGE/mmx64.efi"
    if [ -f "$MOK_CRT" ]; then
        openssl x509 -in "$MOK_CRT" -outform DER -out "$STAGE/MOK.cer"
        echo "[DISK] MOK.cer (DER) copied to ESP root (for MokManager enrollment)"
    fi
elif [ -f "$SHIM" ] && [ -f "$MM" ]; then
    [ "$REQUIRE_SHIM" = "1" ] && refuse_direct "the shim is present without a MOK key ($MOK_KEY)"
    echo "[DISK] Shim present but no MOK key -- keyless dev build uses direct (unsigned) boot, not the shim chain (shim would reject an unsigned grubx64.efi)"
    cp "$EFI_TO_SHIP" "$STAGE/EFI/BOOT/BOOTX64.EFI"
elif [ -f "$SHIM" ] || [ -f "$MM" ]; then
    echo "[ERROR] Partial shim directory -- need both shimx64.efi and mmx64.efi" >&2
    exit 1
else
    [ "$REQUIRE_SHIM" = "1" ] && refuse_direct "no shim is pinned in $SHIM_DIR"
    echo "[DISK] No shim -- using direct boot (run scripts/secure-boot/build-shim.sh for Secure Boot)"
    cp "$EFI_TO_SHIP" "$STAGE/EFI/BOOT/BOOTX64.EFI"
fi
