/* ============================================================================
 * ob_trace.h -- Object Manager tagged reference tracing and handle leak detection
 *
 * Provides ObReferenceObjectWithTag / ObDereferenceObjectWithTag for per-tag
 * reference tracking, and ob_handle_trace for per-handle klog event recording.
 *
 * XREF: 02-kernel-core/TODO-03-object-manager.md S15
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/ob/ob_type.h"

/* ---- Tagged reference trace entry --------------------------------------- */

#define OB_TRACE_RING_SIZE  64  /* entries per object trace log */

typedef struct {
    uint32_t   tag;        /* 4-char tag (e.g. 'Lk01') packed as uint32_t */
    int8_t     delta;      /* +1 = ref, -1 = deref */
    uint8_t    _pad[3];
    uintptr_t  caller;     /* return address of the caller */
    uint64_t   timestamp;  /* PIT ticks */
} OB_REF_TRACE_ENTRY;

/* Per-object trace log -- attached to OBJECT_HEADER when type tracing enabled */
typedef struct ob_trace_info {
    OB_REF_TRACE_ENTRY entries[OB_TRACE_RING_SIZE];
    uint32_t           head;   /* next write index (wraps) */
    uint32_t           count;  /* total entries written (may exceed ring size) */
} OB_TRACE_INFO;

/* ---- Per-type tracing control ------------------------------------------- */

/* Enable/disable tagged reference tracing for a type.
 * When enabled, ob_alloc_object allocates OB_TRACE_INFO alongside the object. */
void ob_enable_type_tracing(const OBJECT_TYPE *type);
void ob_disable_type_tracing(const OBJECT_TYPE *type);
int  ob_type_tracing_enabled(const OBJECT_TYPE *type);

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
