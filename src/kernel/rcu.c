/* ============================================================================
 * rcu.c — Read-Copy-Update (single-core simplified)
 *
 * On a single CPU, a "grace period" is trivially defined as any point at
 * which the current thread has been preempted and rescheduled.  After one
 * scheduler tick, all threads that were in an RCU read section have either:
 *   a) Finished and called rcu_read_unlock() (re-enabled preemption), or
 *   b) Are still running but blocked (sleeping, waiting on a semaphore, etc.)
 *      — blocked threads cannot be inside rcu_read_lock/rcu_read_unlock.
 *
 * Therefore synchronize_rcu() only needs to yield() once: after the yield
 * the scheduler has switched to at least one other thread context (a quiescent
 * state), which guarantees all pre-existing read sections have exited.
 *
 * This is correct because rcu_read_lock() calls scheduler_disable(), which
 * prevents preemption.  A thread in an RCU read section therefore cannot be
 * preempted — it will run to rcu_read_unlock() before the PIT can schedule
 * another thread.  Once the scheduler is re-enabled, any subsequent
 * synchronize_rcu() yield() is safe.
 * ============================================================================ */

#include "kernel/rcu.h"
#include "kernel/sched/task.h"
#include "kernel/barrier.h"

/* ============================================================================
 * Grace period
 * ============================================================================ */

/* synchronize_rcu() — wait for a full grace period.
 *
 * Single-core guarantee:
 *   After yield() returns, the RCU grace period has elapsed.  Any thread
 *   that was in an RCU read section when synchronize_rcu() was called must
 *   have already called rcu_read_unlock() (re-enabled the scheduler) before
 *   the scheduler could switch away from it — so by the time we resume after
 *   yield(), all prior readers are gone.
 *
 * Memory ordering:
 *   A full memory barrier before yield() ensures our pointer update (from
 *   rcu_assign_pointer) is globally visible before we yield.
 *   A full memory barrier after yield() ensures we do not read stale state
 *   from the old version after the grace period completes. */
void synchronize_rcu(void)
{
    mb();      /* ensure the new pointer is visible before we yield */
    yield();   /* relinquish CPU — grace period elapses during this yield */
    mb();      /* ensure we see a fresh view of memory after the yield */
}
