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

/* Free the entry array with the SAME allocator that produced it. The array is
 * kmalloc'd while its byte size fits in a frame and pmm_alloc_contiguous'd once
 * it grows past that (see handle_table_grow). Freeing a PMM-backed array with
 * kfree corrupts the heap, so the free path must branch on the size the same
 * way the alloc path did. NULL entries is a no-op. */
static void handle_table_free_entries(HANDLE_TABLE_ENTRY *entries,
                                      uint32_t capacity)
{
    size_t sz = (size_t)capacity * sizeof(HANDLE_TABLE_ENTRY);

    if (!entries)
        return;

    if (sz <= PMM_FRAME_SIZE) {
        kfree(entries);
    } else {
        uint64_t frames = (sz + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        for (uint64_t i = 0; i < frames; i++)
            pmm_free_frame((uintptr_t)entries + i * PMM_FRAME_SIZE);
    }
}

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

    handle_table_free_entries(table->entries, table->capacity);
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

    /* Free the old array with the allocator that produced it (kmalloc vs PMM
     * depends on the OLD capacity's byte size; once past one frame the old
     * array was pmm_alloc_contiguous'd and must not be kfree'd). */
    handle_table_free_entries(table->entries, table->capacity);

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

    /* Reject non-canonical handles: low 2 bits are reserved and must be zero,
     * else handle/4 aliases a real slot (see ObpLookupHandle). */
    if (handle & 3)
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

    /* HANDLE values are slot_index * 4 -- the low 2 bits are reserved and MUST
     * be zero. Without this check a non-canonical handle (e.g. 0x5) divides down
     * to the same slot index as a real handle (0x4) and aliases it. */
    if (handle & 3)
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

int ob_handle_table_inherit(HANDLE_TABLE *parent, HANDLE_TABLE *child)
{
    uint32_t i, count = 0, max_inherit_idx = 0;
    int any_inheritable = 0;
    OBJECT_HEADER *hdr;

    if (!parent || !child || !parent->entries || !child->entries)
        return -1;

    /* Find the highest parent slot that actually carries an inheritable handle.
     * Only that index has to fit in the child -- growing to the full parent
     * capacity (which may have expanded for NON-inheritable handles) would turn
     * a fits-fine inheritance into a false OOM. */
    for (i = 0; i < parent->capacity; i++) {
        if (parent->entries[i].object
            && (parent->entries[i].attributes & OBJ_INHERIT)) {
            max_inherit_idx = i;
            any_inheritable = 1;
        }
    }
    if (!any_inheritable)
        return 0;  /* nothing to inherit -- success, no growth needed */

    /* Grow the child toward covering the highest inheritable index. Best-effort
     * under memory pressure: if a grow fails, inherit what already fits and warn
     * rather than failing process creation. Failing here would be worse -- the
     * child task is already created and there is no teardown path yet, so a
     * failed CreateProcess would LEAK the task (a partially-inherited process
     * instead runs and frees its task normally on exit). Atomic all-or-fail with
     * proper child teardown is tracked as a Handle-Inheritance follow-up item. */
    while (child->capacity <= max_inherit_idx) {
        if (handle_table_grow(child) < 0)
            break;
    }

    for (i = 0; i <= max_inherit_idx && i < child->capacity; i++) {
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

        /* Mirror ObpAllocateHandle's per-type handle statistics. ObpFreeHandle
         * decrements type->total_handles for EVERY closed handle, inherited
         * ones included, so without this bump the signed counter is driven
         * negative on close and NtQueryObject(ObjectTypesInformation) reports a
         * bogus huge open-handle count. */
        if (hdr->type) {
            OBJECT_TYPE *mtype = (OBJECT_TYPE *)hdr->type;
            int32_t cur = atomic_fetch_add(&mtype->total_handles, 1) + 1;
            if ((uint32_t)cur > mtype->peak_handles)
                mtype->peak_handles = (uint32_t)cur;
        }

        count++;
    }

    /* If the child could not be grown to cover the highest inheritable slot,
     * some inheritable handles were dropped -- surface it instead of silently
     * shipping a process missing handles it was supposed to inherit. */
    if (child->capacity <= max_inherit_idx)
        klog(LOG_WARN, "ob",
             "handle inheritance incomplete: child table OOM (got %u handles)",
             (uint64_t)count);

    return (int)count;
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
