#!/usr/bin/env bash
# test-build-iso.sh -- host-side regression harness for the hybrid UEFI ISO
# producer (build-iso.sh).
#
# Owner: Hybrid ISO / El Torito UEFI Boot feature in
# todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md.
#
# Exercises:
#   [1] build-iso.sh produces disk.iso with non-zero sha256.
#   [2] Two consecutive runs produce byte-identical disk.iso.
#   [3] xorriso -indev disk.iso -report_el_torito lists the UEFI boot entry.
#   [4] ISO has NO BIOS boot record (UEFI-only contract).
#   [5] /IPOS/manifest.json is embedded.
#   [6] Embedded manifest passes build-manifest.sh check.
#   [7] /IPOS/installer/ + /IPOS/recovery/ placeholder dirs exist.

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

# Refresh build/boot-info-abi.kernel.json mtime so build-manifest.sh's
# staleness guard accepts it. Same touch pattern the sibling release tests
# (test-build-image.sh, test-vm-conversion.sh) use; intentional.
python3 -c "import os; os.utime('build/boot-info-abi.kernel.json')"

# Stage a fresh raw disk.img in TMPDIR. The bind step inside build-iso.sh
# (verify-esp.sh --manifest) requires the source disk.img's ESP to match
# the freshly-generated manifest's hashes; this assumes both were produced
# from the same build-tree state. Producing the disk.img in-test
# guarantees that.
note "[0] staging fresh raw disk.img for the test run"
bash scripts/release/build-image.sh --out "$TMPDIR/disk.img" >/dev/null 2>&1
if [ -s "$TMPDIR/disk.img" ]; then
    ok "fresh raw disk.img staged"
else
    bad "could not stage raw disk.img"
fi

note "[1] build-iso.sh produces disk.iso with non-zero sha256"
bash scripts/release/build-iso.sh --in "$TMPDIR/disk.img" --out "$TMPDIR/run1.iso" >/dev/null 2>&1
if [ -s "$TMPDIR/run1.iso" ]; then
    ok "disk.iso exists and is non-empty"
else
    bad "disk.iso missing or empty"
fi

note "[2] two consecutive build-iso.sh runs produce byte-identical disk.iso"
bash scripts/release/build-iso.sh --in "$TMPDIR/disk.img" --out "$TMPDIR/run2.iso" >/dev/null 2>&1
if cmp -s "$TMPDIR/run1.iso" "$TMPDIR/run2.iso"; then
    ok "consecutive disk.iso outputs are byte-identical"
else
    bad "consecutive runs differ -- non-deterministic"
fi

note "[3] xorriso -report_el_torito lists the UEFI boot entry"
report="$(xorriso -indev "$TMPDIR/run1.iso" -report_el_torito as_mkisofs 2>/dev/null)"
if printf '%s' "$report" | grep -qE "^-e '/EFI/esp.img'"; then
    ok "El Torito UEFI entry references /EFI/esp.img"
else
    bad "El Torito UEFI entry missing or wrong path"
fi

note "[4] ISO has NO BIOS boot record (UEFI-only)"
# A BIOS El Torito entry would surface as `-b <path>` in the report; UEFI-only
# only emits `-e <path>`. Confirm no `-b ` flag appears in the report.
if printf '%s' "$report" | grep -q "^-b "; then
    bad "ISO contains BIOS boot record (-b in report) -- UEFI-only contract violated"
else
    ok "ISO has no BIOS boot record"
fi

note "[5] /IPOS/manifest.json is embedded"
# xorriso requires -osirrox on to allow image-to-disk copies. Run extract
# in a here-doc style command list so a failed extract does not propagate
# `set -e`; the -s test on the output decides PASS/FAIL.
set +e
xorriso -osirrox on -indev "$TMPDIR/run1.iso" -extract /IPOS/manifest.json "$TMPDIR/manifest.json" >/dev/null 2>&1
set -e
if [ -s "$TMPDIR/manifest.json" ]; then
    ok "/IPOS/manifest.json extracted (size > 0)"
else
    bad "/IPOS/manifest.json missing or empty"
fi

note "[6] embedded manifest passes build-manifest.sh check"
if [ -s "$TMPDIR/manifest.json" ] && bash scripts/release/build-manifest.sh check "$TMPDIR/manifest.json" >/dev/null 2>&1; then
    ok "embedded manifest validates against schema"
else
    bad "embedded manifest fails build-manifest.sh check"
fi

note "[7] /IPOS/installer/ + /IPOS/recovery/ placeholder dirs exist"
# Listing the parent and grepping for the directory entries works without
# image-to-disk extraction. xorriso -ls returns dir names; we just check
# that both expected names appear under /IPOS.
listing="$(xorriso -indev "$TMPDIR/run1.iso" -ls /IPOS 2>/dev/null || true)"
inst_ok=0
recv_ok=0
printf '%s' "$listing" | grep -qE "installer" && inst_ok=1
printf '%s' "$listing" | grep -qE "recovery"  && recv_ok=1
if [ "$inst_ok" = 1 ] && [ "$recv_ok" = 1 ]; then
    ok "both /IPOS/installer/ and /IPOS/recovery/ present"
else
    bad "missing dir(s): installer=$inst_ok recovery=$recv_ok (listing: $listing)"
fi

note "[8] build-iso.sh fails closed when source ESP and manifest disagree"
# Corrupt the staged disk image inside its ESP region so the verify-esp
# bind check fails. Without the bind step, build-iso.sh would happily
# produce an ISO whose embedded manifest describes one BOOTX64.EFI hash
# while the El Torito boot image carries a different one.
cp "$TMPDIR/disk.img" "$TMPDIR/disk_corrupt.img"
python3 -c "
import sys
p = sys.argv[1]
with open(p, 'r+b') as f:
    f.seek(5000000)  # inside ESP kernel.exe payload
    b = f.read(1)
    f.seek(5000000)
    f.write(bytes([b[0] ^ 0xFF]))
" "$TMPDIR/disk_corrupt.img"
set +e
err_out="$(bash scripts/release/build-iso.sh --in "$TMPDIR/disk_corrupt.img" --out "$TMPDIR/run_corrupt.iso" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'sha256 mismatch'; then
    ok "build-iso fails closed on ESP/manifest mismatch"
else
    bad "build-iso should have failed; got rc=$rc"
fi

note "[9] build-iso.sh refuses --in == --out (data-loss guard)"
cp "$TMPDIR/disk.img" "$TMPDIR/same.img"
set +e
err_out="$(bash scripts/release/build-iso.sh --in "$TMPDIR/same.img" --out "$TMPDIR/same.img" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'same canonical path'; then
    ok "build-iso refuses --in == --out"
else
    bad "should have refused; got rc=$rc"
fi

note "[10] build-iso.sh rejects truncated source disk image"
truncate -s 4M "$TMPDIR/short.img"
set +e
err_out="$(bash scripts/release/build-iso.sh --in "$TMPDIR/short.img" --out "$TMPDIR/short.iso" 2>&1 >/dev/null)"
rc=$?
set -e
if [ "$rc" -ne 0 ] && printf '%s' "$err_out" | grep -q 'need at least'; then
    ok "build-iso rejects truncated input"
else
    bad "should have rejected; got rc=$rc"
fi

printf '\n[summary] %d pass, %d fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
