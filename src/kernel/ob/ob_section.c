/* ============================================================================
 * ob_section.c -- Section object type: callbacks, create/map/unmap
 *
 * Implements TODO-03 §7: ObpSectionType backed by contiguous physical pages.
 *
 * Current memory model: all tasks share the kernel address space via
 * identity mapping, so map views return identity-mapped physical addresses.
 * ============================================================================ */

#include "kernel/ob/ob_section.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_file.h"
#include "kernel/mm/pmm.h"
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
            ObInsertObject(so, name, bno_dir);
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

    map_base = so->phys_base + (uintptr_t)section_offset_bytes;

    so->views[i].task_pid       = view_owner_pid;
    so->views[i].base_addr      = map_base;
    so->views[i].section_offset = section_offset_bytes;
    so->views[i].view_bytes     = span;
    so->views[i].in_use         = 1;
    so->map_count++;

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
                so->views[(uint32_t)idx].in_use = 0;
                so->map_count--;
                spin_unlock_irqrestore(&so->lk, irqf);
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

    if (so->phys_base != old_phys || so->size != old_size) {
        spin_unlock_irqrestore(&so->lk, irqf);
        {
            uint32_t i;
            for (i = 0; i < new_page_count; i++)
                pmm_free_frame(new_phys + (uintptr_t)i * PMM_FRAME_SIZE);
        }
        return STATUS_UNSUCCESSFUL;
    }

    so->phys_base = new_phys;
    so->size = (uint32_t)new_max_bytes;
    so->page_count = new_page_count;

    {
        uint32_t vi;
        for (vi = 0; vi < SECTION_MAX_VIEWS; vi++) {
            if (!so->views[vi].in_use)
                continue;
            so->views[vi].base_addr = so->phys_base
                + (uintptr_t)so->views[vi].section_offset;
        }
    }

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
