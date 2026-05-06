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
# Pre-fill the output path with stale non-zero sentinel bytes inside the
# IXFS zero-fill region (where build-image.sh leaves zeros), then re-run
# the producer against the same path and assert the result matches the
# fresh-tempfile output from [2]. We use a sparse file plus a few sentinel
# bytes rather than 512 MiB of /dev/urandom because the invariant only
# needs unwritten regions to NOT survive; full-image random fills paid
# 512 MiB of I/O per harness run for no extra coverage.
truncate -s "$((512 * 1024 * 1024))" "$TMPDIR/stale_reuse.img"
# IXFS zero-fill region starts at LBA 395264 (byte 202375168) per
# build-image.sh layout. Drop sentinel bytes near the start, middle, and
# end of that region so a producer that left ANY of them intact would
# differ from a fresh run.
python3 -c "
import sys
p = sys.argv[1]
sentinels = [
    (202375168, b'\\xDE\\xAD\\xBE\\xEF'),  # IXFS region start
    (370000000, b'\\xCA\\xFE\\xBA\\xBE'),  # IXFS middle
    (536000000, b'\\xFE\\xED\\xFA\\xCE'),  # near image end
]
with open(p, 'r+b') as f:
    for off, b in sentinels:
        f.seek(off); f.write(b)
" "$TMPDIR/stale_reuse.img"
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

note "[5b] verify-esp.sh --manifest fails when boot_config entry is missing (no fallback)"
# Strip the boot_config entry from the manifest. verify-esp must FAIL in
# --manifest mode rather than silently fall back to build-tree hashes.
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["entries"] = [e for e in m["entries"] if e.get("name") != "boot_config"]
with open(p + ".no_boot_config", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/verify-esp.sh "$TMPDIR/run1.img" --manifest "$TMPDIR/m.json.no_boot_config" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'no matching entry in --manifest'; then
    ok "verify-esp --manifest fails closed on missing boot.conf entry"
else
    bad "verify-esp should have failed; got rc=$rc, err: $err_out"
fi

note "[5c] manifest entries[] includes boot_config when boot.conf source exists"
if grep -q '"name": "boot_config"' "$TMPDIR/m.json"; then
    ok "manifest entries[] includes boot_config row"
else
    bad "manifest entries[] missing boot_config row"
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

note "[9] verify-esp.sh --manifest rejects path-only forgery (bootloader entry renamed)"
# Hand-craft a manifest where the required name=bootloader row is renamed to an
# optional/forward-compatible name with the bootloader's hash at the canonical
# ESP path. Pre-fix verify-esp matched by path alone and would have certified
# the artifact; post-fix it must fail closed because the named-slot contract
# is broken.
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
# Rename the bootloader entry to a non-required allowed name; keep its path
# and sha256 intact so a path-keyed verifier would still happily match.
for e in m["entries"]:
    if e.get("name") == "bootloader":
        e["name"] = "blackbox_skeleton"
        e["optional"] = True
with open(p + ".forged_path", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/verify-esp.sh "$TMPDIR/run1.img" --manifest "$TMPDIR/m.json.forged_path" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'no matching entry in --manifest'; then
    ok "verify-esp --manifest rejects path-keyed forgery (bootloader renamed)"
else
    bad "verify-esp should have rejected; got rc=$rc, err: $err_out"
fi

note "[10] build-manifest.sh check rejects raw-format manifest missing boot_config"
# verify-esp.sh fails closed on missing boot_config (test [5b]); the packaging
# checker must agree, otherwise the two release gates disagree about what a
# valid manifest looks like.
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["entries"] = [e for e in m["entries"] if e.get("name") != "boot_config"]
with open(p + ".no_bc_check", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m.json.no_bc_check" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'name=boot_config'; then
    ok "build-manifest check rejects raw manifest missing boot_config"
else
    bad "should have rejected; got rc=$rc, err: $err_out"
fi

note "[11] parallel build-image.sh runs to different --out paths do not race"
# Pre-fix WORK_DIR was a fixed path so two parallel invocations would clobber
# each other's stage tree and the first to exit would rm -rf the other's
# working state. Post-fix uses mktemp -d for a per-invocation work dir.
# Free TMPDIR before spawning two more 512 MiB images: tests [1]/[2]/[2b]/[4]
# leave four prior images behind that would push peak TMPDIR usage past 3 GiB
# on tmpfs-backed CI runners. We only need run1.img beyond this point if
# later tests reference it; they do not.
rm -f "$TMPDIR/run2.img" "$TMPDIR/stale_reuse.img" "$TMPDIR/run1_bad.img" "$TMPDIR/run1.img"
bash scripts/release/build-image.sh --out "$TMPDIR/par_a.img" >/dev/null 2>&1 &
pid_a=$!
bash scripts/release/build-image.sh --out "$TMPDIR/par_b.img" >/dev/null 2>&1 &
pid_b=$!
wait_rc=0
wait $pid_a || wait_rc=$?
wait $pid_b || wait_rc=$?
if [ "$wait_rc" -eq 0 ] && cmp -s "$TMPDIR/par_a.img" "$TMPDIR/par_b.img"; then
    ok "parallel runs both produced byte-identical output"
else
    bad "parallel runs failed or diverged (rc=$wait_rc)"
fi

printf '\n[summary] %d pass, %d fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
