#!/usr/bin/env bash
# sign-efi.sh -- Sign BOOTX64.EFI + BOOTX64.UKI.efi with the MOK private key.
#
# Signs to a temporary file, verifies the signature, then atomically publishes
# the signed output to a DISTINCT path (build/tools/BOOTX64.signed.efi /
# BOOTX64.UKI.signed.efi), leaving the unsigned inputs in place so the build's
# incremental UKI objcopy stays idempotent (TODO-02 build idempotency).
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

# Two-phase atomic dual-sign: sign + verify BOTH artifacts to temp files
# first, then publish BOTH signed outputs (at distinct paths) only after
# every step succeeded. On any failure, both unsigned inputs stay untouched.
# Codex consistency H1 fix 2026-04-29: the previous one-at-a-time helper could
# leave one artifact freshly signed alongside an unsigned/stale/absent other,
# violating the dual-artifact consistency contract for releases.

UKI_BIN="${UKI_BIN:-$REPO_ROOT/build/tools/BOOTX64.UKI.efi}"

# Approach A -- non-destructive UKI signing for build idempotency (the UKI SBAT
# section must survive an incremental rebuild). Publish signatures to DISTINCT
# output paths and leave the unsigned inputs (EFI_BIN/UKI_BIN) untouched.
# scripts/build.sh runs `objcopy --add-section` against build/tools/BOOTX64.EFI
# on every build; if that input were signed in place (the old Phase 2b behavior),
# an incremental rebuild would run objcopy on an Authenticode-signed PE, corrupt
# the PE section table, and the [sbat] gate could no longer dump .sbat. Keeping
# the input unsigned makes every rebuild idempotent. The Makefile system-disk
# recipe ships these signed outputs to the ESP (falling back to the unsigned
# originals for keyless dev builds). `%.*` strips the extension regardless of case.
EFI_SIGNED="${EFI_SIGNED:-${EFI_BIN%.*}.signed.efi}"
UKI_SIGNED="${UKI_SIGNED:-${UKI_BIN%.*}.signed.efi}"

# Both artifacts MUST exist when the UKI pipeline ran -- the build
# script's `Pack BOOTX64.UKI.efi` step produces both before we sign.
# A missing UKI here is a build pipeline failure, not a soft skip,
# so we fail closed.
if [ ! -f "$EFI_BIN" ]; then
    echo "[ERROR] EFI binary not found: $EFI_BIN" >&2
    echo "        Run: bash scripts/build.sh" >&2
    exit 1
fi
if [ ! -f "$UKI_BIN" ]; then
    echo "[ERROR] UKI binary not found: $UKI_BIN" >&2
    echo "        Run: bash scripts/build.sh (Pack BOOTX64.UKI.efi step)" >&2
    exit 1
fi

EFI_TMP="${EFI_BIN}.signing.tmp"
UKI_TMP="${UKI_BIN}.signing.tmp"
trap 'rm -f "$EFI_TMP" "$UKI_TMP"' EXIT

# Phase 1: sign + verify both to temp files. Failures here leave the
# originals untouched.
echo "[SIGN] Phase 1a: signing BOOTX64.EFI to temp..."
sbsign --key "$MOK_KEY" --cert "$MOK_CRT" --output "$EFI_TMP" "$EFI_BIN"
sbverify --cert "$MOK_CRT" "$EFI_TMP" \
    && echo "[SIGN] BOOTX64.EFI signature verification OK" \
    || { echo "[SIGN] ERROR: BOOTX64.EFI signature verification FAILED" >&2; exit 1; }

echo "[SIGN] Phase 1b: signing BOOTX64.UKI.efi to temp..."
sbsign --key "$MOK_KEY" --cert "$MOK_CRT" --output "$UKI_TMP" "$UKI_BIN"
sbverify --cert "$MOK_CRT" "$UKI_TMP" \
    && echo "[SIGN] BOOTX64.UKI.efi signature verification OK" \
    || { echo "[SIGN] ERROR: BOOTX64.UKI.efi signature verification FAILED" >&2; exit 1; }

# Phase 2: publish both signed outputs at their DISTINCT paths. The unsigned
# inputs EFI_BIN/UKI_BIN are NEVER modified (approach A), so the next incremental
# build's objcopy still runs on an unsigned PE. Preserve the dual-artifact
# contract (Codex H1 2026-04-29): publish both or neither -- if the second mv
# fails, remove the first so the system-disk recipe falls back to the unsigned
# originals rather than shipping a half-signed pair. No backup of the inputs is
# needed because they are never overwritten; the Phase-1 EXIT trap still cleans
# any leftover temp on failure.
echo "[SIGN] Phase 2: publishing signed outputs (unsigned inputs untouched)..."
# Remove any prior-generation signed outputs first, then arm a trap that wipes
# BOTH on any failure or INT/TERM during the publish window (Codex re-adversarial
# M1 2026-06-21: a kill between the two mvs, or a second-mv failure leaving a
# stale prior UKI_SIGNED, could otherwise expose a mixed-generation pair).
rm -f "$EFI_SIGNED" "$UKI_SIGNED"
trap 'rm -f "$EFI_TMP" "$UKI_TMP" "$EFI_SIGNED" "$UKI_SIGNED"' EXIT INT TERM
if ! mv -f "$EFI_TMP" "$EFI_SIGNED"; then
    echo "[SIGN] ERROR: mv to $EFI_SIGNED failed -- inputs untouched" >&2
    exit 1
fi
if ! mv -f "$UKI_TMP" "$UKI_SIGNED"; then
    echo "[SIGN] ERROR: mv to $UKI_SIGNED failed -- both signed outputs wiped by trap" >&2
    exit 1
fi
# Both landed: drop the signed outputs from the cleanup trap so they survive.
trap 'rm -f "$EFI_TMP" "$UKI_TMP"' EXIT INT TERM

# Stamp-identity binding for key rotation (Codex round-5 H1 fix
# 2026-04-29). The Makefile previously trusted MOK_CRT mtime to
# decide whether the stamp was current; a CI run that swaps to a
# different cert with an OLDER mtime than the existing stamp
# silently skipped re-signing and shipped the artifact with the old
# key. Record the cert's SHA-256 content fingerprint AFTER a
# successful dual-sign so the Makefile can compare desired-vs-
# recorded at parse time and invalidate the stamp on identity
# change regardless of mtime.
#
# The fingerprint sidecar path is opt-in: callers that don't
# provide SIGN_FINGERPRINT_FILE keep legacy behavior. The Makefile
# wires this through.
if [ -n "${SIGN_FINGERPRINT_FILE:-}" ]; then
    if command -v sha256sum >/dev/null 2>&1; then
        FP="$(sha256sum "$MOK_CRT" | awk '{print $1}')"
    elif command -v shasum >/dev/null 2>&1; then
        FP="$(shasum -a 256 "$MOK_CRT" | awk '{print $1}')"
    else
        echo "[SIGN] WARN: no sha256sum/shasum on PATH; fingerprint sidecar NOT written" >&2
        FP=""
    fi
    if [ -n "$FP" ]; then
        SIGN_FP_TMP="${SIGN_FINGERPRINT_FILE}.tmp"
        printf '%s\n' "$FP" > "$SIGN_FP_TMP"
        mv -f "$SIGN_FP_TMP" "$SIGN_FINGERPRINT_FILE"
        echo "[SIGN] cert fingerprint recorded: ${FP:0:16}... -> $SIGN_FINGERPRINT_FILE"
    fi
fi
echo "[SIGN] signed outputs published: $EFI_SIGNED + $UKI_SIGNED (unsigned inputs left in place)"
