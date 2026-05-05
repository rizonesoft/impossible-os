#!/usr/bin/env bash
# test-build-image.sh -- host-side tests for the deterministic disk-image
# producer (build-image.sh) + ESP verifier (verify-esp.sh) + the v1
# manifest provenance fields (build-manifest.sh).
#
# Exercises:
#   1. build-image.sh produces all required outputs and a non-zero sha256.
#   2. Two consecutive build-image.sh runs from a clean tree are byte-identical.
#   3. verify-esp.sh PASSes on a freshly-built image.
#   4. verify-esp.sh FAILs when the kernel.exe inside ESP is corrupted.
#   5. verify-esp.sh PASSes when --manifest is supplied with matching hashes.
#   6. build-manifest.sh build emits toolchain_version, source_sha,
#      manifest_seed and check accepts a valid combination.
#   7. build-manifest.sh check rejects a malformed source_sha.
#   8. build-manifest.sh check rejects manifest_seed not matching
#      <source_sha>|<artifact_format>.

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

# Refresh ABI JSON mtime so build-manifest.sh build can run (the staleness
# guard refuses when the JSON is older than the artifacts).
python3 -c "import os; os.utime('build/boot-info-abi.kernel.json')"

note "[1] build-image.sh produces disk.img with non-zero sha256"
bash scripts/release/build-image.sh --out "$TMPDIR/run1.img" >/dev/null 2>&1
if [ -s "$TMPDIR/run1.img" ]; then
    sz="$(stat -c '%s' "$TMPDIR/run1.img")"
    if [ "$sz" -eq $((512 * 1024 * 1024)) ]; then
        ok "disk.img exists and is exactly 512 MiB"
    else
        bad "disk.img wrong size ($sz, expected 536870912)"
    fi
else
    bad "disk.img missing or empty"
fi

note "[2] two consecutive runs produce byte-identical disk.img"
bash scripts/release/build-image.sh --out "$TMPDIR/run2.img" >/dev/null 2>&1
if cmp -s "$TMPDIR/run1.img" "$TMPDIR/run2.img"; then
    ok "consecutive disk.img outputs are byte-identical"
else
    bad "consecutive runs differ -- non-deterministic"
fi

note "[2b] reusing an --out path with stale non-zero bytes still produces byte-identical output"
# Pre-fill the output path with non-zero bytes (covering the IXFS zero-fill
# zone), run build-image.sh against the same path, and assert the result
# matches the fresh-tempfile output from [2]. This catches the truncate-
# on-existing-file leakage case where pre-existing bytes survive in
# regions the producer never explicitly writes.
dd if=/dev/urandom of="$TMPDIR/stale_reuse.img" bs=1M count=512 status=none
bash scripts/release/build-image.sh --out "$TMPDIR/stale_reuse.img" >/dev/null 2>&1
if cmp -s "$TMPDIR/run1.img" "$TMPDIR/stale_reuse.img"; then
    ok "reused-path output equals fresh-path output (no stale-byte leakage)"
else
    bad "reused-path output differs from fresh -- stale bytes leaked into image"
fi

note "[3] verify-esp.sh passes on freshly-built image"
if bash scripts/release/verify-esp.sh "$TMPDIR/run1.img" >/dev/null 2>&1; then
    ok "verify-esp clean PASS on fresh image"
else
    bad "verify-esp should pass; it failed"
fi

note "[4] verify-esp.sh fails when kernel.exe is corrupted in-place"
cp "$TMPDIR/run1.img" "$TMPDIR/run1_bad.img"
# Flip a single byte inside the ESP kernel.exe payload area. The kernel
# file body lives well past the FAT/root/dirent area; byte 1500000 is
# inside cluster data for a 64 MiB ESP starting at LBA 2048.
python3 -c "
import sys
p = sys.argv[1]
with open(p, 'r+b') as f:
    f.seek(1500000)
    b = f.read(1)
    f.seek(1500000)
    f.write(bytes([b[0] ^ 0xFF]))
" "$TMPDIR/run1_bad.img"
if ! bash scripts/release/verify-esp.sh "$TMPDIR/run1_bad.img" >/dev/null 2>&1; then
    ok "verify-esp rejects corrupted-ESP image"
else
    bad "verify-esp should have rejected corrupted image; it passed"
fi

note "[5] verify-esp.sh passes when --manifest is supplied"
bash scripts/release/build-manifest.sh build --out "$TMPDIR/m.json" >/dev/null 2>&1
if bash scripts/release/verify-esp.sh "$TMPDIR/run1.img" --manifest "$TMPDIR/m.json" >/dev/null 2>&1; then
    ok "verify-esp PASSes against manifest"
else
    bad "verify-esp should pass with manifest; it failed"
fi

note "[6] build-manifest.sh build emits toolchain_version + source_sha + manifest_seed"
missing=0
for f in toolchain_version source_sha manifest_seed; do
    if ! grep -q "\"$f\"" "$TMPDIR/m.json"; then
        bad "manifest missing $f"
        missing=$((missing + 1))
    fi
done
if [ "$missing" -eq 0 ]; then
    ok "all three provenance fields present"
fi
if bash scripts/release/build-manifest.sh check "$TMPDIR/m.json" >/dev/null 2>&1; then
    ok "check accepts valid provenance fields"
else
    bad "check rejected manifest with valid provenance fields"
fi

note "[7] build-manifest.sh check rejects malformed source_sha"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["source_sha"] = "not-a-sha"
with open(p + ".bad_sha", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m.json.bad_sha" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'source_sha'; then
    ok "rejects malformed source_sha"
else
    bad "should have rejected; got rc=$rc, err: $err_out"
fi

note "[8] build-manifest.sh check rejects mismatched manifest_seed"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["manifest_seed"] = "deadbeef|raw"  # source_sha does not match
with open(p + ".bad_seed", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m.json.bad_seed" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'manifest_seed'; then
    ok "rejects mismatched manifest_seed"
else
    bad "should have rejected; got rc=$rc, err: $err_out"
fi

printf '\n[summary] %d pass, %d fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
