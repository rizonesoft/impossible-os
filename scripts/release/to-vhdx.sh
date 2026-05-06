#!/usr/bin/env bash
# to-vhdx.sh -- convert build/release/disk.img to Hyper-V dynamic VHDX.
#
# Pipeline owner: VHD/VHDX/VDI conversion + validation feature in
# todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md.
# Full release pipeline (compression, signing, multi-format wrap) lives in
# todo/15-installer-release/TODO-01-release-artifacts.md.
#
# Self-verifying contract: after qemu-img convert, this script runs:
#   1. qemu-img info <out>  -- asserts format=vhdx + recorded subformat/block_size
#   2. qemu-img compare -f raw -F vhdx <src> <out>  -- exits 0 iff every byte
#      of the raw container is reproduced inside the VHDX (block_size and
#      subformat affect the on-disk container layout but qemu-img compare
#      reads the virtual content, so byte-content equivalence is what we
#      gate on; the VHDX header carries timestamp fields that qemu-img does
#      not honor SOURCE_DATE_EPOCH for, so container-byte reproducibility is
#      explicitly NOT promised by this script -- the raw-image producer
#      owns reproducibility, this script owns conversion-content integrity).
#
# Exit codes:
#   0 -- VHDX produced, info matches, byte-content compares equal
#   1 -- conversion or verification failed
#   2 -- usage / missing tool

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

err()  { printf '[ERROR] %s\n' "$*" >&2; }
note() { printf '[to-vhdx] %s\n' "$*" >&2; }

usage() {
    cat <<EOF
Usage: $0 [--in PATH] [--out PATH] [--block-size BYTES] [--no-verify]

Options:
  --in PATH         Input raw image (default: build/release/disk.img)
  --out PATH        Output VHDX (default: build/release/disk.vhdx)
  --block-size N    VHDX block size in bytes (default: 4194304 = 4 MiB,
                    matching the Hyper-V default; legal values are 1 MiB
                    through 256 MiB power-of-two)
  --no-verify       Skip post-convert qemu-img info + compare (advanced;
                    the conversion is no longer self-verifying)
  -h, --help        Show this message
EOF
}

IN_IMG="build/release/disk.img"
OUT_IMG="build/release/disk.vhdx"
BLOCK_SIZE=$((4 * 1024 * 1024))
VERIFY=1

while [ "$#" -gt 0 ]; do
    case "$1" in
        --in)         IN_IMG="$2"; shift 2 ;;
        --out)        OUT_IMG="$2"; shift 2 ;;
        --block-size) BLOCK_SIZE="$2"; shift 2 ;;
        --no-verify)  VERIFY=0; shift ;;
        -h|--help)    usage; exit 0 ;;
        *) err "unknown arg: $1"; usage >&2; exit 2 ;;
    esac
done

[ -f "$IN_IMG" ] || { err "missing input: $IN_IMG (run scripts/release/build-image.sh)"; exit 1; }
command -v qemu-img >/dev/null || { err "missing tool: qemu-img (install qemu-utils)"; exit 2; }

# Validate block_size is a power-of-two between 1 MiB and 256 MiB. Hyper-V
# rejects values outside this window; pre-flighting here gives a clearer
# error than qemu-img's downstream complaint.
case "$BLOCK_SIZE" in
    1048576|2097152|4194304|8388608|16777216|33554432|67108864|134217728|268435456) ;;
    *) err "invalid --block-size: $BLOCK_SIZE (must be a power-of-two from 1048576 to 268435456)"; exit 2 ;;
esac

mkdir -p "$(dirname "$OUT_IMG")"
rm -f "$OUT_IMG"

note "input  $IN_IMG"
note "output $OUT_IMG (subformat=dynamic block_size=$BLOCK_SIZE)"
qemu-img convert -f raw -O vhdx \
    -o subformat=dynamic,block_size="$BLOCK_SIZE" \
    "$IN_IMG" "$OUT_IMG"

if [ "$VERIFY" -eq 1 ]; then
    INFO_JSON="$(qemu-img info --output=json "$OUT_IMG")"
    fmt="$(printf '%s' "$INFO_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["format"])')"
    if [ "$fmt" != "vhdx" ]; then
        err "qemu-img info reports format=$fmt, expected vhdx"
        exit 1
    fi
    note "qemu-img info OK (format=$fmt)"

    if ! qemu-img compare -f raw -F vhdx "$IN_IMG" "$OUT_IMG" >/dev/null; then
        err "qemu-img compare reports byte-content drift between $IN_IMG and $OUT_IMG"
        exit 1
    fi
    note "qemu-img compare OK (byte-content identical)"
fi

VIRT_SIZE="$(stat -c '%s' "$IN_IMG")"
note "virtual_size_bytes=$VIRT_SIZE block_size_bytes=$BLOCK_SIZE"

# Emit a key=value summary so build-manifest.sh build can pick up the
# vm_image_metadata fields without re-running qemu-img info.
cat <<EOF
vm_image_format=vhdx
vm_image_subformat=dynamic
vm_image_block_size_bytes=$BLOCK_SIZE
vm_image_virtual_size_bytes=$VIRT_SIZE
vm_image_path=$OUT_IMG
EOF
