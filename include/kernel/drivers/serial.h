/* ============================================================================
 * serial.h -- Serial port (COM1) driver
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/spinlock.h"

/* Initialize COM1 serial port */
void serial_init(void);

/* Write a single character to serial */
void serial_putchar(char c);

/* Write a null-terminated string to serial */
void serial_write(const char *str);

/* Non-blocking read: returns character or 0 if no data available */
char serial_trygetchar(void);

/* ---------------------------------------------------------------------------
 * Emergency (abort-safe) serial output
 *
 * `serial_write`/`serial_putchar` acquire `g_serial_lock` and spin on the UART
 * transmit-holding bit without a bound. Both are fatal on a terminal path: a
 * #DF / #MC / NMI taken on a CPU already inside `serial_write` re-enters it and
 * self-deadlocks on the lock it already holds, emitting nothing -- and a wedged
 * UART hangs the panic before any evidence reaches the wire.
 *
 * The emergency path never blocks: it TRY-acquires the lock and proceeds
 * without it on failure (concurrent writers interleave bytes, and neither
 * corrupts UART state once the line-control register is re-asserted), and every
 * hardware wait is bounded. Interrupts are masked for the whole call via
 * `local_irq_save`, NOT `spin_lock_irqsave`, because `spin_trylock` raises no
 * IRQL and so the release side must lower none -- see `spin_tryunlock` in
 * spinlock.h. This is the pairing `quota_dump_crash` already uses on the panic
 * path.
 *
 * TERMINAL / FAULT CONTEXTS ONLY. Interrupts stay masked for the entire string,
 * which at the lowest configured baud (9600) is milliseconds for a register
 * dump. That is correct where the machine is already dying and wrong anywhere
 * else -- ordinary code keeps using `serial_write`.
 * ------------------------------------------------------------------------- */

/* Arm emergency mode -- one-way, never disarmed (the machine is terminal).
 * Once armed, `serial_write`/`serial_putchar` THEMSELVES route to the emergency
 * path, so downstream diagnostic emitters become safe against `g_serial_lock`
 * without each one having to opt in.
 *
 * SCOPE -- this bounds the SERIAL layer only. A caller that takes its own lock
 * ABOVE serial is unaffected: `klog_emit` acquires `s_klog_lock` before it ever
 * reaches `serial_write`, so a panic that interrupted logging still stalls in
 * klog regardless of this latch. Making the panic-path dumpers bypass klog is
 * owned by the panic-safe `dump_emit_raw` emitter in the crash-dump-generation
 * roadmap.
 *
 * ARM IT ONLY WHERE THE SYSTEM IS TERMINAL. The latch is global and never
 * cleared, so arming on a path the machine survives permanently routes all
 * later serial output through the lossy try-lock path. A CPU-local halt is NOT
 * terminal: call `serial_write_emergency` directly there instead. */
void serial_enter_emergency(void);

/* 1 once `serial_enter_emergency()` has been called on any CPU. */
int serial_in_emergency(void);

/* Bounded, non-blocking writes. Safe to call before emergency mode is armed --
 * a fault-context caller that is NOT terminal (the WER fault hook) uses these
 * directly so a stuck UART cannot stall it. */
void serial_write_emergency(const char *str);
void serial_putchar_emergency(char c);

/* Acquire/release policy for the emergency path, split out so the never-block
 * contract is unit-testable against a caller-supplied lock with no UART and no
 * boot infrastructure involved.
 *   serial_emergency_acquire() -- 1 = acquired (caller must release), 0 = the
 *                                 lock was held elsewhere, proceed unlocked.
 *   serial_emergency_release() -- releases only when `acquired` is non-zero.
 * Neither touches the interrupt flag or IRQL; that is the caller's business,
 * exactly as it is for spin_trylock/spin_tryunlock. */
int  serial_emergency_acquire(spinlock_t *lock);
void serial_emergency_release(spinlock_t *lock, int acquired);
