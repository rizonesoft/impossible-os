/* ============================================================================
 * ob_trace.h -- Object Manager tagged reference tracing and handle leak detection
 *
 * Provides ObReferenceObjectWithTag / ObDereferenceObjectWithTag for per-tag
 * reference tracking, and ob_handle_trace for per-handle klog event recording.
 *
 * XREF: 02-kernel-core/TODO-05-object-manager.md S15
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/ob/ob_type.h"
#include "kernel/sched/spinlock.h"

struct object_header;   /* full definition in kernel/ob/ob.h */

/* ---- Tagged reference trace entry --------------------------------------- */

#define OB_TRACE_RING_SIZE  64  /* recent caller-history entries per object trace log */
/* Lifetime per-tag table: ring-independent, so per-tag leak attribution is not
 * lost when the recent-history ring wraps. Bounded -- if an object touches more
 * than this many DISTINCT tags over its lifetime, the table overflows and the
 * per-tag verdict falls back to the (always-correct) net lifetime counter. */
#define OB_TRACE_LIFE_TAGS  32

typedef struct {
    uint32_t   tag;        /* 4-char tag (e.g. 'Lk01') packed as uint32_t */
    int8_t     delta;      /* +1 = ref, -1 = deref */
    uint8_t    _pad[3];
    uintptr_t  caller;     /* return address of the caller */
    uint64_t   timestamp;  /* monotonic ticks at record time */
} OB_REF_TRACE_ENTRY;

/* Per-object trace log -- attached to OBJECT_HEADER when type tracing enabled.
 * `lock` serializes the ring against concurrent tagged ref/deref on the same
 * object from multiple CPUs; head/count are 64-bit so they never wrap. A zeroed
 * (kmalloc + memset) instance is a valid unlocked lock (SPINLOCK_INIT == {0}).
 *
 * The `entries` ring holds only the most-recent OB_TRACE_RING_SIZE events for
 * per-tag caller detail; once `count` exceeds the ring the oldest entries are
 * overwritten, so the per-tag dump is a RECENT-history view, not a lifetime
 * ledger. `total_refs`/`total_derefs` are ring-independent net lifetime tallies,
 * and `life_tags[]` is a ring-independent PER-TAG lifetime ledger, so the dump
 * reports authoritative per-tag leak attribution that survives ring wrap. When
 * more than OB_TRACE_LIFE_TAGS distinct tags appear, `life_overflow` is set and
 * the per-tag verdict falls back to the net counters (which are never lost). */
typedef struct ob_trace_info {
    OB_REF_TRACE_ENTRY entries[OB_TRACE_RING_SIZE];
    uint64_t           head;         /* next write index (wraps the ring via % RING_SIZE) */
    uint64_t           count;        /* total entries written (may exceed ring size) */
    uint64_t           total_refs;   /* net lifetime tagged +1 events (never overwritten) */
    uint64_t           total_derefs; /* net lifetime tagged -1 events (never overwritten) */
    struct {
        uint32_t       tag;          /* 4-char tag, 0 == empty slot */
        uint64_t       refs;         /* lifetime +1 events for this tag */
        uint64_t       derefs;       /* lifetime -1 events for this tag */
    }                  life_tags[OB_TRACE_LIFE_TAGS]; /* ring-independent per-tag ledger */
    uint32_t           life_ntags;   /* distinct tags tracked (<= OB_TRACE_LIFE_TAGS) */
    uint8_t            life_overflow;/* set if a distinct tag was dropped (table full) */
    spinlock_t         lock;         /* IRQ-safe; serializes trace_record + dump snapshot */
} OB_TRACE_INFO;

/* ---- Per-type tracing control ------------------------------------------- */

/* Enable/disable tagged reference tracing for a type.
 * When enabled, ob_alloc_object allocates OB_TRACE_INFO alongside the object. */
void ob_enable_type_tracing(const OBJECT_TYPE *type);
void ob_disable_type_tracing(const OBJECT_TYPE *type);
int  ob_type_tracing_enabled(const OBJECT_TYPE *type);

/* Allocate a zeroed per-object trace log (NULL on OOM). Called by
 * ob_alloc_object when the object's type has tracing enabled. */
OB_TRACE_INFO *ob_trace_alloc(void);

/* Dump + free a header's trace log on teardown (refcount 0, single-owner --
 * race-free). Internal to the object free path. */
void ob_dump_trace_hdr(struct object_header *hdr);

/* ---- Tagged reference API ----------------------------------------------- */

/* Reference with a 4-char tag for leak detection.
 * tag is a uint32_t packed from 4 ASCII chars (e.g. 'Lk01'). */
void ObReferenceObjectWithTag(void *body, uint32_t tag);
int32_t ObDereferenceObjectWithTag(void *body, uint32_t tag);

/* ---- Trace dump --------------------------------------------------------- */

/* Dump an object's trace log to klog. Highlights per-tag imbalances.
 * Called automatically when refcount hits 0 with tracing enabled. */
void ob_dump_trace(void *body);

/* ---- Handle event tracing ----------------------------------------------- */

/* Global flag: when set, ObpAllocateHandle/ObpFreeHandle emit klog entries.
 * Controlled via boot.conf ob_handle_trace=1 or runtime toggle. */
extern int g_ob_handle_trace;
