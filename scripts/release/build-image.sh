#!/usr/bin/env bash
# build-image.sh -- produce a deterministic raw GPT disk image for release.
#
# Output: build/release/disk.img -- 512 MiB GPT image with three partitions
#   ESP (FAT32, 64 MiB)  : BOOTX64.EFI + kernel.exe + boot.conf
#   BlackBox (FAT32, 128 MiB) : Logs/Boot/Crash/Perf/Diag/Tools skeleton
#   IXFS-System (4 GiB-ish, fills rest) : zero-filled placeholder
#
# Determinism contract for the boot-media reproducibility feature:
#   -- timestamps zeroed (SOURCE_DATE_EPOCH=0; FAT volume creation, file
#      mtimes, GPT modify time all derive from this single epoch)
#   -- partition + disk GUIDs derived from a manifest-pinned seed
#      (UUID v5 against the boot-artifact-manifest namespace UUID with
#      seed = "<source_sha>|<format>") so a clean checkout reproduces the
#      same GUIDs without an out-of-band registry of random values
#   -- partition ordering fixed (ESP first, BlackBox second, IXFS third)
#   -- FAT volume label fixed (IPOS-ESP / BLACKBOX)
#   -- FAT volume serial derived from the partition GUID's first 8 hex chars
#      (so the FAT serial moves in lockstep with the GPT partition GUID)
#
# Two consecutive runs from a clean tree produce a byte-identical disk.img
# (verified by scripts/release/test-build-image.sh).
#
# Scope boundary: this script owns reproducibility properties only. The full
# release pipeline (compression, signing, multi-format wrap) lives in
# todo/15-installer-release/TODO-01-release-artifacts.md (raw image release +
# USB writer wrapper).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

# ---- Determinism epoch ------------------------------------------------------
# Anchor every timestamp-bearing tool to a fixed epoch. dosfstools >= 4.2
# honors SOURCE_DATE_EPOCH for FAT volume creation timestamps; sgdisk's
# disk modify time is overwritten via --set-alignment / --transform-bsd
# being absent (mod time only updates on writes, which we control).
export SOURCE_DATE_EPOCH=0
export TZ=UTC
export LC_ALL=C

# ---- Inputs -----------------------------------------------------------------
BL_PATH="build/tools/BOOTX64.EFI"
KR_PATH="build/kernel.exe"
BOOT_CONF="resources/boot/boot.conf"
OUT_DIR="build/release"
OUT_IMG="$OUT_DIR/disk.img"
WORK_DIR=""  # per-invocation mktemp dir; populated below after $OUT_DIR exists

err() { printf '[ERROR] %s\n' "$*" >&2; }
note() { printf '[build-image] %s\n' "$*" >&2; }

usage() {
    cat <<EOF
Usage: $0 [--out PATH] [--format FMT] [--keep-work]

Options:
  --out PATH       Output image path (default: $OUT_IMG)
  --format FMT     Artifact format identifier seeded into UUID derivation
                   (default: raw). Must be a schema-legal format from
                   docs/release/boot-artifact-manifest.md.
  --keep-work      Keep $WORK_DIR after success (debugging)
  -h, --help       Show this message
EOF
}

FORMAT="raw"
KEEP_WORK=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --out)        OUT_IMG="$2"; shift 2 ;;
        --format)     FORMAT="$2"; shift 2 ;;
        --keep-work)  KEEP_WORK=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *) err "unknown arg: $1"; usage >&2; exit 2 ;;
    esac
done

# Validate --format against the same enum that build-manifest.sh and the
# schema doc enforce. Without this, a typo (e.g. --format raaw) seeds the
# UUID derivation with a value that no manifest can describe with the same
# seed, producing a deterministic-but-orphan disk image.
case "$FORMAT" in
    raw|usb|vhd|vhdx|vdi|iso|qcow2|ova|recovery|installer) ;;
    *) err "invalid --format: $FORMAT (must be one of: raw, usb, vhd, vhdx, vdi, iso, qcow2, ova, recovery, installer)"; exit 2 ;;
esac

for f in "$BL_PATH" "$KR_PATH" "$BOOT_CONF"; do
    [ -f "$f" ] || { err "missing input: $f (run scripts/build.sh first)"; exit 1; }
done

for tool in sgdisk mkfs.fat mcopy mmd mtype mdir python3 sha256sum truncate; do
    command -v "$tool" >/dev/null || { err "missing tool: $tool"; exit 1; }
done

# ---- Seed derivation --------------------------------------------------------
# source_sha is the git HEAD; in a shallow clone we still see the current
# commit because release builds always run on a checked-out tree.
SOURCE_SHA="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
if [ "$SOURCE_SHA" = "unknown" ]; then
    err "git rev-parse HEAD failed -- this script requires a checked-out tree"
    exit 1
fi
SEED="${SOURCE_SHA}|${FORMAT}"
note "source_sha=$SOURCE_SHA format=$FORMAT"

# UUID v5 namespace shared with build-manifest.sh (boot-artifact-manifest).
NAMESPACE="6f1b3c4a-1d4e-5a6b-8c9d-0e1f2a3b4c5d"

derive_uuid() {
    # $1 = label seed; produces a deterministic UUID v5.
    SEED="$SEED" LABEL="$1" NAMESPACE="$NAMESPACE" python3 - <<'PY'
import os, uuid
ns = uuid.UUID(os.environ["NAMESPACE"])
print(uuid.uuid5(ns, os.environ["SEED"] + "|" + os.environ["LABEL"]))
PY
}

DISK_GUID="$(derive_uuid disk)"
ESP_GUID="$(derive_uuid esp)"
BB_GUID="$(derive_uuid blackbox)"
IXFS_GUID="$(derive_uuid ixfs)"

# FAT volume serial: 8 hex chars from the partition GUID. mkfs.fat -i wants
# 8 hex digits as a single argument; we slice the first 8 from the GUID
# (stripping the dashes first).
fat_serial() { printf '%s' "$1" | tr -d '-' | head -c 8; }
ESP_VOLID="$(fat_serial "$ESP_GUID")"
BB_VOLID="$(fat_serial "$BB_GUID")"

note "DISK_GUID=$DISK_GUID"
note "ESP_GUID=$ESP_GUID  ESP_VOLID=$ESP_VOLID"
note "BB_GUID=$BB_GUID    BB_VOLID=$BB_VOLID"
note "IXFS_GUID=$IXFS_GUID"

# ---- Layout (must match Makefile system-disk for boot-path compatibility) ---
SECTOR=512
DISK_SIZE_BYTES=$((512 * 1024 * 1024))            # 512 MiB
ESP_LBA_FIRST=2048                                # 1 MiB align
ESP_SIZE_MIB=64
ESP_SECTORS=$((ESP_SIZE_MIB * 1024 * 2))           # 64 MiB / 512 B/sector
ESP_LBA_LAST=$((ESP_LBA_FIRST + ESP_SECTORS - 1))

BB_LBA_FIRST=$((ESP_LBA_LAST + 1))
BB_SIZE_MIB=128
BB_SECTORS=$((BB_SIZE_MIB * 1024 * 2))
BB_LBA_LAST=$((BB_LBA_FIRST + BB_SECTORS - 1))

# IXFS fills from BB_LBA_LAST+1 to disk_end - 34 (GPT secondary header room).
IXFS_LBA_FIRST=$((BB_LBA_LAST + 1))
IXFS_LBA_LAST=$((DISK_SIZE_BYTES / SECTOR - 34))

# GPT partition type GUIDs (UEFI 2.10 Appendix A).
ESP_TYPE_GUID="C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
# Microsoft Basic Data partition (used here for BlackBox + IXFS until the
# Impossible-OS-specific type GUIDs are pinned in the IXFS-on-disk-format
# work owned by the kernel-fs domain).
MSBASIC_TYPE_GUID="EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"

# ---- Build ------------------------------------------------------------------
mkdir -p "$OUT_DIR"
# Per-invocation work directory so parallel build-image.sh runs (different
# --out paths under CI or developer workflows) cannot race on a shared
# stage tree. Old fixed path (build/release/build-image-work) was reentrant-
# unsafe: two invocations would clobber each other's mcopy stages and the
# first to exit would rm -rf the other's working state.
WORK_DIR="$(mktemp -d "$OUT_DIR/build-image-work.XXXXXX")"
trap '[ "$KEEP_WORK" -eq 1 ] || rm -rf "$WORK_DIR"' EXIT

# 1. Allocate the empty image. We rm -f first so a pre-existing $OUT_IMG
#    (e.g. from a prior run with the same path) cannot leak stale bytes
#    into untouched regions like the IXFS zero-fill zone -- truncate on an
#    existing file at the same size is a no-op for content.
rm -f "$OUT_IMG"
truncate -s "$DISK_SIZE_BYTES" "$OUT_IMG"

# 2. GPT header + partition table with deterministic GUIDs.
sgdisk \
    --zap-all \
    --disk-guid="$DISK_GUID" \
    --new=1:$ESP_LBA_FIRST:$ESP_LBA_LAST \
    --typecode=1:"$ESP_TYPE_GUID" \
    --partition-guid=1:"$ESP_GUID" \
    --change-name=1:"ESP" \
    --new=2:$BB_LBA_FIRST:$BB_LBA_LAST \
    --typecode=2:"$MSBASIC_TYPE_GUID" \
    --partition-guid=2:"$BB_GUID" \
    --change-name=2:"BlackBox" \
    --new=3:$IXFS_LBA_FIRST:$IXFS_LBA_LAST \
    --typecode=3:"$MSBASIC_TYPE_GUID" \
    --partition-guid=3:"$IXFS_GUID" \
    --change-name=3:"IXFS-System" \
    "$OUT_IMG" >/dev/null

# 3. Format ESP as FAT32 with the derived volume serial.
mkfs.fat --invariant -F 32 -i "$ESP_VOLID" -n "IPOS-ESP" \
    --offset "$ESP_LBA_FIRST" \
    "$OUT_IMG" "$((ESP_SECTORS / 2))" >/dev/null   # size in 1 KiB blocks

# 4. Stage ESP files with deterministic mtimes.
ESP_STAGE="$WORK_DIR/esp_stage"
mkdir -p "$ESP_STAGE/EFI/BOOT" "$ESP_STAGE/EFI/ImpossibleOS" "$ESP_STAGE/boot"
cp "$BL_PATH" "$ESP_STAGE/EFI/BOOT/BOOTX64.EFI"
cp "$KR_PATH" "$ESP_STAGE/boot/kernel.exe"
cp "$BOOT_CONF" "$ESP_STAGE/EFI/ImpossibleOS/boot.conf"
find "$ESP_STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +

# 5. Populate ESP. mtools' mcopy preserves source mtimes (since we just
#    pinned them). MTOOLS_SKIP_CHECK=1 silences the cluster-size warning
#    that mtools prints when the FAT geometry differs from MS-DOS defaults
#    (cosmetic only; does not affect the image bytes).
export MTOOLS_SKIP_CHECK=1
mcopy -i "$OUT_IMG@@$((ESP_LBA_FIRST * SECTOR))" -s -m "$ESP_STAGE/EFI" "::"
mcopy -i "$OUT_IMG@@$((ESP_LBA_FIRST * SECTOR))" -s -m "$ESP_STAGE/boot" "::"

# 6. Format BlackBox as FAT32.
mkfs.fat --invariant -F 32 -i "$BB_VOLID" -n "BLACKBOX" \
    --offset "$BB_LBA_FIRST" \
    "$OUT_IMG" "$((BB_SECTORS / 2))" >/dev/null

# 7. BlackBox skeleton. mtools mmd creates directories with the current
#    SOURCE_DATE_EPOCH-driven mtime; the marker file is stage-touched
#    before mcopy.
mmd -i "$OUT_IMG@@$((BB_LBA_FIRST * SECTOR))" \
    "::Logs" "::Boot" "::Crash" "::Perf" "::Diag" "::Tools" \
    "::Crash/WER" "::Logs/Serial"

BB_STAGE="$WORK_DIR/blackbox_stage"
mkdir -p "$BB_STAGE"
printf 'BlackBox-v1' > "$BB_STAGE/blackbox-marker.txt"
touch -h -d "@$SOURCE_DATE_EPOCH" "$BB_STAGE/blackbox-marker.txt"
mcopy -i "$OUT_IMG@@$((BB_LBA_FIRST * SECTOR))" -m \
    "$BB_STAGE/blackbox-marker.txt" "::Diag/blackbox-marker.txt"

# 8. IXFS partition: zero-filled placeholder. The kernel does not yet
#    mount IXFS on bare metal (see CLAUDE.md "Bare Metal No C: Mount" note);
#    a proper deterministic mkfs-ixfs is owned by the IXFS subsystem TODO.
#    truncate already zero-filled the image, so we do nothing here, but
#    the sgdisk partition record is in place so the bootloader sees the
#    layout it expects.

# 9. Final sha256 + size report.
SHA="$(sha256sum "$OUT_IMG" | awk '{print $1}')"
SZ="$(stat -c '%s' "$OUT_IMG")"
note "OUTPUT $OUT_IMG  size=$SZ  sha256=$SHA"

# 10. Re-emit the image fingerprint key=value pairs that build-manifest.sh
#     wraps into the schema; running build-image.sh standalone produces a
#     consumable summary on stdout for downstream tooling.
cat <<EOF
disk_image=$OUT_IMG
disk_size_bytes=$SZ
disk_sha256=$SHA
disk_guid=$DISK_GUID
esp_guid=$ESP_GUID
blackbox_guid=$BB_GUID
ixfs_guid=$IXFS_GUID
esp_volid=$ESP_VOLID
blackbox_volid=$BB_VOLID
source_sha=$SOURCE_SHA
artifact_format=$FORMAT
seed=$SEED
EOF
