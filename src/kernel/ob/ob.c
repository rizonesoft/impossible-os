/* ============================================================================
 * ob.c -- Object Manager: type registration, object allocation, init
 *
 * Implements the core Object Manager infrastructure (type registry,
 * object allocation, and namespace root setup).
 * ============================================================================ */

#include "kernel/ob/ob.h"
#include "kernel/nt/nt_types.h"
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
#include "kernel/cpu_security.h"
#include "kernel/nt/zw.h"

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
        hdr->trace = ob_trace_alloc();
        if (!hdr->trace)
            klog(LOG_WARN, "ob",
                 "trace alloc failed for type '%s'; this object untraced",
                 type->name ? type->name : "?");
    }

    /* Per-type statistics: increment live object count and lift the high-water
     * mark via the shared guarded CAS-max (ob_stat_lift_peak). Relaxed ordering:
     * these counters are pure diagnostics, they never publish or protect state. */
    {
        OBJECT_TYPE *mtype = (OBJECT_TYPE *)type;
        int32_t cur = atomic_add_fetch_relaxed(&mtype->total_objects, 1);
        ob_stat_lift_peak(&mtype->peak_objects, cur);
    }

    return OB_BODY_FROM_HEADER(hdr);
}

/* --- ObReferenceObject --------------------------------------------------- */

void ObReferenceObject(void *body)
{
    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);
    atomic_inc(&hdr->ref_count);
}

/* Reference only if still alive (ref_count > 0). Closes the resurrection race:
 * a blind atomic_inc on an object whose count is concurrently hitting 0 in
 * ObDereferenceObject would touch a header that is about to be freed. The
 * caller must still ensure the header memory itself is valid for the duration
 * (the handle-table lock holds that invariant for lookup-then-ref paths). */
int ObReferenceObjectSafe(void *body)
{
    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);
    int32_t cur = __atomic_load_n(&hdr->ref_count.val, __ATOMIC_RELAXED);
    while (cur > 0) {
        if (__atomic_compare_exchange_n(&hdr->ref_count.val, &cur, cur + 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
    }
    return -1;  /* object already at count 0 -- do not resurrect */
}

/* --- ObDereferenceObject ------------------------------------------------- */

static void ob_free_object(OBJECT_HEADER *hdr)
{
    /* Per-type statistics: decrement live object count (relaxed -- diagnostic) */
    if (hdr->type) {
        OBJECT_TYPE *mtype = (OBJECT_TYPE *)hdr->type;
        atomic_sub_fetch_relaxed(&mtype->total_objects, 1);
    }

    /* Free trace log if allocated (S15). Dump first: refcount is 0 and this is
     * the sole owner, so the per-tag leak summary is emitted exactly once,
     * race-free, right before the ring is freed. */
    if (hdr->trace) {
        ob_dump_trace_hdr((struct object_header *)hdr);
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

    /* ALWAYS decrement first so this deref unconditionally CONSUMES the caller's
     * reference -- reaching 0 therefore means this really is the last reference
     * (any concurrent holder, including a make-temporary teardown, keeps the
     * count above 0), so neither a reference leak nor a use-after-free is
     * possible here even if OB_FLAG_PERMANENT is cleared concurrently. */
    if (atomic_dec_and_test(&hdr->ref_count)) {
        /* Count hit 0. Re-check permanence AFTER the decrement: if still
         * permanent, restore the standing reference instead of freeing so a
         * later make-temporary + deref frees exactly once and never underflows.
         * The brief ref_count==0 window before the restore is benign: a
         * permanent object only reaches 0 here as it is being torn down (its
         * namespace pin already dropped), so a racing ObReferenceObjectSafe that
         * transiently observes 0 correctly declines a dying object. */
        if (hdr->flags & OB_FLAG_PERMANENT) {
            atomic_set(&hdr->ref_count, 1);
            return 1;
        }

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

    /* Safe-reference: this path is reached from lookups that may not already
     * hold a reference, so a blind inc could resurrect a count-0 object. */
    return ObReferenceObjectSafe(body);
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
    void *src_obj;
    const OBJECT_TYPE *src_type;
    uint32_t src_granted;
    uint32_t src_attrs;
    HANDLE new_h;
    uint32_t access;
    uint32_t request_ceiling;
    uint32_t final_access;

    if (!src_ht || !dst_ht || !dst_handle)
        return -1;

    entry = ObpLookupHandle(src_ht, src_handle);
    if (!entry)
        return -1;

    /* Snapshot object / access / type up front, then NEVER touch `entry`
     * again. ObpAllocateHandle below grows (reallocs) the destination table
     * when it is full; if src_ht == dst_ht that frees the array `entry` points
     * into, so any later `entry->object` read is a use-after-free. The residual
     * concurrent-close lookup race (another CPU freeing the object between the
     * lookup and this snapshot) is owned by the per-handle-table lock /
     * ObpReferenceObjectByHandle pinned-lookup work. */
    src_obj     = entry->object;
    src_granted = entry->granted_access;
    src_attrs   = entry->attributes;
    if (!src_obj)
        return -1;
    src_type = OB_HEADER_FROM_BODY(src_obj)->type;

    /* Atomic-transfer pre-check fast path: a source protected from close
     * (OBJ_PROTECT_CLOSE) can never be transferred, so fail before doing any
     * work. (A re-entrant callback that protects the source after this point is
     * caught by the close attempt below.) */
    if ((options & DUPLICATE_CLOSE_SOURCE) && (src_attrs & OBJ_PROTECT_CLOSE))
        return -1;

    /* Compute the request ceiling: a duplicate must never mint access bits the
     * source never held. DUPLICATE_SAME_ACCESS copies the source mask verbatim;
     * MAXIMUM_ALLOWED means "every right the source actually holds" (a request
     * sentinel, not a real bit, so it must not be AND-masked to zero); otherwise
     * the caller's desired_access is intersected with the source mask.
     * `request_ceiling` is the IMMUTABLE cap -- callbacks may strip below it but
     * never raise above it (neither above the source nor above what the caller
     * asked for). Full SD re-authorization of the raw desired_access lands with
     * SeAccessCheck in the security reference monitor. */
    if (options & DUPLICATE_SAME_ACCESS)
        access = src_granted;
    else if (desired_access & MAXIMUM_ALLOWED)
        access = src_granted;
    else
        access = desired_access & src_granted;
    request_ceiling = access;

    /* Pin the source object across the callbacks, allocation, and source close.
     * A duplicate pre-callback is kernel code that can re-enter the handle table
     * and close the source's LAST handle; without this reference that would free
     * `src_obj` before ObpAllocateHandle reads it (re-entrant UAF). Released on
     * every exit below. */
    ObReferenceObject(src_obj);

    /* Pre-callbacks for HANDLE_DUPLICATE: may strip access bits; a zeroed mask
     * denies the duplicate. Atomic-transfer contract: a denial means the
     * transfer never happened, so the source is left OPEN -- this deliberately
     * does NOT follow Win32's "close regardless of error", because closing a
     * source for a duplicate that did not occur is a footgun. */
    if (src_type
        && ob_invoke_pre_callbacks(OB_OPERATION_HANDLE_DUPLICATE,
                                   src_obj, src_type, &access) != 0) {
        ObDereferenceObject(src_obj);
        return -1;
    }

    /* Re-clamp to the request ceiling: a callback may strip access but must not
     * raise it above what the caller requested AND the source held. */
    access &= request_ceiling;

    /* Re-validate the source after the pre-callback. A re-entrant pre-callback
     * (kernel code) may have closed or protected the source handle. If it closed
     * the source's last handle, the type on_close already ran inside the
     * callback, so we must not hand back a destination to a torn-down object --
     * fail the duplicate. A genuine cross-CPU mutation in this same window is the
     * unlocked-lookup race owned by the per-handle-table lock /
     * ObpReferenceObjectByHandle work. */
    {
        HANDLE_TABLE_ENTRY *re = ObpLookupHandle(src_ht, src_handle);
        if (!re || re->object != src_obj
            || ((options & DUPLICATE_CLOSE_SOURCE)
                && (re->attributes & OBJ_PROTECT_CLOSE))) {
            ObDereferenceObject(src_obj);
            return -1;
        }
    }

    /* Allocate the destination BEFORE closing the source so the object's
     * handle_count never transiently reaches 0 -- otherwise ObpFreeHandle would
     * fire the type on_close (File nulls its backing, Timer detaches) and we
     * would hand back a live handle to a torn-down object. With the dest created
     * first, closing the source leaves handle_count >= 1 throughout. */
    /* No-CREATE-callback variant: this is a DUPLICATE, and the HANDLE_DUPLICATE
     * callbacks already fired above. Firing HANDLE_CREATE here too would let a
     * CREATE-only callback wrongly filter (or deny, or be notified of) the
     * duplicate -- the operation mask must stay authoritative (Win11 parity). */
    new_h = ObpAllocateHandleNoCreateCb(dst_ht, src_obj, access, attrs);
    if (new_h == INVALID_HANDLE_VALUE) {
        ObDereferenceObject(src_obj);
        return -1;
    }

    /* Hard-cap the STORED grant to the request ceiling (defense-in-depth: the
     * dup destination no longer fires a HANDLE_CREATE pre-callback that could
     * raise `access`, but `access` is already clamped to request_ceiling). The
     * stored value is the non-bypassable ceiling; capture it for the
     * post-callback so observers see the real grant. */
    {
        HANDLE_TABLE_ENTRY *de = ObpLookupHandle(dst_ht, new_h);
        if (de) {
            de->granted_access &= request_ceiling;
            final_access = de->granted_access;
        } else {
            final_access = access & request_ceiling;
        }
    }

    /* Atomic-transfer close of the source. The deterministic protected-source
     * case already failed in the pre-check; the only residual close failure is a
     * re-entrant pre-callback or another CPU protecting/closing the source after
     * our snapshot. In that race, roll the destination back (force-free, clearing
     * any protect bit the caller set on it) so we never leave both handles open
     * or return success for a transfer whose source could not be closed. The
     * rolled-back dest fires no spurious HANDLE_CREATE callback (allocated via
     * the no-create-callback variant); the residual atomicity gap is the
     * non-transactional-primitive limit owned by the per-handle-table lock /
     * ObpReferenceObjectByHandle work. */
    if ((options & DUPLICATE_CLOSE_SOURCE)
        && ObpFreeHandle(src_ht, src_handle) != 0) {
        HANDLE_TABLE_ENTRY *de = ObpLookupHandle(dst_ht, new_h);
        if (de)
            de->attributes &= ~(uint32_t)OBJ_PROTECT_CLOSE;
        ObpFreeHandle(dst_ht, new_h);
        ObDereferenceObject(src_obj);
        return -1;
    }

    /* Transfer committed: publish, fire the post-callback with the stored grant,
     * then release the pin. */
    *dst_handle = new_h;
    if (src_type)
        ob_invoke_post_callbacks(OB_OPERATION_HANDLE_DUPLICATE,
                                 src_obj, src_type, final_access);
    ObDereferenceObject(src_obj);
    return 0;
}

/* Export a live HANDLE-count statistic as an unsigned field with a floor at 0.
 * The per-type handle counters can transiently read negative because the
 * handle-table slot claim/free is not yet serialized (the exactly-once
 * accounting fix is the per-handle-table lock owned by S3 item
 * "ObpReferenceObjectByHandle"); a negative transient must NOT surface to
 * callers as a ~4-billion garbage count. Handle live counts are unsigned by
 * definition, so flooring is the correct export semantics for this diagnostic,
 * not a mask of the S3 race (which stays tracked there). The floor is SILENT
 * because a negative here is the EXPECTED transient, not a bug. */
static inline uint32_t ob_stat_export(int32_t v)
{
    return v > 0 ? (uint32_t)v : 0;
}

/* Export a live OBJECT-count statistic. Unlike handles, total_objects is only
 * touched by balanced ob_alloc_object/ob_free_object (refcount-driven, no slot
 * race), so a negative is NOT an expected transient -- it signals an alloc/free
 * or refcount accounting bug. LOUDLY report it before flooring, so the bug is
 * not hidden behind a clean-looking empty count (Codex re-adversarial [M]). */
static inline uint32_t ob_stat_export_object(int32_t v, const char *type_name)
{
    if (v < 0) {
        klog(LOG_ERROR, "ob",
             "type '%s' total_objects is %d (<0) -- alloc/free accounting bug",
             type_name ? type_name : "?", (int64_t)v);
        return 0;
    }
    return (uint32_t)v;
}

int NtQueryObject(HANDLE_TABLE *ht, HANDLE handle,
                  OBJECT_INFORMATION_CLASS info_class,
                  void *buffer, uint32_t size, uint32_t *return_length)
{
    HANDLE_TABLE_ENTRY *entry = NULL;
    OBJECT_HEADER *hdr = NULL;

    if (!ht || !buffer)
        return -1;

    /* ObjectTypesInformation enumerates the GLOBAL registered-type table and
     * needs no per-object handle (a Win11-style caller queries the type list
     * with a NULL/zero handle). Every other class resolves the handle first. */
    if (info_class != ObjectTypesInformation) {
        entry = ObpLookupHandle(ht, handle);
        if (!entry)
            return -1;
        hdr = OB_HEADER_FROM_BODY(entry->object);
    }

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
            ti->total_objects = ob_stat_export_object(atomic_read(&hdr->type->total_objects), hdr->type->name);
            ti->total_handles = ob_stat_export(atomic_read(&hdr->type->total_handles));
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
            ti->total_objects = ob_stat_export_object(atomic_read(&types[t].total_objects), types[t].name);
            ti->total_handles = ob_stat_export(atomic_read(&types[t].total_handles));
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

/* Kernel bounce-buffer depth: rows assembled per call under the directory lock
 * before being copied to the caller. The browser API is context-iterative, so a
 * larger directory is enumerated across several calls. 16 * 96 B = 1536 B keeps
 * the buffer comfortably on the kernel stack. */
#define OBQDIR_BOUNCE_ROWS 16

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
    OBJECT_DIRECTORY_INFORMATION local[OBQDIR_BOUNCE_ROWS];

    if (!ht || !buffer || !context || !return_count || buffer_count == 0)
        return -1;

    entry = ObpLookupHandle(ht, dir_handle);
    if (!entry)
        return -1;

    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpDirectoryType)
        return -1;

    dir = (OBJECT_DIRECTORY *)entry->object;

    /* Cap to the kernel bounce-buffer depth; the caller resumes via *context. */
    if (buffer_count > OBQDIR_BOUNCE_ROWS)
        buffer_count = OBQDIR_BOUNCE_ROWS;

    /* Zero the whole bounce buffer so trailing name/type_name padding never
     * leaks uninitialized kernel-stack bytes to the caller. */
    memset(local, 0, sizeof(local));

    skip = *context;
    filled = 0;
    idx = 0;

    /* Pin the directory across the enumeration. Without this, a concurrent
     * NtClose on the last handle + ObMakeTemporaryObject + ObpRemoveFromDirectory
     * on another CPU could free the body between the handle-table lookup
     * and the spin_lock_irqsave below, causing a UAF on dir->lock itself.
     * The extra ref is dropped on every exit path. */
    ObReferenceObject(dir);

    /* Build the rows into the kernel-local bounce buffer under dir->lock -- NOT
     * the caller buffer. Writing a caller (user) pointer under the IRQ-off
     * spinlock would let a faulting / invalid / kernel-range pointer #PF with
     * the lock held. The lock also keeps the list stable against a concurrent
     * ObInsertObject / ObpRemoveFromDirectory. */
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
                    local[filled].name[i] = e->name[i];
                local[filled].name[i] = '\0';
            }

            /* Copy type name from object header */
            {
                OBJECT_HEADER *obj_hdr = OB_HEADER_FROM_BODY(e->object);
                const char *tname = (obj_hdr->type && obj_hdr->type->name)
                                  ? obj_hdr->type->name : "Unknown";
                uint32_t i;
                for (i = 0; i < 31 && tname[i]; i++)
                    local[filled].type_name[i] = tname[i];
                local[filled].type_name[i] = '\0';
            }

            filled++;
        }
        spin_unlock_irqrestore(&dir->lock, irqf);
    }

    ObDereferenceObject(dir);

    /* Copy the assembled rows to the caller buffer AFTER releasing the lock,
     * through the user-copy path: probe the destination when the caller is
     * user-mode, then do the SMAP-gated copy. A bad pointer now fails cleanly
     * instead of faulting under the lock or writing kernel memory. */
    if (filled) {
        uint32_t bytes = filled * (uint32_t)sizeof(OBJECT_DIRECTORY_INFORMATION);
        if (ProbeForWriteIfUser(buffer, bytes, 1) != STATUS_SUCCESS)
            return -1;
        if (copy_to_user(buffer, local, bytes) != 0)
            return -1;
    }

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
