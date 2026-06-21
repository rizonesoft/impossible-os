/* ============================================================================
 * ob.c -- Object Manager: type registration, object allocation, init
 *
 * Implements the core Object Manager infrastructure (type registry,
 * object allocation, and namespace root setup).
 * ============================================================================ */

#include "kernel/ob/ob.h"
#include "kernel/ob/ob_callback.h"
#include "kernel/ob/ob_trace.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/ob/ob_file.h"
#include "kernel/ob/ob_info_file.h"
#include "kernel/ob/ob_process.h"
#include "kernel/ob/ob_thread.h"
#include "kernel/ob/ob_event.h"
#include "kernel/ob/ob_mutex.h"
#include "kernel/ob/ob_semaphore.h"
#include "kernel/ob/ob_timer.h"
#include "kernel/ob/ob_section.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"
#include "kernel/boot_init.h"
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"
#include "kernel/security/default_sds.h"
#include "kernel/security/token.h"

extern void *memset(void *s, int c, size_t n);

/* --- Static type table --------------------------------------------------- */

static OBJECT_TYPE g_ob_types[OB_MAX_TYPES];
static uint32_t    g_ob_type_count = 0;
/* Serialises ob_create_type: the registry is a global the SMP-by-default
 * invariant forbids mutating with an unlocked check-then-increment. Built-in
 * types register on the BSP during boot, but the API is also reachable from
 * ALPC/test registration paths, so the slot reservation must be atomic. */
static DEFINE_SPINLOCK(s_type_lock);

/* --- Built-in type singleton pointers ------------------------------------ */

const OBJECT_TYPE *ObpFileType      = NULL;
const OBJECT_TYPE *ObpProcessType   = NULL;
const OBJECT_TYPE *ObpThreadType    = NULL;
const OBJECT_TYPE *ObpDirectoryType = NULL;
const OBJECT_TYPE *ObpSymlinkType   = NULL;
const OBJECT_TYPE *ObpEventType     = NULL;
const OBJECT_TYPE *ObpMutexType     = NULL;
const OBJECT_TYPE *ObpSemaphoreType = NULL;
const OBJECT_TYPE *ObpSectionType   = NULL;
const OBJECT_TYPE *ObpTimerType     = NULL;
const OBJECT_TYPE *ObpTokenType     = NULL;
const OBJECT_TYPE *ObpPebType       = NULL;
const OBJECT_TYPE *ObpTebType       = NULL;

/* --- ob_create_type ------------------------------------------------------ */

const OBJECT_TYPE *ob_create_type(const OBJECT_TYPE *tmpl)
{
    OBJECT_TYPE *slot;
    uint64_t irqf;

    spin_lock_irqsave(&s_type_lock, &irqf);
    if (g_ob_type_count >= OB_MAX_TYPES) {
        spin_unlock_irqrestore(&s_type_lock, irqf);
        klog(LOG_ERROR, "ob", "type table full (%u/%u) -- cannot register '%s'",
             (uint64_t)g_ob_type_count, (uint64_t)OB_MAX_TYPES,
             tmpl->name ? tmpl->name : "?");
        return NULL;
    }

    /* Fully initialise the slot BEFORE publishing the new count: an unlocked
     * reader (ob_get_types -> NtQueryObject(ObjectTypesInformation)) that sees
     * the incremented count must never observe a half-written row. The release
     * store pairs with the acquire load in ob_get_types so the slot writes are
     * visible before the count bump. */
    slot = &g_ob_types[g_ob_type_count];
    slot->name      = tmpl->name;
    slot->body_size = tmpl->body_size;
    slot->on_close  = tmpl->on_close;
    slot->on_delete = tmpl->on_delete;
    slot->on_open   = tmpl->on_open;
    slot->on_parse  = tmpl->on_parse;
    __atomic_store_n(&g_ob_type_count, g_ob_type_count + 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&s_type_lock, irqf);
    return slot;
}

const OBJECT_TYPE *ob_get_types(uint32_t *out_count)
{
    /* Acquire-load the count so the table rows the writer published before the
     * count bump are visible to this (lockfree) reader. */
    if (out_count) *out_count = __atomic_load_n(&g_ob_type_count, __ATOMIC_ACQUIRE);
    return g_ob_types;
}

/* --- ob_alloc_object ----------------------------------------------------- */

/* Tail-packed per-object creator security descriptor. When the allocating task
 * carries a token, this private SD+DACL rides in the SAME allocation block as
 * the object (just past header+body). One allocation, per-object (no shared
 * static aliasing/race across objects), freed with the object -- no second
 * heap node on the universal object hot path. OB_FLAG_TAIL_SD records that the
 * block carries it so the pmm free path counts the extra bytes. */
struct ob_creator_sd { SECURITY_DESCRIPTOR sd; uint8_t dacl[128]; };

void *ob_alloc_object(const OBJECT_TYPE *type)
{
    size_t total, sd_off = 0;
    int want_creator_sd;
    struct task *cur;
    void *block;

    /* Reject a NULL type and a body_size that would wrap the header+body sum.
     * body_size is copied verbatim from the registering template, so a malformed
     * or future dynamic type must not be able to wrap `total` to a small value,
     * take the kmalloc path, and hand back a body that callers treat as
     * body_size bytes (heap/page corruption). */
    if (!type || type->body_size > (size_t)-1 - sizeof(OBJECT_HEADER))
        return NULL;
    total = sizeof(OBJECT_HEADER) + type->body_size;

    /* Reserve tail space for the creator SD when the caller has a token. */
    cur = task_current();
    want_creator_sd = (cur && cur->token);
    if (want_creator_sd) {
        if (sizeof(struct ob_creator_sd) > (size_t)-1 - total)
            return NULL;
        sd_off = total;
        total += sizeof(struct ob_creator_sd);
    }

    if (total <= PMM_FRAME_SIZE) {
        block = kmalloc(total);
    } else {
        uint64_t frames = (total + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uintptr_t phys = pmm_alloc_contiguous(frames);
        block = phys ? (void *)phys : NULL;
    }

    if (!block) {
        klog(LOG_ERROR, "ob", "alloc failed for type '%s' (%u bytes)",
             type->name ? type->name : "?", (uint64_t)total);
        return NULL;
    }

    memset(block, 0, total);

    OBJECT_HEADER *hdr = (OBJECT_HEADER *)block;
    atomic_set(&hdr->ref_count, 1);
    hdr->type = type;
    if (total > PMM_FRAME_SIZE)
        hdr->flags |= OB_FLAG_PMM_ALLOC;
    /* Flag the reserved tail whenever tail space was allocated -- NOT only when
     * the SD build succeeds. The free path sizes the block from this flag, so it
     * must match what was allocated even on the SeCreateCreatorSD fallback path
     * (otherwise a pmm object whose tail crossed a frame boundary leaks it). */
    if (want_creator_sd)
        hdr->flags |= OB_FLAG_TAIL_SD;

    /* Attach the security descriptor. A token-bearing creator gets the per-object
     * tail-packed creator SD; everyone else (and the SD-build fallback) shares the
     * immutable kernel-default SD (a static, never freed). */
    if (want_creator_sd) {
        struct ob_creator_sd *csd = (struct ob_creator_sd *)((uint8_t *)block + sd_off);
        ACCESS_TOKEN *tok = (ACCESS_TOKEN *)cur->token;
        if (SeCreateCreatorSD(&csd->sd, tok->UserSid, csd->dacl, sizeof(csd->dacl)) == 0)
            hdr->security = &csd->sd;
        else
            hdr->security = (SECURITY_DESCRIPTOR *)SeCreateDefaultSD(SE_SD_TYPE_DEFAULT);
    } else {
        hdr->security = (SECURITY_DESCRIPTOR *)SeCreateDefaultSD(SE_SD_TYPE_DEFAULT);
    }

    /* Per-type tracing (S15): allocate trace log if type tracing is enabled */
    if (type->tracing_enabled) {
        extern OB_TRACE_INFO *ob_trace_alloc(void);
        hdr->trace = ob_trace_alloc();
        if (!hdr->trace)
            klog(LOG_WARN, "ob",
                 "trace alloc failed for type '%s'; this object untraced",
                 type->name ? type->name : "?");
    }

    /* Per-type statistics: increment live object count and lift the high-water
     * mark with an atomic compare-exchange max so two CPUs allocating the same
     * type concurrently cannot lose a peak update (the plain RMW raced). */
    {
        OBJECT_TYPE *mtype = (OBJECT_TYPE *)type;
        uint32_t cur_n = (uint32_t)(atomic_fetch_add(&mtype->total_objects, 1) + 1);
        uint32_t peak = __atomic_load_n(&mtype->peak_objects, __ATOMIC_RELAXED);
        while (cur_n > peak &&
               !__atomic_compare_exchange_n(&mtype->peak_objects, &peak, cur_n, 0,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            ;  /* peak reloaded by the CAS on failure */
    }

    return OB_BODY_FROM_HEADER(hdr);
}

/* --- ObReferenceObject --------------------------------------------------- */

void ObReferenceObject(void *body)
{
    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);
    atomic_inc(&hdr->ref_count);
}

/* --- ObDereferenceObject ------------------------------------------------- */

static void ob_free_object(OBJECT_HEADER *hdr)
{
    /* Per-type statistics: decrement live object count */
    if (hdr->type) {
        OBJECT_TYPE *mtype = (OBJECT_TYPE *)hdr->type;
        atomic_dec(&mtype->total_objects);
    }

    /* Free trace log if allocated (S15) */
    if (hdr->trace) {
        kfree(hdr->trace);
        hdr->trace = (void *)0;
    }

    /* The creator SD (if any) is tail-packed in this same block, so it is freed
     * with the object -- no separate free. The pmm path must count its bytes. */
    if (hdr->flags & OB_FLAG_PMM_ALLOC) {
        size_t total = sizeof(OBJECT_HEADER) + hdr->type->body_size;
        uint64_t frames;
        if (hdr->flags & OB_FLAG_TAIL_SD)
            total += sizeof(struct ob_creator_sd);
        frames = (total + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uint64_t i;
        for (i = 0; i < frames; i++)
            pmm_free_frame((uintptr_t)hdr + i * PMM_FRAME_SIZE);
    } else {
        kfree(hdr);
    }
}

int32_t ObDereferenceObject(void *body)
{
    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);

    if (atomic_dec_and_test(&hdr->ref_count)) {
        /* Refcount hit 0 */
        if (hdr->flags & OB_FLAG_PERMANENT)
            return 0;  /* permanent objects stay alive */

        if (hdr->type->on_delete)
            hdr->type->on_delete(body);

        ob_free_object(hdr);
        return 0;
    }

    return atomic_read(&hdr->ref_count);
}

/* --- ObReferenceObjectByPointer ------------------------------------------ */

int ObReferenceObjectByPointer(void *body, const OBJECT_TYPE *expected_type,
                               uint32_t access)
{
    (void)access;  /* used by security-check retrofit */

    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);
    if (hdr->type != expected_type)
        return -1;

    atomic_inc(&hdr->ref_count);
    return 0;
}

/* --- ObMakeTemporaryObject ----------------------------------------------- */

void ObMakeTemporaryObject(void *body)
{
    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);
    hdr->flags &= ~OB_FLAG_PERMANENT;
}

/* --- ObSetSecurityDescriptor / ObGetSecurityDescriptor -------------------- */

void ObSetSecurityDescriptor(void *body, SECURITY_DESCRIPTOR *sd)
{
    if (body) {
        OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);
        /* Replacing the SD pointer is safe even when a tail-packed creator SD is
         * present: that tail region lives in the object block and is reclaimed
         * with the object (OB_FLAG_TAIL_SD stays set so the pmm free still counts
         * it), so there is nothing to free here. */
        hdr->security = sd;
    }
}

SECURITY_DESCRIPTOR *ObGetSecurityDescriptor(void *body)
{
    if (!body)
        return (SECURITY_DESCRIPTOR *)0;
    return OB_HEADER_FROM_BODY(body)->security;
}

/* --- NtClose / NtDuplicateObject / NtQueryObject ------------------------- */

extern void *memcpy(void *dst, const void *src, size_t n);
extern size_t strlen(const char *s);

int NtClose(HANDLE_TABLE *ht, HANDLE handle)
{
    if (!ht || handle < 0)
        return -1;
    return ObpFreeHandle(ht, handle);
}

int NtDuplicateObject(HANDLE_TABLE *src_ht, HANDLE src_handle,
                      HANDLE_TABLE *dst_ht, HANDLE *dst_handle,
                      uint32_t desired_access, uint32_t attrs,
                      uint32_t options)
{
    HANDLE_TABLE_ENTRY *entry;
    HANDLE new_h;
    uint32_t access;

    if (!src_ht || !dst_ht || !dst_handle)
        return -1;

    entry = ObpLookupHandle(src_ht, src_handle);
    if (!entry)
        return -1;

    access = (options & DUPLICATE_SAME_ACCESS)
           ? entry->granted_access : desired_access;

    /* Object callbacks (S13): pre-callbacks for HANDLE_DUPLICATE.
     * Callbacks may strip access bits; if zeroed, deny the duplicate. */
    {
        OBJECT_HEADER *dup_hdr = OB_HEADER_FROM_BODY(entry->object);
        if (dup_hdr->type) {
            if (ob_invoke_pre_callbacks(OB_OPERATION_HANDLE_DUPLICATE,
                                        entry->object, dup_hdr->type,
                                        &access) != 0)
                return -1;
        }
    }

    new_h = ObpAllocateHandle(dst_ht, entry->object, access, attrs);
    if (new_h == INVALID_HANDLE_VALUE)
        return -1;

    *dst_handle = new_h;

    /* Object callbacks (S13): post-callbacks for HANDLE_DUPLICATE */
    {
        OBJECT_HEADER *dup_hdr = OB_HEADER_FROM_BODY(entry->object);
        if (dup_hdr->type)
            ob_invoke_post_callbacks(OB_OPERATION_HANDLE_DUPLICATE,
                                     entry->object, dup_hdr->type, access);
    }

    if (options & DUPLICATE_CLOSE_SOURCE)
        ObpFreeHandle(src_ht, src_handle);

    return 0;
}

int NtQueryObject(HANDLE_TABLE *ht, HANDLE handle,
                  OBJECT_INFORMATION_CLASS info_class,
                  void *buffer, uint32_t size, uint32_t *return_length)
{
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;

    if (!ht || !buffer)
        return -1;

    entry = ObpLookupHandle(ht, handle);
    if (!entry)
        return -1;

    hdr = OB_HEADER_FROM_BODY(entry->object);

    switch (info_class) {

    case ObjectBasicInformation: {
        /* Return: ref_count, handle_count, attributes (flags) */
        uint32_t needed = 3 * sizeof(uint32_t);
        if (return_length) *return_length = needed;
        if (size < needed) return -1;
        ((uint32_t *)buffer)[0] = (uint32_t)atomic_read(&hdr->ref_count);
        ((uint32_t *)buffer)[1] = hdr->handle_count;
        ((uint32_t *)buffer)[2] = hdr->flags;
        return 0;
    }

    case ObjectNameInformation: {
        /* Return: object name string (component name from namespace) */
        const char *name = hdr->name ? hdr->name : "";
        uint32_t len = (uint32_t)strlen(name) + 1;
        if (return_length) *return_length = len;
        if (size < len) return -1;
        memcpy(buffer, name, len);
        return 0;
    }

    case ObjectTypeInformation: {
        /* Return: full OBJECT_TYPE_INFORMATION struct with statistics */
        uint32_t needed = sizeof(OBJECT_TYPE_INFORMATION);
        if (return_length) *return_length = needed;
        if (size < needed) return -1;
        OBJECT_TYPE_INFORMATION *ti = (OBJECT_TYPE_INFORMATION *)buffer;
        memset(ti, 0, sizeof(*ti));
        if (hdr->type) {
            const char *tname = hdr->type->name ? hdr->type->name : "";
            uint32_t nlen = 0;
            while (tname[nlen] && nlen < sizeof(ti->type_name) - 1) {
                ti->type_name[nlen] = tname[nlen]; nlen++;
            }
            ti->type_name[nlen] = '\0';
            ti->total_objects = (uint32_t)atomic_read(&hdr->type->total_objects);
            ti->total_handles = (uint32_t)atomic_read(&hdr->type->total_handles);
            ti->peak_objects  = hdr->type->peak_objects;
            ti->peak_handles  = hdr->type->peak_handles;
            ti->body_size     = (uint32_t)hdr->type->body_size;
            ti->valid_access  = 0;  /* reserved */
        }
        return 0;
    }

    case ObjectTypesInformation: {
        /* Enumerate all registered types (no handle needed -- ignore entry) */
        uint32_t type_count = 0;
        const OBJECT_TYPE *types = ob_get_types(&type_count);
        uint32_t needed = sizeof(uint32_t) + type_count * sizeof(OBJECT_TYPE_INFORMATION);
        if (return_length) *return_length = needed;
        if (size < needed) return -1;
        OBJECT_TYPES_INFORMATION *oti = (OBJECT_TYPES_INFORMATION *)buffer;
        oti->number_of_types = type_count;
        for (uint32_t t = 0; t < type_count; t++) {
            OBJECT_TYPE_INFORMATION *ti = &oti->types[t];
            memset(ti, 0, sizeof(*ti));
            const char *tname = types[t].name ? types[t].name : "";
            uint32_t nlen = 0;
            while (tname[nlen] && nlen < sizeof(ti->type_name) - 1) {
                ti->type_name[nlen] = tname[nlen]; nlen++;
            }
            ti->type_name[nlen] = '\0';
            ti->total_objects = (uint32_t)atomic_read(&types[t].total_objects);
            ti->total_handles = (uint32_t)atomic_read(&types[t].total_handles);
            ti->peak_objects  = types[t].peak_objects;
            ti->peak_handles  = types[t].peak_handles;
            ti->body_size     = (uint32_t)types[t].body_size;
            ti->valid_access  = 0;
        }
        return 0;
    }

    default:
        return -1;
    }
}

/* --- NtOpenDirectoryObject / NtQueryDirectoryObject ---------------------- */

int NtOpenDirectoryObject(HANDLE_TABLE *ht, const char *name,
                          uint32_t access, HANDLE *out_handle)
{
    void *body = NULL;
    HANDLE h;

    if (!ht || !name || !out_handle)
        return -1;

    if (ObLookupObjectByName(name, ObpDirectoryType, access, &body) != 0
        || !body)
        return -1;

    h = ObpAllocateHandle(ht, body, access, 0);
    ObDereferenceObject(body);  /* drop lookup ref -- handle holds its own */

    if (h == INVALID_HANDLE_VALUE)
        return -1;

    *out_handle = h;
    return 0;
}

int NtQueryDirectoryObject(HANDLE_TABLE *ht, HANDLE dir_handle,
                           OBJECT_DIRECTORY_INFORMATION *buffer,
                           uint32_t buffer_count,
                           uint32_t *context,
                           uint32_t *return_count)
{
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;
    OBJECT_DIRECTORY *dir;
    OBJECT_DIRECTORY_ENTRY *e;
    uint32_t skip, filled, idx;

    if (!ht || !buffer || !context || !return_count || buffer_count == 0)
        return -1;

    entry = ObpLookupHandle(ht, dir_handle);
    if (!entry)
        return -1;

    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpDirectoryType)
        return -1;

    dir = (OBJECT_DIRECTORY *)entry->object;
    skip = *context;
    filled = 0;
    idx = 0;

    /* Pin the directory across the enumeration. Without this, a concurrent
     * NtClose on the last handle + ObMakeTemporaryObject + ObpRemoveFromDirectory
     * on another CPU could free the body between the handle-table lookup
     * and the spin_lock_irqsave below, causing a UAF on dir->lock itself.
     * The extra ref is dropped on every exit path. */
    ObReferenceObject(dir);

    /* Hold dir->lock for the full enumeration so the list cannot be
     * mutated underneath us by a concurrent ObInsertObject /
     * ObpRemoveFromDirectory on another CPU. buffer_count is bounded
     * at the Win32 syscall layer (typically a single entry), so the
     * lock hold time stays short. */
    {
        uint64_t irqf;
        spin_lock_irqsave(&dir->lock, &irqf);
        for (e = dir->first; e && filled < buffer_count; e = e->next, idx++) {
            if (idx < skip)
                continue;

            /* Copy entry name */
            {
                uint32_t i;
                for (i = 0; i < 63 && e->name[i]; i++)
                    buffer[filled].name[i] = e->name[i];
                buffer[filled].name[i] = '\0';
            }

            /* Copy type name from object header */
            {
                OBJECT_HEADER *obj_hdr = OB_HEADER_FROM_BODY(e->object);
                const char *tname = (obj_hdr->type && obj_hdr->type->name)
                                  ? obj_hdr->type->name : "Unknown";
                uint32_t i;
                for (i = 0; i < 31 && tname[i]; i++)
                    buffer[filled].type_name[i] = tname[i];
                buffer[filled].type_name[i] = '\0';
            }

            filled++;
        }
        spin_unlock_irqrestore(&dir->lock, irqf);
    }

    ObDereferenceObject(dir);

    *context = idx;
    *return_count = filled;
    return 0;
}

/* --- ob_init ------------------------------------------------------------- */

boot_result_t ob_init(void)
{
    BOOT_REQUIRE(SUBSYS_HEAP);

    POST16(POST16_OB);
    klog(LOG_INFO, "ob", "Object Manager initializing...");

    ob_file_type_init();
    ob_info_file_type_init();
    ob_process_type_init();
    ob_thread_type_init();
    ObpDirectoryType = ob_create_type(&(OBJECT_TYPE){
        .name = "Directory", .body_size = sizeof(OBJECT_DIRECTORY)
    });
    ObpSymlinkType = ob_create_type(&(OBJECT_TYPE){
        .name = "SymbolicLink", .body_size = sizeof(OBJECT_SYMBOLIC_LINK)
    });
    ob_event_type_init();
    ob_mutex_type_init();
    ob_semaphore_type_init();
    ob_section_type_init();
    ob_timer_type_init();
    ob_token_type_init();

    /* PEB and TEB types -- exposed in \KernelObjects\Process<PID>\ */
    ObpPebType = ob_create_type(&(OBJECT_TYPE){
        .name = "Peb", .body_size = 4096
    });
    ObpTebType = ob_create_type(&(OBJECT_TYPE){
        .name = "Teb", .body_size = 4096
    });

    klog(LOG_INFO, "ob", "Registered %u built-in types",
         (uint64_t)g_ob_type_count);

    /* Create the root namespace tree */
    ob_ns_init();

    /* PID 0 was created before ob_init -- register it retroactively */
    {
        struct task *t0 = task_get_by_pid(0);
        if (t0) {
            ob_process_create(t0);
            ob_thread_create(&t0->threads[0], 0);
        }
    }

    POST16(POST16_OB_OK);
    return BOOT_OK;
}
