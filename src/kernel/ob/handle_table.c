/* ============================================================================
 * handle_table.c -- Per-process handle table
 *
 * HANDLE values = slot_index * 4 (low 2 bits reserved for future use).
 * The table grows by doubling when full, up to HANDLE_TABLE_MAX_CAP.
 * ============================================================================ */

#include "kernel/ob/handle_table.h"
#include "kernel/tunables.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_callback.h"
#include "kernel/ob/ob_trace.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"
#include "kernel/sched/task.h"

extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);

/* --- ob_handle_table_init ------------------------------------------------ */

int ob_handle_table_init(HANDLE_TABLE *table)
{
    size_t sz = HANDLE_TABLE_INIT_CAP * sizeof(HANDLE_TABLE_ENTRY);

    table->entries = (HANDLE_TABLE_ENTRY *)kmalloc(sz);
    if (!table->entries)
        return -1;

    memset(table->entries, 0, sz);
    table->capacity = HANDLE_TABLE_INIT_CAP;
    table->count = 0;
    /* Per-process handle limit is operator-tunable (handle.quota_default);
     * falls back to the compiled default before the registry is up. */
    table->handle_limit = (uint32_t)kernel_tunable_get_u64(
        "handle.quota_default", HANDLE_TABLE_DEFAULT_LIMIT);
    return 0;
}

/* --- ob_handle_table_destroy --------------------------------------------- */

void ob_handle_table_destroy(HANDLE_TABLE *table)
{
    uint32_t i;

    if (!table->entries)
        return;

    /* Close every open handle */
    for (i = 0; i < table->capacity; i++) {
        if (table->entries[i].object) {
            HANDLE h = (HANDLE)(i * 4);
            ObpFreeHandle(table, h);
        }
    }

    kfree(table->entries);
    table->entries = NULL;
    table->capacity = 0;
    table->count = 0;
}

/* --- grow ---------------------------------------------------------------- */

static int handle_table_grow(HANDLE_TABLE *table)
{
    uint32_t new_cap = table->capacity * 2;
    size_t new_sz;
    HANDLE_TABLE_ENTRY *new_entries;

    if (new_cap > HANDLE_TABLE_MAX_CAP)
        return -1;

    new_sz = new_cap * sizeof(HANDLE_TABLE_ENTRY);

    if (new_sz <= PMM_FRAME_SIZE) {
        new_entries = (HANDLE_TABLE_ENTRY *)kmalloc(new_sz);
    } else {
        uint64_t frames = (new_sz + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uintptr_t phys = pmm_alloc_contiguous(frames);
        new_entries = phys ? (HANDLE_TABLE_ENTRY *)phys : NULL;
    }

    if (!new_entries)
        return -1;

    /* Copy existing entries and zero the new slots */
    memcpy(new_entries, table->entries,
           table->capacity * sizeof(HANDLE_TABLE_ENTRY));
    memset(&new_entries[table->capacity], 0,
           (new_cap - table->capacity) * sizeof(HANDLE_TABLE_ENTRY));

    /* Free old array (always was kmalloc'd if capacity <= PMM_FRAME_SIZE/entry_size) */
    kfree(table->entries);

    table->entries = new_entries;
    table->capacity = new_cap;
    return 0;
}

/* --- ObpAllocateHandle --------------------------------------------------- */

HANDLE ObpAllocateHandle(HANDLE_TABLE *table, void *object,
                         uint32_t access, uint32_t attrs)
{
    uint32_t i;
    OBJECT_HEADER *hdr;

    if (!table->entries || !object)
        return INVALID_HANDLE_VALUE;

    /* Object callbacks (S13): pre-callbacks can strip access bits or deny.
     * Runs before the security check so callbacks filter first. */
    hdr = OB_HEADER_FROM_BODY(object);
    if (hdr->type) {
        if (ob_invoke_pre_callbacks(OB_OPERATION_HANDLE_CREATE,
                                    object, hdr->type, &access) != 0)
            return INVALID_HANDLE_VALUE;  /* callback denied */
    }

    /* Security check hook: if the object has an SD and the type provides
     * an on_open callback, call it to validate access.  Full SeAccessCheck
     * enforcement is wired in TODO-11 S5; this is the structural hook. */
    if (hdr->security && hdr->type && hdr->type->on_open) {
        if (hdr->type->on_open(object, access) != 0)
            return INVALID_HANDLE_VALUE;  /* access denied */
    }

    /* Per-process handle quota (S14): deny if at limit */
    if (table->handle_limit > 0 && table->count >= table->handle_limit) {
        struct task *cur = task_current();
        klog(LOG_DEBUG, "ob", "PID %u handle quota exhausted (%u/%u)",
             cur ? (uint64_t)cur->pid : 0,
             (uint64_t)table->count, (uint64_t)table->handle_limit);
        return INVALID_HANDLE_VALUE;
    }

    /* Find a free slot.
     *
     * Slot 0 is deliberately reserved (scan starts at i=1) so that
     * `HANDLE = i * 4 = 0` is never a valid handle value. This matches
     * Windows semantics where (HANDLE)0 == NULL means "no handle" and
     * CloseHandle(NULL) returns FALSE. Without the reservation the
     * first CreateFile would return handle 0, and Win32 callers
     * (user/lib/win32.c CloseHandle) could not distinguish it from an
     * uninitialized sentinel -- rejecting NULL at the Win32 boundary
     * would then break legitimate closes. Codex quality review
     * 2026-04-22 Critical. */
    for (i = 1; i < table->capacity; i++) {
        if (!table->entries[i].object)
            goto found;
    }

    /* Table full -- try to grow */
    if (handle_table_grow(table) < 0)
        return INVALID_HANDLE_VALUE;

    /* i is still == old capacity, which is now the first free slot */

found:
    table->entries[i].object         = object;
    table->entries[i].granted_access = access;
    table->entries[i].attributes     = attrs;
    table->count++;

    /* Bump object ref and handle counts */
    ObReferenceObject(object);
    hdr = OB_HEADER_FROM_BODY(object);
    hdr->handle_count++;

    /* Per-type handle statistics */
    if (hdr->type) {
        OBJECT_TYPE *mtype = (OBJECT_TYPE *)hdr->type;
        int32_t cur = atomic_fetch_add(&mtype->total_handles, 1) + 1;
        if ((uint32_t)cur > mtype->peak_handles)
            mtype->peak_handles = (uint32_t)cur;
    }

    /* Object callbacks (S13): post-callbacks with granted access */
    if (hdr->type)
        ob_invoke_post_callbacks(OB_OPERATION_HANDLE_CREATE,
                                 object, hdr->type, access);

    /* Handle event tracing (S15) */
    if (g_ob_handle_trace) {
        struct task *cur = task_current();
        klog(LOG_DEBUG, "ob", "HANDLE CREATE PID=%u H=0x%x obj=%p type=%s",
             cur ? (uint64_t)cur->pid : 0,
             (uint64_t)(i * 4), object,
             hdr->type && hdr->type->name ? hdr->type->name : "?");
    }

    /* Per-process cumulative handle counter (S14 diagnostics) */
    {
        struct task *cur = task_current();
        if (cur)
            cur->total_handles_created++;
    }

    return (HANDLE)(i * 4);
}

/* --- ObpFreeHandle ------------------------------------------------------- */

int ObpFreeHandle(HANDLE_TABLE *table, HANDLE handle)
{
    uint32_t idx;
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;
    void *body;

    if (handle < 0 || !table->entries)
        return -1;

    idx = (uint32_t)handle / 4;
    if (idx >= table->capacity)
        return -1;

    entry = &table->entries[idx];
    if (!entry->object)
        return -1;

    if (entry->attributes & OBJ_PROTECT_CLOSE)
        return -1;

    body = entry->object;
    hdr = OB_HEADER_FROM_BODY(body);

    /* Handle event tracing (S15) */
    if (g_ob_handle_trace) {
        struct task *cur = task_current();
        klog(LOG_DEBUG, "ob", "HANDLE FREE PID=%u H=0x%x obj=%p type=%s",
             cur ? (uint64_t)cur->pid : 0,
             (uint64_t)handle, body,
             hdr->type && hdr->type->name ? hdr->type->name : "?");
    }

    /* Clear the slot first */
    entry->object = NULL;
    entry->granted_access = 0;
    entry->attributes = 0;
    table->count--;

    /* Decrement handle count; call on_close if it drops to 0 */
    if (hdr->handle_count > 0)
        hdr->handle_count--;
    if (hdr->handle_count == 0 && hdr->type->on_close)
        hdr->type->on_close(body, 0);

    /* Per-type handle statistics */
    if (hdr->type) {
        OBJECT_TYPE *mtype = (OBJECT_TYPE *)hdr->type;
        atomic_dec(&mtype->total_handles);
    }

    /* Drop the reference taken by ObpAllocateHandle */
    ObDereferenceObject(body);
    return 0;
}

/* --- ObpLookupHandle ----------------------------------------------------- */

HANDLE_TABLE_ENTRY *ObpLookupHandle(HANDLE_TABLE *table, HANDLE handle)
{
    uint32_t idx;

    /* Pseudo-handles are resolved by the caller (syscall layer) */
    if (handle < 0)
        return NULL;

    if (!table->entries)
        return NULL;

    idx = (uint32_t)handle / 4;
    if (idx >= table->capacity)
        return NULL;

    if (!table->entries[idx].object)
        return NULL;

    return &table->entries[idx];
}

/* --- ob_handle_table_inherit --------------------------------------------- */

uint32_t ob_handle_table_inherit(HANDLE_TABLE *parent, HANDLE_TABLE *child)
{
    uint32_t i, count = 0;
    OBJECT_HEADER *hdr;

    if (!parent || !child || !parent->entries || !child->entries)
        return 0;

    /* Grow child table to match parent capacity if needed */
    while (child->capacity < parent->capacity) {
        if (handle_table_grow(child) < 0)
            break;  /* can't grow further -- inherit what fits */
    }

    for (i = 0; i < parent->capacity && i < child->capacity; i++) {
        if (!parent->entries[i].object)
            continue;
        if (!(parent->entries[i].attributes & OBJ_INHERIT))
            continue;

        /* Copy entry at the same slot index (preserves HANDLE value) */
        child->entries[i].object         = parent->entries[i].object;
        child->entries[i].granted_access = parent->entries[i].granted_access;
        child->entries[i].attributes     = parent->entries[i].attributes;
        child->count++;

        /* Child holds its own reference */
        ObReferenceObject(parent->entries[i].object);
        hdr = OB_HEADER_FROM_BODY(parent->entries[i].object);
        hdr->handle_count++;

        count++;
    }

    return count;
}

/* --- ob_handle_table_set_limit (S14) ------------------------------------- */

void ob_handle_table_set_limit(HANDLE_TABLE *table, uint32_t new_limit)
{
    if (!table)
        return;
    if (new_limit > HANDLE_TABLE_ABSOLUTE_MAX)
        new_limit = HANDLE_TABLE_ABSOLUTE_MAX;
    table->handle_limit = new_limit;
}
