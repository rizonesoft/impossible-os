/* ============================================================================
 * atomic.h -- Atomic operation primitives
 *
 * Wraps GCC __atomic_* builtins in thin kernel-friendly inline functions.
 * All operations carry the minimum necessary memory ordering:
 *   - Loads:          __ATOMIC_ACQUIRE  (prevent hoisting past the load)
 *   - Stores:         __ATOMIC_RELEASE  (prevent sinking past the store)
 *   - RMW (CAS, add): __ATOMIC_ACQ_REL (full ordering on the operation)
 *
 * barrier.h is included so callers can pair atomic ops with barrier() / mb()
 * for stronger guarantees when needed (seqlock write path, RCU pointer publish).
 *
 * Used by: rwlock_t (reader_count, writer_held, writer_pending), mutex_t
 * (locked flag), kernel object reference counts, ticket locks, and
 * lock-free ring buffers.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/barrier.h"   /* barrier(), mb(), smp_mb() */

/* ---------------------------------------------------------------------------
 * atomic_t -- 32-bit atomic integer
 * Use atomic64_t for 64-bit values (e.g. tick counters, byte offsets).
 * ------------------------------------------------------------------------- */
typedef struct { volatile int32_t val; } atomic_t;
typedef struct { volatile int64_t val; } atomic64_t;

#define ATOMIC_INIT(v)   { .val = (v) }
#define ATOMIC64_INIT(v) { .val = (v) }

/* ---------------------------------------------------------------------------
 * atomic_read / atomic_set
 *
 * acquire-ordered read and release-ordered write.  The memory ordering
 * ensures that any loads issued after atomic_read() observe the full
 * critical section that was released by atomic_set() on another CPU.
 * ------------------------------------------------------------------------- */
static inline int32_t atomic_read(const atomic_t *a) {
    return __atomic_load_n(&a->val, __ATOMIC_ACQUIRE);
}

static inline void atomic_set(atomic_t *a, int32_t v) {
    __atomic_store_n(&a->val, v, __ATOMIC_RELEASE);
}

static inline int64_t atomic64_read(const atomic64_t *a) {
    return __atomic_load_n(&a->val, __ATOMIC_ACQUIRE);
}

static inline void atomic64_set(atomic64_t *a, int64_t v) {
    __atomic_store_n(&a->val, v, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------------------
 * atomic_inc / atomic_dec / atomic_dec_and_test
 *
 * Used for reference counting.  atomic_dec_and_test() returns 1 (true) when
 * the counter reaches zero -- the caller is responsible for freeing the object.
 * Uses __ATOMIC_ACQ_REL so the decrement is visible before any subsequent
 * free().
 * ------------------------------------------------------------------------- */
static inline void atomic_inc(atomic_t *a) {
    __atomic_fetch_add(&a->val, 1, __ATOMIC_ACQ_REL);
}

static inline void atomic_dec(atomic_t *a) {
    __atomic_fetch_sub(&a->val, 1, __ATOMIC_ACQ_REL);
}

static inline int atomic_dec_and_test(atomic_t *a) {
    return __atomic_sub_fetch(&a->val, 1, __ATOMIC_ACQ_REL) == 0;
}

/* ---------------------------------------------------------------------------
 * atomic_cmpxchg -- Compare-and-swap (CAS)
 *
 * Atomically: if *a == old, write new and return old; otherwise return the
 * current value. This is the primitive used by spinlocks and
 * lock-free ring buffers.
 *
 * After a successful CAS that acquires a lock, a barrier() is sufficient
 * on x86 (the LOCK CMPXCHG instruction carries an implicit hardware barrier).
 * On SMP non-x86 targets, use smp_mb() on the acquire path.
 * ------------------------------------------------------------------------- */
static inline int32_t atomic_cmpxchg(atomic_t *a, int32_t old_val, int32_t new_val) {
    __atomic_compare_exchange_n(&a->val, &old_val, new_val,
                                /*weak=*/0,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return old_val;
}

/* ---------------------------------------------------------------------------
 * atomic_fetch_add -- Atomic add, returns old value
 *
 * Used by ticket locks: each waiter atomically fetches-and-increments
 * `next_ticket` to obtain its unique ticket number.
 * ------------------------------------------------------------------------- */
static inline int32_t atomic_fetch_add(atomic_t *a, int32_t delta) {
    return __atomic_fetch_add(&a->val, delta, __ATOMIC_ACQ_REL);
}

static inline int64_t atomic64_fetch_add(atomic64_t *a, int64_t delta) {
    return __atomic_fetch_add(&a->val, delta, __ATOMIC_ACQ_REL);
}
