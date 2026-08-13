/* ============================================================================
 * serial.h -- Serial port (COM1) driver
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

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
 * The emergency path never blocks: it TRY-acquires the lock and, for a DIRECT
 * caller, proceeds without it on failure (concurrent writers interleave bytes,
 * and neither corrupts UART state once the line-control register is
 * re-asserted). Every hardware wait is bounded. Interrupts are masked via
 * `local_irq_save`, NOT `spin_lock_irqsave`, because `spin_trylock` raises no
 * IRQL and so the release side must lower none -- see `spin_tryunlock` in
 * spinlock.h. This is the pairing `quota_dump_crash` already uses on the panic
 * path.
 *
 * Masking is per EMITTED CHUNK, not per call: a long string is emitted as
 * successive bounded chunks, so interrupts are masked for at most one chunk at
 * a time rather than for the whole record.
 *
 * TERMINAL / FAULT CONTEXTS ONLY -- ordinary code keeps using `serial_write`.
 * ------------------------------------------------------------------------- */

/* Arm emergency mode -- one-way, never disarmed (the machine is terminal).
 * Once armed, `serial_write`/`serial_putchar` THEMSELVES route to the emergency
 * path, so downstream diagnostic emitters become safe against `g_serial_lock`
 * without each one having to opt in.
 *
 * A REROUTED caller is not the same as a direct one: where a direct
 * `serial_write_emergency` proceeds unlocked on a failed try-lock, a rerouted
 * ordinary write DROPS its output instead. Otherwise any CPU still running
 * user code -- the stdout syscalls reach `serial_putchar` with user-controlled
 * bytes, and the panic path does not quiesce other CPUs -- would interleave
 * arbitrary text into the crash record it was armed to protect.
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
 * directly so a stuck UART cannot stall it. Such a RECOVERABLE caller gets a
 * much shorter per-byte wait and its own per-call budget, so it can never spend
 * the terminal allowance nor mask interrupts for long on a surviving system. */
void serial_write_emergency(const char *str);
void serial_putchar_emergency(char c);

/* PANIC CONTEXT -- which abort handler this output is being emitted from.
 *
 * It exists for exactly one decision: whether the caller-string walk may use the
 * fault-suppressed read (`__kread_u8`, cpu_security.h) to survive a corrupt
 * pointer. That read recovers by taking a real #PF and returning through IRETQ.
 *
 *   PANIC_CTX_NORMAL -- ordinary panic, #DF or #MC. The guarded read is used. A
 *     #PF taken by a handler that is ALREADY RUNNING is delivered normally
 *     (shutdown requires a fault while DELIVERING #DF), and the fixup is matched
 *     ahead of the pager so it cannot enter blocking I/O.
 *   PANIC_CTX_NMI -- the walk falls back to a plain load. The fixup's IRETQ
 *     would re-arm NMI delivery while the outer NMI still owns IST2, letting a
 *     second NMI reuse that stack and overwrite the frames -- worse than the
 *     unguarded load, whose fault goes terminal and never returns into the NMI
 *     handler. Until nested-NMI latch/replay exists (T10 section 2), NMI output
 *     keeps the bounded-walk-only guarantee.
 *   PANIC_CTX_UNKNOWN -- the caller cannot prove its vector, so it gets the
 *     restrictive behavior. This is the REROUTED ordinary path: once the latch
 *     is armed, serial_write / serial_putchar funnel indirect emitters (the klog
 *     sink, subsystem dumps) in here, and none of them knows which vector is
 *     being handled above them. Treating unknown as permissive would silently
 *     hand the NMI hazard to every indirect emitter; treating it as restrictive
 *     leaves those callers exactly as they behave today.
 *
 * The test is `ctx == PANIC_CTX_NORMAL`, not `ctx == PANIC_CTX_NMI`, so the
 * guarded read is opt-IN: an unrecognized value degrades to the safe behavior
 * rather than the dangerous one.
 *
 * DECLARED by the caller, never probed: the vector is a hardware fact known at
 * panic entry (`frame->int_no`), whereas any state the writer could consult is
 * exactly the state a panic may have corrupted. */
#define PANIC_CTX_NORMAL   0u
#define PANIC_CTX_NMI      1u
#define PANIC_CTX_UNKNOWN  2u

void serial_write_emergency_ctx(const char *str, uint32_t ctx);

/* Same bounded, non-blocking writer, but for a caller the system SURVIVES (the
 * WER user-fault report). Gets a much shorter per-byte wait and a per-call
 * budget, so it can neither spend the terminal allowance nor mask interrupts
 * for as long as a dying machine may. */
void serial_write_recoverable(const char *str);
