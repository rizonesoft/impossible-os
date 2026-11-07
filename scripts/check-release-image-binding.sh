#!/usr/bin/env bash
# =============================================================================
# check-release-image-binding.sh -- prove the packaged image ships the gated kernel
# =============================================================================
# Extracts every kernel copy from a system disk image (ESP /boot/kernel.exe and
# recovery kernel.bak), requires each to be byte-identical to the kernel the
# release gate proved, and requires each to carry the provenance stamp.
#
# Partition offsets come from the image, never from a Makefile constant. The
# GPT is the authority (as in test-secureboot-smoke.sh), the .info sidecar that
# make-system-disk wrote is a required cross-check, and any disagreement fails:
# a guessed offset can read a stale or decoy FAT holding the expected bytes.
#
# Usage: scripts/check-release-image-binding.sh <image> <gated-kernel>
# Exit 0 = every copy bound; non-zero = refused, with the reason on stdout.
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SFDISK_BIN="${SFDISK_BIN:-sfdisk}"
MCOPY_BIN="${MCOPY_BIN:-mcopy}"
# Overridable so the fixture test does not need a stamped ELF kernel.
PROVENANCE_CHECK="${RELEASE_PROVENANCE_CHECK:-$REPO_ROOT/scripts/check-release-symbols.sh}"

ESP_TYPE="C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
RECOVERY_TYPE="49504F53-7265-636F-7665-727900000001"

die() { echo "::error::$*"; exit 1; }

[ "$#" -eq 2 ] || die "usage: check-release-image-binding.sh <image> <gated-kernel>"
IMAGE="$1"
GATED_KERNEL="$2"
SIDECAR="$IMAGE.info"

[ -f "$IMAGE" ] || die "$IMAGE not found"
[ -f "$GATED_KERNEL" ] || die "$GATED_KERNEL not found"
[ -f "$SIDECAR" ] || die "$SIDECAR not found -- cannot cross-check the partition offsets"
command -v "$SFDISK_BIN" >/dev/null 2>&1 || die "$SFDISK_BIN not on PATH -- cannot read the image's GPT"
command -v "$MCOPY_BIN" >/dev/null 2>&1 || die "$MCOPY_BIN not on PATH -- cannot extract the kernel copies"

# <key>: the single value the sidecar records, or empty when absent or repeated.
sidecar_value() {
    local values
    values="$(grep -E "^$1=[0-9]+$" "$SIDECAR" | cut -d= -f2)" || true
    [ "$(printf '%s\n' "$values" | wc -l)" -eq 1 ] || values=""
    printf '%s' "$values"
}

# <type-guid>: byte offset of the single GPT partition of that type, or empty.
gpt_offset() {
    "$SFDISK_BIN" --json "$IMAGE" 2>/dev/null | python3 -c '
import json, sys
try:
    table = json.load(sys.stdin)["partitiontable"]
    sector = int(table.get("sectorsize", 512))
    hits = [int(p["start"]) * sector for p in table.get("partitions", [])
            if str(p.get("type", "")).upper() == sys.argv[1]]
except (ValueError, KeyError, TypeError):
    # fail-direction: empty means the caller refuses the image.
    sys.exit(0)
if len(hits) == 1:
    print(hits[0])
' "$1" || true
}

# <label> <type-guid> <sidecar-key>: return the authenticated offset in OFFSET, or
# refuse. Not a $(...) function: a refusal printed there would be captured and lost.
partition_offset() {
    local gpt side
    gpt="$(gpt_offset "$2")"
    side="$(sidecar_value "$3")"
    [ -n "$gpt" ] || die "$1: no single partition of type $2 in the GPT of $IMAGE"
    [ -n "$side" ] || die "$1: $3 missing or repeated in $SIDECAR"
    [ "$gpt" = "$side" ] || die "$1: $SIDECAR says $3=$side, the GPT says $gpt"
    OFFSET="$gpt"
}

partition_offset "ESP" "$ESP_TYPE" EFI_OFFSET
ESP_OFFSET="$OFFSET"
partition_offset "recovery" "$RECOVERY_TYPE" RECOVERY_OFFSET
RECOVERY_OFFSET="$OFFSET"

GATED_SHA="$(sha256sum "$GATED_KERNEL" | awk '{print $1}')"
echo "Gated kernel sha256=$GATED_SHA (ESP offset $ESP_OFFSET, recovery offset $RECOVERY_OFFSET)"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# <label> <offset> <path-in-fat>
check_copy() {
    local extracted="$WORK/kernel-$2" sha
    "$MCOPY_BIN" -n -i "$IMAGE@@$2" "$3" "$extracted" || die "$1: cannot extract $3 at offset $2"
    sha="$(sha256sum "$extracted" | awk '{print $1}')"
    [ "$sha" = "$GATED_SHA" ] || die "$1 kernel ($sha) != gated $GATED_KERNEL ($GATED_SHA)"
    bash "$PROVENANCE_CHECK" --verify-provenance "$extracted" || die "$1 kernel failed the provenance check"
    echo "OK: $1 kernel matches the gated, provenance-stamped kernel"
}

check_copy "ESP /boot/kernel.exe" "$ESP_OFFSET" "::/boot/kernel.exe"
check_copy "recovery kernel.bak" "$RECOVERY_OFFSET" "::kernel.bak"
