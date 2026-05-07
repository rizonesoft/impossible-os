#!/usr/bin/env bash
# test_manifest_parity.sh -- byte-identity parity between bash and PowerShell
# peers of build-manifest.
#
# Runs `scripts/release/build-manifest.sh build --out <a>` and
# `scripts/release/build-manifest.ps1 build --Out <b>` against the same
# source tree, then asserts sha256(a) == sha256(b).
#
# Why this test exists:
#   The two peers MUST produce byte-identical manifest.json output (same
#   field order, same JSON whitespace, same UUID v5 namespace, same
#   trailing newline) so that a Windows host and a Linux host releasing
#   the same git revision both produce a manifest that reconciles against
#   the artifact's own bootloader/kernel sha256s. Drift here would split
#   the release artifact's identity by host, which defeats the
#   reproducible-image-build feature contract from TODO-06.
#
# Skip behavior:
#   If `pwsh` is not installed on PATH, this test SKIPs (exit 0) with a
#   note. The bash peer is the authoritative source on Linux dev hosts;
#   the PowerShell peer ships for Windows hosts. PR CI runs this on
#   matrix images that include pwsh; local Linux dev runs may legitimately
#   skip.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$REPO_ROOT"

if ! command -v pwsh >/dev/null 2>&1; then
    printf '[SKIP] test_manifest_parity: pwsh not installed (Windows-host parity is verified on PR CI)\n'
    exit 0
fi

if [[ ! -f build/tools/BOOTX64.EFI ]] || [[ ! -f build/kernel.exe ]] || [[ ! -f build/boot-info-abi.kernel.json ]]; then
    printf '[SKIP] test_manifest_parity: build artifacts absent; run scripts/build.sh first\n'
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

BASH_OUT="$TMPDIR/bash.json"
PS_OUT="$TMPDIR/ps.json"

# Bump the ABI manifest's mtime so the build-manifest stale-binding
# guard does not fire. Same rationale as the touch in
# scripts/release/test-build-manifest.ps1: this is a test harness, not
# a release-time pipeline, so we want to exercise build-manifest's
# logic rather than re-test the release-time stale-detection.
touch build/boot-info-abi.kernel.json

bash scripts/release/build-manifest.sh build --out "$BASH_OUT" >/dev/null
pwsh -NoProfile -File scripts/release/build-manifest.ps1 build --Out "$PS_OUT" >/dev/null

bash_hash="$(sha256sum "$BASH_OUT" | awk '{print $1}')"
ps_hash="$(sha256sum "$PS_OUT" | awk '{print $1}')"

if [[ "$bash_hash" == "$ps_hash" ]]; then
    printf '[PASS] manifest parity: bash and pwsh produce byte-identical output (sha256=%s)\n' "$bash_hash"
    exit 0
fi

printf '[FAIL] manifest parity: bash and pwsh outputs differ\n' >&2
printf '       bash sha256: %s (%d bytes)\n' "$bash_hash" "$(wc -c <"$BASH_OUT")" >&2
printf '       pwsh sha256: %s (%d bytes)\n' "$ps_hash"   "$(wc -c <"$PS_OUT")"   >&2
printf '       diff:\n' >&2
diff -u "$BASH_OUT" "$PS_OUT" >&2 || true
exit 1
