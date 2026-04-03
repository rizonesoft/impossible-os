/* ============================================================================
 * barrier.h -- Memory barriers and compiler fences
 *
 * Provides macros to prevent the CPU and compiler from reordering memory
 * accesses across synchronization boundaries.  All macros compile to a single
 * inline asm statement with a "memory" clobber so GCC treats every in-memory
 * value as potentially changed/read at that point.
 *
 * Quick reference
 * ---------------
 *   barrier()   -- compiler fence only; no CPU instruction emitted.
 *                 Use when: protecting volatile flag reads/writes from
 *                 compiler CSE or hoisting (e.g. spin-wait loops, seqlocks).
 *
 *   mb()        -- full memory barrier (MFENCE).
 *                 Use when: releasing a lock or publishing data that another
 *                 CPU/thread will read.  Prevents all CPU store→load and
 *                 store→store reordering across the barrier.
 *
 *   rmb()       -- read (load) barrier (LFENCE).
 *                 Use when: you've read a pointer from shared memory and are
 *                 about to dereference it -- ensures the pointer load is not
 *                 speculated ahead of dependent loads.  Rare on x86 (TSO
 *                 guarantees load ordering), but required for correctness with
 *                 non-temporal loads or after acquiring a seqlock read-side.
 *
 *   wmb()       -- write (store) barrier (SFENCE).
 *                 Use when: writing a sequence of values that must be visible
 *                 to other CPUs in order (e.g. DMA ring descriptor, NIC Tx
 *                 ring).  On x86 TSO wmb() is effectively free (all stores
 *                 are already ordered), but SFENCE is still required before
 *                 non-temporal stores (MOVNTQ / MOVNTI).
 *
 * SMP aliases
 * -----------
 *   smp_mb()  / smp_rmb() / smp_wmb() -- identical to mb/rmb/wmb today.
 *   When CONFIG_SMP is not defined these become bare compiler barriers so
 *   release builds for single-core targets pay zero CPU cost.
 *
 * x86 memory model reminder
 * -------------------------
 *   x86 uses Total Store Order (TSO): all stores are globally visible in
 *   program order, and loads observe stores in program order.  The one
 *   exception is store→load reordering (a later load can pass an earlier
 *   store).  MFENCE closes that gap.  Therefore:
 *     - wmb()  is almost free on x86 (compiler clobber + SFENCE for NT stores)
 *     - rmb()  is almost free on x86 (compiler clobber + LFENCE for rare cases)
 *     - mb()   costs ~100 cycles on modern x86 due to store-buffer drain
 *   On SMP ARM or RISC-V these costs are reversed -- every barrier matters.
 *
 * Design: header-only macros, no .c file required.
 * ============================================================================ */

#pragma once

/* ---------------------------------------------------------------------------
 * barrier() -- compiler-only fence
 *
 * Tells GCC that every in-memory value may have changed at this point.
 * No CPU instruction is emitted; prevents GCC from:
 *   - caching a memory read in a register across the barrier (CSE)
 *   - hoisting a store out of a loop past the barrier
 *   - reordering loads/stores with respect to the barrier in the IR
 *
 * Usage:
 *   while (!flag) barrier();   // prevent CSE on `flag`
 *   x = val; barrier(); ptr = &x;  // ensure x is written before ptr is set
 * ------------------------------------------------------------------------- */
#define barrier() __asm__ volatile("" ::: "memory")

/* ---------------------------------------------------------------------------
 * mb() -- full memory barrier (MFENCE)
 *
 * Serialises all loads and stores issued before the barrier with respect to
 * all loads and stores issued after.  Drains the store buffer on x86 so that
 * all prior stores are globally visible before any subsequent load is issued.
 *
 * Use for:
 *   - Releasing a spinlock / mutex (ensure critical-section stores are visible
 *     before the lock flag is cleared)
 *   - Publishing a newly allocated object (stores to object fields before the
 *     store of the pointer into a shared location)
 *   - Any inter-CPU handshake where ordering of both loads and stores matters
 * ------------------------------------------------------------------------- */
#define mb()  __asm__ volatile("mfence" ::: "memory")

/* ---------------------------------------------------------------------------
 * rmb() -- read (load) barrier (LFENCE)
 *
 * Ensures that all loads issued before the barrier complete before any load
 * issued after.  On standard x86 TSO this is already guaranteed for regular
 * loads, but LFENCE is required:
 *   - After reading a seqlock sequence counter (ensure the counter load is not
 *     reordered past the data loads by speculative execution)
 *   - After a non-temporal prefetch or streaming load (MOVNTDQA)
 *   - For Spectre-v1 mitigation (serialize speculative loads)
 * ------------------------------------------------------------------------- */
#define rmb() __asm__ volatile("lfence" ::: "memory")

/* ---------------------------------------------------------------------------
 * wmb() -- write (store) barrier (SFENCE)
 *
 * Ensures that all stores issued before the barrier are globally observable
 * before any store issued after.  On x86 TSO regular stores are already
 * ordered, so SFENCE only has an effect for non-temporal stores (MOVNTQ /
 * MOVNTI) -- e.g. framebuffer blits, DMA ring writes, NIC descriptor updates.
 *
 * Use for:
 *   - Flushing non-temporal framebuffer writes before updating the flip flag
 *   - Writing NIC/AHCI descriptor fields before updating the tail pointer
 *   - Seqlock write-side: wmb between data write and sequence counter update
 * ------------------------------------------------------------------------- */
#define wmb() __asm__ volatile("sfence" ::: "memory")

/* ---------------------------------------------------------------------------
 * SMP-aware aliases
 *
 * Currently identical to the full barrier macros.  When CONFIG_SMP is NOT
 * defined (single-core build), these collapse to bare compiler barriers so
 * a single-core release image pays zero CPU cost from hardware barriers.
 *
 * Single-core rationale: on single-core x86, the CPU executes instructions
 * for only one thread at a time -- there is no concurrent observer on another
 * core.  The compiler fence (barrier()) is still necessary to prevent GCC from
 * optimising away volatile flag reads/writes.  The MFENCE/LFENCE/SFENCE
 * instructions add latency without correctness benefit on uniprocessor.
 * ------------------------------------------------------------------------- */
#ifdef CONFIG_SMP
#  define smp_mb()  mb()
#  define smp_rmb() rmb()
#  define smp_wmb() wmb()
#else
#  define smp_mb()  barrier()
#  define smp_rmb() barrier()
#  define smp_wmb() barrier()
#endif

