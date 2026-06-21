/* ============================================================================
 * ob_section.c -- Section object type: callbacks, create/map/unmap
 *
 * ObpSectionType backed by contiguous physical pages.
 *
 * Current memory model: all tasks share the kernel address space via
 * identity mapping, so map views return identity-mapped physical addresses.
 * ============================================================================ */

#include "kernel/ob/ob_section.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_file.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/user_range.h"
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"
#include "kernel/fs/vfs.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_memory.h"
#include "kernel/nt/nt_section.h"
#include "kernel/klog.h"

extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);
extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* --- Callbacks ----------------------------------------------------------- */

static void section_on_delete(void *body)
{
    SECTION_OBJECT *so = (SECTION_OBJECT *)body;
    uint32_t i;
    uint64_t irqf;

    spin_lock_irqsave(&so->lk, &irqf);

    if (so->phys_base && so->page_count > 0) {
        for (i = 0; i < so->page_count; i++)
            pmm_free_frame(so->phys_base + (uintptr_t)i * PMM_FRAME_SIZE);

        klog(LOG_DEBUG, "ob", "Section freed: %u pages at 0x%x",
             (uint64_t)so->page_count, (uint64_t)so->phys_base);

        so->phys_base = 0;
        so->page_count = 0;
    }
    spin_unlock_irqrestore(&so->lk, irqf);
}

/* --- Type registration --------------------------------------------------- */

void ob_section_type_init(void)
{
    extern const OBJECT_TYPE *ObpSectionType;

    ObpSectionType = ob_create_type(&(OBJECT_TYPE){
        .name      = "Section",
        .body_size = sizeof(SECTION_OBJECT),
        .on_close  = NULL,
        .on_delete = section_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,
    });

    if (!ObpSectionType)
        klog(LOG_ERROR, "ob", "Failed to register ObpSectionType");
}

/* --- Helpers ------------------------------------------------------------- */

static void section_fill_backing_path(SECTION_OBJECT *so, struct vfs_node *node)
{
    uint32_t n;

    so->backing_path[0] = '\0';
    if (!node)
        return;
    n = vfs_get_path_from_node(node, so->backing_path,
                               SECTION_BACKING_PATH - 1u);
    if (n > 0)
        so->backing_path[n] = '\0';
}

static int section_find_view_index(SECTION_OBJECT *so, uint32_t pid,
                                   uintptr_t base)
{
    uint32_t i;

    for (i = 0; i < SECTION_MAX_VIEWS; i++) {
        if (so->views[i].in_use && so->views[i].task_pid == pid
            && so->views[i].base_addr == base)
            return (int)i;
    }
    return -1;
}

/* Page count in 64-bit to avoid 32-bit wrap on (size + PAGE - 1). */
static int section_page_count_from_size(uint32_t sz, uint32_t *out_pc)
{
    uint64_t nbytes = (uint64_t)sz;
    uint64_t npg = (nbytes + (uint64_t)PMM_FRAME_SIZE - 1ULL)
                 / (uint64_t)PMM_FRAME_SIZE;

    if (npg == 0ULL || npg > 0xFFFFFFFFULL)
        return -1;
    *out_pc = (uint32_t)npg;
    return 0;
}

/* --- ObCreateSectionEx --------------------------------------------------- */

HANDLE ObCreateSectionEx(HANDLE_TABLE *ht, uint64_t max_size_bytes,
                         uint32_t section_page_protect,
                         uint32_t allocation_attributes, HANDLE file_handle,
                         const char *name, uint32_t section_desired_access)
{
    SECTION_OBJECT *so;
    uint32_t page_count;
    uintptr_t phys;
    HANDLE h;
    uint32_t size_u32;
    int has_file;
    FILE_OBJECT *src_fo = (FILE_OBJECT *)0;

    if (!ht)
        return INVALID_HANDLE_VALUE;

    has_file = (file_handle != INVALID_HANDLE_VALUE && file_handle != 0);

    if (!has_file && max_size_bytes == 0)
        return INVALID_HANDLE_VALUE;

    if (name) {
        void *existing = NULL;
        char path[128];

        snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);

        if (ObLookupObjectByName(path, ObpSectionType, 0, &existing) == 0
            && existing) {
            h = ObpAllocateHandle(ht, existing, section_desired_access, 0);
            ObDereferenceObject(existing);
            return h;
        }
    }

    if (section_page_protect == 0)
        section_page_protect = PAGE_READWRITE;

    if ((allocation_attributes & (SEC_COMMIT | SEC_RESERVE)) == 0)
        allocation_attributes |= SEC_COMMIT;

    so = (SECTION_OBJECT *)ob_alloc_object(ObpSectionType);
    if (!so)
        return INVALID_HANDLE_VALUE;

    memset(so->backing_path, 0, sizeof(so->backing_path));
    so->section_page_protect = section_page_protect;
    so->allocation_attributes = allocation_attributes;
    so->protect = section_desired_access;

    if (has_file) {
        HANDLE_TABLE_ENTRY *fe;
        OBJECT_HEADER *fhdr;
        FILE_OBJECT *fo;
        struct vfs_node *node;
        uint64_t file_sz;
        uint64_t use_sz;

        fe = ObpLookupHandle(ht, file_handle);
        if (!fe) {
            ObDereferenceObject(so);
            return INVALID_HANDLE_VALUE;
        }
        fhdr = OB_HEADER_FROM_BODY(fe->object);
        if (fhdr->type != ObpFileType) {
            ObDereferenceObject(so);
            return INVALID_HANDLE_VALUE;
        }
        fo = (FILE_OBJECT *)fe->object;
        node = fo->vfs_node;
        if (!node || node->type != VFS_FILE) {
            ObDereferenceObject(so);
            return INVALID_HANDLE_VALUE;
        }
        file_sz = node->size;
        if (file_sz == 0) {
            ObDereferenceObject(so);
            return INVALID_HANDLE_VALUE;
        }
        use_sz = max_size_bytes ? max_size_bytes : file_sz;
        if (use_sz > file_sz)
            use_sz = file_sz;
        if (use_sz > (uint64_t)0xFFFFFFFFu) {
            ObDereferenceObject(so);
            return INVALID_HANDLE_VALUE;
        }
        size_u32 = (uint32_t)use_sz;
        section_fill_backing_path(so, node);
        src_fo = fo;
    } else {
        if (max_size_bytes > (uint64_t)0xFFFFFFFFu) {
            ObDereferenceObject(so);
            return INVALID_HANDLE_VALUE;
        }
        size_u32 = (uint32_t)max_size_bytes;
    }

    if (section_page_count_from_size(size_u32, &page_count) != 0) {
        ObDereferenceObject(so);
        return INVALID_HANDLE_VALUE;
    }

    phys = pmm_alloc_contiguous((uint64_t)page_count);
    if (!phys) {
        ObDereferenceObject(so);
        klog(LOG_ERROR, "ob", "Section alloc failed: %u pages",
             (uint64_t)page_count);
        return INVALID_HANDLE_VALUE;
    }

    memset((void *)phys, 0, (size_t)page_count * PMM_FRAME_SIZE);

    if (src_fo) {
        int rd = vfs_read(src_fo->vfs_node, 0, size_u32, (uint8_t *)phys);

        if (rd < 0 || (uint32_t)rd != size_u32) {
            uint32_t i;
            for (i = 0; i < page_count; i++)
                pmm_free_frame(phys + (uintptr_t)i * PMM_FRAME_SIZE);
            ObDereferenceObject(so);
            return INVALID_HANDLE_VALUE;
        }
    }

    so->phys_base  = phys;
    so->size       = size_u32;
    so->page_count = page_count;
    so->map_count  = 0;
    memset(so->views, 0, sizeof(so->views));

    if (name) {
        void *bno_dir = NULL;
        if (ObLookupObjectByName("\\BaseNamedObjects", ObpDirectoryType, 0,
                                 &bno_dir) == 0 && bno_dir) {
            if (ObInsertObject(so, name, bno_dir) < 0) {
                /* Collision: another thread inserted the same name between
                 * our initial lookup and here. Redirect to the winner. */
                void *winner = NULL;
                char path[128];

                ObDereferenceObject(bno_dir);
                snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);
                if (ObLookupObjectByName(path, ObpSectionType, 0, &winner) == 0
                    && winner) {
                    h = ObpAllocateHandle(ht, winner, section_desired_access,
                                          0);
                    ObDereferenceObject(winner);
                    ObDereferenceObject(so); /* discard our losing section */
                    klog(LOG_DEBUG, "ob",
                         "Section create race lost: redirected to winner");
                    return h;
                }
                /* Insert failed and no winner exists (name-cache glitch,
                 * OOM on the namespace entry, etc.). The caller requested a
                 * named section; returning an unnamed orphan would split
                 * creator and opener views silently. Fail deterministically. */
                ObDereferenceObject(so);
                klog(LOG_WARN, "ob",
                     "Section named-insert failed with no winner; aborting");
                return INVALID_HANDLE_VALUE;
            }
            ObDereferenceObject(bno_dir);
        }
    }

    h = ObpAllocateHandle(ht, so, section_desired_access, 0);
    ObDereferenceObject(so);

    klog(LOG_DEBUG, "ob", "Section created: %u pages at 0x%x (handle=%d)",
         (uint64_t)page_count, (uint64_t)phys, (int32_t)h);

    return h;
}

/* --- ObCreateSection (legacy) -------------------------------------------- */

HANDLE ObCreateSection(HANDLE_TABLE *ht, uint32_t size, uint32_t protect,
                       const char *name)
{
    uint32_t page_prot = PAGE_READWRITE;

    if (!(protect & SECTION_MAP_WRITE))
        page_prot = PAGE_READONLY;

    return ObCreateSectionEx(ht, (uint64_t)size, page_prot, SEC_COMMIT,
                             INVALID_HANDLE_VALUE, name, protect);
}

/* --- ObOpenSection ------------------------------------------------------- */

HANDLE ObOpenSection(HANDLE_TABLE *ht, uint32_t desired_access,
                     const char *name)
{
    void *body = NULL;
    char path[128];
    HANDLE h;

    if (!ht || !name)
        return INVALID_HANDLE_VALUE;

    snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);

    if (ObLookupObjectByName(path, ObpSectionType, 0, &body) != 0 || !body)
        return INVALID_HANDLE_VALUE;

    h = ObpAllocateHandle(ht, body, desired_access, 0);
    ObDereferenceObject(body);
    return h;
}

/* --- ObMapViewOfSectionFull ---------------------------------------------- */

uintptr_t ObMapViewOfSectionFull(HANDLE_TABLE *ht, HANDLE section_handle,
                                 uint32_t view_owner_pid,
                                 uint64_t section_offset_bytes,
                                 uint32_t view_size_bytes)
{
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;
    SECTION_OBJECT *so;
    uint32_t i;
    uintptr_t map_base;
    uint32_t span;
    uint64_t irqf;

    if (!ht)
        return 0;

    entry = ObpLookupHandle(ht, section_handle);
    if (!entry)
        return 0;

    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpSectionType)
        return 0;

    so = (SECTION_OBJECT *)entry->object;

    spin_lock_irqsave(&so->lk, &irqf);

    if (!so->phys_base) {
        spin_unlock_irqrestore(&so->lk, irqf);
        return 0;
    }

    if (section_offset_bytes >= (uint64_t)so->size) {
        spin_unlock_irqrestore(&so->lk, irqf);
        return 0;
    }

    /* Require a page-aligned section offset (NT requires SectionOffset aligned
     * to allocation granularity; page alignment is the floor). vmm_share_user_page
     * maps page-aligned physical frames, so an unaligned offset would silently
     * map bytes BEFORE the requested offset and omit the tail -- a wrong-span /
     * out-of-bounds map of the backing frames. */
    if (section_offset_bytes & (PMM_FRAME_SIZE - 1)) {
        spin_unlock_irqrestore(&so->lk, irqf);
        return 0;
    }

    if (view_size_bytes == 0)
        span = so->size - (uint32_t)section_offset_bytes;
    else {
        if ((uint64_t)view_size_bytes
            > (uint64_t)so->size - section_offset_bytes) {
            spin_unlock_irqrestore(&so->lk, irqf);
            return 0;
        }
        span = view_size_bytes;
    }

    for (i = 0; i < SECTION_MAX_VIEWS; i++) {
        if (!so->views[i].in_use)
            break;
    }
    if (i >= SECTION_MAX_VIEWS) {
        spin_unlock_irqrestore(&so->lk, irqf);
        klog(LOG_WARN, "ob", "Section view table full");
        return 0;
    }

    /* Resolve the target task and decide which kind of mapping to install:
     *
     *   - USER task (cr3 != 0) -- install per-task PTEs at a bump-allocated
     *     VA in the SECTION_VIEW_BASE range (0x10000000+). Section frames
     *     get User+Writable bits only in THIS task's address space, so
     *     other processes cannot touch them through the same VA. Return
     *     the user VA so the caller can dereference it. Required for
     *     sys_shmem_map to produce a pointer the user binary can write to.
     *
     *   - KERNEL task (cr3 == 0) -- return the identity-mapped physical
     *     address as before. Kernel consumers (tests, ipc.c kernel-side
     *     paths) dereference via kernel identity map; no user PTE needed.
     *
     * The previous implementation ALWAYS returned `so->phys_base + offset`,
     * which works for kernel callers but handed user callers a kernel
     * VA they could not dereference without a #PF. Observed 2026-04-22
     * as test_ipc's sys_shmem_map round-trip panic'd on the first write
     * with CR2 = the returned phys address, User-bit error. */
    struct task *t = (view_owner_pid < TASK_MAX) ?
                     task_get_by_pid(view_owner_pid) : (struct task *)0;

    if (t && t->cr3) {
        /* User task: install the view at a bump-allocated VA. */
        uintptr_t user_va;
        uint32_t pages = (span + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
        uint32_t pi;

        /* Initialize the bump pointer lazily on first view. */
        if (t->next_section_view_va == 0)
            t->next_section_view_va = SECTION_VIEW_BASE;

        /* Check we have room in the view range. */
        if (t->next_section_view_va
            + (uintptr_t)pages * PMM_FRAME_SIZE > SECTION_VIEW_LIMIT) {
            so->views[i].in_use = 0;  /* roll back the slot */
            spin_unlock_irqrestore(&so->lk, irqf);
            klog(LOG_WARN, "ob",
                 "Section view VA range exhausted for PID %u (limit 0x%x)",
                 (uint64_t)view_owner_pid, (uint64_t)SECTION_VIEW_LIMIT);
            return 0;
        }

        user_va = t->next_section_view_va;
        t->next_section_view_va += (uintptr_t)pages * PMM_FRAME_SIZE;

        /* Install PTEs for each page of the view in the task's cr3.
         * vmm_share_user_page preserves existing frame content (no
         * zero-fill) because section backing may already hold data
         * from other mappers or named-section initialization. */
        for (pi = 0; pi < pages; pi++) {
            uintptr_t va_pi   = user_va   + (uintptr_t)pi * PMM_FRAME_SIZE;
            uintptr_t phys_pi = so->phys_base
                              + (uintptr_t)section_offset_bytes
                              + (uintptr_t)pi * PMM_FRAME_SIZE;
            if (vmm_share_user_page(t->cr3, va_pi, phys_pi) != 0) {
                /* Partial mapping failure -- UNSHARE the PTEs already installed
                 * before returning. The view is never recorded (in_use stays 0)
                 * and the section is never pinned on this path, so nothing else
                 * can ever tear these down; leaving them mapped would strand user
                 * PTEs onto frames that section_on_delete is free to release ==
                 * a cross-process use-after-free / info-leak boundary. */
                uint32_t pj;
                for (pj = 0; pj < pi; pj++)
                    vmm_unshare_user_page(t->cr3,
                        user_va + (uintptr_t)pj * PMM_FRAME_SIZE);
                so->views[i].in_use = 0;
                spin_unlock_irqrestore(&so->lk, irqf);
                klog(LOG_WARN, "ob",
                     "ObMapViewOfSection: vmm_share_user_page failed at VA 0x%x",
                     (uint64_t)va_pi);
                return 0;
            }
        }

        map_base = user_va;
    } else {
        /* Kernel consumer -- identity-mapped physical address works. */
        map_base = so->phys_base + (uintptr_t)section_offset_bytes;
    }

    so->views[i].task_pid       = view_owner_pid;
    so->views[i].base_addr      = map_base;
    so->views[i].section_offset = section_offset_bytes;
    so->views[i].view_bytes     = span;
    so->views[i].in_use         = 1;
    so->map_count++;

    /* Pin the section BEFORE dropping the lock -- otherwise a concurrent
     * NtClose could drive refcount to zero between unlock and
     * ObReferenceObject and free the backing underneath us. */
    ObReferenceObject(so);

    spin_unlock_irqrestore(&so->lk, irqf);
    return map_base;
}

uintptr_t ObMapViewOfSection(HANDLE_TABLE *ht, HANDLE section_handle,
                             uint32_t view_owner_pid)
{
    return ObMapViewOfSectionFull(ht, section_handle, view_owner_pid, 0, 0);
}

/* --- ObUnmapViewOfSection ------------------------------------------------ */

int ObUnmapViewOfSection(HANDLE_TABLE *ht, HANDLE section_handle,
                         uintptr_t base_address)
{
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;
    SECTION_OBJECT *so;
    int idx;
    uint32_t pid;
    uint64_t irqf;

    if (!ht)
        return -1;

    entry = ObpLookupHandle(ht, section_handle);
    if (!entry)
        return -1;

    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpSectionType)
        return -1;

    so = (SECTION_OBJECT *)entry->object;
    pid = task_current()->pid;

    spin_lock_irqsave(&so->lk, &irqf);
    idx = section_find_view_index(so, pid, base_address);
    if (idx < 0) {
        spin_unlock_irqrestore(&so->lk, irqf);
        return -1;
    }

    so->views[(uint32_t)idx].in_use = 0;
    so->map_count--;
    spin_unlock_irqrestore(&so->lk, irqf);

    /* Drop the pin that ObMapViewOfSectionFull took on view install. */
    ObDereferenceObject(so);
    return 0;
}

/* --- ObUnmapViewOfSectionByBase ----------------------------------------- */

int ObUnmapViewOfSectionByBase(HANDLE_TABLE *ht, uint32_t view_owner_pid,
                               uintptr_t base_address)
{
    uint32_t slot;

    if (!ht || !ht->entries)
        return -1;

    for (slot = 0; slot < ht->capacity; slot++) {
        HANDLE_TABLE_ENTRY *e = &ht->entries[slot];
        OBJECT_HEADER *hdr;
        SECTION_OBJECT *so;
        int idx;

        if (!e->object)
            continue;
        hdr = OB_HEADER_FROM_BODY(e->object);
        if (hdr->type != ObpSectionType)
            continue;
        so = (SECTION_OBJECT *)e->object;
        {
            uint64_t irqf;

            spin_lock_irqsave(&so->lk, &irqf);
            idx = section_find_view_index(so, view_owner_pid, base_address);
            if (idx >= 0) {
                /* Snapshot the view's VA range + pid under the lock so the
                 * PTE teardown can happen after the unlock (vmm_unshare_user_page
                 * may do its own TLB operations that would deadlock against
                 * other per-section waiters if held). */
                uintptr_t view_base  = so->views[(uint32_t)idx].base_addr;
                uint32_t  view_bytes = so->views[(uint32_t)idx].view_bytes;
                uint32_t  view_pid   = so->views[(uint32_t)idx].task_pid;

                so->views[(uint32_t)idx].in_use = 0;
                so->map_count--;
                spin_unlock_irqrestore(&so->lk, irqf);

                /* Tear down the per-task PTEs that ObMapViewOfSectionFull
                 * installed via vmm_share_user_page. Skip for kernel-task
                 * mappings (cr3 == 0) where no user PTEs exist. The phys
                 * frames themselves stay alive -- vmm_unshare_user_page
                 * does NOT pmm_free_frame; the section's on_delete
                 * callback reclaims them when the last handle closes. */
                struct task *t = (view_pid < TASK_MAX) ?
                                 task_get_by_pid(view_pid) : (struct task *)0;
                if (t && t->cr3 &&
                    view_base >= SECTION_VIEW_BASE &&
                    view_base <  SECTION_VIEW_LIMIT) {
                    uint32_t pages =
                        (view_bytes + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
                    uint32_t pi;
                    for (pi = 0; pi < pages; pi++) {
                        vmm_unshare_user_page(
                            t->cr3,
                            view_base + (uintptr_t)pi * PMM_FRAME_SIZE);
                    }
                }

                /* Drop the pin that ObMapViewOfSectionFull took. */
                ObDereferenceObject(so);
                return 0;
            }
            spin_unlock_irqrestore(&so->lk, irqf);
        }
    }
    return -1;
}

/* --- ObExtendSectionObject ----------------------------------------------- */

NTSTATUS ObExtendSectionObject(SECTION_OBJECT *so, uint64_t new_max_bytes)
{
    uintptr_t new_phys;
    uint32_t new_page_count;
    uint32_t old_page_count;
    uintptr_t old_phys;
    uint32_t old_size;
    uint32_t copy_sz;
    uint64_t irqf;

    if (!so)
        return STATUS_INVALID_PARAMETER;

    if (so->allocation_attributes & SEC_IMAGE)
        return STATUS_SECTION_NOT_EXTENDED;

    if (new_max_bytes > (uint64_t)0xFFFFFFFFu)
        return STATUS_INVALID_PARAMETER;

    if (section_page_count_from_size((uint32_t)new_max_bytes,
                                     &new_page_count) != 0)
        return STATUS_INVALID_PARAMETER;

    spin_lock_irqsave(&so->lk, &irqf);

    if (!so->phys_base) {
        spin_unlock_irqrestore(&so->lk, irqf);
        return STATUS_INVALID_PARAMETER;
    }

    if (new_max_bytes <= (uint64_t)so->size) {
        spin_unlock_irqrestore(&so->lk, irqf);
        return STATUS_SUCCESS;
    }

    /* Extending while views are mapped would strand callers on freed pages
     * once the backing is swapped. Reject until a versioned backing handoff
     * exists (per-process page tables + VAD). */
    if (so->map_count > 0) {
        spin_unlock_irqrestore(&so->lk, irqf);
        return STATUS_SECTION_NOT_EXTENDED;
    }

    copy_sz = so->size;
    old_phys = so->phys_base;
    old_page_count = so->page_count;
    old_size = so->size;

    spin_unlock_irqrestore(&so->lk, irqf);

    new_phys = pmm_alloc_contiguous((uint64_t)new_page_count);
    if (!new_phys)
        return STATUS_NO_MEMORY;

    memset((void *)new_phys, 0, (size_t)new_page_count * PMM_FRAME_SIZE);
    memcpy((void *)new_phys, (void *)old_phys, (size_t)copy_sz);

    spin_lock_irqsave(&so->lk, &irqf);

    /* Abort if another thread mutated phys_base/size OR installed a view
     * during the unlocked alloc/copy window. A new view would be pinned to
     * old_phys which we are about to free; returning STATUS_RETRY lets the
     * caller re-attempt after the racing view is unmapped. */
    if (so->phys_base != old_phys || so->size != old_size
        || so->map_count != 0) {
        spin_unlock_irqrestore(&so->lk, irqf);
        {
            uint32_t i;
            for (i = 0; i < new_page_count; i++)
                pmm_free_frame(new_phys + (uintptr_t)i * PMM_FRAME_SIZE);
        }
        return STATUS_SECTION_NOT_EXTENDED;
    }

    so->phys_base = new_phys;
    so->size = (uint32_t)new_max_bytes;
    so->page_count = new_page_count;

    spin_unlock_irqrestore(&so->lk, irqf);

    if (old_phys && old_page_count > 0) {
        uint32_t i;
        for (i = 0; i < old_page_count; i++)
            pmm_free_frame(old_phys + (uintptr_t)i * PMM_FRAME_SIZE);
    }

    return STATUS_SUCCESS;
}

/* --- ObQuerySectionObject ------------------------------------------------ */

NTSTATUS ObQuerySectionObject(SECTION_OBJECT *so, uint32_t info_class,
                               void *buffer, uint64_t buffer_length,
                               uint64_t *return_length)
{
    uint64_t rl = 0;
    uint64_t irqf;
    void *first_base = (void *)0;
    uint32_t sz_copy;
    uint32_t attr_copy;
    uint32_t vi;

    if (!so)
        return STATUS_INVALID_PARAMETER;

    if (!buffer)
        return STATUS_INVALID_PARAMETER;

    if (info_class == SectionBasicInformation) {
        SECTION_BASIC_INFORMATION *bi = (SECTION_BASIC_INFORMATION *)buffer;

        if (buffer_length < sizeof(SECTION_BASIC_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;

        spin_lock_irqsave(&so->lk, &irqf);
        for (vi = 0; vi < SECTION_MAX_VIEWS; vi++) {
            if (so->views[vi].in_use) {
                first_base = (void *)so->views[vi].base_addr;
                break;
            }
        }
        sz_copy = so->size;
        attr_copy = so->allocation_attributes;
        spin_unlock_irqrestore(&so->lk, irqf);

        bi->BaseAddress = first_base;
        bi->MaximumSize.QuadPart = (int64_t)(uint64_t)sz_copy;
        bi->AllocationAttributes = attr_copy;
        bi->_pad = 0;
        rl = sizeof(SECTION_BASIC_INFORMATION);
    } else if (info_class == SectionImageInformation) {
        SECTION_IMAGE_INFORMATION *ii;

        if (!(so->allocation_attributes & SEC_IMAGE))
            return STATUS_INVALID_PARAMETER;

        if (buffer_length < sizeof(SECTION_IMAGE_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;

        ii = (SECTION_IMAGE_INFORMATION *)buffer;
        memset(ii, 0, sizeof(*ii));
        rl = sizeof(SECTION_IMAGE_INFORMATION);
    } else
        return STATUS_INVALID_INFO_CLASS;

    if (return_length)
        *return_length = rl;
    return STATUS_SUCCESS;
}

/* --- ObAreMappedFilesTheSame --------------------------------------------- */

static int section_copy_backing_path(HANDLE_TABLE *ht, uint32_t pid,
                                     uintptr_t addr, char *out, uint32_t cap)
{
    uint32_t slot;

    if (!ht || !ht->entries || !out || cap < 2)
        return 0;

    for (slot = 0; slot < ht->capacity; slot++) {
        HANDLE_TABLE_ENTRY *e = &ht->entries[slot];
        OBJECT_HEADER *hdr;
        SECTION_OBJECT *so;
        uint32_t vi;
        uint64_t irqf;
        int found = 0;

        if (!e->object)
            continue;
        hdr = OB_HEADER_FROM_BODY(e->object);
        if (hdr->type != ObpSectionType)
            continue;
        so = (SECTION_OBJECT *)e->object;

        spin_lock_irqsave(&so->lk, &irqf);
        for (vi = 0; vi < SECTION_MAX_VIEWS; vi++) {
            if (!so->views[vi].in_use || so->views[vi].task_pid != pid)
                continue;
            if (so->views[vi].base_addr == addr
                && so->views[vi].view_bytes > 0) {
                uint32_t j;
                if (so->backing_path[0] == '\0') {
                    spin_unlock_irqrestore(&so->lk, irqf);
                    return 0;
                }
                for (j = 0; j < cap - 1u && so->backing_path[j]; j++)
                    out[j] = so->backing_path[j];
                out[j] = '\0';
                found = 1;
                break;
            }
        }
        spin_unlock_irqrestore(&so->lk, irqf);
        if (found)
            return 1;
    }
    return 0;
}

int ObAreMappedFilesTheSame(HANDLE_TABLE *ht, uint32_t pid,
                            uintptr_t addr1, uintptr_t addr2)
{
    char p1[SECTION_BACKING_PATH];
    char p2[SECTION_BACKING_PATH];
    uint32_t i;

    if (!section_copy_backing_path(ht, pid, addr1, p1, sizeof(p1)))
        return 0;
    if (!section_copy_backing_path(ht, pid, addr2, p2, sizeof(p2)))
        return 0;

    for (i = 0; p1[i] || p2[i]; i++) {
        if (p1[i] != p2[i])
            return 0;
    }
    return 1;
}
