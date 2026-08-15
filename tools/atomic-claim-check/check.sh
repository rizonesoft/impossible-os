#!/usr/bin/env bash
# ============================================================================
# atomic-claim-check -- prove, against the GENERATED OBJECT, that each audited
# ownership transition is exactly ONE lock-prefixed instruction.
#
# WHY THIS IS NOT A UNIT TEST. The property is "taking the thing and recording
# who owns it happen in one atomic transition". No post-state assertion can see
# it: an implementation that CASes the word and then stores the owner separately
# reaches a byte-identical post-state, and passes every fixture. The difference
# is only visible in the instruction stream, and only matters when an NMI or #MC
# lands between the two -- which no fixture can schedule either.
#
# The alternative considered was a mutation hook that simulates an abort after
# each write to the word. Rejected: it puts test scaffolding inside the
# production atomic path, on the panic path, which the kernel-code-quality
# production-quality gate forbids. Disassembling the shipped object costs the
# production code nothing.
#
# WHAT IS CHECKED, per audited function:
#   - the symbol EXISTS in the object (a rename or an accidental `static` that
#     lets it be inlined away must FAIL, not silently pass with zero findings --
#     that is the failure mode a grep-based gate would have);
#   - it contains exactly ONE `lock` prefix;
#   - that prefix is on a `cmpxchg`, so a `lock or` / `lock and` / `lock xchg`
#     that mutates the word in a way the exact-compare contract does not allow
#     is rejected rather than counted.
#
# Usage: bash tools/atomic-claim-check/check.sh [object]
#   object defaults to build/kernel/drivers/serial.o. Exits 0 on pass, 1 on a
#   violation, and 2 when the object has not been built (advisory, not a
#   failure: a caller that has not compiled cannot be asked to prove codegen).
# ============================================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "${1:-}" in
    --required|--selftest|"") OBJ="$REPO_ROOT/build/kernel/drivers/serial.o" ;;
    *)                        OBJ="$1" ;;
esac

# The audited transitions. Each is a single compare-exchange that both takes (or
# releases) a resource AND records (or verifies) its owner, so that an abort at
# any instruction boundary leaves the resource either free or attributably owned.
AUDITED=(
    serial_lock_try_acquire_owned   # the UART lock: owner INSIDE the lock word
    serial_lock_try_release_owned   # its exact-owner inverse
    serial_emerg_claim_slot         # a wedged-UART allowance: {generation, owner}
    serial_emerg_release_slot       # its exact inverse
    serial_emerg_mark_timeout       # marking one spent: {..., timed-out}
)

# ORDERED transitions: functions where the audited compare-exchange must PRECEDE
# a port write, because the compare-exchange is the linearization point of an
# event the port write makes externally visible.
#
# WHY THIS IS A SECOND CHECK AND NOT A THIRD AUDITED ENTRY. Everything in
# AUDITED asserts a transition is indivisible; this asserts that two divisible
# things happen in a particular ORDER. serial_emerg_timeout_byte marks a wedged
# UART reservation as spent and then pushes the byte, and reversing those two
# lines still yields exactly one locked cmpxchg and zero other memory writes --
# so AUDITED passes the reversed function without complaint.
#
# The reversed order is not hypothetical: it was the originally proposed design
# for abandoned-reservation reclamation and was rejected in design review. An
# NMI or #MC landing
# between the port write and the mark enters the nested async-isolation panic,
# finds an unmarked claim, reclaims it on the way to parking forever, and erases
# a timeout that really happened -- which hands the next real panic an extra
# full-length wait on a UART already known to be wedged. No fixture can assert
# it: both orders reach the same post-state, and no fixture can schedule the
# abort into the gap either. The order is only visible in the instruction stream.
ORDERED=(
    serial_emerg_timeout_byte       # mark the charge, THEN emit the byte
)

# ac_count_locks -- read a disassembly on stdin, print
# "<locks> <bound> <writes> <badwidth>":
#   locks    = how many lock prefixes appear
#   bound    = how many of them are attached to a cmpxchg
#   writes   = OTHER instructions that write memory
#   badwidth = locked compare-exchanges that are not the 32-bit cmpxchgl
#
# THE COUNTS MUST BE BOUND TO THE SAME INSTRUCTION, which is the whole reason
# this is not two greps. Counting `lock` occurrences and `cmpxchg` occurrences
# independently passes a function containing one `lock or` plus a separate
# UNLOCKED `cmpxchg` -- both counts read 1, and the exact-compare property the
# gate exists to prove is violated.
#
# ONE LOCKED CMPXCHG IS NOT SUFFICIENT ON ITS OWN, either. A function that
# stores the owner with a plain `movl %eax, (%rdi)` and THEN performs one
# unrelated locked compare-exchange also yields exactly one lock on a cmpxchg --
# and is precisely the two-step, NMI-visible mutation this gate exists to
# reject. So every OTHER memory write is counted and must be zero. Both shapes
# are negative controls in the selftest.
#
# A memory WRITE is an instruction whose operand string ends in a memory
# reference, which in AT&T order is the destination: `movl %eax, (%rdi)` writes,
# `movl (%rdi), %eax` reads. Frame and padding instructions are allowlisted by
# mnemonic -- push/pop touch the stack at %rsp and so cannot be the audited
# word, lea computes an address without dereferencing it, cmp/test read only,
# and the nop forms carry a memory operand they never touch.
#
# Two renderings to handle: llvm-objdump emits the prefix on a line of its own
# with the instruction on the next line, while GNU objdump folds `lock cmpxchg`
# into one line.
#
# NOT CHECKED, deliberately, and stated so that its absence is a decision rather
# than an oversight: WHICH WORD the compare-exchange targets. The two serial-lock
# helpers compare-exchange through a caller-supplied pointer
# (`cmpxchgl %esi, (%rdi)`), so there is no target to pin -- it is whatever the
# caller passed, which the C signature already fixes. The three claim helpers
# (claim, release, mark-timeout) relocate as
# `R_X86_64_32S .bss+<offset>`, a section-relative relocation carrying no symbol
# name for an assertion to compare against. Target exactness is carried by the
# signatures and the unit tests; this gate carries atomicity.
ac_count_locks() {
    awk '
    {
        line = $0
        # Strip the address column and the raw-byte column explicitly rather
        # than by tab position: llvm-objdump puts ONE tab before the mnemonic
        # and another before the operands, GNU objdump puts two before the
        # mnemonic and none after. Cutting at "the first tab" or "the last tab"
        # is therefore correct for exactly one of them.
        sub(/^[ \t]+/, "", line)
        sub(/^[0-9a-f]+:[ \t]*/, "", line)      # address
        sub(/^([0-9a-f][0-9a-f][ ]+)+/, "", line)  # raw bytes
        sub(/^[ \t]+/, "", line)
        sub(/[ \t]+$/, "", line)
        if (line == "")            next

        # A prefix ALONE on its line (llvm). Matched against the whole line, not
        # against the mnemonic: `lock cmpxchgl ...` (GNU) also has "lock" as its
        # first field, and treating that as a bare prefix loses the instruction
        # it is attached to -- which is the binding this gate is entirely about.
        if (line == "lock")        { locks++; pending = 1; next }

        locked = 0
        if (pending)               { pending = 0; locked = 1 }

        if (line ~ /^lock[ \t]+/) {             # GNU folded rendering
            locks++
            locked = 1
            sub(/^lock[ \t]+/, "", line)
        }

        mn = line; sub(/[ \t].*$/, "", mn)      # mnemonic
        ops = line; sub(/^[^ \t]*[ \t]*/, "", ops)

        if (locked && mn ~ /^cmpxchg/) {
            bound++
            if (mn != "cmpxchgl") badwidth++
            next
        }

        # An UNLOCKED compare-exchange still writes memory, and must not be
        # excused by the read-only allowlist below on the strength of its `cmp`
        # prefix -- that would let the very shape this gate rejects go uncounted.
        if (mn ~ /^cmpxchg/)       { writes++; next }

        # A STACK SPILL IS NOT A SHARED-STATE WRITE, and excluding it is what
        # keeps this gate from failing a build over a codegen change. The
        # audited words are statics or caller-supplied pointers; a destination
        # relative to %rsp or %rbp is this frame, which no other CPU can see and
        # no abort can observe across. Without this, clang spilling a by-value
        # parameter at a different optimisation level or version turns a
        # REQUIRED CI gate red with a diagnosis about two-step mutation that is
        # simply wrong.
        if (ops ~ /\((%rsp|%rbp|%esp|%ebp)[,)]/)
            next

        # Any other instruction whose destination is memory, INCLUDING a locked
        # one: `lock orl %esi, (%rdi)` mutates the word without comparing it.
        if (ops ~ /\)$/ && mn !~ /^(nop|push|pop|lea|cmp|test|prefetch|ret|hlt|ud2)/)
            writes++
    }
    END { printf "%d %d %d %d\n", locks + 0, bound + 0, writes + 0, badwidth + 0 }'
}

# ac_order_lock_vs_out -- read a disassembly on stdin, print "<cx> <cxlast> <out>":
#   cx     = position of the FIRST locked compare-exchange, 0 if absent
#   cxlast = position of the LAST locked compare-exchange, 0 if absent
#   out    = position of the FIRST port write, 0 if absent
#
# Positions rather than a verdict so the caller can report WHICH way round a
# violation is, and so the absent cases (a mark that vanished, a byte that is no
# longer emitted here) are distinguishable from a mere reordering. Deliberately
# a separate matcher from ac_count_locks: that one answers "is this transition
# indivisible", a property of a single instruction, and this one answers "did
# these two land in this order", a property of the sequence.
#
# LAST as well as FIRST, because "the first cmpxchg precedes the first out" is
# satisfied by a function that marks, emits, and then marks AGAIN -- and the
# second mark is a charge recorded after the byte it accounts for, which is the
# very window this gate exists to close. Checking both ends means no locked
# compare-exchange may follow the port write, in any shape.
#
# WHAT THIS GATE DOES NOT PROVE, stated so its absence is a decision rather than
# an oversight: DOMINANCE. These are textual positions in one symbol's
# disassembly, not a control-flow analysis, so a branch that jumps forward over
# the compare-exchange and lands on the port write is NOT rejected. That shape is
# expected and correct in the audited function: the branches are the inlined
# validator's early exits -- a malformed token, a slot the array does not have, a
# claim from a dead epoch, one owned by another CPU, or one already marked -- and
# on every one of them this CPU provably holds no live unmarked claim, so there
# is nothing to record before the byte goes out. That those exits are the only
# unmarked paths is carried by the unit tests, which drive each of them through
# the public API; what is carried HERE is that no mark is ever emitted after its
# byte, which no fixture can observe at all.
ac_order_lock_vs_out() {
    awk '
    {
        line = $0
        sub(/^[ \t]+/, "", line)
        sub(/^[0-9a-f]+:[ \t]*/, "", line)          # address
        sub(/^([0-9a-f][0-9a-f][ ]+)+/, "", line)   # raw bytes
        sub(/^[ \t]+/, "", line)
        sub(/[ \t]+$/, "", line)
        if (line == "")            next

        n++
        if (line == "lock")        { pending = 1; next }

        locked = 0
        if (pending)               { pending = 0; locked = 1 }
        if (line ~ /^lock[ \t]+/)  { locked = 1; sub(/^lock[ \t]+/, "", line) }

        mn = line; sub(/[ \t].*$/, "", mn)

        if (locked && mn ~ /^cmpxchg/) { if (!cx) cx = n; cxlast = n }
        if (mn ~ /^out/ && !ot)          ot = n
    }
    END { printf "%d %d %d\n", cx + 0, cxlast + 0, ot + 0 }'
}

# Negative controls for the matcher itself. A gate whose own logic is untested
# is a gate that reports "ok" for reasons nobody has checked.
if [ "${1:-}" = "--selftest" ]; then
    ac_expect() {
        local desc="$1" want="$2" got
        got="$(printf '%s\n' "$3" | ac_count_locks)"
        if [ "$got" = "$want" ]; then
            echo "ok   selftest: $desc"
        else
            echo "FAIL selftest: $desc -- expected '$want', got '$got'"
            return 1
        fi
    }
    rc=0
    ac_expect "llvm split prefix on a cmpxchg" "1 1 0 0" \
"       0: 55                    	pushq	%rbp
      17: f0                    	lock
      18: 0f b1 37              	cmpxchgl	%esi, (%rdi)
      24: c3                    	retq" || rc=1
    ac_expect "GNU folded lock cmpxchg" "1 1 0 0" \
"  17:	f0 0f b1 37          	lock cmpxchgl %esi,(%rdi)" || rc=1
    # Both of these write memory: the locked OR mutates without comparing, and
    # the cmpxchg is unlocked. Two writes, no bound transition.
    ac_expect "one lock OR plus a separate UNLOCKED cmpxchg is REJECTED" "1 0 2 0" \
"       0: f0                    	lock
       1: 09 37                 	orl	%esi, (%rdi)
       8: 0f b1 37              	cmpxchgl	%esi, (%rdi)" || rc=1
    # THE TWO-STEP SHAPE. One locked cmpxchg, correctly bound, correct width --
    # and a plain store of the owner before it. This is what a naive
    # "exactly one lock cmpxchg" gate accepts and what an NMI can split.
    ac_expect "a plain store plus one locked cmpxchg is REJECTED" "1 1 1 0" \
"       0: 89 07                 	movl	%eax, (%rdi)
       2: f0                    	lock
       3: 0f b1 37              	cmpxchgl	%esi, (%rdi)" || rc=1
    ac_expect "a memory READ before the cmpxchg is fine" "1 1 0 0" \
"       0: 8b 07                 	movl	(%rdi), %eax
       2: f0                    	lock
       3: 0f b1 37              	cmpxchgl	%esi, (%rdi)" || rc=1
    ac_expect "a wider compare-exchange is REJECTED" "1 1 0 1" \
"       0: f0                    	lock
       1: 0f c7 0f              	cmpxchg8b	(%rdi)" || rc=1
    ac_expect "frame and padding instructions are not writes" "1 1 0 0" \
"       0: 55                    	pushq	%rbp
       1: 48 8d 04 37           	leaq	(%rdi,%rsi), %rax
       5: f0                    	lock
       6: 0f b1 37              	cmpxchgl	%esi, (%rdi)
       9: 5d                    	popq	%rbp
       a: c3                    	retq
       b: 66 0f 1f 84 00        	nopw	%cs:(%rax,%rax)" || rc=1
    ac_expect "a stack spill is not a shared-state write" "1 1 0 0" \
"       0: 89 74 24 fc           	movl	%esi, -0x4(%rsp)
       4: 89 45 f8              	movl	%eax, -0x8(%rbp)
       7: f0                    	lock
       8: 0f b1 37              	cmpxchgl	%esi, (%rdi)" || rc=1
    ac_expect "two locked cmpxchg is counted as two" "2 2 0 0" \
"       0: f0                    	lock
       1: 0f b1 37              	cmpxchgl	%esi, (%rdi)
       8: f0                    	lock
       9: 0f b1 37              	cmpxchgl	%esi, (%rdi)" || rc=1
    ac_expect "no atomics at all" "0 0 0 0" \
"       0: 55                    	pushq	%rbp
       1: c3                    	retq" || rc=1

    # The ORDER matcher, with the rejected design as its negative control.
    ac_expect_order() {
        local desc="$1" want="$2" got
        got="$(printf '%s\n' "$3" | ac_order_lock_vs_out)"
        if [ "$got" = "$want" ]; then
            echo "ok   selftest: $desc"
        else
            echo "FAIL selftest: $desc -- expected '$want', got '$got'"
            return 1
        fi
    }
    ac_expect_order "mark BEFORE the byte (llvm split prefix)" "3 3 5" \
"       0: 55                    	pushq	%rbp
       1: f0                    	lock
       2: 0f b1 37              	cmpxchgl	%esi, (%rdi)
       9: 89 d0                 	movl	%edx, %eax
       b: ee                    	outb	%al, %dx" || rc=1
    # THE REJECTED DESIGN. Same instructions, same atomicity counts, opposite
    # order -- and an NMI in the gap erases a real timeout.
    ac_expect_order "byte BEFORE the mark is DETECTED" "3 3 1" \
"       0: ee                    	outb	%al, %dx
       1: f0                    	lock
       2: 0f b1 37              	cmpxchgl	%esi, (%rdi)" || rc=1
    # A mark on BOTH sides of the byte. The first-cmpxchg test alone passes this,
    # and the trailing mark is still a charge recorded after its byte.
    ac_expect_order "a mark AFTER the byte is DETECTED even when one precedes it" "2 5 3" \
"       0: f0                    	lock
       1: 0f b1 37              	cmpxchgl	%esi, (%rdi)
       8: ee                    	outb	%al, %dx
       9: f0                    	lock
       a: 0f b1 37              	cmpxchgl	%esi, (%rdi)" || rc=1
    ac_expect_order "GNU folded rendering is ordered too" "1 1 2" \
"  17:	f0 0f b1 37          	lock cmpxchgl %esi,(%rdi)
  1b:	ee                   	out    %al,(%dx)" || rc=1
    ac_expect_order "a missing mark is not silently ordered" "0 0 1" \
"       0: ee                    	outb	%al, %dx" || rc=1
    ac_expect_order "a missing port write is not silently ordered" "2 2 0" \
"       0: f0                    	lock
       1: 0f b1 37              	cmpxchgl	%esi, (%rdi)" || rc=1
    # A forward branch OVER the mark, landing on the byte. Accepted BY DESIGN and
    # kept as a control so the limit is visible rather than assumed: this gate is
    # positional, not a dominance analysis, and in the audited function these
    # branches are the inlined validator's early exits, on which no live unmarked
    # claim exists to record. The unit tests carry that; this carries ordering.
    ac_expect_order "a branch around the mark is NOT rejected (stated limit)" "3 3 4" \
"       0: 79 05                 	jns	0x8
       2: f0                    	lock
       3: 0f b1 37              	cmpxchgl	%esi, (%rdi)
       8: ee                    	outb	%al, %dx" || rc=1

    [ "$rc" = "0" ] && echo "atomic-claim-check: selftest passed"
    exit "$rc"
fi

OBJDUMP=""
for c in llvm-objdump-19 llvm-objdump objdump; do
    if command -v "$c" >/dev/null 2>&1; then OBJDUMP="$c"; break; fi
done
if [ -z "$OBJDUMP" ]; then
    echo "atomic-claim-check: no objdump available -- SKIP" >&2
    exit 2
fi

# REQUIRED MODE: refuse to pass on a missing or stale object.
#
# The advisory skip exists so a pre-build tooling run does not fail on an absent
# artifact. But a gate that skips is a gate that reports nothing, and the
# tooling pack runs BEFORE the clean build in CI -- so without a required mode
# invoked after the build, the shipped object would never actually be inspected
# on a clean runner. Staleness matters for the same reason: an object left over
# from an earlier edit proves the codegen of code that is no longer there.
REQUIRED=0
[ "${1:-}" = "--required" ] && { REQUIRED=1; OBJ="${2:-$REPO_ROOT/build/kernel/drivers/serial.o}"; }

# The sources whose codegen this object is supposed to represent.
SOURCES=(
    "$REPO_ROOT/src/kernel/drivers/serial.c"
    "$REPO_ROOT/include/kernel/drivers/serial_emergency.h"
)

if [ ! -f "$OBJ" ]; then
    if [ "$REQUIRED" = "1" ]; then
        echo "FAIL atomic-claim-check: $OBJ does not exist, and --required means" >&2
        echo "     the object MUST be inspected. Build before invoking this." >&2
        exit 1
    fi
    echo "atomic-claim-check: $OBJ not built -- SKIP (run scripts/build.sh first)" >&2
    exit 2
fi

for src in "${SOURCES[@]}"; do
    [ -f "$src" ] || continue
    if [ "$src" -nt "$OBJ" ]; then
        if [ "$REQUIRED" = "1" ]; then
            echo "FAIL atomic-claim-check: $(basename "$src") is newer than the object." >&2
            echo "     The disassembly would prove the codegen of code that has since" >&2
            echo "     changed. Rebuild before invoking this." >&2
            exit 1
        fi
        echo "atomic-claim-check: object older than $(basename "$src") -- SKIP" >&2
        exit 2
    fi
done

fail=0
for sym in "${AUDITED[@]}"; do
    dis="$("$OBJDUMP" -d --disassemble-symbols="$sym" "$OBJ" 2>&1)"

    if printf '%s' "$dis" | grep -q "failed to disassemble missing symbol"; then
        echo "FAIL $sym: no such symbol in $OBJ."
        echo "     The audited transition must keep external linkage -- a static"
        echo "     helper is inlined into its caller and the property becomes"
        echo "     uncheckable. Do not delete this entry to make the gate pass."
        fail=1
        continue
    fi

    read -r locks bound writes badwidth <<<"$(printf '%s\n' "$dis" | ac_count_locks)"

    if [ "$locks" -ne 1 ]; then
        echo "FAIL $sym: expected exactly 1 lock-prefixed instruction, found $locks."
        echo "     More than one means the transition is no longer atomic: an abort"
        echo "     between them leaves the resource claimed by nobody. Zero means"
        echo "     the compare-exchange was optimised out or replaced."
        printf '%s\n' "$dis" | sed 's/^/     | /'
        fail=1
        continue
    fi

    if [ "$bound" -ne 1 ]; then
        echo "FAIL $sym: its one lock prefix is not attached to a cmpxchg."
        echo "     An unconditional lock or/and/xchg cannot honour the exact-compare"
        echo "     contract: it would overwrite a claim the caller does not hold."
        printf '%s\n' "$dis" | sed 's/^/     | /'
        fail=1
        continue
    fi

    if [ "$badwidth" -ne 0 ]; then
        echo "FAIL $sym: the locked compare-exchange is not the 32-bit cmpxchgl."
        echo "     The audited words are uint32_t. A wider compare-exchange spans"
        echo "     memory the caller did not name."
        printf '%s\n' "$dis" | sed 's/^/     | /'
        fail=1
        continue
    fi

    if [ "$writes" -ne 0 ]; then
        echo "FAIL $sym: $writes memory write(s) besides the compare-exchange."
        echo "     One locked cmpxchg is NOT the property -- a plain store of the"
        echo "     owner followed by an unrelated locked compare-exchange has one"
        echo "     too, and is exactly the two-step mutation an NMI or #MC can"
        echo "     land inside. The transition must be the ONLY write."
        printf '%s\n' "$dis" | sed 's/^/     | /'
        fail=1
        continue
    fi

    echo "ok   $sym: one lock cmpxchgl, and it is the only memory write"
done

for sym in "${ORDERED[@]}"; do
    dis="$("$OBJDUMP" -d --disassemble-symbols="$sym" "$OBJ" 2>&1)"

    if printf '%s' "$dis" | grep -q "failed to disassemble missing symbol"; then
        echo "FAIL $sym: no such symbol in $OBJ."
        echo "     The ordered transition must keep external linkage -- inlined into"
        echo "     its caller there is no symbol whose instruction order can be read,"
        echo "     and the ordering is the entire property. Do not delete this entry"
        echo "     to make the gate pass."
        fail=1
        continue
    fi

    read -r cx cxlast ot <<<"$(printf '%s\n' "$dis" | ac_order_lock_vs_out)"

    if [ "$cx" -eq 0 ]; then
        echo "FAIL $sym: no locked compare-exchange in the ordered transition."
        echo "     The mark is what records the charge; without it the reservation"
        echo "     reads as merely taken and a parking CPU reclaims it."
        printf '%s\n' "$dis" | sed 's/^/     | /'
        fail=1
        continue
    fi

    if [ "$ot" -eq 0 ]; then
        echo "FAIL $sym: no port write in the ordered transition."
        echo "     This function exists to pair the mark with the byte it accounts"
        echo "     for. With the write gone, the pairing this gate checks is not"
        echo "     what the code does any more."
        printf '%s\n' "$dis" | sed 's/^/     | /'
        fail=1
        continue
    fi

    if [ "$cxlast" -gt "$ot" ]; then
        echo "FAIL $sym: the byte is written BEFORE the charge is marked."
        echo "     An NMI or #MC in that gap enters the nested async-isolation"
        echo "     panic, finds an unmarked claim, reclaims it on the way to"
        echo "     parking forever, and erases a timeout that really happened --"
        echo "     handing the next real panic an extra full-length wait on a UART"
        echo "     already known to be wedged. Mark first, then emit."
        printf '%s\n' "$dis" | sed 's/^/     | /'
        fail=1
        continue
    fi

    echo "ok   $sym: the locked cmpxchgl precedes the port write"
done

if [ "$fail" -ne 0 ]; then
    echo "atomic-claim-check: FAILED" >&2
    exit 1
fi
echo "atomic-claim-check: ${#AUDITED[@]} atomic + ${#ORDERED[@]} ordered transitions" \
     "verified in $(basename "$OBJ")"
exit 0
