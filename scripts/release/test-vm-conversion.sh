#!/usr/bin/env bash
# test-vm-conversion.sh -- host-side regression harness for the VHDX/VDI
# conversion + manifest pipeline.
#
# Owner: VHD/VHDX/VDI conversion + validation feature in
# todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md.
#
# Exercises (no boot test -- that's owned by boot-test-vhdx.sh / boot-test-vbox.sh):
#   1. to-vhdx.sh produces a VHDX with byte-identical raw content.
#   2. qemu-img info reports format=vhdx + cluster-size matching --block-size.
#   3. to-vdi.sh produces a VDI with byte-identical raw content.
#   4. build-manifest.sh build --format vhdx populates vm_image_metadata.
#   5. build-manifest.sh build --format vdi populates vm_image_metadata.
#   6. build-manifest.sh check rejects vm_image_metadata.format outside enum.
#   7. build-manifest.sh check rejects vm_image_metadata.format/artifact_format mismatch.
#   8. build-manifest.sh build --format raw OMITS vm_image_metadata.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

PASS=0
FAIL=0
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

note() { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  [PASS] %s\n' "$*"; }
bad()  { FAIL=$((FAIL+1)); printf '  [FAIL] %s\n' "$*"; }

# Refresh ABI JSON mtime so build-manifest.sh build can run.
python3 -c "import os; os.utime('build/boot-info-abi.kernel.json')"

# Build the source raw image once if it doesn't already exist; reuse if present
# (saves ~3s). The harness does NOT depend on byte-identity with a prior run --
# scripts/release/test-build-image.sh owns that gate.
if [ ! -f "$TMPDIR/disk.img" ]; then
    bash scripts/release/build-image.sh --out "$TMPDIR/disk.img" >/dev/null 2>&1
fi

note "[1] to-vhdx.sh produces VHDX with byte-identical raw content"
if bash scripts/release/to-vhdx.sh --in "$TMPDIR/disk.img" --out "$TMPDIR/disk.vhdx" >/dev/null 2>&1; then
    if qemu-img compare -f raw -F vhdx "$TMPDIR/disk.img" "$TMPDIR/disk.vhdx" >/dev/null 2>&1; then
        ok "VHDX byte-content matches source raw"
    else
        bad "qemu-img compare reported drift between raw and VHDX"
    fi
else
    bad "to-vhdx.sh failed"
fi

note "[2] qemu-img info reports format=vhdx + cluster-size 4 MiB"
fmt="$(qemu-img info --output=json "$TMPDIR/disk.vhdx" | python3 -c 'import json,sys; print(json.load(sys.stdin)["format"])')"
cs="$(qemu-img info --output=json "$TMPDIR/disk.vhdx" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("cluster-size", 0))')"
if [ "$fmt" = "vhdx" ] && [ "$cs" = "4194304" ]; then
    ok "info OK (format=$fmt cluster-size=$cs)"
else
    bad "info wrong (format=$fmt cluster-size=$cs)"
fi

note "[3] to-vdi.sh produces VDI with byte-identical raw content"
if bash scripts/release/to-vdi.sh --in "$TMPDIR/disk.img" --out "$TMPDIR/disk.vdi" >/dev/null 2>&1; then
    if qemu-img compare -f raw -F vdi "$TMPDIR/disk.img" "$TMPDIR/disk.vdi" >/dev/null 2>&1; then
        ok "VDI byte-content matches source raw"
    else
        bad "qemu-img compare reported drift between raw and VDI"
    fi
else
    bad "to-vdi.sh failed"
fi

# Use --vm-image with the TMPDIR-staged artifacts so the harness never
# touches the canonical build/release/ paths (parallel runs and concurrent
# release builds must not see staged artifacts vanish under them).

note "[4] build-manifest.sh build --format vhdx populates vm_image_metadata"
bash scripts/release/build-manifest.sh build --out "$TMPDIR/m_vhdx.json" \
    --format vhdx --vm-image "$TMPDIR/disk.vhdx" >/dev/null 2>&1
expected_vhdx='{"format": "vhdx", "subformat": "dynamic", "block_size_bytes": 4194304, "virtual_size_bytes": 536870912}'
got_vhdx="$(python3 -c 'import json,sys; print(json.dumps(json.load(open(sys.argv[1])).get("vm_image_metadata"), sort_keys=False))' "$TMPDIR/m_vhdx.json")"
if [ "$got_vhdx" = "$expected_vhdx" ]; then
    ok "vhdx vm_image_metadata correct"
else
    bad "vhdx vm_image_metadata wrong: $got_vhdx"
fi

note "[5] build-manifest.sh build --format vdi populates vm_image_metadata"
bash scripts/release/build-manifest.sh build --out "$TMPDIR/m_vdi.json" \
    --format vdi --vm-image "$TMPDIR/disk.vdi" >/dev/null 2>&1
expected_vdi='{"format": "vdi", "subformat": "dynamic", "block_size_bytes": 1048576, "virtual_size_bytes": 536870912}'
got_vdi="$(python3 -c 'import json,sys; print(json.dumps(json.load(open(sys.argv[1])).get("vm_image_metadata"), sort_keys=False))' "$TMPDIR/m_vdi.json")"
if [ "$got_vdi" = "$expected_vdi" ]; then
    ok "vdi vm_image_metadata correct"
else
    bad "vdi vm_image_metadata wrong: $got_vdi"
fi

note "[6] build-manifest.sh check rejects vm_image_metadata.format outside enum"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["vm_image_metadata"]["format"] = "bogus"
with open(p + ".bad_fmt", "w") as f: json.dump(m, f)
' "$TMPDIR/m_vhdx.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m_vhdx.json.bad_fmt" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'vm_image_metadata.format'; then
    ok "rejects bogus vm_image_metadata.format"
else
    bad "should have rejected; got rc=$rc"
fi

note "[7] build-manifest.sh check rejects vm_image_metadata.format vs artifact_format mismatch"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["vm_image_metadata"]["format"] = "vdi"
with open(p + ".mismatch", "w") as f: json.dump(m, f)
' "$TMPDIR/m_vhdx.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m_vhdx.json.mismatch" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'disagrees with artifact_format'; then
    ok "rejects vm_image_metadata/artifact_format mismatch"
else
    bad "should have rejected; got rc=$rc"
fi

note "[8] build-manifest.sh build --format raw omits vm_image_metadata"
bash scripts/release/build-manifest.sh build --out "$TMPDIR/m_raw.json" --format raw >/dev/null 2>&1
has_field="$(python3 -c 'import json,sys; print("vm_image_metadata" in json.load(open(sys.argv[1])))' "$TMPDIR/m_raw.json")"
if [ "$has_field" = "False" ]; then
    ok "raw manifest correctly omits vm_image_metadata"
else
    bad "raw manifest should not contain vm_image_metadata"
fi

note "[9] check rejects zero block_size_bytes / virtual_size_bytes"
# Hand-craft a manifest that claims to be a VHDX but reports zeros for the
# size fields. Pre-fix check accepted any non-negative uint64; post-fix
# requires positive values to fail closed on broken qemu-img extraction.
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["vm_image_metadata"]["block_size_bytes"] = 0
with open(p + ".zero_bs", "w") as f: json.dump(m, f)
m["vm_image_metadata"]["block_size_bytes"] = 4194304
m["vm_image_metadata"]["virtual_size_bytes"] = 0
with open(p + ".zero_vs", "w") as f: json.dump(m, f)
' "$TMPDIR/m_vhdx.json"
zero_bs_rejected=0
zero_vs_rejected=0
set +e
err_bs="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m_vhdx.json.zero_bs" 2>&1 >/dev/null)"
[ "$?" -ne 0 ] && printf '%s' "$err_bs" | grep -q 'block_size_bytes' && zero_bs_rejected=1
err_vs="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m_vhdx.json.zero_vs" 2>&1 >/dev/null)"
[ "$?" -ne 0 ] && printf '%s' "$err_vs" | grep -q 'virtual_size_bytes' && zero_vs_rejected=1
set -e
if [ "$zero_bs_rejected" = 1 ] && [ "$zero_vs_rejected" = 1 ]; then
    ok "rejects zero block_size_bytes and zero virtual_size_bytes"
else
    bad "should have rejected; got block_size=$zero_bs_rejected virtual_size=$zero_vs_rejected"
fi

printf '\n[summary] %d pass, %d fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
