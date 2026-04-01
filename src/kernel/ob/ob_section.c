/* ============================================================================
 * ob_section.c — Section object type: callbacks, create/map/unmap
 *
 * Implements TODO-03 §7: ObpSectionType backed by contiguous physical pages.
 *
 * Current memory model: all tasks share the kernel address space via
 * identity mapping, so ObMapViewOfSection returns the physical address
 * directly.  When per-process address spaces are fully implemented,
 * this will create VMM page-table entries in the target process's CR3.
 * ============================================================================ */

#include "kernel/ob/ob_section.h"
#include "kernel/ob/ob.h"
#include "kernel/mm/pmm.h"
#include "kernel/sched/task.h"
#include "kernel/klog.h"

extern void *memset(void *s, int c, size_t n);
extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* --- Callbacks ----------------------------------------------------------- */

static void section_on_delete(void *body)
{
    SECTION_OBJECT *so = (SECTION_OBJECT *)body;
    uint32_t i;

    if (so->phys_base && so->page_count > 0) {
        /* Free all backing physical pages */
        for (i = 0; i < so->page_count; i++)
            pmm_free_frame(so->phys_base + (uintptr_t)i * PMM_FRAME_SIZE);

        klog(LOG_DEBUG, "ob", "Section freed: %u pages at 0x%x",
             (uint64_t)so->page_count, (uint64_t)so->phys_base);

        so->phys_base = 0;
        so->page_count = 0;
    }
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

/* --- ObCreateSection ----------------------------------------------------- */

HANDLE ObCreateSection(HANDLE_TABLE *ht, uint32_t size, uint32_t protect,
                       const char *name)
{
    SECTION_OBJECT *so;
    uint32_t page_count;
    uintptr_t phys;
    HANDLE h;

    if (!ht || size == 0)
        return INVALID_HANDLE_VALUE;

    /* If named, try to open existing */
    if (name) {
        void *existing = NULL;
        char path[128];
        snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", name);

        if (ObLookupObjectByName(path, ObpSectionType, 0, &existing) == 0
            && existing) {
            h = ObpAllocateHandle(ht, existing, protect, 0);
            ObDereferenceObject(existing);
            return h;
        }
    }

    /* Round up to page boundary */
    page_count = (size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;

    /* Allocate contiguous physical pages */
    phys = pmm_alloc_contiguous((uint64_t)page_count);
    if (!phys) {
        klog(LOG_ERROR, "ob", "Section alloc failed: %u pages",
             (uint64_t)page_count);
        return INVALID_HANDLE_VALUE;
    }

    /* Zero the backing pages */
    memset((void *)phys, 0, (size_t)page_count * PMM_FRAME_SIZE);

    /* Allocate section object */
    so = (SECTION_OBJECT *)ob_alloc_object(ObpSectionType);
    if (!so) {
        uint32_t i;
        for (i = 0; i < page_count; i++)
            pmm_free_frame(phys + (uintptr_t)i * PMM_FRAME_SIZE);
        return INVALID_HANDLE_VALUE;
    }

    so->phys_base  = phys;
    so->size       = page_count * PMM_FRAME_SIZE;
    so->page_count = page_count;
    so->protect    = protect;
    so->map_count  = 0;
    memset(so->views, 0, sizeof(so->views));

    /* Insert into \BaseNamedObjects if named */
    if (name) {
        void *bno_dir = NULL;
        if (ObLookupObjectByName("\\BaseNamedObjects", ObpDirectoryType, 0,
                                 &bno_dir) == 0 && bno_dir) {
            ObInsertObject(so, name, bno_dir);
            ObDereferenceObject(bno_dir);
        }
    }

    h = ObpAllocateHandle(ht, so, protect, 0);
    ObDereferenceObject(so);  /* drop creation ref */

    klog(LOG_DEBUG, "ob", "Section created: %u pages at 0x%x (handle=%d)",
         (uint64_t)page_count, (uint64_t)phys, (uint64_t)(int32_t)h);

    return h;
}

/* --- ObMapViewOfSection -------------------------------------------------- */

uintptr_t ObMapViewOfSection(HANDLE_TABLE *ht, HANDLE section_handle)
{
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;
    SECTION_OBJECT *so;
    uint32_t i;
    uint32_t pid;

    if (!ht)
        return 0;

    entry = ObpLookupHandle(ht, section_handle);
    if (!entry)
        return 0;

    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpSectionType)
        return 0;

    so = (SECTION_OBJECT *)entry->object;
    if (!so->phys_base)
        return 0;

    pid = task_current()->pid;

    /* Find a free view slot */
    for (i = 0; i < SECTION_MAX_VIEWS; i++) {
        if (!so->views[i].in_use)
            break;
    }
    if (i >= SECTION_MAX_VIEWS) {
        klog(LOG_WARN, "ob", "Section view table full");
        return 0;
    }

    /* Record the mapping.
     * Currently returns identity-mapped physical address since all tasks
     * share the kernel address space.  When per-process VMM is implemented,
     * this will allocate a virtual range and create page table entries. */
    so->views[i].task_pid  = pid;
    so->views[i].base_addr = so->phys_base;
    so->views[i].size      = so->size;
    so->views[i].in_use    = 1;
    so->map_count++;

    return so->phys_base;
}

/* --- ObUnmapViewOfSection ------------------------------------------------ */

int ObUnmapViewOfSection(HANDLE_TABLE *ht, HANDLE section_handle,
                         uintptr_t base_address)
{
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;
    SECTION_OBJECT *so;
    uint32_t i;
    uint32_t pid;

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

    /* Find and remove the view */
    for (i = 0; i < SECTION_MAX_VIEWS; i++) {
        if (so->views[i].in_use &&
            so->views[i].task_pid == pid &&
            so->views[i].base_addr == base_address) {
            so->views[i].in_use = 0;
            so->map_count--;
            return 0;
        }
    }

    return -1;  /* view not found */
}
