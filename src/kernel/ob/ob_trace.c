/* ============================================================================
 * ob_trace.c -- Tagged reference tracing and handle leak detection
 *
 * Per-object circular trace log records tag + delta + caller + timestamp.
 * On refcount 0, ob_dump_trace emits per-tag summaries highlighting leaks.
 * Handle event tracing emits klog entries for every handle alloc/free.
 *
 * XREF: 02-kernel-core/TODO-05-object-manager.md S15
 * ============================================================================ */

#include "kernel/ob/ob_trace.h"
#include "kernel/ob/ob.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"
#include "kernel/timer.h"

extern void *memset(void *s, int c, size_t n);

/* ---- Global handle event tracing flag ----------------------------------- */

int g_ob_handle_trace = 0;

/* ---- Per-type tracing control ------------------------------------------- */

void ob_enable_type_tracing(const OBJECT_TYPE *type)
{
    if (type)
        ((OBJECT_TYPE *)type)->tracing_enabled = 1;
}

void ob_disable_type_tracing(const OBJECT_TYPE *type)
{
    if (type)
        ((OBJECT_TYPE *)type)->tracing_enabled = 0;
}

int ob_type_tracing_enabled(const OBJECT_TYPE *type)
{
    return type ? type->tracing_enabled : 0;
}

/* ---- Trace log allocation ----------------------------------------------- */

OB_TRACE_INFO *ob_trace_alloc(void)
{
    OB_TRACE_INFO *ti = (OB_TRACE_INFO *)kmalloc(sizeof(OB_TRACE_INFO));
    if (ti)
        memset(ti, 0, sizeof(OB_TRACE_INFO));
    return ti;
}

/* ---- Record a trace entry ----------------------------------------------- */

static void trace_record(OB_TRACE_INFO *ti, uint32_t tag, int8_t delta,
                         uintptr_t caller)
{
    if (!ti) return;

    /* Capture the timestamp BEFORE the trace lock: system_get_ticks() may take
     * the timer backend's own lock (e.g. PIT), and nesting that under ti->lock
     * would impose a lock order. Tracing is debug-only (off by default), so the
     * observer-effect of the timestamp on the traced ref/deref path is accepted. */
    uint64_t ts = system_get_ticks();
    uint64_t irq_flags;

    /* Serialize the ring against concurrent tagged ref/deref on the SAME object
     * from other CPUs -- OBJECT_HEADER.trace is shared by every holder. */
    spin_lock_irqsave(&ti->lock, &irq_flags);
    uint64_t idx = ti->head % OB_TRACE_RING_SIZE;
    ti->entries[idx].tag       = tag;
    ti->entries[idx].delta     = delta;
    ti->entries[idx].caller    = caller;
    ti->entries[idx].timestamp = ts;
    ti->head++;
    ti->count++;
    /* Net lifetime tallies: the ring overwrites old entries past
     * OB_TRACE_RING_SIZE, but these never lose a tagged event, so the dump can
     * always report whether ANY net leak exists even after the ring wraps. */
    if (delta > 0) ti->total_refs++;
    else           ti->total_derefs++;
    /* Per-tag lifetime ledger (ring-independent): find-or-insert this tag so
     * per-tag leak attribution survives ring wrap. If the table is full and the
     * tag is new, drop it and set life_overflow -- the per-tag dump then flags
     * itself partial and the net tally above remains the authoritative verdict. */
    {
        uint32_t li;
        for (li = 0; li < ti->life_ntags; li++)
            if (ti->life_tags[li].tag == tag) break;
        if (li == ti->life_ntags) {
            if (ti->life_ntags < OB_TRACE_LIFE_TAGS) {
                ti->life_tags[li].tag    = tag;
                ti->life_tags[li].refs   = 0;
                ti->life_tags[li].derefs = 0;
                ti->life_ntags++;
            } else {
                ti->life_overflow = 1;
                li = OB_TRACE_LIFE_TAGS;   /* sentinel: not tracked */
            }
        }
        if (li < OB_TRACE_LIFE_TAGS) {
            if (delta > 0) ti->life_tags[li].refs++;
            else           ti->life_tags[li].derefs++;
        }
    }
    spin_unlock_irqrestore(&ti->lock, irq_flags);
}

/* ---- Tagged reference API ----------------------------------------------- */

void ObReferenceObjectWithTag(void *body, uint32_t tag)
{
    if (!body) return;

    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);
    ObReferenceObject(body);

    if (hdr->trace) {
        uintptr_t caller = (uintptr_t)__builtin_return_address(0);
        trace_record(hdr->trace, tag, +1, caller);
    }
}

int32_t ObDereferenceObjectWithTag(void *body, uint32_t tag)
{
    if (!body) return 0;

    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);

    if (hdr->trace) {
        uintptr_t caller = (uintptr_t)__builtin_return_address(0);
        trace_record(hdr->trace, tag, -1, caller);
    }

    /* The leak dump fires from ob_free_object on the true 0-refcount teardown
     * (single owner -> race-free), NOT here off a speculative ref_count read --
     * that read was a TOCTOU: a concurrent ref could keep the object alive,
     * dumping a false "final" trace and skipping the real one. */
    return ObDereferenceObject(body);
}

/* ---- Trace dump --------------------------------------------------------- */

void ob_dump_trace_hdr(struct object_header *hdr_)
{
    OBJECT_HEADER *hdr = (OBJECT_HEADER *)hdr_;
    if (!hdr) return;
    OB_TRACE_INFO *ti = hdr->trace;
    if (!ti) return;

    /* Snapshot the net counters AND the ring-independent per-tag lifetime ledger
     * UNDER the lock (no I/O), then release and klog outside it (Gate 2: never
     * hold a spinlock across klog / serial I/O). The per-tag ledger survives ring
     * wrap, so a leak attributed to any tracked tag is reported authoritatively;
     * the net counters catch any leak even when the ledger overflowed (> the
     * tracked-tag bound). The ring is only recent caller history and its `count`
     * is reported with a `(recent detail truncated)` note once it wraps, so no
     * per-tag view is ever mistaken for the full picture. */
    struct { uint32_t tag; uint64_t refs; uint64_t derefs; } life[OB_TRACE_LIFE_TAGS];
    uint32_t life_n = 0;
    uint8_t  life_of = 0;
    uint64_t snap_count;
    uint64_t snap_refs;
    uint64_t snap_derefs;
    uint64_t irq_flags;

    spin_lock_irqsave(&ti->lock, &irq_flags);
    snap_count  = ti->count;
    snap_refs   = ti->total_refs;
    snap_derefs = ti->total_derefs;
    life_n      = ti->life_ntags;
    life_of     = ti->life_overflow;
    for (uint32_t i = 0; i < life_n && i < OB_TRACE_LIFE_TAGS; i++) {
        life[i].tag    = ti->life_tags[i].tag;
        life[i].refs   = ti->life_tags[i].refs;
        life[i].derefs = ti->life_tags[i].derefs;
    }
    spin_unlock_irqrestore(&ti->lock, irq_flags);

    if (snap_count == 0) return;

    const char *tname = (hdr->type && hdr->type->name) ? hdr->type->name : "?";
    void *body = (void *)((uint8_t *)hdr + sizeof(OBJECT_HEADER));
    int truncated = snap_count > OB_TRACE_RING_SIZE;
    klog(LOG_DEBUG, "ob", "[TRACE] Object %p type=%s -- %u tagged events%s:",
         body, tname, (uint64_t)snap_count,
         truncated ? " (recent caller detail truncated to last 64)" : "");

    /* Net lifetime leak verdict -- independent of ring wrap AND of ledger
     * overflow, so a NET leak is never hidden. */
    int64_t net_balance = (int64_t)snap_refs - (int64_t)snap_derefs;
    if (net_balance != 0)
        klog(LOG_WARN, "ob",
             "[TRACE]   LIFETIME ref=%d deref=%d LEAK=%d (tagged refs outstanding)",
             (int64_t)snap_refs, (int64_t)snap_derefs, net_balance);
    else
        klog(LOG_DEBUG, "ob",
             "[TRACE]   LIFETIME ref=%d deref=%d (net balanced)",
             (int64_t)snap_refs, (int64_t)snap_derefs);

    /* Per-tag lifetime attribution -- survives ring wrap. A net-balanced object
     * can still hide a mis-tag (one tag over-refs, another over-derefs), which
     * this per-tag ledger catches. */
    if (life_of)
        klog(LOG_WARN, "ob",
             "[TRACE]   per-tag ledger OVERFLOWED (> %u distinct tags) -- "
             "per-tag attribution partial; net verdict above is authoritative",
             (uint64_t)OB_TRACE_LIFE_TAGS);

    for (uint32_t i = 0; i < life_n && i < OB_TRACE_LIFE_TAGS; i++) {
        char tag_str[5];
        tag_str[0] = (char)((life[i].tag >> 24) & 0xFF);
        tag_str[1] = (char)((life[i].tag >> 16) & 0xFF);
        tag_str[2] = (char)((life[i].tag >>  8) & 0xFF);
        tag_str[3] = (char)((life[i].tag      ) & 0xFF);
        tag_str[4] = '\0';

        int64_t balance = (int64_t)life[i].refs - (int64_t)life[i].derefs;
        if (balance != 0)
            klog(LOG_WARN, "ob",
                 "[TRACE]   tag='%s' ref=%d deref=%d IMBALANCE=%d",
                 tag_str, (int64_t)life[i].refs,
                 (int64_t)life[i].derefs, balance);
        else
            klog(LOG_DEBUG, "ob",
                 "[TRACE]   tag='%s' ref=%d deref=%d (balanced)",
                 tag_str, (int64_t)life[i].refs,
                 (int64_t)life[i].derefs);
    }
}

void ob_dump_trace(void *body)
{
    if (!body) return;
    ob_dump_trace_hdr((struct object_header *)OB_HEADER_FROM_BODY(body));
}
