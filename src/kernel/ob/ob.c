/* ============================================================================
 * ob.c — Object Manager: type registration, object allocation, init
 *
 * Implements the core Object Manager infrastructure (TODO-03 §1-§2, §4).
 * ============================================================================ */

#include "kernel/ob/ob.h"
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

/* --- ob_create_type ------------------------------------------------------ */

const OBJECT_TYPE *ob_create_type(const OBJECT_TYPE *tmpl)
{
    if (g_ob_type_count >= OB_MAX_TYPES) {
        klog(LOG_ERROR, "ob", "type table full (%u/%u) — cannot register '%s'",
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

    /* Attach default security descriptor (kernel objects get the generic SD) */
    hdr->security = (SECURITY_DESCRIPTOR *)SeCreateDefaultSD(SE_SD_TYPE_DEFAULT);

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

    klog(LOG_INFO, "ob", "Registered %u built-in types",
         (uint64_t)g_ob_type_count);

    /* Create the root namespace tree */
    ob_ns_init();

    /* PID 0 was created before ob_init — register it retroactively */
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
