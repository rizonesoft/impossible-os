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
CAPTURES=""
START_ALL=$(date +%s)

echo -e "${CYAN}=== smoke matrix: $LEGS ===${NC}"

for leg in $LEGS; do
    engine="${leg%%:*}"; cpus="${leg##*:}"
    log="$LOG_DIR/${engine}-${cpus}cpu.log"
    t0=$(date +%s)

    # Capture OUTSIDE build/, then move the log in afterwards.
    #
    # An earlier fix recreated $LOG_DIR before each leg, on the theory that the
    # build only deleted logs written by EARLIER legs. It does worse than that:
    # the redirect below opens the log before test-smoke.sh runs, test-smoke.sh
    # builds, and the build removes subdirectories under build/ -- unlinking the
    # file while the shell still holds the fd. The leg then writes to an inode
    # with no name, so the log is gone even for the leg that produced it, the
    # FAIL branch's `grep ... "$log"` reads nothing, and a failing configuration
    # reports no reason at all. Measured 2026-07-28: a full green matrix left
    # build/smoke-matrix/ EMPTY while printing four paths into it.
    #
    # A capture path outside build/ cannot be unlinked by the build, so the log
    # survives to be read by the FAIL branch below and to be moved into its
    # documented location for the operator.
    cap="$(mktemp "${TMPDIR:-/tmp}/smoke-matrix-${engine}-${cpus}cpu.XXXXXX")"

    # Each leg is a full test-smoke.sh run: it rebuilds nothing new (the build is
    # already current after the first leg) but re-boots and re-asserts, which is
    # the point -- the assertions are what differ per configuration.
    if [ "$engine" = "tcg" ]; then
        SMOKE_FORCE_TCG=1 SMOKE_SMP="$cpus" bash scripts/test-smoke.sh > "$cap" 2>&1
    else
        SMOKE_SMP="$cpus" bash scripts/test-smoke.sh > "$cap" 2>&1
    fi
    rc=$?
    # Publish at the END of the run, not here: a LATER leg's build wipes
    # build/ subdirectories again, so a log moved into place now would be
    # deleted by the next leg. Only the final leg's log ever survived that
    # way (measured 2026-07-28: a green four-leg matrix left exactly one
    # file behind). Keep every capture on its unlinkable-by-build path and
    # move them all in once the last leg is done.
    CAPTURES="$CAPTURES $cap:$log"
    t1=$(date +%s); dur=$((t1 - t0))

    if [ "$rc" = "0" ]; then
        PASS=$((PASS + 1))
        printf "  ${GREEN}PASS${NC}  %-8s %s cpu   %2ds   ${DIM}%s${NC}\n" "$engine" "$cpus" "$dur" "$log"
    else
        FAIL=$((FAIL + 1)); FAILED_LEGS="$FAILED_LEGS ${engine}:${cpus}cpu"
        printf "  ${RED}FAIL${NC}  %-8s %s cpu   %2ds   ${DIM}%s${NC}\n" "$engine" "$cpus" "$dur" "$log"
        # One reason line, not the log: the operator (or the artifact envelope)
        # has the path when they want the rest. Read the live CAPTURE, not the
        # published path -- publication happens after the last leg, so $log
        # does not exist yet while this leg is being reported.
        reason=$(grep -aoE 'Detected[^"]{0,90}|Log ends on a fatal shape[^"]{0,70}' "$cap" 2>/dev/null | head -1)
        [ -n "$reason" ] && printf "        ${DIM}%s${NC}\n" "$reason"
    fi
done

TOTAL=$(( $(date +%s) - START_ALL ))

# Publish every capture now that no further build can wipe the directory.
# Recreated here because the last leg's build removed it again.
mkdir -p "$LOG_DIR"
for pair in $CAPTURES; do
    mv -f "${pair%%:*}" "${pair##*:}" 2>/dev/null || true
done

echo ""
if [ "$FAIL" = "0" ]; then
    echo -e "${GREEN}SMOKE MATRIX PASSED${NC}  ($PASS/$((PASS+FAIL)) legs, ${TOTAL}s)"
    exit 0
fi
echo -e "${RED}SMOKE MATRIX FAILED${NC}  ($FAIL/$((PASS+FAIL)) legs failed:${FAILED_LEGS} )"
echo -e "  ${DIM}A leg that fails on ONE configuration is the finding, not noise --${NC}"
echo -e "  ${DIM}the engine/CPU difference IS the evidence (CLAUDE.md).${NC}"
exit 1
