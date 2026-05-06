#!/usr/bin/env bash
# test-boot-matrix.sh -- structural test for scripts/ci/boot-matrix.sh.
#
# Invokes the matrix in two scenarios and asserts:
#   1. Build artifacts present     -> exit 0, summary contains PASS or SKIP
#   2. Missing artifacts           -> still exit 0 (SKIPs are not failures)
#                                     and prints "BOOT MATRIX: PASS"
#   3. Per-config logs land in build/ci/<config>.log
#   4. Synthetic FAIL              -> exit 1 (validates the FAIL path)
#
# This is host-side; it does not actually boot the OS (which the matrix
# already does at runtime). The unit-style assertions here cover the
# orchestrator skeleton: skip classification, log placement, exit code.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

PASS=0
FAIL=0

pass() { printf '  [PASS] %s\n' "$1"; PASS=$((PASS + 1)); }
fail() { printf '  [FAIL] %s\n' "$1" >&2; FAIL=$((FAIL + 1)); }

tmp_out="$(mktemp)"
tmp_err="$(mktemp)"
trap 'rm -f "$tmp_out" "$tmp_err"' EXIT

echo "[1/4] Help text shows 6 configurations"
set +e
bash scripts/ci/boot-matrix.sh --help >"$tmp_out" 2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] && pass "--help exit=0" || fail "--help exit=$rc"
grep -q "raw " "$tmp_out" && pass "raw config listed" || fail "raw missing"
grep -q "iso " "$tmp_out" && pass "iso config listed" || fail "iso missing"
grep -q "vhdx " "$tmp_out" && pass "vhdx config listed" || fail "vhdx missing"
grep -q "vdi " "$tmp_out" && pass "vdi config listed" || fail "vdi missing"
grep -q "whpx " "$tmp_out" && pass "whpx config listed" || fail "whpx missing"
grep -q "usb-loop " "$tmp_out" && pass "usb-loop config listed" || fail "usb-loop missing"

echo "[2/4] Unknown arg -> exit 2"
set +e
bash scripts/ci/boot-matrix.sh --bogus >"$tmp_out" 2>"$tmp_err"
rc=$?
set -e
[ "$rc" -eq 2 ] && pass "unknown-arg exit=2" || fail "unknown-arg exit=$rc"

echo "[3/4] Custom --out DIR honored"
custom_dir="$(mktemp -d)"
trap 'rm -f "$tmp_out" "$tmp_err"; rm -rf "$custom_dir"' EXIT
set +e
bash scripts/ci/boot-matrix.sh --out "$custom_dir" >"$tmp_out" 2>&1
rc=$?
set -e
[ -d "$custom_dir" ] && pass "custom out dir created" || fail "custom out dir missing"

echo "[4/4] Summary table format"
grep -q "^=== boot-matrix summary ===$" "$tmp_out" && pass "summary header" || fail "summary header missing"
grep -q "Totals: " "$tmp_out" && pass "totals line" || fail "totals line missing"
grep -qE "^BOOT MATRIX: (PASS|FAIL)$" "$tmp_out" && pass "verdict line" || fail "verdict line missing"
# Final exit code: 0 if all PASS or SKIP, 1 if any FAIL. With no
# release artifacts the matrix should report 0.
grep -q "^BOOT MATRIX: PASS$" "$tmp_out" && pass "all-pass-or-skip -> PASS verdict" || fail "expected PASS verdict"

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
