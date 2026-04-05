/* ============================================================================
 * ob_trace.c -- Tagged reference tracing and handle leak detection
 *
 * Per-object circular trace log records tag + delta + caller + timestamp.
 * On refcount 0, ob_dump_trace emits per-tag summaries highlighting leaks.
 * Handle event tracing emits klog entries for every handle alloc/free.
 *
 * XREF: 02-kernel-core/TODO-03-object-manager.md S15
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

    uint32_t idx = ti->head % OB_TRACE_RING_SIZE;
    ti->entries[idx].tag       = tag;
    ti->entries[idx].delta     = delta;
    ti->entries[idx].caller    = caller;
    ti->entries[idx].timestamp = system_get_ticks();
    ti->head++;
    ti->count++;
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

    /* Check if this will be the final deref -- dump trace before freeing */
    if (atomic_read(&hdr->ref_count) == 1 && hdr->trace) {
        ob_dump_trace(body);
    }

    return ObDereferenceObject(body);
}

/* ---- Trace dump --------------------------------------------------------- */

void ob_dump_trace(void *body)
{
    if (!body) return;

    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);
    OB_TRACE_INFO *ti = hdr->trace;
    if (!ti || ti->count == 0) return;

    const char *tname = (hdr->type && hdr->type->name) ? hdr->type->name : "?";

    klog(LOG_DEBUG, "ob", "[TRACE] Object %p type=%s -- %u trace entries:",
         body, tname, ti->count);

    /* Per-tag summary: scan all recorded entries and tally ref/deref */
    #define MAX_TAGS 16
    struct { uint32_t tag; int32_t refs; int32_t derefs; } summary[MAX_TAGS];
    uint32_t ntags = 0;

    uint32_t total = ti->count < OB_TRACE_RING_SIZE ? ti->count : OB_TRACE_RING_SIZE;
    uint32_t start = ti->count <= OB_TRACE_RING_SIZE ? 0 : ti->head % OB_TRACE_RING_SIZE;

    for (uint32_t i = 0; i < total; i++) {
        uint32_t idx = (start + i) % OB_TRACE_RING_SIZE;
        OB_REF_TRACE_ENTRY *e = &ti->entries[idx];

        /* Find or create tag entry in summary */
        uint32_t si;
        for (si = 0; si < ntags; si++) {
            if (summary[si].tag == e->tag)
                break;
        }
        if (si == ntags && ntags < MAX_TAGS) {
            summary[ntags].tag = e->tag;
            summary[ntags].refs = 0;
            summary[ntags].derefs = 0;
            ntags++;
        }
        if (si < ntags) {
            if (e->delta > 0) summary[si].refs++;
            else              summary[si].derefs++;
        }
    }

    /* Emit per-tag summary */
    for (uint32_t i = 0; i < ntags; i++) {
        char tag_str[5];
        tag_str[0] = (char)((summary[i].tag >> 24) & 0xFF);
        tag_str[1] = (char)((summary[i].tag >> 16) & 0xFF);
        tag_str[2] = (char)((summary[i].tag >>  8) & 0xFF);
        tag_str[3] = (char)((summary[i].tag      ) & 0xFF);
        tag_str[4] = '\0';

        int32_t balance = summary[i].refs - summary[i].derefs;
        if (balance != 0) {
            klog(LOG_WARN, "ob",
                 "[TRACE]   tag='%s' ref=%d deref=%d IMBALANCE=%d",
                 tag_str,
                 (int64_t)summary[i].refs,
                 (int64_t)summary[i].derefs,
                 (int64_t)balance);
        } else {
            klog(LOG_DEBUG, "ob",
                 "[TRACE]   tag='%s' ref=%d deref=%d (balanced)",
                 tag_str,
                 (int64_t)summary[i].refs,
                 (int64_t)summary[i].derefs);
        }
    }

    #undef MAX_TAGS
}
