#!/usr/bin/env bash
# sign-efi.sh — Sign BOOTX64.EFI with the MOK private key.
#
# This script is a thin CLI wrapper around the 'sign-efi' Makefile target.
# The build system (scripts/build.sh) calls 'make sign-efi' directly.
# Use this script for standalone signing or CI pipelines that need an
# explicit shell entry point.
#
# Usage:
#   bash scripts/sign-efi.sh
#
# Environment overrides (optional):
#   MOK_KEY  — path to private key   (default: keys/MOK.key)
#   MOK_CRT  — path to certificate   (default: keys/MOK.cer)
#   EFI_BIN  — binary to sign        (default: build/tools/BOOTX64.EFI)
#
# CI usage:
#   MOK_KEY=/run/secrets/mok.key MOK_CRT=/run/secrets/mok.cer bash scripts/sign-efi.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MOK_KEY="${MOK_KEY:-$REPO_ROOT/keys/MOK.key}"
MOK_CRT="${MOK_CRT:-$REPO_ROOT/keys/MOK.cer}"
EFI_BIN="${EFI_BIN:-$REPO_ROOT/build/tools/BOOTX64.EFI}"

if [ ! -f "$MOK_KEY" ]; then
    echo "[SIGN] keys/MOK.key not found — skipping signing (dev build)"
    exit 0
fi

if [ ! -f "$EFI_BIN" ]; then
    echo "[ERROR] EFI binary not found: $EFI_BIN"
    echo "        Run: bash scripts/build.sh"
    exit 1
fi

echo "[SIGN] Signing $EFI_BIN with MOK..."
sbsign --key "$MOK_KEY" --cert "$MOK_CRT" --output "$EFI_BIN" "$EFI_BIN"
sbverify --cert "$MOK_CRT" "$EFI_BIN" \
    && echo "[SIGN] Signature verification OK"
