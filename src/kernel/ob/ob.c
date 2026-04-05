/* ============================================================================
 * ob.c -- Object Manager: type registration, object allocation, init
 *
 * Implements the core Object Manager infrastructure (TODO-03 §1-§2, §4).
 * ============================================================================ */

#include "kernel/ob/ob.h"
#include "kernel/ob/ob_callback.h"
#include "kernel/ob/ob_trace.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/ob/ob_file.h"
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
#include "kernel/security/default_sds.h"
#include "kernel/security/token.h"

extern void *memset(void *s, int c, size_t n);

/* --- Static type table --------------------------------------------------- */

static OBJECT_TYPE g_ob_types[OB_MAX_TYPES];
static uint32_t    g_ob_type_count = 0;

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
    if (g_ob_type_count >= OB_MAX_TYPES) {
        klog(LOG_ERROR, "ob", "type table full (%u/%u) -- cannot register '%s'",
             (uint64_t)g_ob_type_count, (uint64_t)OB_MAX_TYPES,
             tmpl->name ? tmpl->name : "?");
        return NULL;
    }

    OBJECT_TYPE *slot = &g_ob_types[g_ob_type_count++];
    slot->name      = tmpl->name;
    slot->body_size = tmpl->body_size;
    slot->on_close  = tmpl->on_close;
    slot->on_delete = tmpl->on_delete;
    slot->on_open   = tmpl->on_open;
    slot->on_parse  = tmpl->on_parse;
    return slot;
}

const OBJECT_TYPE *ob_get_types(uint32_t *out_count)
{
    if (out_count) *out_count = g_ob_type_count;
    return g_ob_types;
}

/* --- ob_alloc_object ----------------------------------------------------- */

void *ob_alloc_object(const OBJECT_TYPE *type)
{
    size_t total = sizeof(OBJECT_HEADER) + type->body_size;
    void *block;

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

    /* Attach security descriptor: if the current task has a token, build
     * a creator SD with the user's SID; otherwise use the kernel default. */
    {
        struct task *cur = task_current();
        if (cur && cur->token) {
            /* Use a stack-local absolute SD + DACL workspace.
             * The SD pointers reference static SIDs so they remain valid. */
            static SECURITY_DESCRIPTOR s_creator_sd;
            static uint8_t s_creator_dacl[128];
            ACCESS_TOKEN *tok = (ACCESS_TOKEN *)cur->token;
            if (SeCreateCreatorSD(&s_creator_sd, tok->UserSid,
                                  s_creator_dacl, sizeof(s_creator_dacl)) == 0)
                hdr->security = &s_creator_sd;
            else
                hdr->security = (SECURITY_DESCRIPTOR *)SeCreateDefaultSD(SE_SD_TYPE_DEFAULT);
        } else {
            hdr->security = (SECURITY_DESCRIPTOR *)SeCreateDefaultSD(SE_SD_TYPE_DEFAULT);
        }
    }

    /* Per-type tracing (S15): allocate trace log if type tracing is enabled */
    if (type->tracing_enabled) {
        extern OB_TRACE_INFO *ob_trace_alloc(void);
        hdr->trace = ob_trace_alloc();
    }

    /* Per-type statistics: increment live object count */
    {
        OBJECT_TYPE *mtype = (OBJECT_TYPE *)type;
        int32_t cur = atomic_fetch_add(&mtype->total_objects, 1) + 1;
        if ((uint32_t)cur > mtype->peak_objects)
            mtype->peak_objects = (uint32_t)cur;
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

    if (hdr->flags & OB_FLAG_PMM_ALLOC) {
        size_t total = sizeof(OBJECT_HEADER) + hdr->type->body_size;
        uint64_t frames = (total + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
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
    (void)access;  /* used by §8 security checks */

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
