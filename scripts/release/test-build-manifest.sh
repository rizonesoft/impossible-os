#!/usr/bin/env bash
# test-build-manifest.sh -- host-side tests for build-manifest.sh.
#
# Exercises:
#   1. build mode produces a manifest with all required fields populated.
#   2. check mode passes on a freshly-built manifest.
#   3. check mode fails (exit non-zero) when kernel_sha256 is removed,
#      with [ERROR] message naming the missing field on stderr.
#   4. Deterministic build: two consecutive `build` invocations produce
#      byte-identical manifests (preserves the reproducible-image-build
#      feature contract from the boot-media TODO).

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

note "[1] build mode produces required fields"
bash scripts/release/build-manifest.sh build --out "$TMPDIR/m.json" >/dev/null
for f in bootloader_sha256 kernel_sha256 partition_map secure_boot_status boot_info_version artifact_uuid; do
    if grep -q "\"$f\"" "$TMPDIR/m.json"; then
        ok "$f present"
    else
        bad "$f missing"
    fi
done

note "[2] check mode passes on a fresh manifest"
if bash scripts/release/build-manifest.sh check "$TMPDIR/m.json" >/dev/null 2>&1; then
    ok "check exits 0"
else
    bad "check did not exit 0"
fi

note "[3] check mode fails with [ERROR] when kernel_sha256 removed"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
del m["kernel_sha256"]
with open(p + ".broken", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m.json.broken" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
    ok "check exits non-zero ($rc) on missing kernel_sha256"
else
    bad "check should have failed"
fi
if printf '%s\n' "$err_out" | grep -q '\[ERROR\].*kernel_sha256'; then
    ok "stderr names missing field (kernel_sha256)"
else
    bad "stderr did not name missing field; got: $err_out"
fi

note "[4a] check rejects manifest_version != 1"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["manifest_version"] = 2
with open(p + ".bad_ver", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m.json.bad_ver" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'manifest_version'; then
    ok "rejects manifest_version=2"
else
    bad "should have rejected; got rc=$rc, err: $err_out"
fi

note "[4b] check rejects unknown artifact_format"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["artifact_format"] = "bogus"
with open(p + ".bad_fmt", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m.json.bad_fmt" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'artifact_format'; then
    ok "rejects artifact_format=bogus"
else
    bad "should have rejected; got rc=$rc, err: $err_out"
fi

note "[4c] check rejects malformed artifact_uuid"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["artifact_uuid"] = "not-a-uuid"
with open(p + ".bad_uuid", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m.json.bad_uuid" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'artifact_uuid'; then
    ok "rejects malformed artifact_uuid"
else
    bad "should have rejected; got rc=$rc, err: $err_out"
fi

note "[4d] check rejects well-formed but wrong artifact_uuid"
python3 -c '
import json, sys, uuid
p = sys.argv[1]
with open(p) as f: m = json.load(f)
# Replace with a syntactically-valid but content-mismatched UUID.
m["artifact_uuid"] = "00000000-0000-5000-8000-000000000000"
with open(p + ".bad_uuid_sem", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m.json.bad_uuid_sem" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'deterministic UUID'; then
    ok "rejects mismatched-content artifact_uuid"
else
    bad "should have rejected; got rc=$rc, err: $err_out"
fi

note "[4e] check rejects malformed disk_guid when present"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["disk_guid"] = "not-a-guid"
with open(p + ".bad_disk_guid", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m.json.bad_disk_guid" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'disk_guid'; then
    ok "rejects malformed disk_guid"
else
    bad "should have rejected; got rc=$rc, err: $err_out"
fi

note "[4f] check rejects bogus sector_size when present"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["sector_size"] = 999
with open(p + ".bad_sector", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m.json.bad_sector" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'sector_size'; then
    ok "rejects sector_size=999"
else
    bad "should have rejected; got rc=$rc, err: $err_out"
fi

note "[4g] build --format installer emits media_role=installer"
bash scripts/release/build-manifest.sh build --format installer --out "$TMPDIR/m_inst.json" >/dev/null
if grep -q '"media_role": "installer"' "$TMPDIR/m_inst.json"; then
    ok "installer format derives media_role=installer"
else
    bad "installer format did not set media_role=installer"
fi
if bash scripts/release/build-manifest.sh check "$TMPDIR/m_inst.json" >/dev/null 2>&1; then
    ok "installer manifest passes check"
else
    bad "installer manifest should pass check"
fi

note "[4h] check rejects artifact_format=installer with media_role=normal"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["media_role"] = "normal"
# UUID was computed with installer in the seed, leave as-is so the cross-field
# check fires before the UUID semantic check.
with open(p + ".bad_role", "w") as f: json.dump(m, f)
' "$TMPDIR/m_inst.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m_inst.json.bad_role" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'artifact_format=installer requires media_role=installer'; then
    ok "rejects installer/normal mismatch"
else
    bad "should have rejected; got rc=$rc, err: $err_out"
fi

note "[4i] secure_boot_status reports unsigned on stale stamp"
# Simulate stale signing: create stamp + fingerprint with older mtime than the
# kernel artifact; verify build emits secure_boot_status=unsigned.
fp_tmp="$TMPDIR/sign.fingerprint"
stamp_tmp="$TMPDIR/sign.stamp"
echo "deadbeef" > "$fp_tmp"
touch "$stamp_tmp"
# Backdate stamps to be older than build/kernel.exe (which mtime is "now").
touch -d "1 hour ago" "$fp_tmp" "$stamp_tmp"
SIGN_FINGERPRINT_FILE="$fp_tmp" SIGN_STAMP_FILE="$stamp_tmp" \
    bash scripts/release/build-manifest.sh build --out "$TMPDIR/m_stale.json" >/dev/null
if grep -q '"secure_boot_status": "unsigned"' "$TMPDIR/m_stale.json"; then
    ok "stale signing stamp -> unsigned"
else
    bad "stale stamp should produce unsigned"
fi

note "[4j] build refuses stale boot-info-abi.kernel.json (older than artifacts)"
abi_tmp="$TMPDIR/abi.json"
cp build/boot-info-abi.kernel.json "$abi_tmp"
touch -d "1 hour ago" "$abi_tmp"
set +e
err_out="$(BOOT_INFO_ABI_FILE="$abi_tmp" bash scripts/release/build-manifest.sh build --out "$TMPDIR/m_stale_abi.json" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'older than build artifacts'; then
    ok "stale ABI JSON refuses build"
else
    bad "stale ABI JSON should fail; got rc=$rc, err: $err_out"
fi

note "[4k] check accepts additive optional entry name (v1 forward-compat)"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["entries"].append({
    "name": "future_payload",
    "path": "\\\\IPOS\\\\future.bin",
    "sha256": "0"*64,
    "size_bytes": 4096,
    "optional": True,
})
with open(p + ".additive", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
if bash scripts/release/build-manifest.sh check "$TMPDIR/m.json.additive" >/dev/null 2>&1; then
    ok "additive optional entry passes check"
else
    bad "additive optional entry should pass; check rejected it"
fi

note "[4l] check rejects unknown entry name when optional=false"
python3 -c '
import json, sys
p = sys.argv[1]
with open(p) as f: m = json.load(f)
m["entries"].append({
    "name": "future_payload",
    "path": "\\\\IPOS\\\\future.bin",
    "sha256": "0"*64,
    "size_bytes": 4096,
    "optional": False,
})
with open(p + ".bad_additive", "w") as f: json.dump(m, f)
' "$TMPDIR/m.json"
set +e
err_out="$(bash scripts/release/build-manifest.sh check "$TMPDIR/m.json.bad_additive" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'must be one of'; then
    ok "rejects unknown non-optional entry name"
else
    bad "should have rejected; got rc=$rc, err: $err_out"
fi

note "[5] deterministic build (artifact_uuid is reproducibility-friendly)"
bash scripts/release/build-manifest.sh build --out "$TMPDIR/m2.json" >/dev/null
if cmp -s "$TMPDIR/m.json" "$TMPDIR/m2.json"; then
    ok "two consecutive builds are byte-identical"
else
    bad "consecutive builds differ; UUID is not deterministic"
fi

printf '\n[summary] %d pass, %d fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
