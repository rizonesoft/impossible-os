#!/usr/bin/env bash
# to-vdi.sh -- convert build/release/disk.img to VirtualBox VDI (dynamic).
#
# Pipeline owner: VHD/VHDX/VDI conversion + validation feature in
# todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md.
# Full release pipeline lives in todo/15-installer-release/TODO-01-release-artifacts.md.
#
# Self-verifying contract:
#   1. qemu-img info <out>  -- asserts format=vdi
#   2. qemu-img compare -f raw -F vdi <src> <out>  -- byte-content identical
#
# VDI does NOT expose a tunable block size at qemu-img-convert time the way
# VHDX does (qemu-img writes a fixed 1 MiB block VDI); we report the qemu-img
# default in the manifest summary rather than accepting --block-size.
#
# Exit codes match to-vhdx.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

err()  { printf '[ERROR] %s\n' "$*" >&2; }
note() { printf '[to-vdi] %s\n' "$*" >&2; }

usage() {
    cat <<EOF
Usage: $0 [--in PATH] [--out PATH] [--no-verify]

Options:
  --in PATH    Input raw image (default: build/release/disk.img)
  --out PATH   Output VDI (default: build/release/disk.vdi)
  --no-verify  Skip post-convert qemu-img info + compare
  -h, --help   Show this message
EOF
}

IN_IMG="build/release/disk.img"
OUT_IMG="build/release/disk.vdi"
VERIFY=1

while [ "$#" -gt 0 ]; do
    case "$1" in
        --in)        IN_IMG="$2"; shift 2 ;;
        --out)       OUT_IMG="$2"; shift 2 ;;
        --no-verify) VERIFY=0; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) err "unknown arg: $1"; usage >&2; exit 2 ;;
    esac
done

[ -f "$IN_IMG" ] || { err "missing input: $IN_IMG (run scripts/release/build-image.sh)"; exit 1; }
command -v qemu-img >/dev/null || { err "missing tool: qemu-img (install qemu-utils)"; exit 2; }

mkdir -p "$(dirname "$OUT_IMG")"
rm -f "$OUT_IMG"

note "input  $IN_IMG"
note "output $OUT_IMG (dynamic)"
# VDI is dynamic by default in qemu-img; static=on switches to pre-allocated.
# Pass static=off to make the intent explicit (qemu-img option set differs
# from VHDX's subformat= semantics).
qemu-img convert -f raw -O vdi -o static=off "$IN_IMG" "$OUT_IMG"

if [ "$VERIFY" -eq 1 ]; then
    INFO_JSON="$(qemu-img info --output=json "$OUT_IMG")"
    fmt="$(printf '%s' "$INFO_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["format"])')"
    if [ "$fmt" != "vdi" ]; then
        err "qemu-img info reports format=$fmt, expected vdi"
        exit 1
    fi
    note "qemu-img info OK (format=$fmt)"

    if ! qemu-img compare -f raw -F vdi "$IN_IMG" "$OUT_IMG" >/dev/null; then
        err "qemu-img compare reports byte-content drift between $IN_IMG and $OUT_IMG"
        exit 1
    fi
    note "qemu-img compare OK (byte-content identical)"
fi

VIRT_SIZE="$(stat -c '%s' "$IN_IMG")"
note "virtual_size_bytes=$VIRT_SIZE"

cat <<EOF
vm_image_format=vdi
vm_image_subformat=dynamic
vm_image_block_size_bytes=1048576
vm_image_virtual_size_bytes=$VIRT_SIZE
vm_image_path=$OUT_IMG
EOF
