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

# Shim CA expiry gate -- verifies the pinned shim/shimx64.efi against the
# graduated MS UEFI CA 2011 deprecation policy. Runs BEFORE the MOK-key
# early-return so dev/CI builds without a local MOK key still refuse to
# package an expired shim. The shim binary is independent of the
# BOOTX64.EFI we sign locally, so the gate has no dependency on sbsign
# success and runs once.
#
# Graduated deprecation policy:
#   pre-WARN-window:    accept 2011 silently (transition window not open)
#   WARN window..expiry: emit WARN (60-day pre-expiry margin)
#   post-expiry:        FAIL hard (sign pipeline aborts)
#
# Test-only override SHIM_CA_TEST_TODAY exercises pre-warn/warn/expired
# windows from test-tooling.sh without touching the system clock.
shim_ca_gate() {
    local SHIM_BIN_LOCAL="${SHIM_BIN:-$REPO_ROOT/shim/shimx64.efi}"
    local MS_UEFI_CA_2011_EXPIRY_LOCAL="2026-06-30"
    local MS_UEFI_CA_2011_WARN_FROM_LOCAL="2026-05-01"
    [ -f "$SHIM_BIN_LOCAL" ] || { echo "[shim] $SHIM_BIN_LOCAL not pinned (waiting on MS to publish 2023-CA-signed binary)"; return 0; }
    command -v sbverify >/dev/null 2>&1 || { echo "[shim] sbverify not on PATH; skipping shim CA generation check (install sbsigntools for release-image signing)"; return 0; }
    # Capture sbverify rc separately from output -- a nonzero exit on a
    # present shim binary is a hard fail (Codex final adversarial M1
    # 2026-04-29). Conflating tool failure with "no recognized CA"
    # would let a corrupt/unreadable/unsupported shim through the WARN
    # path and ship after the 2026-06-30 cutoff without verification.
    local SBVERIFY_OUT_LOCAL
    local SBVERIFY_RC_LOCAL=0
    SBVERIFY_OUT_LOCAL="$(sbverify --list "$SHIM_BIN_LOCAL" 2>&1)" || SBVERIFY_RC_LOCAL=$?
    if [ "$SBVERIFY_RC_LOCAL" -ne 0 ]; then
        echo "[shim] FAIL -- sbverify --list \"$SHIM_BIN_LOCAL\" exited $SBVERIFY_RC_LOCAL; cannot verify CA generation. Output:" >&2
        printf '%s\n' "$SBVERIFY_OUT_LOCAL" >&2
        return 1
    fi
    local SHIM_CA_YEAR_LOCAL
    SHIM_CA_YEAR_LOCAL="$(printf '%s\n' "$SBVERIFY_OUT_LOCAL" \
        | grep -oE 'Microsoft Corporation UEFI CA 20[12][0-9]' \
        | grep -oE '20[12][0-9]' \
        | sort -u | tail -1 || true)"
    local TODAY_LOCAL="${SHIM_CA_TEST_TODAY:-$(date -u +%Y-%m-%d)}"
    if [ -z "$SHIM_CA_YEAR_LOCAL" ]; then
        echo "[shim] WARN -- $SHIM_BIN_LOCAL present and sbverify exited 0 but --list reports no recognized Microsoft Corporation UEFI CA generation; verify the binary." >&2
        return 0
    fi
    echo "[shim] signed-by: Microsoft Corporation UEFI CA $SHIM_CA_YEAR_LOCAL"
    if [ "$SHIM_CA_YEAR_LOCAL" = "2011" ]; then
        if [ "$TODAY_LOCAL" \> "$MS_UEFI_CA_2011_EXPIRY_LOCAL" ]; then
            echo "[shim] FAIL -- shim is signed by 2011 CA which expired $MS_UEFI_CA_2011_EXPIRY_LOCAL. Pin a 2023-CA-signed shim binary; abort signing pipeline." >&2
            return 1
        elif [ "$TODAY_LOCAL" \> "$MS_UEFI_CA_2011_WARN_FROM_LOCAL" ] || [ "$TODAY_LOCAL" = "$MS_UEFI_CA_2011_WARN_FROM_LOCAL" ]; then
            echo "[shim] WARN -- shim is signed only by deprecated MS UEFI CA 2011 (expires $MS_UEFI_CA_2011_EXPIRY_LOCAL); firmware DB rotation past $MS_UEFI_CA_2011_WARN_FROM_LOCAL may refuse boot. Re-sign against the 2023 CA before expiry." >&2
        fi
    fi
    return 0
}

# Run the gate first -- any expiry FAIL aborts before MOK-key skip.
shim_ca_gate || exit 1

if [ ! -f "$MOK_KEY" ]; then
    echo "[SIGN] keys/MOK.key not found -- skipping signing (dev build)"
    exit 0
fi

if [ ! -f "$EFI_BIN" ]; then
    echo "[ERROR] EFI binary not found: $EFI_BIN"
    echo "        Run: bash scripts/build.sh"
    exit 1
fi

# Helper: sign one PE binary in-place with sbsign. Atomic via temp +
# verify + mv. Used for both BOOTX64.EFI (split path) and BOOTX64.UKI.efi
# (Unified Kernel Image, whole-chain Secure Boot signature).
sign_one() {
    local target="$1"
    local label="$2"
    if [ ! -f "$target" ]; then
        return 0
    fi
    local tmp="${target}.signing.tmp"
    local cleanup_trap_was=$(trap -p EXIT)
    trap "rm -f \"$tmp\"" EXIT
    echo "[SIGN] Signing $label ($target) with MOK..."
    sbsign --key "$MOK_KEY" --cert "$MOK_CRT" --output "$tmp" "$target"
    sbverify --cert "$MOK_CRT" "$tmp" \
        && echo "[SIGN] $label signature verification OK" \
        || { echo "[SIGN] ERROR: $label signature verification FAILED"; exit 1; }
    mv "$tmp" "$target"
    trap - EXIT
    eval "$cleanup_trap_was"
}

sign_one "$EFI_BIN" "BOOTX64.EFI"

# Unified Kernel Image alongside the split artifact. UAPI Group spec;
# whole-chain signed PE that bundles BOOTX64.EFI stub + .linux kernel +
# .cmdline + .osrel into one signable unit. Built by scripts/build.sh
# UKI pack step right before this signing stage.
UKI_BIN="${UKI_BIN:-$REPO_ROOT/build/tools/BOOTX64.UKI.efi}"
sign_one "$UKI_BIN" "BOOTX64.UKI.efi"
