/* ============================================================================
 * rcu.h -- Read-Copy-Update (single-core simplified)
 *
 * RCU gives readers zero-cost, lock-free access to shared data.  Writers
 * atomically publish a new version by updating a pointer, then wait for all
 * currently-executing readers to finish (the "grace period") before freeing
 * the old version.
 *
 * Single-core simplification
 * --------------------------
 * On a single CPU, an RCU read-side critical section simply disables
 * preemption.  A context switch is a quiescent state -- after every READY
 * thread has been scheduled at least once, all old readers have either
 * completed or are blocked (not in an RCU read section).
 *
 * Therefore:
 *   rcu_read_lock()    → scheduler_disable()   (prevent preemption)
 *   rcu_read_unlock()  → scheduler_enable()    (allow preemption again)
 *   synchronize_rcu()  → yield() until the current thread has been
 *                        rescheduled at least once (one full quiescent state)
 *
 * This is equivalent to Linux's CONFIG_PREEMPT_NONE RCU on UP kernels.
 *
 * Writer protocol
 * ---------------
 *   1. Allocate and initialise new_node.
 *   2. rcu_assign_pointer(ptr, new_node)  -- wmb() + store.
 *      All readers from this point see new_node.
 *   3. synchronize_rcu()                  -- wait for all old readers.
 *   4. Free old_node.
 *
 * Reader protocol
 * ---------------
 *   rcu_read_lock();
 *   node = rcu_dereference(ptr);  // read barrier + load
 *   // use node -- guaranteed stable while in RCU read section
 *   rcu_read_unlock();
 *   // do NOT dereference node after rcu_read_unlock()
 *
 * Pointer helpers
 * ---------------
 *   rcu_assign_pointer(ptr, val) -- wmb() before pointer store so all
 *       initialisation writes to *val are visible to readers before the
 *       pointer itself becomes visible.
 *   rcu_dereference(ptr) -- rmb()-class barrier after pointer load so
 *       subsequent reads of *ptr are not hoisted before the pointer load.
 *       On x86 TSO this is a compiler barrier only.
 *
 * Constraints / gotchas
 * ---------------------
 *   - NEVER sleep or block inside rcu_read_lock/rcu_read_unlock.
 *   - NEVER call synchronize_rcu() while holding an RCU read lock
 *     (deadlock on single-core: synchronize_rcu yields; scheduler is
 *     disabled by rcu_read_lock so the yield is a no-op).
 *   - NEVER dereference an RCU-protected pointer outside a read section.
 *   - Free old pointers only AFTER synchronize_rcu() returns.
 *   - rcu_assign_pointer/rcu_dereference are macros that work on any
 *     pointer type -- no void* cast required.
 *
 * SMP future
 * ----------
 *   Full SMP RCU requires quiescent-state tracking per CPU and a grace-period
 *   kthread.  This single-core version is intentionally trivial -- the SMP
 *   upgrade path replaces scheduler_disable/enable with per-CPU preempt
 *   counters and adds a real grace-period mechanism.
 * ============================================================================ */

#pragma once

#include "kernel/barrier.h"
#include "kernel/sched/task.h"

/* ============================================================================
 * Read-side critical section
 * ============================================================================ */

/* rcu_read_lock() -- enter an RCU read-side critical section.
 * Disables preemption so the current thread cannot be scheduled out while
 * it holds an RCU reference.  On single-core, this guarantees no other
 * thread can run and no grace period can complete during our read. */
static inline void rcu_read_lock(void)
{
    scheduler_disable();
}

/* rcu_read_unlock() -- exit an RCU read-side critical section.
 * Re-enables preemption.  The reader MUST NOT access any RCU-protected
 * pointer after this call. */
static inline void rcu_read_unlock(void)
{
    scheduler_enable();
}

/* ============================================================================
 * Grace period
 * ============================================================================ */

/* synchronize_rcu() -- wait for all pre-existing RCU read sections to complete.
 * On single-core, a yield() is sufficient: any thread that was in a read
 * section when we called synchronize_rcu() will have either finished or
 * been blocked (not in a read section) by the time we are rescheduled.
 * Writers call this after publishing a new pointer, before freeing the old. */
void synchronize_rcu(void);

/* ============================================================================
 * Pointer publish / dereference helpers
 * ============================================================================ */

/* rcu_assign_pointer(ptr, val) -- publish a new RCU-protected pointer.
 * Inserts a write memory barrier (wmb) BEFORE the store so that all
 * initialisation of *val is visible to readers before they can load ptr.
 * Use only in write-side code (outside read sections). */
#define rcu_assign_pointer(ptr, val)    \
    do {                                \
        wmb();                          \
        (ptr) = (val);                  \
    } while (0)

/* rcu_dereference(ptr) -- safely load an RCU-protected pointer inside a
 * read-side critical section.  Inserts a compiler barrier after the load
 * (on x86 TSO, load-load reordering cannot happen; the barrier prevents
 * the compiler from hoisting dependent reads above the pointer load).
 * Returns the loaded pointer -- assign to a local variable and use within
 * the same read section only. */
#define rcu_dereference(ptr)            \
    ({                                  \
        __typeof__(ptr) _p = (ptr);     \
        barrier();                      \
        _p;                             \
    })
