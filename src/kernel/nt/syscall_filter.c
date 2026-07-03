/* ============================================================================
 * syscall_filter.c -- Per-process SSDT syscall filtering
 *
 * Implements the immutable-snapshot filter model declared in syscall_filter.h:
 * a NULL filter allows everything; an installed filter is an allow bitmap over
 * SSDT indices. Snapshots are never mutated in place -- tightening publishes a
 * fresh snapshot (release store) and retires the old one on a per-filter chain
 * freed at task teardown, so a concurrent dispatch reader on another CPU can
 * never dereference freed memory. See the header for the full lifetime rationale.
 * ============================================================================ */

#include "kernel/nt/syscall_filter.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/zw.h"                /* ssdt_previous_mode, SSDT_KERNEL_MODE */
#include "kernel/nt/service_numbers.h"   /* SSDT_NtFsControlFile */
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/memops.h"
#include "kernel/klog.h"

/* Number of tasks currently contributing to the filter fast-path gate. Zero
 * lets the dispatch fast path skip the check with a single atomic load (see
 * syscall_filter_active). Accessed only via __atomic_* -- no `volatile`. */
uint32_t g_syscall_filter_count = 0;

/* Audit-mode log throttle. A process can self-install an AUDIT filter on a hot
 * syscall and loop it; without throttling, each would-block would enter the
 * global klog lock path on the dispatch hot path. The lockless atomic bump
 * gates the klog to at most 1 in AUDIT_LOG_INTERVAL, keeping the read path a
 * cheap counter test for the vast majority of looped attempts. */
#define SYSCALL_FILTER_AUDIT_LOG_INTERVAL 1024u
static uint64_t s_audit_log_seq;

/* Serializes filter install/retire/teardown (the writer side). Readers use a
 * lockless __atomic acquire load of task->syscall_filter -- the immutable
 * snapshot + retire-until-teardown discipline is what makes that safe. */
static DEFINE_SPINLOCK(s_filter_lock);

/* ---- Dispatch-time check ------------------------------------------------- */

int syscall_filter_index_allowed(const SYSCALL_FILTER *f, uint32_t table_id,
                                 uint32_t index)
{
    const uint64_t *bitmap;
    uint32_t words;
    uint32_t w, b;

    if (!f)
        return 1;   /* NULL filter allows everything */

    if (table_id == SSDT_TABLE_MAIN) {
        bitmap = f->allow_main;
        words  = SYSCALL_FILTER_MAIN_WORDS;
    } else if (table_id == SSDT_TABLE_SHADOW) {
        bitmap = f->allow_shadow;
        words  = SYSCALL_FILTER_SHADOW_WORDS;
    } else {
        /* Unknown table: the dispatcher's own table switch rejects it. */
        return 1;
    }

    w = index >> 6;
    b = index & 63;
    if (w >= words)
        return 1;   /* out of range: dispatch bounds-check owns it */

    return (bitmap[w] & (1ULL << b)) ? 1 : 0;
}

NTSTATUS syscall_filter_check(uint32_t table_id, uint32_t index,
                              uint32_t service_number)
{
    struct task *t;
    SYSCALL_FILTER *f;

    /* Internal kernel (Zw) callers are trusted and never sandboxed. Checking
     * previous mode also avoids denying early-boot kernel syscalls. */
    if (ssdt_previous_mode() == SSDT_KERNEL_MODE)
        return STATUS_SUCCESS;

    t = task_current();
    if (!t)
        return STATUS_SUCCESS;

    /* Acquire-load pairs with the release store in the install path so we see
     * a fully-initialized snapshot. */
    f = __atomic_load_n(&t->syscall_filter, __ATOMIC_ACQUIRE);
    if (!f)
        return STATUS_SUCCESS;

    if (syscall_filter_index_allowed(f, table_id, index))
        return STATUS_SUCCESS;

    /* Blocked. Audit mode logs (throttled) and allows; otherwise deny. The
     * atomic bump is lockless; only 1 in AUDIT_LOG_INTERVAL enters klog, so a
     * looped audited syscall cannot flood the global logging/lock path. */
    if (f->flags & SYSCALL_FILTER_AUDIT) {
        uint64_t seq = __atomic_add_fetch(&s_audit_log_seq, 1, __ATOMIC_RELAXED);
        if ((seq & (SYSCALL_FILTER_AUDIT_LOG_INTERVAL - 1)) == 0)
            klog(LOG_WARN, "sfilter",
                 "audit: pid=%u would-block svc=0x%04X (table=%u idx=0x%03X) [sampled 1/%u]",
                 (uint64_t)t->pid, (uint64_t)service_number,
                 (uint64_t)table_id, (uint64_t)index,
                 (uint64_t)SYSCALL_FILTER_AUDIT_LOG_INTERVAL);
        return STATUS_SUCCESS;
    }
    return STATUS_ACCESS_DENIED;
}

/* ---- Install / tighten --------------------------------------------------- */

/* True if `sub` allows no index that `sup` blocks (sub is a subset of sup). */
static int filter_is_subset(const SYSCALL_FILTER *sub, const SYSCALL_FILTER *sup)
{
    uint32_t i;
    for (i = 0; i < SYSCALL_FILTER_MAIN_WORDS; i++)
        if (sub->allow_main[i] & ~sup->allow_main[i])
            return 0;
    for (i = 0; i < SYSCALL_FILTER_SHADOW_WORDS; i++)
        if (sub->allow_shadow[i] & ~sup->allow_shadow[i])
            return 0;
    return 1;
}

static uint32_t filter_chain_len(const SYSCALL_FILTER *f)
{
    uint32_t n = 0;
    while (f) {
        n++;
        f = f->retired_prev;
    }
    return n;
}

NTSTATUS syscall_filter_set_policy(struct task *target,
                                   const PROCESS_SYSCALL_FILTER_POLICY *pol)
{
    SYSCALL_FILTER *nf;
    SYSCALL_FILTER *old;
    uint64_t irq;
    uint32_t i;

    if (!target || !pol)
        return STATUS_INVALID_PARAMETER;
    if (pol->flags & ~SYSCALL_FILTER_FLAGS_MASK)
        return STATUS_INVALID_PARAMETER;
    if (pol->operation != SYSCALL_FILTER_OP_DISALLOW_WIN32K &&
        pol->operation != SYSCALL_FILTER_OP_DISALLOW_FSCTL &&
        pol->operation != SYSCALL_FILTER_OP_CUSTOM_BITMAP)
        return STATUS_INVALID_PARAMETER;

    /* Allocate the snapshot outside the lock (kmalloc may take internal locks;
     * ~264 bytes, well under the 4 KB kmalloc ceiling). */
    nf = (SYSCALL_FILTER *)kmalloc(sizeof(*nf));
    if (!nf)
        return STATUS_INSUFFICIENT_RESOURCES;

    spin_lock_irqsave(&s_filter_lock, &irq);

    old = target->syscall_filter;   /* writer side: plain read under the lock */

    if (old && filter_chain_len(old) >= SYSCALL_FILTER_MAX_GENERATIONS) {
        spin_unlock_irqrestore(&s_filter_lock, irq);
        kfree(nf);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Base bitmaps: start from the existing filter (tighten) or all-allowed. */
    if (old) {
        for (i = 0; i < SYSCALL_FILTER_MAIN_WORDS; i++)
            nf->allow_main[i] = old->allow_main[i];
        for (i = 0; i < SYSCALL_FILTER_SHADOW_WORDS; i++)
            nf->allow_shadow[i] = old->allow_shadow[i];
    } else {
        for (i = 0; i < SYSCALL_FILTER_MAIN_WORDS; i++)
            nf->allow_main[i] = ~0ULL;
        for (i = 0; i < SYSCALL_FILTER_SHADOW_WORDS; i++)
            nf->allow_shadow[i] = ~0ULL;
    }

    switch (pol->operation) {
    case SYSCALL_FILTER_OP_DISALLOW_WIN32K:
        /* Block the entire shadow (Win32k) table. */
        for (i = 0; i < SYSCALL_FILTER_SHADOW_WORDS; i++)
            nf->allow_shadow[i] = 0;
        break;
    case SYSCALL_FILTER_OP_DISALLOW_FSCTL: {
        /* Coarse block of NtFsControlFile. The Windows "except pipe FSCTL
         * codes" carve-out is argument-level (the FSCTL code is a syscall
         * arg) and belongs in the NtFsControlFile handler, not this index
         * bitmap; tracked as the arg-inspection follow-up item. */
        uint32_t idx = SSDT_NtFsControlFile & SSDT_INDEX_MASK;
        uint32_t w = idx >> 6, b = idx & 63;
        if (w < SYSCALL_FILTER_MAIN_WORDS)
            nf->allow_main[w] &= ~(1ULL << b);
        break;
    }
    case SYSCALL_FILTER_OP_CUSTOM_BITMAP:
        /* Replace with the caller bitmaps. When LOCKED is already in force the
         * subset check below rejects any relaxation; unlocked callers may set
         * an arbitrary (even looser) map per the section contract. */
        for (i = 0; i < SYSCALL_FILTER_MAIN_WORDS; i++)
            nf->allow_main[i] = pol->allow_main[i];
        for (i = 0; i < SYSCALL_FILTER_SHADOW_WORDS; i++)
            nf->allow_shadow[i] = pol->allow_shadow[i];
        break;
    }

    /* Flags: apply the caller's requested flags but never clear an existing
     * LOCKED bit (locked is irreversible). */
    nf->flags = pol->flags;
    if (old)
        nf->flags |= (old->flags & SYSCALL_FILTER_LOCKED);

    /* LOCKED enforcement: the new snapshot must be a subset of the old one
     * (tighten-only) and may not change flags (no relaxing audit-vs-deny). */
    if (old && (old->flags & SYSCALL_FILTER_LOCKED)) {
        if (!filter_is_subset(nf, old) || nf->flags != old->flags) {
            spin_unlock_irqrestore(&s_filter_lock, irq);
            kfree(nf);
            return STATUS_ACCESS_DENIED;
        }
    }

    nf->retired_prev = old;   /* chain the superseded snapshot (freed at teardown) */

    /* Release store publishes a fully-initialized snapshot to lockless readers. */
    __atomic_store_n(&target->syscall_filter, nf, __ATOMIC_RELEASE);

    /* Count this task once, on first install. syscall_filter_counted lets the
     * count drop at TASK_DEAD while the memory free waits for the reap barrier,
     * without double-counting. RELEASE pairs with the ACQUIRE gate load. */
    if (!target->syscall_filter_counted) {
        target->syscall_filter_counted = 1;
        __atomic_add_fetch(&g_syscall_filter_count, 1, __ATOMIC_RELEASE);
    }

    spin_unlock_irqrestore(&s_filter_lock, irq);
    return STATUS_SUCCESS;
}

/* ---- Inheritance --------------------------------------------------------- */

SYSCALL_FILTER *syscall_filter_clone(const SYSCALL_FILTER *src)
{
    SYSCALL_FILTER *c;
    uint32_t i;

    if (!src)
        return (SYSCALL_FILTER *)0;

    c = (SYSCALL_FILTER *)kmalloc(sizeof(*c));
    if (!c)
        return (SYSCALL_FILTER *)0;

    for (i = 0; i < SYSCALL_FILTER_MAIN_WORDS; i++)
        c->allow_main[i] = src->allow_main[i];
    for (i = 0; i < SYSCALL_FILTER_SHADOW_WORDS; i++)
        c->allow_shadow[i] = src->allow_shadow[i];
    c->flags = src->flags;
    c->retired_prev = (SYSCALL_FILTER *)0;   /* fresh single-generation snapshot */
    return c;
}

void syscall_filter_attach(struct task *child, SYSCALL_FILTER *clone)
{
    uint64_t irq;

    if (!child || !clone)
        return;

    /* The child is not yet runnable; the lock only orders the count update
     * against concurrent installs on other tasks. A fresh child is never
     * counted yet, but guard on the flag anyway so the accounting stays
     * single-source-of-truth. */
    spin_lock_irqsave(&s_filter_lock, &irq);
    clone->retired_prev = (SYSCALL_FILTER *)0;
    __atomic_store_n(&child->syscall_filter, clone, __ATOMIC_RELEASE);
    if (!child->syscall_filter_counted) {
        child->syscall_filter_counted = 1;
        __atomic_add_fetch(&g_syscall_filter_count, 1, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&s_filter_lock, irq);
}

/* ---- Death / teardown ---------------------------------------------------- */

void syscall_filter_task_dead(struct task *t)
{
    uint64_t irq;

    if (!t)
        return;

    /* Drop the count contribution now (a DEAD task never dispatches again, so
     * it should stop taxing the global fast path) but leave the snapshot memory
     * intact -- freeing it here would run before sibling threads are proven
     * off-CPU on SMP; the free waits for the reap barrier in
     * syscall_filter_task_teardown. Idempotent via syscall_filter_counted. */
    spin_lock_irqsave(&s_filter_lock, &irq);
    if (t->syscall_filter_counted) {
        t->syscall_filter_counted = 0;
        __atomic_sub_fetch(&g_syscall_filter_count, 1, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&s_filter_lock, irq);
}

void syscall_filter_task_teardown(struct task *t)
{
    SYSCALL_FILTER *f;
    uint64_t irq;

    if (!t)
        return;

    spin_lock_irqsave(&s_filter_lock, &irq);
    /* Drop the count if TASK_DEAD did not already (a task torn down without
     * passing through the DEAD transition -- boot init-loop slots, or a future
     * reap path). */
    if (t->syscall_filter_counted) {
        t->syscall_filter_counted = 0;
        __atomic_sub_fetch(&g_syscall_filter_count, 1, __ATOMIC_RELEASE);
    }
    f = t->syscall_filter;
    t->syscall_filter = (SYSCALL_FILTER *)0;
    spin_unlock_irqrestore(&s_filter_lock, irq);

    /* Free the live snapshot + its whole retire chain. Safe outside the lock:
     * this runs at the reap barrier (task_cleanup, TASK_DEAD, every thread
     * off-CPU), so no thread of the task can be mid-dispatch. */
    while (f) {
        SYSCALL_FILTER *prev = f->retired_prev;
        kfree(f);
        f = prev;
    }
}
