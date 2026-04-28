#!/usr/bin/env bash
# sign-efi.sh -- Sign BOOTX64.EFI with the MOK private key.
#
# Signs to a temporary file, verifies the signature, then atomically
# replaces the original. This prevents corruption if sbsign fails mid-write.
#
# Usage:
#   bash scripts/sign-efi.sh
#
# Environment overrides (optional):
#   MOK_KEY  -- path to private key   (default: keys/MOK.key)
#   MOK_CRT  -- path to certificate   (default: keys/MOK.cer)
#   EFI_BIN  -- binary to sign        (default: build/tools/BOOTX64.EFI)
#
# CI usage:
#   MOK_KEY=/run/secrets/mok.key MOK_CRT=/run/secrets/mok.cer bash scripts/sign-efi.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MOK_KEY="${MOK_KEY:-$REPO_ROOT/keys/MOK.key}"
MOK_CRT="${MOK_CRT:-$REPO_ROOT/keys/MOK.cer}"
EFI_BIN="${EFI_BIN:-$REPO_ROOT/build/tools/BOOTX64.EFI}"

if [ ! -f "$MOK_KEY" ]; then
    echo "[SIGN] keys/MOK.key not found -- skipping signing (dev build)"
    exit 0
fi

if [ ! -f "$EFI_BIN" ]; then
    echo "[ERROR] EFI binary not found: $EFI_BIN"
    echo "        Run: bash scripts/build.sh"
    exit 1
fi

# Sign to temp file, verify, then atomic replace
TMPFILE="${EFI_BIN}.signing.tmp"
trap 'rm -f "$TMPFILE"' EXIT

echo "[SIGN] Signing $EFI_BIN with MOK..."
sbsign --key "$MOK_KEY" --cert "$MOK_CRT" --output "$TMPFILE" "$EFI_BIN"
sbverify --cert "$MOK_CRT" "$TMPFILE" \
    && echo "[SIGN] Signature verification OK" \
    || { echo "[SIGN] ERROR: signature verification FAILED"; exit 1; }

# ----------------------------------------------------------------------------
# MS UEFI CA generation tracking
#
# Microsoft Corporation UEFI CA 2011 is scheduled to expire June 2026.
# A replacement Microsoft Corporation UEFI CA 2023 is being enrolled into
# firmware DBs via Windows Update through 2025-2026. Devices booting a
# shim signed only by 2011 CA will start failing on machines whose
# firmware DB has rotated to 2023-only.
#
# When shim/shimx64.efi is pinned in-tree (currently NOT pinned -- waiting
# on MS to publish the 2023-CA-signed binary), parse `sbverify --list`
# and emit `[shim] signed-by: Microsoft Corporation UEFI CA YYYY`.
#
# Graduated deprecation policy (Codex design H2 fix 2026-04-29):
#   pre-WARN-window:   accept 2011 silently (transition window not open)
#   WARN window..expiry: emit WARN (60-day pre-expiry margin)
#   post-expiry:       FAIL hard (sign pipeline aborts)
#
# Thresholds derived from named constants (Codex Q3 fix):
#   MS_UEFI_CA_2011_EXPIRY    = 2026-06-30 (June 2026 per MS guidance)
#   MS_UEFI_CA_2011_WARN_FROM = 2026-05-01 (expiry minus 60 days)
# ----------------------------------------------------------------------------
SHIM_BIN="${SHIM_BIN:-$REPO_ROOT/shim/shimx64.efi}"
MS_UEFI_CA_2011_EXPIRY="2026-06-30"
MS_UEFI_CA_2011_WARN_FROM="2026-05-01"

if [ -f "$SHIM_BIN" ]; then
    if command -v sbverify >/dev/null 2>&1; then
        SBVERIFY_OUT="$(sbverify --list "$SHIM_BIN" 2>&1 || true)"
        SHIM_CA_YEAR="$(printf '%s\n' "$SBVERIFY_OUT" \
            | grep -oE 'Microsoft Corporation UEFI CA 20[12][0-9]' \
            | grep -oE '20[12][0-9]' \
            | sort -u | tail -1)"
        # Test-only override: SHIM_CA_TEST_TODAY lets the tooling
        # test exercise pre-warn/warn/post-expiry windows without a
        # system clock-set. Production paths read live system date.
        TODAY="${SHIM_CA_TEST_TODAY:-$(date -u +%Y-%m-%d)}"
        if [ -n "$SHIM_CA_YEAR" ]; then
            echo "[shim] signed-by: Microsoft Corporation UEFI CA $SHIM_CA_YEAR"
            if [ "$SHIM_CA_YEAR" = "2011" ]; then
                if [ "$TODAY" \> "$MS_UEFI_CA_2011_EXPIRY" ]; then
                    echo "[shim] FAIL -- shim is signed by 2011 CA which expired $MS_UEFI_CA_2011_EXPIRY. Pin a 2023-CA-signed shim binary; abort signing pipeline." >&2
                    exit 1
                elif [ "$TODAY" \> "$MS_UEFI_CA_2011_WARN_FROM" ] || [ "$TODAY" = "$MS_UEFI_CA_2011_WARN_FROM" ]; then
                    echo "[shim] WARN -- shim is signed only by deprecated MS UEFI CA 2011 (expires $MS_UEFI_CA_2011_EXPIRY); firmware DB rotation past $MS_UEFI_CA_2011_WARN_FROM may refuse boot. Re-sign against the 2023 CA before expiry." >&2
                fi
            fi
        else
            echo "[shim] WARN -- $SHIM_BIN present but sbverify --list reports no recognized Microsoft Corporation UEFI CA generation; verify the binary." >&2
        fi
    else
        echo "[shim] sbverify not on PATH; skipping shim CA generation check (install sbsigntools for release-image signing)"
    fi
else
    echo "[shim] $SHIM_BIN not pinned (waiting on MS to publish 2023-CA-signed binary)"
fi

mv "$TMPFILE" "$EFI_BIN"
