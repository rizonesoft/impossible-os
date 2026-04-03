/* ============================================================================
 * seqlock.h -- Sequence lock (seqlock) for read-mostly shared data
 *
 * Seqlocks provide extremely fast reads with zero locking overhead.  The
 * sequence counter protocol:
 *
 *   Writers:
 *     seqlock_write_lock(sl)   -- acquire spinlock, increment counter (→ odd)
 *     < modify shared data >
 *     seqlock_write_unlock(sl) -- increment counter again (→ even), release lock
 *
 *   Readers (lock-free, retry on conflict):
 *     do {
 *         seq = seqlock_read_begin(sl);   // spin until even (no writer)
 *         < read shared data >
 *     } while (seqlock_read_retry(sl, seq));  // retry if writer intervened
 *
 * Properties:
 *   - Reads never block writers and take no lock at all.
 *   - Writes are serialized by the internal spinlock (only one writer at a time).
 *   - An odd sequence counter means a write is in progress -- readers spin and
 *     retry rather than reading partially-updated data.
 *   - A changed (even) counter means a write happened mid-read -- readers retry.
 *   - Read retries are extremely rare for slowly-changing data (clock, uptime).
 *
 * Equivalent to Linux seqlock_t / seqcount_t.
 * Windows has no direct equivalent (uses ERESOURCE for read-mostly locks).
 *
 * When to use:
 *   ✓ System uptime counter (updated every tick, read by every process)
 *   ✓ jiffies / PIT tick counter
 *   ✓ Cached RTC time (updated once per second)
 *   ✓ Network statistics counters
 *   ✗ Data containing pointers (readers may see torn pointer during write)
 *   ✗ Very large structures (long reader critical section → many retries)
 *
 * IRQ safety:
 *   seqlock_write_lock uses spin_lock_irqsave -- safe from any context.
 *   seqlock_read_begin / seqlock_read_retry are reader-side and take no lock --
 *   safe to call from IRQ context.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/barrier.h"
#include "kernel/sched/spinlock.h"

/* ---------------------------------------------------------------------------
 * seqlock_t -- spinlock + sequence counter.
 * seq is EVEN when no write is in progress, ODD during a write.
 * ------------------------------------------------------------------------- */
typedef struct {
    spinlock_t        lock;      /* serializes concurrent writers */
    volatile uint64_t seq;       /* monotonically incrementing counter */
    uint64_t          irq_flags; /* saved RFLAGS from seqlock_write_lock */
} seqlock_t;

#define SEQLOCK_INIT  { .lock = SPINLOCK_INIT, .seq = 0 }

/* Static initializer */
#define DEFINE_SEQLOCK(name)  seqlock_t name = SEQLOCK_INIT

/* ---------------------------------------------------------------------------
 * seqlock_init(sl) -- runtime initializer (use SEQLOCK_INIT for statics).
 * ------------------------------------------------------------------------- */
void seqlock_init(seqlock_t *sl);

/* ---------------------------------------------------------------------------
 * Writer API -- serialize with spinlock, bracket data update with seq inc.
 *
 * seqlock_write_lock(sl)   -- acquire write-side spinlock, increment seq → odd
 * seqlock_write_unlock(sl) -- increment seq → even (signals write complete),
 *                            release spinlock
 *
 * IRQ safe: seqlock_write_lock saves/restores RFLAGS.
 * The saved flags are stored inside seqlock_t so they survive the call.
 * ------------------------------------------------------------------------- */
void seqlock_write_lock(seqlock_t *sl);
void seqlock_write_unlock(seqlock_t *sl);

/* ---------------------------------------------------------------------------
 * Reader API -- lock-free retry loop.
 *
 * seqlock_read_begin(sl)
 *   Returns the current sequence number.  Spins if a write is in progress
 *   (odd seq) so the caller does not read partially-updated data.
 *   Inserts an acquire memory barrier after reading seq so the compiler
 *   cannot hoist data reads before the seq read.
 *
 * seqlock_read_retry(sl, seq)
 *   Returns 1 (true) if the sequence counter changed since seq was sampled --
 *   meaning a write occurred during the read and the data may be inconsistent.
 *   The reader MUST retry the entire read if this returns true.
 *   Inserts a read memory barrier before comparing so all data reads
 *   complete before the final seq is sampled.
 *
 * Usage:
 *   uint64_t seq;
 *   uint64_t uptime;
 *   do {
 *       seq    = seqlock_read_begin(&sys_seqlock);
 *       uptime = g_uptime_ticks;         // read shared data
 *   } while (seqlock_read_retry(&sys_seqlock, seq));
 * ------------------------------------------------------------------------- */
uint64_t seqlock_read_begin(const seqlock_t *sl);
int      seqlock_read_retry(const seqlock_t *sl, uint64_t seq);
