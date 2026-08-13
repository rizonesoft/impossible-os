/* ============================================================================
 * serial_emergency.h -- internal contract for the abort-safe serial path
 *
 * Split out of serial.h deliberately. These declarations need `spinlock_t`,
 * and serial.h is included by seventeen files -- four of them architecture-
 * neutral surfaces (klog.c, fs/partition.c, fs/gpt.c, nt/nt_syscall.c) that
 * would then transitively pull spinlock.h's inline x86-64 asm. A device-driver
 * header should not be the vector that widens the arch include closure, so the
 * lock-policy surface lives here and is included only by serial.c and its unit
 * test.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/spinlock.h"

/* Acquire/release policy for the emergency path, exposed so the never-block
 * contract is unit-testable against a caller-supplied lock with no UART and no
 * boot infrastructure involved.
 *   serial_emergency_acquire() -- 1 = acquired (caller must release), 0 = the
 *                                 lock was held elsewhere.
 *   serial_emergency_release() -- releases only when `acquired` is non-zero.
 * Neither touches the interrupt flag or IRQL; that is the caller's business,
 * exactly as it is for spin_trylock/spin_tryunlock. */
int  serial_emergency_acquire(spinlock_t *lock);
void serial_emergency_release(spinlock_t *lock, int acquired);

/* Wedged-transmitter budget accounting, exposed for unit test. The arithmetic
 * here already regressed once (a revision charged each timeout twice, halving
 * the effective budget), which is why it is testable rather than private.
 *   serial_emerg_reserve()  -- 1 = this caller may perform a full-length wait,
 *                              0 = saturated or lost the race; probe once.
 *   serial_emerg_return()   -- give back a reservation that did NOT time out.
 *   serial_emerg_waits()    -- current charge; 0 when nothing has timed out.
 *   serial_emerg_reset_for_test() -- restore the pristine state.
 * The budget is MONOTONIC in timeouts: a reservation that times out is never
 * returned, and nothing else ever lowers the count. See serial.c. */
/* Pure routing predicate: given the CPU attempting a REROUTED ordinary write
 * and the recorded arming CPU, must the write be discarded? Exposed so the
 * owner / non-owner / unknown cases are testable without arming the one-way
 * latch (which would degrade serial for the rest of the boot).
 *   `writer` or `owner` == SERIAL_EMERG_NO_OWNER means "cannot identify", and
 *   the answer is always 0 -- FAIL OPEN, preserve the evidence. */
#define SERIAL_EMERG_NO_OWNER  0xFFFFFFFFu
int serial_emerg_should_drop_for(uint32_t writer, uint32_t owner);

int      serial_emerg_reserve(void);
void     serial_emerg_return(void);
uint32_t serial_emerg_waits(void);
void     serial_emerg_reset_for_test(void);
