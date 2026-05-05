#!/usr/bin/env bash
# verify-esp.sh -- assert ESP content for a release disk image.
#
# Reads the ESP partition (FAT32 at LBA 2048) read-only via mtools and
# checks:
#   1. Required files present: EFI/BOOT/BOOTX64.EFI, boot/kernel.exe,
#      EFI/ImpossibleOS/boot.conf
#   2. Each file's sha256 matches the expected value
#   3. Volume label is IPOS-ESP
#
# Usage:  bash scripts/release/verify-esp.sh <disk.img>
#         bash scripts/release/verify-esp.sh <disk.img> --manifest <manifest.json>
#
# Without --manifest, hashes are computed against the build-tree source
# files (build/tools/BOOTX64.EFI + build/kernel.exe + resources/boot/boot.conf).
# With --manifest, hashes are pulled from the entries[] array of the
# boot-artifact manifest schema (see docs/release/boot-artifact-manifest.md).
#
# Exit codes:
#   0 -- all checks pass
#   1 -- one or more checks failed (named on stderr with [ERROR])
#   2 -- usage / missing tool

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

err() { printf '[ERROR] %s\n' "$*" >&2; }
info() { printf '[verify-esp] %s\n' "$*" >&2; }

usage() {
    cat <<EOF
Usage: $0 <disk.img> [--manifest <manifest.json>]
EOF
}

[ "$#" -ge 1 ] || { usage >&2; exit 2; }
DISK="$1"; shift
MANIFEST=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --manifest) MANIFEST="$2"; shift 2 ;;
        -h|--help)  usage; exit 0 ;;
        *) err "unknown arg: $1"; exit 2 ;;
    esac
done

[ -f "$DISK" ] || { err "missing disk image: $DISK"; exit 2; }
for tool in mtype mdir mlabel sha256sum python3; do
    command -v "$tool" >/dev/null || { err "missing tool: $tool"; exit 2; }
done

ESP_OFFSET=$((2048 * 512))   # ESP starts at LBA 2048 per build-image.sh layout.
export MTOOLS_SKIP_CHECK=1

errors=0
fail() { err "$*"; errors=$((errors + 1)); }

# 1. Volume label.
LABEL="$(mlabel -i "$DISK@@$ESP_OFFSET" -s :: 2>/dev/null | sed -n 's/^ Volume label is //p' | head -1 | sed -e 's/[[:space:]]*$//')"
if [ "$LABEL" != "IPOS-ESP" ]; then
    fail "ESP volume label: got '$LABEL', expected 'IPOS-ESP'"
else
    info "ESP volume label OK ($LABEL)"
fi

# 2. Required file presence + hash.
declare -A REQUIRED
REQUIRED["EFI/BOOT/BOOTX64.EFI"]="build/tools/BOOTX64.EFI"
REQUIRED["boot/kernel.exe"]="build/kernel.exe"
REQUIRED["EFI/ImpossibleOS/boot.conf"]="resources/boot/boot.conf"

# Optional manifest hash override (key = path-on-ESP, value = sha256).
declare -A MANIFEST_SHA
if [ -n "$MANIFEST" ]; then
    [ -f "$MANIFEST" ] || { err "missing manifest: $MANIFEST"; exit 2; }
    while IFS=$'\t' read -r path sha; do
        # path comes through with backslash separators in the manifest;
        # normalize to forward-slash for matching against ESP paths.
        path="${path#\\}"
        path="${path//\\/\/}"
        MANIFEST_SHA["$path"]="$sha"
    done < <(python3 -c '
import json, sys
m = json.load(open(sys.argv[1]))
for e in m.get("entries", []):
    print(e["path"] + "\t" + e["sha256"])
' "$MANIFEST")
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

for esp_path in "${!REQUIRED[@]}"; do
    src_path="${REQUIRED[$esp_path]}"
    out="$TMPDIR/$(basename "$esp_path")"
    if ! mtype -i "$DISK@@$ESP_OFFSET" "::$esp_path" > "$out" 2>/dev/null; then
        fail "ESP missing required file: $esp_path"
        continue
    fi
    actual_sha="$(sha256sum "$out" | awk '{print $1}')"

    expected_sha=""
    expected_src=""
    if [ -n "${MANIFEST_SHA[$esp_path]:-}" ]; then
        expected_sha="${MANIFEST_SHA[$esp_path]}"
        expected_src="manifest"
    elif [ -f "$src_path" ]; then
        expected_sha="$(sha256sum "$src_path" | awk '{print $1}')"
        expected_src="build-tree"
    else
        info "no expected sha for $esp_path (no manifest, no build source); presence-only check"
        continue
    fi

    if [ "$actual_sha" = "$expected_sha" ]; then
        info "ESP $esp_path OK (sha256 matches $expected_src)"
    else
        fail "ESP $esp_path sha256 mismatch: got $actual_sha, expected $expected_sha (from $expected_src)"
    fi
done

if [ "$errors" -gt 0 ]; then
    err "verify-esp: $errors check(s) failed"
    exit 1
fi
info "verify-esp: all checks passed"
exit 0
