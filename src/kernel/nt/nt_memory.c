/* ============================================================================
 * nt_memory.c -- NT virtual memory SSDT handlers
 *
 * NtAllocateVirtualMemory, NtFreeVirtualMemory, NtProtectVirtualMemory,
 * NtQueryVirtualMemory, NtLock/Unlock/Flush, NtRead/WriteVirtualMemory,
 * and AWE stubs.  Routes through the existing VMM and PMM infrastructure.
 * ============================================================================ */

#include "kernel/nt/nt_memory.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/pmm.h"
#include "kernel/sched/task.h"
#include "kernel/klog.h"

/* ---- Helper: convert PAGE_* protection to VMM flags --------------------- */
static uint64_t page_protect_to_vmm(uint32_t protect)
{
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;

    if (protect & PAGE_NOACCESS)
        return 0;  /* not present */

    if (protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
        flags |= VMM_FLAG_WRITABLE;

    if (!(protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        flags |= VMM_FLAG_NX;

    if (protect & PAGE_NOCACHE)
        flags |= VMM_FLAG_NOCACHE;

    if (protect & PAGE_WRITECOMBINE)
        flags |= VMM_FLAG_WRITETHROUGH;

    return flags;
}

/* ---- Helper: checked page-range validation ------------------------------
 * Rounds size up to a whole number of pages with overflow rejection, and
 * confirms base + rounded_size - 1 does not wrap.  Rejects a zero or
 * pathological (near-max) RegionSize that would round to 0 and succeed as
 * a no-op, and a base+size that would wrap the per-page loop into
 * unrelated low memory.  Returns STATUS_SUCCESS with *out_size (rounded)
 * and *out_pages set, or an NTSTATUS error. (types.h is freestanding and
 * defines no UINT64_MAX/UINTPTR_MAX, so explicit literals are used.) */
#define NT_VM_U64_MAX   0xFFFFFFFFFFFFFFFFULL
#define NT_VM_MAX_PAGES 0x100000ULL           /* 4 GiB in 4 KiB pages */

static NTSTATUS nt_vm_check_range(uintptr_t base, uint64_t size,
                                  uint64_t *out_size, uint64_t *out_pages)
{
    uint64_t rounded, pages;

    if (size == 0)
        return STATUS_INVALID_PARAMETER;
    if (size > NT_VM_U64_MAX - (VMM_PAGE_SIZE - 1))
        return STATUS_INVALID_PARAMETER;  /* rounding would overflow */

    rounded = (size + VMM_PAGE_SIZE - 1) & ~((uint64_t)VMM_PAGE_SIZE - 1);
    if (rounded == 0)
        return STATUS_INVALID_PARAMETER;

    pages = rounded / VMM_PAGE_SIZE;
    if (pages > NT_VM_MAX_PAGES)
        return STATUS_INVALID_PARAMETER;

    /* base + rounded - 1 must not wrap the address space */
    if (base != 0 && (uint64_t)base > NT_VM_U64_MAX - (rounded - 1))
        return STATUS_INVALID_PARAMETER;

    *out_size = rounded;
    *out_pages = pages;
    return STATUS_SUCCESS;
}

/* ---- Helper: convert VMM flags to PAGE_* protection --------------------- */
static uint32_t __attribute__((unused))
vmm_to_page_protect(uint64_t flags)
{
    if (!(flags & VMM_FLAG_PRESENT))
        return PAGE_NOACCESS;

    if ((flags & VMM_FLAG_WRITABLE) && !(flags & VMM_FLAG_NX))
        return PAGE_EXECUTE_READWRITE;
    if (flags & VMM_FLAG_WRITABLE)
        return PAGE_READWRITE;
    if (!(flags & VMM_FLAG_NX))
        return PAGE_EXECUTE_READ;
    return PAGE_READONLY;
}

/* ---- NtAllocateVirtualMemory (0x0050) -----------------------------------
 * a1 = HANDLE ProcessHandle, a2 = uintptr_t* BaseAddress (in/out),
 * a3 = uint64_t ZeroBits (ignored), a4 = uint64_t* RegionSize (in/out),
 * a5 = AllocationType, a6 = Protect.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtAllocateVirtualMemory_handler(uint64_t a1, uint64_t a2,
                                                 uint64_t a3, uint64_t a4,
                                                 uint64_t a5, uint64_t a6)
{
    uintptr_t *base_ptr = (uintptr_t *)a2;
    uint64_t *size_ptr = (uint64_t *)a4;
    uint32_t alloc_type = (uint32_t)a5;
    uint32_t protect = (uint32_t)a6;
    uintptr_t base;
    uint64_t size, pages, i;
    uint64_t vmm_flags;
    NTSTATUS rc;

    (void)a1; (void)a3;

    if (!base_ptr || !size_ptr)
        return STATUS_INVALID_PARAMETER;

    /* Checked page rounding + base+size wrap rejection */
    rc = nt_vm_check_range(*base_ptr, *size_ptr, &size, &pages);
    if (!NT_SUCCESS(rc))
        return rc;
    base = *base_ptr;

    /* If no base address specified, allocate contiguous physical pages */
    if (base == 0) {
        uintptr_t phys = pmm_alloc_contiguous((uint64_t)pages);
        if (phys == 0)
            return STATUS_NO_MEMORY;
        /* Identity-mapped: virt == phys */
        base = phys;
    }

    vmm_flags = page_protect_to_vmm(protect);

    if (alloc_type & MEM_COMMIT) {
        /* Map pages with requested protection.  NOTE: partial-alloc
         * rollback and per-page protection enforcement are blocked on
         * per-process page tables + a PMM free-contiguous helper --
         * tracked as deferred VirtualAlloc items in the native-API TODO. */
        for (i = 0; i < pages; i++) {
            uintptr_t addr = base + i * VMM_PAGE_SIZE;
            uintptr_t phys = vmm_get_physical(addr);

            if (phys == 0) {
                /* Not yet backed -- allocate physical frame */
                phys = pmm_alloc_frame();
                if (phys == 0)
                    return STATUS_NO_MEMORY;
                vmm_map_page(addr, phys, vmm_flags);
            }
        }
    }

    /* Zero the allocated memory */
    {
        uint8_t *p = (uint8_t *)base;
        uint64_t total = pages * VMM_PAGE_SIZE;
        uint64_t j;
        for (j = 0; j < total; j++)
            p[j] = 0;
    }

    *base_ptr = base;
    *size_ptr = size;
    return STATUS_SUCCESS;
}

/* ---- NtFreeVirtualMemory (0x0051) ---------------------------------------
 * a1 = HANDLE ProcessHandle, a2 = uintptr_t* BaseAddress (in/out),
 * a3 = uint64_t* RegionSize (in/out), a4 = FreeType.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtFreeVirtualMemory_handler(uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    uintptr_t *base_ptr = (uintptr_t *)a2;
    uint64_t *size_ptr = (uint64_t *)a3;
    uint32_t free_type = (uint32_t)a4;
    uintptr_t base;
    uint64_t size, pages, i;

    (void)a1; (void)a5; (void)a6;

    if (!base_ptr || !size_ptr)
        return STATUS_INVALID_PARAMETER;

    base = *base_ptr;
    size = *size_ptr;

    if (base == 0)
        return STATUS_MEMORY_NOT_ALLOCATED;

    if (free_type & MEM_RELEASE) {
        /* Full release (size == 0) requires per-region extent tracking to
         * know how many frames to return.  Without per-process page tables
         * that tracking does not exist, and silently returning SUCCESS
         * would leak the whole allocation -- reject explicitly so callers
         * do not believe the memory was freed.  (Deferred VirtualFree item
         * in the native-API TODO.) */
        if (size == 0)
            return STATUS_INVALID_PARAMETER;
    }

    /* Checked page rounding + base+size wrap rejection */
    {
        NTSTATUS rc = nt_vm_check_range(base, size, &size, &pages);
        if (!NT_SUCCESS(rc))
            return rc;
    }

    for (i = 0; i < pages; i++) {
        uintptr_t addr = base + i * VMM_PAGE_SIZE;
        vmm_unmap_page(addr, 1);  /* free_frame = 1: return to PMM */
    }

    *size_ptr = size;
    return STATUS_SUCCESS;
}

/* ---- NtProtectVirtualMemory (0x0052) ------------------------------------
 * a1 = HANDLE ProcessHandle, a2 = uintptr_t* BaseAddress (in/out),
 * a3 = uint64_t* RegionSize (in/out), a4 = NewProtect,
 * a5 = uint32_t* OldProtect (out).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtProtectVirtualMemory_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    uintptr_t *base_ptr = (uintptr_t *)a2;
    uint64_t *size_ptr = (uint64_t *)a3;
    uint32_t new_protect = (uint32_t)a4;
    uint32_t *old_protect = (uint32_t *)a5;
    uintptr_t base;
    uint64_t size, pages, i;
    uint64_t new_flags;

    (void)a1; (void)a6;

    if (!base_ptr || !size_ptr)
        return STATUS_INVALID_PARAMETER;

    base = *base_ptr;
    size = *size_ptr;

    if (base == 0)
        return STATUS_INVALID_PARAMETER;

    /* Checked page rounding + base+size wrap rejection */
    {
        NTSTATUS rc = nt_vm_check_range(base, size, &size, &pages);
        if (!NT_SUCCESS(rc))
            return rc;
    }
    new_flags = page_protect_to_vmm(new_protect);

    /* Get old protection from first page */
    if (old_protect) {
        uintptr_t phys = vmm_get_physical(base);
        if (phys == 0)
            *old_protect = PAGE_NOACCESS;
        else
            *old_protect = PAGE_READWRITE;  /* simplified: no PTE flag readback yet */
    }

    /* Re-map each page with new flags */
    for (i = 0; i < pages; i++) {
        uintptr_t addr = base + i * VMM_PAGE_SIZE;
        uintptr_t phys = vmm_get_physical(addr);
        if (phys != 0)
            vmm_map_page(addr, phys, new_flags);
    }

    *size_ptr = size;
    return STATUS_SUCCESS;
}

/* ---- NtQueryVirtualMemory (0x0053) --------------------------------------
 * a1 = HANDLE ProcessHandle, a2 = uintptr_t BaseAddress,
 * a3 = MemoryInformationClass, a4 = void* Buffer,
 * a5 = uint64_t Length, a6 = uint64_t* ReturnLength.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtQueryVirtualMemory_handler(uint64_t a1, uint64_t a2,
                                              uint64_t a3, uint64_t a4,
                                              uint64_t a5, uint64_t a6)
{
    uintptr_t addr = (uintptr_t)a2;
    uint32_t info_class = (uint32_t)a3;
    void *buffer = (void *)a4;
    uint64_t length = a5;
    uint64_t *ret_length = (uint64_t *)a6;

    (void)a1;

    if (info_class != MemoryBasicInformation)
        return STATUS_INVALID_INFO_CLASS;

    if (!buffer || length < sizeof(MEMORY_BASIC_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    {
        MEMORY_BASIC_INFORMATION *mbi = (MEMORY_BASIC_INFORMATION *)buffer;
        uintptr_t phys = vmm_get_physical(addr);

        mbi->BaseAddress = addr & ~((uint64_t)VMM_PAGE_SIZE - 1);
        mbi->AllocationBase = mbi->BaseAddress;
        mbi->RegionSize = VMM_PAGE_SIZE;
        mbi->_pad0 = 0;
        mbi->_pad1 = 0;

        if (phys == 0) {
            mbi->State = MEM_FREE;
            mbi->Protect = PAGE_NOACCESS;
            mbi->AllocationProtect = 0;
            mbi->Type = 0;
        } else {
            mbi->State = MEM_COMMITTED;
            mbi->Protect = PAGE_READWRITE;
            mbi->AllocationProtect = PAGE_READWRITE;
            mbi->Type = MEM_PRIVATE;
        }

        if (ret_length)
            *ret_length = sizeof(MEMORY_BASIC_INFORMATION);
    }

    return STATUS_SUCCESS;
}

/* ---- NtReadVirtualMemory (0x0057) ---------------------------------------
 * a1 = HANDLE ProcessHandle, a2 = uintptr_t BaseAddress,
 * a3 = void* Buffer, a4 = uint64_t BufferSize,
 * a5 = uint64_t* NumberOfBytesRead.
 * ----------------------------------------------------------------------- */
static NTSTATUS NtReadVirtualMemory_handler(uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    uintptr_t src = (uintptr_t)a2;
    uint8_t *dst = (uint8_t *)a3;
    uint64_t size = a4;
    uint64_t *bytes_read = (uint64_t *)a5;
    uint64_t i;

    (void)a1; (void)a6;

    if (!dst || size == 0)
        return STATUS_INVALID_PARAMETER;

    /* Identity-mapped: direct memcpy from source address.
     * For cross-process, would need to translate through target CR3. */
    {
        const uint8_t *s = (const uint8_t *)src;
        for (i = 0; i < size; i++)
            dst[i] = s[i];
    }

    if (bytes_read)
        *bytes_read = size;
    return STATUS_SUCCESS;
}

/* ---- NtWriteVirtualMemory (0x0058) -------------------------------------- */
static NTSTATUS NtWriteVirtualMemory_handler(uint64_t a1, uint64_t a2,
                                              uint64_t a3, uint64_t a4,
                                              uint64_t a5, uint64_t a6)
{
    uintptr_t dst_addr = (uintptr_t)a2;
    const uint8_t *src = (const uint8_t *)a3;
    uint64_t size = a4;
    uint64_t *bytes_written = (uint64_t *)a5;
    uint64_t i;

    (void)a1; (void)a6;

    if (!src || size == 0)
        return STATUS_INVALID_PARAMETER;

    /* Identity-mapped: direct memcpy to target address */
    {
        uint8_t *d = (uint8_t *)dst_addr;
        for (i = 0; i < size; i++)
            d[i] = src[i];
    }

    if (bytes_written)
        *bytes_written = size;
    return STATUS_SUCCESS;
}

/* ---- NtLockVirtualMemory (0x0054) --------------------------------------- */
static NTSTATUS NtLockVirtualMemory_handler(uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* All pages are already pinned (no swap yet) */
    return STATUS_SUCCESS;
}

/* ---- NtUnlockVirtualMemory (0x0055) ------------------------------------- */
static NTSTATUS NtUnlockVirtualMemory_handler(uint64_t a1, uint64_t a2,
                                               uint64_t a3, uint64_t a4,
                                               uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return STATUS_SUCCESS;
}

/* ---- NtFlushVirtualMemory (0x0056) -------------------------------------- */
static NTSTATUS NtFlushVirtualMemory_handler(uint64_t a1, uint64_t a2,
                                              uint64_t a3, uint64_t a4,
                                              uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* No dirty page tracking or backing store yet */
    return STATUS_SUCCESS;
}

/* ---- AWE stubs (0x0059-0x005B) -- owned by advanced-virtual-memory
 * Address Windowing Extensions (needs per-process physical page windows) - */
static NTSTATUS NtAWE_stub(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return STATUS_NOT_IMPLEMENTED;
}

/* ---- Registration ------------------------------------------------------- */

void nt_memory_register_ssdt(void)
{
    /* Core virtual memory (0x0050-0x0058) */
    ssdt_register(SSDT_NtAllocateVirtualMemory,  (SSDT_HANDLER)NtAllocateVirtualMemory_handler);
    ssdt_register(SSDT_NtFreeVirtualMemory,      (SSDT_HANDLER)NtFreeVirtualMemory_handler);
    ssdt_register(SSDT_NtProtectVirtualMemory,   (SSDT_HANDLER)NtProtectVirtualMemory_handler);
    ssdt_register(SSDT_NtQueryVirtualMemory,     (SSDT_HANDLER)NtQueryVirtualMemory_handler);
    ssdt_register(SSDT_NtLockVirtualMemory,      (SSDT_HANDLER)NtLockVirtualMemory_handler);
    ssdt_register(SSDT_NtUnlockVirtualMemory,    (SSDT_HANDLER)NtUnlockVirtualMemory_handler);
    ssdt_register(SSDT_NtFlushVirtualMemory,     (SSDT_HANDLER)NtFlushVirtualMemory_handler);
    ssdt_register(SSDT_NtReadVirtualMemory,      (SSDT_HANDLER)NtReadVirtualMemory_handler);
    ssdt_register(SSDT_NtWriteVirtualMemory,     (SSDT_HANDLER)NtWriteVirtualMemory_handler);

    /* AWE stubs (0x0059-0x005B) */
    ssdt_register(SSDT_NtAllocateUserPhysicalPages, (SSDT_HANDLER)NtAWE_stub);
    ssdt_register(SSDT_NtFreeUserPhysicalPages,     (SSDT_HANDLER)NtAWE_stub);
    ssdt_register(SSDT_NtMapUserPhysicalPages,      (SSDT_HANDLER)NtAWE_stub);

    klog(LOG_INFO, "nt", "NT memory: 12 handlers registered (SSDT 0x0050-0x005B)");
}
