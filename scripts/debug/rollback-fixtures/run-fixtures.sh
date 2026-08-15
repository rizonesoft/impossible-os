#!/usr/bin/env bash
# ============================================================================
# run-fixtures.sh -- drive the anti-rollback NVRAM fixture harness.
#
# Proves the compositor-steady gate on the IPOSRequiredSecVersion NVRAM
# floor end-to-end, through the LIVE UEFI Runtime Services SetVariable
# path that a kernel unit test may not call. Five boots share ONE
# persistent OVMF_VARS.fd so the NVRAM state carries across them:
#
#   A  stock disk (anti_rollback_raise=0), boots to the shell prompt.
#      The floor must stay absent: `required=0`, no raise line.
#   B  opt-in disk with crash_test=2, which panics on the last statement
#      before compositor_run(). That boot performed every Phase-3 action
#      there is EXCEPT becoming user-visible, so its floor must not move.
#   B2 stock disk, SAME OVMF_VARS: an INDEPENDENT read of the store after
#      that death, which must still say `required=0`. This is the only
#      check that does not rely on the rollback code describing its own
#      behavior, so it holds whatever route a raise might have taken --
#      including a direct SetVariable that never touches the helpers.
#   C  opt-in disk, allowed to reach the first frame. The floor must
#      advance exactly once, and steady -> enqueued -> raised in order.
#   D  stock disk again, SAME OVMF_VARS. Its Phase-0 validator must now
#      report `required=<shipped>`, proving the write persisted AND that
#      the bootloader reads it back through the same GUID + attributes.
#
# Why boot D exists: the `boot_rollback: security version shipped=X
# required=Y` line is emitted by boot_rollback_validate() in Phase 0,
# BEFORE the steady worker calls SetVariable. Run C's own log therefore
# reports the value C started with, never the value C wrote. Only a
# LATER boot can observe the write.
#
# Why the dev host needs no virt-fw-vars: NVRAM state is read back
# through boot D's own serial log. `virt-fw-vars --print` is used only
# as an extra corroborating dump when it happens to be installed.
#
# Exit 0 when every fixture PASSed or the whole set SKIPped for a local
# environment gap. Exit 1 on any FAIL, on a forced (non-orderly) QEMU
# teardown, and on a full SKIP under GITHUB_ACTIONS -- a silent no-op
# must never satisfy a regression gate.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
FIXTURES_DIR="$BUILD_DIR/fixtures/rollback"

OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"

# ESP offset -- must match Makefile EFI_OFFSET, same derivation as
# scripts/patch-boot-conf.sh so the two cannot drift apart.
EFI_OFFSET="$(grep -oP 'EFI_OFFSET\s*:=\s*\K\d+' "$REPO_ROOT/Makefile" 2>/dev/null || echo "1048576")"

# Per-boot deadline. Each fixture stops early on its own marker, so these
# only bound a boot that never reaches its marker at all.
TIMEOUT_KVM="${ROLLBACK_TIMEOUT_KVM:-60}"
TIMEOUT_TCG="${ROLLBACK_TIMEOUT_TCG:-150}"
# How long an orderly SIGTERM shutdown may take before we escalate. QEMU
# flushes its block backends (including the pflash VARS file) during that
# shutdown, so escalating early is exactly the durability hazard this
# bound exists to avoid.
ORDERLY_WAIT="${ROLLBACK_ORDERLY_WAIT:-20}"
# Serial poll interval. Only affects how promptly a marker is noticed.
POLL_INTERVAL="${ROLLBACK_POLL_INTERVAL:-0.2}"
# How long a boot keeps running after its stop marker before the orderly
# shutdown. This is what makes "exactly once" falsifiable: a duplicate or
# late write has a window in which to appear in the log.
SETTLE_SECS="${ROLLBACK_SETTLE_SECS:-5}"

# --- QEMU process tracking -------------------------------------------
# Only LIVE pids are tracked: a waited pid is released back to the OS and
# can be reused, so an EXIT trap signalling a stale number could kill an
# unrelated process. Same discipline as the stale-ABI harness.
QEMU_PIDS=()
_qemu_register() { QEMU_PIDS+=("$1"); }
_qemu_unregister() {
    local target="$1" pid new=()
    for pid in "${QEMU_PIDS[@]:-}"; do
        [ -n "$pid" ] || continue
        [ "$pid" = "$target" ] && continue
        new+=("$pid")
    done
    QEMU_PIDS=("${new[@]:-}")
}
_kill_tracked_qemu() {
    local pid
    for pid in "${QEMU_PIDS[@]:-}"; do
        [ -n "$pid" ] || continue
        kill -TERM "$pid" 2>/dev/null || true
    done
    sleep 0.5
    for pid in "${QEMU_PIDS[@]:-}"; do
        [ -n "$pid" ] || continue
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    QEMU_PIDS=()
}
# EXIT is unconditional cleanup. INT/TERM must ALSO propagate the
# cancellation: bash does not auto-exit after a trapped signal, so a
# Ctrl-C between fixtures would otherwise be swallowed.
trap _kill_tracked_qemu EXIT
trap '_kill_tracked_qemu; exit 130' INT
trap '_kill_tracked_qemu; exit 143' TERM

USE_KVM=0
[ -w /dev/kvm ] && USE_KVM=1

if [ -t 1 ]; then
    RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
    CYAN=$'\033[0;36m'; DIM=$'\033[0;90m'; NC=$'\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; DIM=''; NC=''
fi
say_pass() { printf "%s[PASS]%s %s\n" "$GREEN" "$NC" "$1"; }
say_fail() { printf "%s[FAIL]%s %s\n" "$RED"   "$NC" "$1"; }
say_skip() { printf "%s[SKIP]%s %s\n" "$YELLOW" "$NC" "$1"; }
say_info() { printf "%s%s%s\n"        "$DIM"   "$1"  "$NC"; }

FAILED=0
EXECUTED=0

# --- Env probe -------------------------------------------------------
# Missing tooling is a developer-machine gap, not a product bug. KVM is
# deliberately NOT probed: a KVM-less host still runs every fixture under
# TCG, which exercises the identical NVRAM path more slowly.
probe_env() {
    local missing=()
    [ -f "$OVMF_CODE" ]     || missing+=("OVMF not at $OVMF_CODE")
    [ -f "$OVMF_VARS_SRC" ] || missing+=("OVMF_VARS not at $OVMF_VARS_SRC")
    command -v mcopy >/dev/null || missing+=("mtools (mcopy) not installed")
    command -v mtype >/dev/null || missing+=("mtools (mtype) not installed")
    command -v qemu-system-x86_64 >/dev/null || \
        missing+=("qemu-system-x86_64 not installed")
    if [ ${#missing[@]} -gt 0 ]; then
        local m
        for m in "${missing[@]}"; do say_info "  env gap: $m"; done
        return 1
    fi
    if [ "$USE_KVM" -eq 1 ]; then
        say_info "  accelerator: KVM (enabled)"
    else
        say_info "  accelerator: TCG (KVM unavailable; software emulation)"
    fi
    return 0
}

# --- Build a fixture disk with a patched boot.conf -------------------
# $1 = label, remaining args = key=value pairs written into boot.conf.
# Emits the fixture disk path on stdout; all chatter goes to stderr so
# command substitution captures only the path.
# Every step is checked explicitly and the disk is published to its final
# name only once it is fully built. `set -e` does NOT apply inside the
# command substitution this runs in (the caller tests its status), so an
# unchecked cp or sed here would sail past its own failure -- and with a
# fixed output name, a stale disk from a PREVIOUS run would then be booted
# and could pass while the current tree is broken.
make_fixture_disk() {
    local label="$1"; shift
    local out="$FIXTURES_DIR/$label-disk.img"
    local conf="$FIXTURES_DIR/$label-boot.conf"
    local tmp_disk="$out.partial"
    local tmp_conf="$conf.partial"
    local readback="$FIXTURES_DIR/$label-boot.conf.readback"

    rm -f "$out" "$conf" "$tmp_disk" "$tmp_conf" "$readback"

    if ! cp --reflink=auto --sparse=always \
            "$BUILD_DIR/system-disk.img" "$tmp_disk"; then
        say_fail "$label: could not copy system-disk.img" >&2
        rm -f "$tmp_disk"; return 1
    fi
    if ! cp "$REPO_ROOT/resources/boot/boot.conf" "$tmp_conf"; then
        say_fail "$label: could not copy resources/boot/boot.conf" >&2
        rm -f "$tmp_disk" "$tmp_conf"; return 1
    fi

    local kv key value
    for kv in "$@"; do
        key="${kv%%=*}"
        value="${kv#*=}"
        # Rebuild the file rather than sed-substituting: a value
        # containing / or & would otherwise be mangled by the s///
        # replacement, silently installing a different policy.
        if ! { grep -v "^${key}=" "$tmp_conf" > "$tmp_conf.next" &&
               printf '%s=%s\n' "$key" "$value" >> "$tmp_conf.next" &&
               mv "$tmp_conf.next" "$tmp_conf"; }; then
            say_fail "$label: could not set ${key}=${value} in boot.conf" >&2
            rm -f "$tmp_disk" "$tmp_conf" "$tmp_conf.next"; return 1
        fi
    done

    if ! mcopy -o -i "${tmp_disk}@@${EFI_OFFSET}" "$tmp_conf" \
               ::/EFI/ImpossibleOS/boot.conf >&2; then
        say_fail "$label: mcopy failed writing boot.conf into the fixture disk" >&2
        rm -f "$tmp_disk" "$tmp_conf"; return 1
    fi

    # Read the file back OUT of the ESP and compare it byte-for-byte with
    # what we meant to install. An `mdir` listing is not enough: it proves
    # a directory entry exists, not that this content reached it, and a
    # name-only check happily matches the boot.conf that was already there.
    if ! mtype -i "${tmp_disk}@@${EFI_OFFSET}" \
               ::/EFI/ImpossibleOS/boot.conf > "$readback" 2>/dev/null; then
        say_fail "$label: could not read boot.conf back out of the fixture ESP" >&2
        rm -f "$tmp_disk" "$tmp_conf" "$readback"; return 1
    fi
    if ! cmp -s "$tmp_conf" "$readback"; then
        say_fail "$label: boot.conf in the fixture ESP does not match what was written" >&2
        say_info "  wrote:  $tmp_conf" >&2
        say_info "  read:   $readback" >&2
        rm -f "$tmp_disk" "$tmp_conf" "$readback"; return 1
    fi
    # And the requested keys really are in the installed file.
    for kv in "$@"; do
        if ! grep -qxF "$kv" "$readback"; then
            say_fail "$label: '$kv' missing from the boot.conf installed in the ESP" >&2
            rm -f "$tmp_disk" "$tmp_conf" "$readback"; return 1
        fi
    done

    mv "$tmp_conf" "$conf" || return 1
    mv "$tmp_disk" "$out"  || return 1
    printf '%s\n' "$out"
}

# --- Stop a running QEMU in an orderly way ---------------------------
# SIGTERM makes QEMU run its normal shutdown, which closes the pflash
# backend and flushes the OVMF_VARS file. A SIGKILL here can drop an
# NVRAM write that firmware already reported as successful, so a forced
# teardown is reported as a FIXTURE FAILURE rather than being carried
# silently into the next boot, which reuses that very file.
# Returns 0 on orderly exit, 1 if escalation to SIGKILL was needed.
stop_qemu_orderly() {
    local qpid="$1"
    kill -TERM "$qpid" 2>/dev/null || true
    local deadline=$(( $(date +%s) + ORDERLY_WAIT ))
    while kill -0 "$qpid" 2>/dev/null; do
        if [ "$(date +%s)" -ge "$deadline" ]; then
            kill -KILL "$qpid" 2>/dev/null || true
            wait "$qpid" 2>/dev/null || true
            _qemu_unregister "$qpid"
            return 1
        fi
        sleep 0.2
    done
    wait "$qpid" 2>/dev/null || true
    _qemu_unregister "$qpid"
    return 0
}

# --- Boot one fixture ------------------------------------------------
# $1 = disk image, $2 = OVMF_VARS path, $3 = serial log, $4 = stop marker
# (a grep -F pattern; the boot is stopped once it appears), $5 = optional
# settle seconds to keep running AFTER the marker before stopping.
#
# Returns 0 when the boot stopped in an orderly way AND the requested
# marker was observed; 2 when it stopped cleanly but the marker never
# appeared; 1 when the teardown had to be forced. A marker timeout is NOT
# success: every caller asks for a marker because reaching it is part of
# what the fixture asserts, so silently returning 0 would let a boot that
# stalled early satisfy an absence-based assertion vacuously.
boot_fixture() {
    local disk="$1" vars="$2" serial="$3" marker="$4" settle="${5:-0}"

    local args=(
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
        -drive "if=pflash,format=raw,file=$vars"
        -drive "id=disk0,file=$disk,format=raw,if=none"
        -device "ich9-ahci,id=ahci0"
        -device "ide-hd,drive=disk0,bus=ahci0.0"
        -m 2G
        # Mirrors scripts/test-smoke.sh: the compositor must actually come
        # up for the steady gate to be exercised, and that gate is the
        # whole subject here, so the fixture uses the same SMP width the
        # smoke gate boots.
        -smp "${ROLLBACK_SMP:-2}"
        -serial "file:$serial"
        -no-reboot
        -no-shutdown
        -display none
    )
    local timeout
    if [ "$USE_KVM" -eq 1 ]; then
        args+=(-enable-kvm -cpu host)
        timeout="$TIMEOUT_KVM"
    else
        args+=(-machine q35 -cpu qemu64)
        timeout="$TIMEOUT_TCG"
    fi

    : > "$serial"
    qemu-system-x86_64 "${args[@]}" &
    local qpid=$!
    _qemu_register "$qpid"

    local deadline=$(( $(date +%s) + timeout ))
    local saw_marker=0
    while kill -0 "$qpid" 2>/dev/null; do
        if grep -qF "$marker" "$serial" 2>/dev/null; then
            saw_marker=1
            break
        fi
        if [ "$(date +%s)" -ge "$deadline" ]; then
            break
        fi
        sleep "$POLL_INTERVAL"
    done

    # Keep running past the marker when the caller asked for it, so a
    # late or DUPLICATE event still lands in the log. Stopping the instant
    # the first occurrence appears makes any "exactly once" claim
    # unfalsifiable -- there is no window in which a second one could be
    # recorded.
    if [ "$saw_marker" -eq 1 ] && [ "${settle%.*}" != "0" ]; then
        local settle_end=$(( $(date +%s) + ${settle%.*} ))
        while kill -0 "$qpid" 2>/dev/null; do
            [ "$(date +%s)" -ge "$settle_end" ] && break
            sleep "$POLL_INTERVAL"
        done
    fi

    if ! kill -0 "$qpid" 2>/dev/null; then
        # Guest died on its own (panic + -no-reboot, or firmware reset).
        wait "$qpid" 2>/dev/null || true
        _qemu_unregister "$qpid"
        [ "$saw_marker" -eq 1 ] || return 2
        return 0
    fi
    if ! stop_qemu_orderly "$qpid"; then
        say_info "  QEMU did not exit within ${ORDERLY_WAIT}s of SIGTERM;"
        say_info "  escalated to SIGKILL -- NVRAM durability is not"
        say_info "  guaranteed for this boot"
        return 1
    fi
    if [ "$saw_marker" -eq 0 ]; then
        say_info "  marker never appeared within ${timeout}s: $marker"
        return 2
    fi
    return 0
}

# Report how a boot_fixture status should be read, so every fixture
# handles a marker-miss and a forced teardown the same way.
# $1 = label, $2 = status, $3 = marker description
boot_status_ok() {
    case "$2" in
        0) return 0 ;;
        2) say_fail "$1: boot never reached $3"
           say_info "  an absence-based assertion on this boot would be vacuous"
           return 1 ;;
        *) say_fail "$1: boot did not stop in an orderly way"
           say_info "  NVRAM durability is not guaranteed; refusing to judge it"
           return 1 ;;
    esac
}

strip_log() {
    local serial="$1"
    local stripped="${serial%.log}.stripped.log"
    sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\x1b[=>]//g' "$serial" \
        > "$stripped" 2>/dev/null || cp "$serial" "$stripped"
    printf '%s\n' "$stripped"
}

# $1 = fixture label, $2 = stripped log, $3 = ERE, $4 = human description
assert_present() {
    if grep -qE "$3" "$2"; then return 0; fi
    say_fail "$1: MISSING $4"
    say_info "  expected to match: $3"
    say_info "  --- last 30 lines of $2 ---"
    tail -n 30 "$2" >&2 || true
    say_info "  --- end ---"
    return 1
}
assert_absent() {
    if ! grep -qE "$3" "$2"; then return 0; fi
    say_fail "$1: UNEXPECTED $4"
    say_info "  matched: $(grep -E "$3" "$2" | head -n 2)"
    return 1
}

# Shell prompt. This is a LATE, RELIABLE stop point -- the same sentinel
# the smoke gate uses -- but it is NOT proof of a composited frame:
# measured 2026-08-15, it is emitted during Phase-3 shell init, ahead of
# compositor_run(). A fixture that needs "the compositor really painted"
# must assert RE_STEADY; this marker only says the boot got that far.
# Fixtures that stop here run on for SETTLE_SECS so the first frame lands
# inside the captured log; if that window were ever too short, the
# RE_STEADY assertion fails loudly rather than passing vacuously.
MARKER_USERSPACE='C:\>'
# Fixture B stops the guest with the SHIPPED `crash_test=1` boot.conf
# knob, which panics at a fixed point in boot_desktop.c, rather than by
# racing a serial marker. Two earlier designs were measured and rejected
# on 2026-08-15:
#
#   "Boot complete in"  -- sits BEFORE the pre-section-16 raise site, so
#                          a regression that moved the raise back there
#                          would be masked.
#   "KB used / "        -- correctly placed AFTER that site, but only
#                          ~260 ms ahead of the first frame under KVM,
#                          so a 0.2 s poll routinely stopped the guest
#                          too late and the fixture failed on margin.
#                          (Its shorter form "Heap: " was worse still:
#                          it also matches the mm subsystem's early
#                          heap-init line at 0.000 s.)
#
# A panic needs no margin at all: the boot cannot proceed past it, on
# any accelerator, at any speed.
#
# crash_test=2 specifically, NOT 1. The early site (crash_test=1) sits
# ahead of the pre-section-16 raise site, so a regression that moved the
# raise there would never execute in fixture B. The late site is the last
# statement before compositor_run(), so B's boot performs every Phase-3
# action there is EXCEPT becoming user-visible -- which is exactly the
# boot whose floor must not move.
MARKER_CRASH_TEST='crash_test=2 -- triggering deliberate BSOD before compositor'

RE_VALIDATE='boot_rollback: security version shipped=[0-9]+ required=[0-9]+'
RE_RAISED='anti-rollback: raised IPOSRequiredSecVersion to [0-9]+ \(steady\)'
# The two ordering markers. STEADY is emitted synchronously by
# boot_rollback_mark_steady() on the compositor first-frame path;
# ENQUEUED synchronously by boot_rollback_request_raise() when it claims
# the single raise slot. Both fire on the CALLER's thread, so their order
# in the log is the real happens-before order -- unlike RE_RAISED, which
# the sys_wq worker emits whenever it gets scheduled and which therefore
# says nothing about when the gate opened.
RE_STEADY='anti-rollback: compositor steady latched'
RE_ENQUEUED='anti-rollback: raise request enqueued'

# First 1-based line number matching $2 in file $1, or empty.
line_of() { grep -nE "$2" "$1" 2>/dev/null | head -n 1 | cut -d: -f1; }

# Assert $3 occurs strictly before $4 in $2. This is the assertion the
# whole harness exists for: "the floor moved only after a user-visible
# frame" is an ORDERING claim, and a timing-based stop cannot express it.
assert_order() {
    local label="$1" f="$2" first="$3" second="$4" desc="$5"
    local a b
    a="$(line_of "$f" "$first")"
    b="$(line_of "$f" "$second")"
    if [ -z "$a" ] || [ -z "$b" ]; then
        say_fail "$label: cannot check order -- $desc"
        say_info "  first marker line: ${a:-<absent>}; second: ${b:-<absent>}"
        return 1
    fi
    if [ "$a" -ge "$b" ]; then
        say_fail "$label: WRONG ORDER -- $desc"
        say_info "  expected line $a (first marker) < line $b (second marker)"
        return 1
    fi
    return 0
}

# Assert $3 matches exactly $4 times in $2.
assert_count() {
    local label="$1" f="$2" re="$3" want="$4" desc="$5"
    local n
    n="$(grep -cE "$re" "$f" || true)"
    if [ "${n:-0}" != "$want" ]; then
        say_fail "$label: expected exactly $want $desc, saw ${n:-0}"
        return 1
    fi
    return 0
}

# Parsed from fixture A so the harness follows a release that bumps
# IPOS_KERNEL_SECURITY_VERSION instead of pinning the literal 1.
SHIPPED=""

fixture_a_cold_optout() {
    local label="A-cold-optout"
    EXECUTED=$((EXECUTED + 1))
    echo; printf "%s=== %s ===%s\n" "$CYAN" "$label" "$NC"
    say_info "  stock disk (anti_rollback_raise=0), boot to userspace"

    local serial="$FIXTURES_DIR/$label.serial.log"
    local st=0
    boot_fixture "$BUILD_DIR/system-disk.img" "$SHARED_VARS" \
                 "$serial" "$MARKER_USERSPACE" "$SETTLE_SECS" || st=$?
    local stripped; stripped="$(strip_log "$serial")"

    if ! boot_status_ok "$label" "$st" "the shell prompt"; then
        FAILED=$((FAILED + 1)); return
    fi

    local bad=0
    assert_present "$label" "$stripped" "$RE_VALIDATE" \
        "Phase-0 rollback validator line" || bad=1
    assert_present "$label" "$stripped" 'boot_rollback: security version shipped=[0-9]+ required=0 ' \
        "required=0 on a cold NVRAM store" || bad=1
    assert_present "$label" "$stripped" 'Boot complete in' \
        "Boot complete sentinel" || bad=1
    # The whole point of an opt-OUT fixture is that it reached the state
    # where an opt-IN boot WOULD have raised. Without this the no-raise
    # assertion is satisfied by any boot that simply died early.
    assert_present "$label" "$stripped" "$RE_STEADY" \
        "compositor steady latch (proves the raise was actually declined, not skipped)" || bad=1
    # The request IS expected here: the compositor first-frame path calls
    # boot_rollback_request_raise() unconditionally and the OPT-IN check
    # happens later, inside raise_if_steady(). So an opt-out boot enqueues
    # and then declines to write, and the absence of the WRITE is what
    # distinguishes the policy.
    assert_present "$label" "$stripped" "$RE_ENQUEUED" \
        "raise request (enqueued regardless of policy; the write is what is gated)" || bad=1
    assert_absent  "$label" "$stripped" "$RE_RAISED" \
        "raise on an opt-OUT boot" || bad=1

    SHIPPED="$(grep -oE 'shipped=[0-9]+' "$stripped" | head -n 1 | cut -d= -f2 || true)"
    if [ -z "$SHIPPED" ]; then
        say_fail "$label: could not parse shipped= from the validator line"
        bad=1
    else
        say_info "  shipped security version = $SHIPPED"
    fi

    if [ "$bad" -eq 0 ]; then
        say_pass "$label (cold store stays at required=0; opt-out withholds the raise)"
    else
        FAILED=$((FAILED + 1))
    fi
}

fixture_b_optin_pre_steady() {
    local label="B-optin-pre-steady"
    EXECUTED=$((EXECUTED + 1))
    echo; printf "%s=== %s ===%s\n" "$CYAN" "$label" "$NC"
    say_info "  opt-in disk + crash_test=2: all of Phase 3 runs, then the boot"
    say_info "  dies deterministically immediately before the compositor"

    local disk
    if ! disk="$(make_fixture_disk "$label" anti_rollback_raise=1 crash_test=2)"; then
        FAILED=$((FAILED + 1)); return
    fi
    local serial="$FIXTURES_DIR/$label.serial.log"
    local st=0
    boot_fixture "$disk" "$SHARED_VARS" "$serial" \
                 "$MARKER_CRASH_TEST" "$SETTLE_SECS" || st=$?
    local stripped; stripped="$(strip_log "$serial")"

    if ! boot_status_ok "$label" "$st" "the deliberate crash point"; then
        FAILED=$((FAILED + 1)); return
    fi

    local bad=0
    assert_present "$label" "$stripped" "$MARKER_CRASH_TEST" \
        "deliberate crash marker (the boot must actually have died here)" || bad=1
    assert_present "$label" "$stripped" "$RE_VALIDATE" \
        "Phase-0 validator (boot got far enough to be a meaningful test)" || bad=1
    assert_present "$label" "$stripped" 'boot_rollback: security version shipped=[0-9]+ required=0 ' \
        "required=0 still, before any raise" || bad=1
    # The load-bearing assertion. The raise runs async on sys_wq, so the
    # ABSENCE of its success log proves nothing on its own -- a regression
    # that requests the raise early can simply not have been scheduled yet.
    # The enqueue marker is synchronous on the requesting thread, so it
    # catches the request even when the write never lands.
    assert_absent "$label" "$stripped" "$RE_STEADY" \
        "steady latch on a boot that never reached the compositor" || bad=1
    assert_absent "$label" "$stripped" "$RE_ENQUEUED" \
        "raise REQUEST before the first composited frame" || bad=1

    # Safety net: the panic must really have stopped the boot. Deliberately
    # NOT keyed on the shell prompt -- that is emitted during Phase-3 shell
    # init, BEFORE compositor_run(), so it appears in this log even on a
    # correct run (measured 2026-08-15, prompt at serial line 753 against
    # the crash marker at 755). RE_STEADY, asserted absent above, is the
    # marker that actually means a frame was painted.
    assert_present "$label" "$stripped" 'KERNEL_PANIC|\[PANIC\]' \
        "panic banner (the deliberate crash must really have halted the boot)" || bad=1

    assert_absent "$label" "$stripped" "$RE_RAISED" \
        "raise on a boot that never reached the first composited frame" || bad=1

    if [ "$bad" -eq 0 ]; then
        say_pass "$label (deep boot, no first frame -> floor held)"
    else
        FAILED=$((FAILED + 1))
    fi
}

# The INDEPENDENT pre-frame oracle. Everything fixture B asserts is read
# out of B's own serial log using markers the rollback code emits about
# itself, so a regression that relocates the whole steady -> enqueued ->
# raised sequence wholesale still produces one correctly ordered triplet
# and slips past every log-based check. This fixture asks the only
# question those markers cannot answer: after a boot that did all of
# Phase 3 and then died before its first frame, what does the NVRAM
# store actually say? It must still say zero, whatever route a raise
# might have taken -- including a direct uefi_set_variable() that never
# touches the helpers at all.
fixture_b2_pre_steady_readback() {
    local label="B2-pre-steady-readback"
    EXECUTED=$((EXECUTED + 1))
    echo; printf "%s=== %s ===%s\n" "$CYAN" "$label" "$NC"
    say_info "  stock disk, SAME OVMF_VARS -- the floor must be untouched by"
    say_info "  the pre-frame boot that just died"

    local serial="$FIXTURES_DIR/$label.serial.log"
    local st=0
    boot_fixture "$BUILD_DIR/system-disk.img" "$SHARED_VARS" \
                 "$serial" "$MARKER_USERSPACE" "$SETTLE_SECS" || st=$?
    local stripped; stripped="$(strip_log "$serial")"

    if [ ! -s "$stripped" ]; then
        say_fail "$label: boot produced NO serial output at all"
        say_info "  the shared variable store is damaged or unreadable after"
        say_info "  the pre-frame death -- an in-flight raise is the usual cause"
        FAILED=$((FAILED + 1)); return
    fi
    if ! boot_status_ok "$label" "$st" "the shell prompt"; then
        FAILED=$((FAILED + 1)); return
    fi

    local bad=0
    # Read THIS boot's own validator record, not any line that happens to
    # say required=0. The previous fixture panicked on purpose, so its klog
    # was persisted and this boot replays it through klog_crash_recover()
    # with a `[CRASH-PREV] ` prefix -- including B's own pre-crash
    # "required=0". A floor that partially advanced would then be masked by
    # B's recovered evidence, and the independent oracle would be reading
    # the very boot it is supposed to be checking up on.
    local own_validator
    own_validator="$(grep -a 'boot_rollback: security version shipped=' "$stripped" \
                     | grep -av 'CRASH-PREV' | head -n 1 || true)"
    if [ -z "$own_validator" ]; then
        say_fail "$label: no Phase-0 validator record of its own"
        say_info "  (replayed CRASH-PREV lines do not count -- they describe"
        say_info "   the previous boot, which is the one under suspicion)"
        FAILED=$((FAILED + 1)); return
    fi
    say_info "  own validator record: ${own_validator##*] }"
    if ! printf '%s\n' "$own_validator" \
         | grep -qE 'shipped=[0-9]+ required=0 '; then
        say_fail "$label: THE FLOOR MOVED on a boot that never presented a frame"
        say_info "  this boot's own validator reads: ${own_validator##*] }"
        bad=1
    fi
    assert_absent "$label" "$stripped" 'ANTI-ROLLBACK REFUSAL' \
        "pre-jump downgrade refusal (the floor moved and stranded the image)" || bad=1

    if [ "$bad" -eq 0 ]; then
        say_pass "$label (NVRAM independently confirms the floor held)"
    else
        FAILED=$((FAILED + 1))
    fi
}

fixture_c_optin_steady() {
    local label="C-optin-steady"
    EXECUTED=$((EXECUTED + 1))
    echo; printf "%s=== %s ===%s\n" "$CYAN" "$label" "$NC"
    say_info "  opt-in disk, allowed to reach the first composited frame"

    local disk
    if ! disk="$(make_fixture_disk "$label" anti_rollback_raise=1)"; then
        FAILED=$((FAILED + 1)); return
    fi
    local serial="$FIXTURES_DIR/$label.serial.log"
    local st=0
    # Stop at the SHELL PROMPT, not at the raise line, and then settle.
    # Stopping on the raise made every "exactly once" claim unfalsifiable
    # (the log was truncated at the first occurrence) and let a raise that
    # fired BEFORE the first frame satisfy the fixture. Running on to
    # userspace gives both a window for a duplicate write to appear and
    # the later marker needed to check ordering.
    boot_fixture "$disk" "$SHARED_VARS" "$serial" \
                 "$MARKER_USERSPACE" "$SETTLE_SECS" || st=$?
    local stripped; stripped="$(strip_log "$serial")"

    if ! boot_status_ok "$label" "$st" "the shell prompt"; then
        FAILED=$((FAILED + 1)); return
    fi

    local bad=0
    assert_present "$label" "$stripped" "$RE_STEADY" \
        "compositor steady latch" || bad=1
    assert_present "$label" "$stripped" "$RE_ENQUEUED" \
        "raise request" || bad=1
    assert_present "$label" "$stripped" "$RE_RAISED" \
        "raise on a steady opt-in boot" || bad=1
    assert_absent "$label" "$stripped" 'anti-rollback: SetVariable failed' \
        "SetVariable failure" || bad=1
    assert_absent "$label" "$stripped" 'anti-rollback: raise skipped in headless mode' \
        "headless-mode skip (fixture must boot a real compositor)" || bad=1

    # The gate, stated as the ordering it actually is: the floor may move
    # only after a user-visible frame. A raise requested from anywhere
    # earlier in the boot fails here even when the write itself succeeds.
    assert_order "$label" "$stripped" "$RE_STEADY" "$RE_ENQUEUED" \
        "the raise must be requested AFTER the steady latch" || bad=1
    assert_order "$label" "$stripped" "$RE_ENQUEUED" "$RE_RAISED" \
        "the NVRAM write must follow its request" || bad=1

    if [ -n "$SHIPPED" ]; then
        assert_present "$label" "$stripped" \
            "raised IPOSRequiredSecVersion to $SHIPPED \(steady\)" \
            "raise to the shipped version ($SHIPPED)" || bad=1
    fi
    # Exactly once, now over a log that ran well past the first occurrence.
    assert_count "$label" "$stripped" "$RE_STEADY"   1 "steady latch"  || bad=1
    assert_count "$label" "$stripped" "$RE_ENQUEUED" 1 "raise request" || bad=1
    assert_count "$label" "$stripped" "$RE_RAISED"   1 "raise line"    || bad=1

    if [ "$bad" -eq 0 ]; then
        say_pass "$label (first frame -> floor advanced exactly once)"
    else
        FAILED=$((FAILED + 1))
    fi
}

fixture_d_readback() {
    local label="D-readback"
    EXECUTED=$((EXECUTED + 1))
    echo; printf "%s=== %s ===%s\n" "$CYAN" "$label" "$NC"
    say_info "  stock disk, SAME OVMF_VARS -- the bootloader must read the"
    say_info "  floor C wrote (this is the only boot that can observe it)"

    local serial="$FIXTURES_DIR/$label.serial.log"
    local st=0
    boot_fixture "$BUILD_DIR/system-disk.img" "$SHARED_VARS" \
                 "$serial" "$MARKER_USERSPACE" "$SETTLE_SECS" || st=$?
    local stripped; stripped="$(strip_log "$serial")"

    # A silent boot is the loudest result this fixture can produce, and it
    # deserves a named diagnosis rather than two generic MISSING lines.
    # Observed 2026-08-15 while proving the harness catches its own
    # regression: with the raise moved back to the pre-section-16 site, the
    # opt-in boot that dies before the first frame leaves a SetVariable in
    # flight, the variable store is damaged, and the NEXT boot emits nothing
    # at all. That unbootable machine IS the stranding section 16 exists to
    # prevent, so name it here instead of making a reader infer it.
    if [ ! -s "$stripped" ]; then
        say_fail "$label: boot produced NO serial output at all"
        say_info "  the firmware never reached BOOTX64.EFI. The shared variable"
        say_info "  store is damaged or unreadable -- which is what a raise that"
        say_info "  is in flight when a pre-steady boot dies leaves behind."
        say_info "  This is the stranded-machine outcome the steady gate exists"
        say_info "  to prevent; treat it as a rollback regression, not a flake."
        FAILED=$((FAILED + 1)); return
    fi

    if ! boot_status_ok "$label" "$st" "the shell prompt"; then
        FAILED=$((FAILED + 1)); return
    fi

    local bad=0
    local want="${SHIPPED:-1}"
    assert_present "$label" "$stripped" \
        "boot_rollback: security version shipped=[0-9]+ required=$want " \
        "required=$want read back from NVRAM" || bad=1
    # A wrong GUID, a wrong attribute set, or a corrupted store makes the
    # bootloader fail closed rather than report a value, so these two
    # banners are the discriminating evidence for HOW a readback broke.
    assert_absent "$label" "$stripped" 'ANTI-ROLLBACK: IPOSRequiredSecVersion read failed' \
        "fail-closed NVRAM read (wrong attrs/size/GUID?)" || bad=1
    assert_absent "$label" "$stripped" 'ANTI-ROLLBACK REFUSAL' \
        "pre-jump downgrade refusal" || bad=1
    assert_present "$label" "$stripped" 'Boot complete in' \
        "Boot complete sentinel (image still boots at the raised floor)" || bad=1
    # Opt-out again, so the floor must not move a second time. The request
    # is expected (see fixture A); only the WRITE is policy-gated.
    assert_present "$label" "$stripped" "$RE_STEADY" \
        "compositor steady latch on the readback boot" || bad=1
    assert_absent "$label" "$stripped" "$RE_RAISED" \
        "second raise on the opt-OUT readback boot" || bad=1

    if command -v virt-fw-vars >/dev/null 2>&1; then
        say_info "  virt-fw-vars corroboration:"
        virt-fw-vars --input "$SHARED_VARS" --print 2>/dev/null \
            | grep -i "IPOSRequiredSecVersion" || \
            say_info "    (variable not listed by virt-fw-vars)"
    fi

    if [ "$bad" -eq 0 ]; then
        say_pass "$label (floor persisted across reboot and is consumed pre-jump)"
    else
        FAILED=$((FAILED + 1))
    fi
}

# --- Orchestrate -----------------------------------------------------
mkdir -p "$FIXTURES_DIR"

echo "========================================"
echo "  Anti-Rollback NVRAM Fixture Harness"
echo "========================================"

if ! probe_env; then
    say_skip "all fixtures (env gaps listed above)"
    echo
    echo "========================================"
    echo "  SKIPPED (environment not ready)"
    echo "========================================"
    # A silent SKIP must never satisfy the CI regression gate; on a
    # runner, an env gap is a broken runner, not a developer-machine gap.
    if [ "${GITHUB_ACTIONS:-}" = "true" ]; then
        printf "%s  CI SKIP-ALL UPGRADED TO FAIL (env gaps on Actions runner)%s\n" \
            "$RED" "$NC" >&2
        exit 1
    fi
    exit 0
fi

# Freshness, not merely presence. Booting whatever image happens to be on
# disk can report 4/4 PASS for code that is no longer in the tree, which
# is the worst failure a regression gate has: a green verdict about
# something that was never tested. Rebuild when any build input is newer
# than the image. `build.sh` is incremental, so this costs nothing when
# the image is already current.
if [ ! -f "$BUILD_DIR/system-disk.img" ]; then
    say_info "  system-disk.img missing; running bash scripts/build.sh..."
    bash "$REPO_ROOT/scripts/build.sh" >/dev/null 2>&1 || {
        say_fail "preflight: scripts/build.sh failed"
        exit 1
    }
else
    # The roots are the documented build-input set (the same list the
    # receipt machinery fingerprints), NOT a subset: tools/ in particular
    # holds the disk-image producer and asset generators, so leaving it
    # out would let a change there boot yesterday's image.
    #
    # find errors are FATAL rather than discarded. With 2>/dev/null a
    # missing root or an unreadable directory produces empty output --
    # exactly what "nothing changed" looks like -- so the guard would
    # fail open in precisely the situation where it is least trustworthy.
    _newer_err="$FIXTURES_DIR/.freshness.err"
    mkdir -p "$FIXTURES_DIR"
    _stale_check() {
        find "$REPO_ROOT/src" "$REPO_ROOT/include" "$REPO_ROOT/user" \
             "$REPO_ROOT/resources" "$REPO_ROOT/tools" "$REPO_ROOT/Makefile" \
             "$REPO_ROOT/scripts/build.sh" \
             -newer "$BUILD_DIR/system-disk.img" -print -quit 2>"$_newer_err"
    }
    _newer="$(_stale_check)" || {
        say_fail "preflight: could not scan build inputs for freshness"
        cat "$_newer_err" >&2 || true
        exit 1
    }
    if [ -s "$_newer_err" ]; then
        say_fail "preflight: freshness scan reported errors; refusing to"
        say_fail "           certify an image it could not check"
        cat "$_newer_err" >&2 || true
        exit 1
    fi
    if [ -n "$_newer" ]; then
        say_info "  build inputs are newer than system-disk.img; rebuilding..."
        bash "$REPO_ROOT/scripts/build.sh" >/dev/null 2>&1 || {
            say_fail "preflight: scripts/build.sh failed"
            exit 1
        }
        # Re-check AFTER the build. An incremental build that leaves an
        # input newer than the image it produced has not made the image
        # current, and booting it would test something other than the tree.
        _newer="$(_stale_check)" || true
        if [ -n "$_newer" ]; then
            say_fail "preflight: build inputs are STILL newer than system-disk.img"
            say_info "  first offender: $_newer"
            exit 1
        fi
    fi
fi

# The single persistent variable store. Copied fresh from the firmware
# template so the run starts from a cold NVRAM with the floor absent.
SHARED_VARS="$FIXTURES_DIR/shared.OVMF_VARS.fd"
cp "$OVMF_VARS_SRC" "$SHARED_VARS"

fixture_a_cold_optout
fixture_b_optin_pre_steady
fixture_b2_pre_steady_readback
fixture_c_optin_steady
fixture_d_readback

echo
echo "========================================"
printf "  executed=%d failed=%d\n" "$EXECUTED" "$FAILED"
if [ "$FAILED" -eq 0 ] && [ "$EXECUTED" -gt 0 ]; then
    printf "%s  ROLLBACK HARNESS PASSED%s\n" "$GREEN" "$NC"
    echo "========================================"
    exit 0
elif [ "$EXECUTED" -eq 0 ]; then
    # Env probe passed but nothing ran -- treat as FAIL so a silent no-op
    # cannot be mistaken for a green regression gate.
    printf "%s  ROLLBACK HARNESS NO-OP (zero fixtures executed)%s\n" "$RED" "$NC"
    echo "========================================"
    exit 1
else
    printf "%s  ROLLBACK HARNESS FAILED (%d fixture(s))%s\n" \
        "$RED" "$FAILED" "$NC"
    echo "========================================"
    exit 1
fi
