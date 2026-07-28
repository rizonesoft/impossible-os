#!/usr/bin/env bash
# test-smoke-matrix.sh -- boot the image across the engine x CPU-count matrix.
#
# WHY THIS EXISTS. `test-smoke.sh` proves the image boots to userspace on ONE
# configuration. Until 2026-07-28 that configuration was KVM with no `-smp` flag
# at all, i.e. a single CPU -- in a project whose stated doctrine is "SMP-safe by
# default ... SMP from day one". A section that changed the TEB <-> kernel_gs_base
# handoff across context switches shipped green through it and then halted on the
# operator's 2-CPU WHPX boot with
#     [CRIT] sched: ring-3 task N thread 0 has TEB but kernel_gs_base=0
# The single-CPU run reached the identical point and survived.
#
# The two engines are NOT redundant, and CLAUDE.md says so: KVM catches real-CPU
# MSR traps and SMP timing; TCG catches "timing-sensitive races that KVM's speed
# hides" -- a SYS_EXEC frame-handoff bug survived 297 commits and a green local
# suite because KVM always landed a tick that TCG did not. Crossing both engines
# with both CPU counts is four boots and roughly half a minute, against a class
# of bug that has twice reached a shipped section.
#
# OUTPUT IS DELIBERATELY BOUNDED. Each leg's full log stays on disk; this prints
# one line per leg plus a verdict. Cache-read is ~77% of an overnight run's bill
# and per-turn context is the driver, so a matrix that dumped four boot logs into
# the conversation would cost more than the regressions it catches. Run it under
# scripts/overnight/run-artifact.sh for the JSON envelope.
#
# WHPX is NOT in this matrix: it is a Windows accelerator reached through
# scripts/machines/run-qemu.ps1, costs ~141s per boot against ~3-10s here, and
# belongs at the section boundary rather than the per-edit loop.
#
# Usage:
#   bash scripts/test-smoke-matrix.sh              # all four legs
#   SMOKE_MATRIX_LEGS="kvm:2 tcg:2" bash ...       # a subset
# Exit: 0 only if every leg passed.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

GREEN='\033[0;32m'; RED='\033[0;31m'; DIM='\033[0;90m'; CYAN='\033[0;36m'; NC='\033[0m'

# engine:cpus. KVM first: it is the fastest, so a break that both engines see is
# reported in seconds rather than after the slow leg.
LEGS="${SMOKE_MATRIX_LEGS:-kvm:1 kvm:2 tcg:1 tcg:2}"

LOG_DIR="$REPO_ROOT/build/smoke-matrix"
mkdir -p "$LOG_DIR"

PASS=0; FAIL=0; FAILED_LEGS=""
START_ALL=$(date +%s)

echo -e "${CYAN}=== smoke matrix: $LEGS ===${NC}"

for leg in $LEGS; do
    engine="${leg%%:*}"; cpus="${leg##*:}"
    # Recreate per leg, not once up front: test-smoke.sh runs a build, and the
    # build removes subdirectories under build/ -- which silently deleted this
    # directory after the first leg and made every later leg fail on "No such
    # file or directory" rather than on anything about the kernel.
    mkdir -p "$LOG_DIR"
    log="$LOG_DIR/${engine}-${cpus}cpu.log"
    t0=$(date +%s)

    # Each leg is a full test-smoke.sh run: it rebuilds nothing new (the build is
    # already current after the first leg) but re-boots and re-asserts, which is
    # the point -- the assertions are what differ per configuration.
    if [ "$engine" = "tcg" ]; then
        SMOKE_FORCE_TCG=1 SMOKE_SMP="$cpus" bash scripts/test-smoke.sh > "$log" 2>&1
    else
        SMOKE_SMP="$cpus" bash scripts/test-smoke.sh > "$log" 2>&1
    fi
    rc=$?
    t1=$(date +%s); dur=$((t1 - t0))

    if [ "$rc" = "0" ]; then
        PASS=$((PASS + 1))
        printf "  ${GREEN}PASS${NC}  %-8s %s cpu   %2ds   ${DIM}%s${NC}\n" "$engine" "$cpus" "$dur" "$log"
    else
        FAIL=$((FAIL + 1)); FAILED_LEGS="$FAILED_LEGS ${engine}:${cpus}cpu"
        printf "  ${RED}FAIL${NC}  %-8s %s cpu   %2ds   ${DIM}%s${NC}\n" "$engine" "$cpus" "$dur" "$log"
        # One reason line, not the log: the operator (or the artifact envelope)
        # has the path when they want the rest.
        reason=$(grep -aoE 'Detected[^"]{0,90}|Log ends on a fatal shape[^"]{0,70}' "$log" 2>/dev/null | head -1)
        [ -n "$reason" ] && printf "        ${DIM}%s${NC}\n" "$reason"
    fi
done

TOTAL=$(( $(date +%s) - START_ALL ))
echo ""
if [ "$FAIL" = "0" ]; then
    echo -e "${GREEN}SMOKE MATRIX PASSED${NC}  ($PASS/$((PASS+FAIL)) legs, ${TOTAL}s)"
    exit 0
fi
echo -e "${RED}SMOKE MATRIX FAILED${NC}  ($FAIL/$((PASS+FAIL)) legs failed:${FAILED_LEGS} )"
echo -e "  ${DIM}A leg that fails on ONE configuration is the finding, not noise --${NC}"
echo -e "  ${DIM}the engine/CPU difference IS the evidence (CLAUDE.md).${NC}"
exit 1
