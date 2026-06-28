/* ============================================================================
 * wx.c -- Kernel-image W^X (STRICT_KERNEL_RWX)
 *
 * Clears the WRITABLE bit on the static kernel `.text` and `.rodata` so a
 * kernel write primitive cannot patch executable code or constant data. NX
 * on non-`.text` kernel pages is already applied by vmm_apply_nx_policy
 * (Phase 0); this pass adds the WRITABLE clear that the NX policy never does.
 *
 * Ordering: run single-CPU, before AP launch (Phase 2). vmm_set_ro splits the
 * containing huge pages + clears WRITABLE with a LOCAL invlpg only (no TLB
 * shootdown), so it is unsafe once other CPUs are running. With CR0.WP pinned
 * (=1, from cpu_pin_control_regs) the kernel then honors the read-only PTE bit
 * and can no longer write its own code/constants. There are no runtime `.text`
 * patch sites today (the KPTI/AP trampolines live in separately-allocated
 * pages outside __text_start..__text_end); a future live-patch path would need
 * to clear WP via the raw write_cr0 and re-set it before any #GP can observe
 * the cleared bit, since cr0_write_safe force-re-ORs the pinned WP.
 * ============================================================================ */

#include "kernel/security/wx.h"
#include "kernel/mm/vmm.h"
#include "kernel/klog.h"

/* Linker-script section bounds (src/boot/linker.ld). */
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];

int kernel_wx_protect(void)
{
    uint64_t start = (uint64_t)(uintptr_t)__text_start;
    uint64_t end   = (uint64_t)(uintptr_t)__text_end;
    uint64_t size  = end - start;

    if (size == 0)
        return 0;   /* empty .text -- nothing to protect */

    if (vmm_set_ro(start, size) != 0) {
        klog(LOG_ERROR, "wx", "kernel .text RO failed (%p..%p)",
             (void *)(uintptr_t)start, (void *)(uintptr_t)end);
        return -1;
    }

    klog(LOG_INFO, "wx", "kernel image: .text RO (%lu KiB)",
         (uint64_t)(size / 1024));
    return 0;
}

int kernel_rodata_protect(void)
{
    uint64_t start = (uint64_t)(uintptr_t)__rodata_start;
    uint64_t end   = (uint64_t)(uintptr_t)__rodata_end;
    uint64_t size  = end - start;

    if (size == 0)
        return 0;   /* empty .rodata -- nothing to protect */

    /* [__rodata_start, __rodata_end) is widened by the linker to the .data
     * start, so it also covers .bootproto + .reloc + .firmware_capsule_refused
     * (const, never-written alloc sections). All are already NX
     * (vmm_apply_nx_policy); this adds the WRITABLE clear. */
    if (vmm_set_ro(start, size) != 0) {
        klog(LOG_ERROR, "wx", "kernel .rodata RO failed (%p..%p)",
             (void *)(uintptr_t)start, (void *)(uintptr_t)end);
        return -1;
    }

    klog(LOG_INFO, "wx", "kernel image: .rodata RO (%lu KiB)",
         (uint64_t)(size / 1024));
    return 0;
}
