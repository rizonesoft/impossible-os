#!/usr/bin/env bash
# Approved-hash allowlist check for pinned shim binaries (todo/answers.md Q27,
# option B: shim/SHA256SUMS is the trust anchor until a Microsoft UEFI CA
# certificate is pinned).
#
# Usage: shim-hash-check.sh <sums-file> <binary>...
#
# Every binary must have an entry in <sums-file> under its exact basename
# (`<digest>  <name>` or binary-mode `<digest> *<name>`), and its SHA-256 must
# equal that entry. This is stricter than `sha256sum -c`, which checks only the
# files the list names: an unlisted binary passes `-c` and fails here.
#
# Exit: 0 all approved, 1 any refusal (reason on stderr), 2 usage.
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <sums-file> <binary>..." >&2
    exit 2
fi
SUMS="$1"; shift

command -v sha256sum >/dev/null 2>&1 \
    || { echo "[shim-hash] FAIL -- sha256sum not on PATH; cannot check $SUMS" >&2; exit 1; }
[ -f "$SUMS" ] \
    || { echo "[shim-hash] FAIL -- $SUMS absent: a pinned shim must carry an approved hash list" >&2; exit 1; }

for bin in "$@"; do
    name="$(basename "$bin")"
    # A record is `<64 hex> <space|*><filename>`; the WHOLE remainder is the
    # filename, so `shimx64.efi backup` never approves `shimx64.efi`.
    want="$(awk -v f="$name" 'length($0) > 66 && substr($0, 1, 64) ~ /^[0-9a-fA-F]+$/ && substr($0, 65, 1) == " " && (substr($0, 66, 1) == " " || substr($0, 66, 1) == "*") && substr($0, 67) == f { print tolower(substr($0, 1, 64)); exit }' "$SUMS")"
    if [ -z "$want" ]; then
        echo "[shim-hash] FAIL -- $name has no entry in $SUMS" >&2
        exit 1
    fi
    if ! have="$(sha256sum "$bin" 2>/dev/null)"; then
        echo "[shim-hash] FAIL -- $bin is unreadable; cannot hash it" >&2
        exit 1
    fi
    have="${have%% *}"
    if [ "$want" != "$have" ]; then
        echo "[shim-hash] FAIL -- $name does not match $SUMS (approved $want, got $have)" >&2
        exit 1
    fi
    echo "[shim-hash] $name matches its approved entry"
done
