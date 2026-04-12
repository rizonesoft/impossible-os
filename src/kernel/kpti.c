/* ============================================================================
 * kpti.c -- Kernel Page Table Isolation infrastructure
 *
 * Allocates the trampoline page, copies assembly stubs into it, and
 * provides the kpti_active() query. Does NOT activate CR3 swapping --
 * that is done by S4 (SYSCALL) and S5 (IDT) when they redirect LSTAR
 * and IDT entries to the trampoline page.
 *
 * XREF: 02-kernel-core/TODO-17-kernel-security-hardening.md S3
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/kpti.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/klog.h"

/* ---- Trampoline stub symbols from kpti_trampoline.asm ---- */
extern uint8_t kpti_stub_syscall_entry[];
extern uint8_t kpti_stub_syscall_entry_end[];
extern uint8_t kpti_stub_syscall_return[];
extern uint8_t kpti_stub_syscall_return_end[];
extern uint8_t kpti_stub_isr_entry[];
extern uint8_t kpti_stub_isr_entry_end[];
extern uint8_t kpti_stub_isr_return[];
extern uint8_t kpti_stub_isr_return_end[];

static int s_kpti_active = 0;
static uintptr_t s_trampoline_phys = 0;

int kpti_active(void)
{
    return s_kpti_active;
}

/* Copy a stub into the trampoline page at a given offset */
static void copy_stub(uint8_t *page, uint32_t offset,
                       const uint8_t *start, const uint8_t *end)
{
    uint32_t len = (uint32_t)(end - start);
    uint32_t i;

    if (offset + len > 4096) {
        klog(LOG_ERROR, "kpti", "Stub at offset 0x%x too large (%u bytes)",
             (uint64_t)offset, (uint64_t)len);
        return;
    }

    for (i = 0; i < len; i++)
        page[offset + i] = start[i];
}

void kpti_init(void)
{
    uintptr_t phys;
    uint8_t *page;

    /* Allocate a physical frame for the trampoline page */
    phys = pmm_alloc_frame();
    if (!phys) {
        klog(LOG_ERROR, "kpti", "Failed to allocate trampoline page");
        return;
    }

    /* Write stubs via identity mapping first (phys < 4 GiB, always accessible),
     * then map at the fixed high VA for the trampoline entry points. */
    page = (uint8_t *)phys;

    /* Zero the page */
    {
        uint32_t i;
        for (i = 0; i < 4096; i++)
            page[i] = 0;
    }

    /* Copy assembly stubs at their designated offsets */
    copy_stub(page, KPTI_OFF_SYSCALL_ENTRY,
              kpti_stub_syscall_entry, kpti_stub_syscall_entry_end);
    copy_stub(page, KPTI_OFF_SYSCALL_RETURN,
              kpti_stub_syscall_return, kpti_stub_syscall_return_end);
    copy_stub(page, KPTI_OFF_ISR_ENTRY,
              kpti_stub_isr_entry, kpti_stub_isr_entry_end);
    copy_stub(page, KPTI_OFF_ISR_RETURN,
              kpti_stub_isr_return, kpti_stub_isr_return_end);

    /* Map the physical frame at the fixed high VA (Present + Writable,
     * no NX -- this page must be executable for SYSCALL/IDT entry).
     * Supervisor-only: no User bit. S6 will also map it in user_cr3. */
    if (vmm_map_page(KPTI_TRAMPOLINE_VA, phys,
                     VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE) != 0) {
        klog(LOG_ERROR, "kpti", "Failed to map trampoline at VA 0x%x",
             KPTI_TRAMPOLINE_VA);
        pmm_free_frame(phys);
        return;
    }

    s_trampoline_phys = phys;

    klog(LOG_INFO, "kpti",
         "Trampoline page at phys 0x%x (VA 0x%x), stubs copied",
         (uint64_t)phys, KPTI_TRAMPOLINE_VA);
    klog(LOG_INFO, "kpti",
         "Stub sizes: syscall_entry=%u, syscall_return=%u, "
         "isr_entry=%u, isr_return=%u",
         (uint64_t)(kpti_stub_syscall_entry_end - kpti_stub_syscall_entry),
         (uint64_t)(kpti_stub_syscall_return_end - kpti_stub_syscall_return),
         (uint64_t)(kpti_stub_isr_entry_end - kpti_stub_isr_entry),
         (uint64_t)(kpti_stub_isr_return_end - kpti_stub_isr_return));

    /* s_kpti_active stays 0 until S4/S5 redirect LSTAR and IDT entries
     * to the trampoline page. This is infrastructure only. */
}
