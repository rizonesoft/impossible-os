/* ============================================================================
 * ex_slist.c -- Interlocked SLIST: lock-free LIFO list (TODO-06 S2).
 *
 * Lock-free push/pop/flush over a 16-byte {Next, SeqDepth} header swung
 * atomically with cmpxchg16b. The sequence counter in SeqDepth's high 48 bits
 * defeats the value-level ABA problem; the low 16 bits hold the depth, kept
 * linearizable with the pointer by living in the same DCAS-covered word.
 *
 * Storage contract is documented in ex.h: callers own SLIST_ENTRY storage and
 * must keep it mapped while the list is live (no return-to-pmm).
 * ============================================================================ */

#include "kernel/ex.h"
#include "kernel/klog.h"
#include "kernel/bugcheck.h"

/* ARCH: x86-64 -- cmpxchg16b; will move to arch/x86_64/ during the ARM port.
 *
 * Atomic 16-byte double-CAS. If the 16 bytes at `dst` equal {exp_lo, exp_hi},
 * store {des_lo, des_hi} and return true. Otherwise load the current value into
 * out_lo and out_hi and return false. `dst` MUST be 16-byte aligned.
 *
 * cmpxchg16b: compares RDX:RAX with m128; on equal sets m128 = RCX:RBX (ZF=1),
 * else loads RDX:RAX = m128 (ZF=0). RBX is callee-saved; under -fno-pie (no GOT
 * in RBX) the "b" input constraint lets the compiler save/restore it safely. */
static inline bool dcas16b(volatile void *dst,
                           uint64_t exp_lo, uint64_t exp_hi,
                           uint64_t des_lo, uint64_t des_hi,
                           uint64_t *out_lo, uint64_t *out_hi)
{
    bool ok;
    uint64_t rax = exp_lo, rdx = exp_hi;
    __asm__ __volatile__(
        "lock cmpxchg16b %[mem]"
        : "+a"(rax), "+d"(rdx),
          [mem] "+m"(*(volatile uint64_t(*)[2])dst),
          "=@ccz"(ok)
        : "b"(des_lo), "c"(des_hi)
        : "memory", "cc");
    if (!ok) { *out_lo = rax; *out_hi = rdx; }
    return ok;
}

/* SeqDepth helpers: high 48 bits = ABA sequence, low 16 bits = depth. */
#define SD_SEQ(sd)        ((sd) >> 16)
#define SD_DEPTH(sd)      ((uint16_t)((sd) & 0xFFFFu))
#define SD_MAKE(seq, dep) (((uint64_t)(seq) << 16) | ((uint16_t)(dep)))

void ExInitializeSListHead(SLIST_HEADER *head)
{
    /* cmpxchg16b #GPs on an unaligned m128 operand. The SLIST_HEADER type is
     * __attribute__((aligned(16))), but kmalloc does NOT honor type alignment
     * (the 24-byte heap block header yields 8-mod-16 payloads), so a header
     * embedded in dynamically-allocated storage can be under-aligned. Catch it
     * here with a clear invariant bugcheck instead of a cryptic #GP deep in a
     * later push. Callers using raw/kmalloc storage must 16-byte-align the head. */
    if ((uintptr_t)head & 0xFu) {
        /* LOG_ERROR, NOT LOG_FATAL: LOG_FATAL halts in an hlt loop (klog.c),
         * which would pre-empt KeBugCheckEx and lose the bugcheck code +
         * crash-dump forensics. KeBugCheckEx owns the fatal path here. */
        klog(LOG_ERROR, "ex",
             "SLIST_HEADER %p not 16-byte aligned (cmpxchg16b requires it)",
             (uint64_t)(uintptr_t)head);
        KeBugCheckEx(BUGCHECK_IOS_INVARIANT_VIOLATION,
                     (uint64_t)(uintptr_t)head, 16, 0, 0);
    }
    head->Next = (SLIST_ENTRY *)0;
    head->SeqDepth = 0;
}

void InitializeSListHead(SLIST_HEADER *head)
{
    ExInitializeSListHead(head);
}

SLIST_ENTRY *ExInterlockedPushEntrySList(SLIST_HEADER *head, SLIST_ENTRY *entry)
{
    /* Seed from a relaxed read; a torn seed just fails the first DCAS and
     * retries with the coherent value the failed DCAS reports. */
    uint64_t exp_lo = (uint64_t)__atomic_load_n(
        (uint64_t *)&head->Next, __ATOMIC_RELAXED);
    uint64_t exp_hi = __atomic_load_n(&head->SeqDepth, __ATOMIC_RELAXED);

    for (;;) {
        entry->Next = (SLIST_ENTRY *)exp_lo;       /* link new node */
        /* Saturate depth at SLIST_DEPTH_MAX so the 65536th push never wraps the
         * count to 0 (which would corrupt ExQueryDepthSList and any depth-based
         * lookaside trim logic). A list deeper than 65535 reports 65535; the
         * list itself stays valid (Next/seq are unaffected). */
        uint16_t d = SD_DEPTH(exp_hi);
        uint64_t new_hi = SD_MAKE(SD_SEQ(exp_hi) + 1,
                                  d < SLIST_DEPTH_MAX ? d + 1 : SLIST_DEPTH_MAX);
        uint64_t cur_lo, cur_hi;
        if (dcas16b(head, exp_lo, exp_hi, (uint64_t)entry, new_hi,
                    &cur_lo, &cur_hi))
            return (SLIST_ENTRY *)exp_lo;          /* previous head */
        exp_lo = cur_lo;
        exp_hi = cur_hi;
    }
}

SLIST_ENTRY *ExInterlockedPopEntrySList(SLIST_HEADER *head)
{
    uint64_t exp_lo = (uint64_t)__atomic_load_n(
        (uint64_t *)&head->Next, __ATOMIC_ACQUIRE);
    uint64_t exp_hi = __atomic_load_n(&head->SeqDepth, __ATOMIC_RELAXED);

    for (;;) {
        SLIST_ENTRY *node = (SLIST_ENTRY *)exp_lo;
        if (!node)
            return (SLIST_ENTRY *)0;               /* empty */
        /* Safe per the mapped-while-live storage contract: node stays mapped
         * even if another CPU popped it, so this read cannot fault; the ABA
         * sequence makes a stale next-value lose the DCAS. */
        uint64_t next = (uint64_t)node->Next;
        /* Floor depth at 0: it can read 0 with a non-NULL head after a
         * saturated overflow (true count exceeded 65535), so never underflow. */
        uint16_t d = SD_DEPTH(exp_hi);
        uint64_t new_hi = SD_MAKE(SD_SEQ(exp_hi) + 1, d > 0 ? d - 1 : 0);
        uint64_t cur_lo, cur_hi;
        if (dcas16b(head, exp_lo, exp_hi, next, new_hi, &cur_lo, &cur_hi))
            return node;
        exp_lo = cur_lo;
        exp_hi = cur_hi;
    }
}

SLIST_ENTRY *ExInterlockedFlushSList(SLIST_HEADER *head)
{
    uint64_t exp_lo = (uint64_t)__atomic_load_n(
        (uint64_t *)&head->Next, __ATOMIC_ACQUIRE);
    uint64_t exp_hi = __atomic_load_n(&head->SeqDepth, __ATOMIC_RELAXED);

    for (;;) {
        uint64_t new_hi = SD_MAKE(SD_SEQ(exp_hi) + 1, 0);  /* seq++, depth=0 */
        uint64_t cur_lo, cur_hi;
        if (dcas16b(head, exp_lo, exp_hi, 0 /*Next=NULL*/, new_hi,
                    &cur_lo, &cur_hi))
            return (SLIST_ENTRY *)exp_lo;          /* detached chain */
        exp_lo = cur_lo;
        exp_hi = cur_hi;
    }
}

uint16_t ExQueryDepthSList(SLIST_HEADER *head)
{
    return SD_DEPTH(__atomic_load_n(&head->SeqDepth, __ATOMIC_ACQUIRE));
}
