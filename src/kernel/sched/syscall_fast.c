/* ============================================================================
 * syscall_fast.c -- SYSCALL/SYSRET MSR setup and fast dispatch
 *
 * Configures IA32_STAR, IA32_LSTAR, IA32_FMASK, and enables EFER_SCE.
 * The assembly entry point (syscall_entry.asm) calls syscall_dispatch_fast()
 * which routes to the SSDT.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/msr.h"
#include "kernel/gdt.h"
#include "kernel/klog.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/ssdt.h"

/* Assembly entry point defined in syscall_entry.asm */
extern void syscall_entry(void);

void syscall_init_fast(void)
{
    uint64_t star, efer;

    /* GDT layout: 0x08=KCODE, 0x10=KDATA, 0x18=UDATA, 0x20=UCODE.
     * SYSRET in 64-bit mode: CS=STAR[63:48]+16, SS=STAR[63:48]+8.
     * With STAR[63:48]=0x10: CS=0x20 (user code), SS=0x18 (user data). */

    /* Check if GDT order is compatible with SYSRET.
     * SYSRET: CS = STAR[63:48]+16, SS = STAR[63:48]+8.
     * So GDT_USER_CODE must be GDT_USER_DATA + 8 (data before code). */
    if (GDT_USER_CODE != GDT_USER_DATA + 8) {
        klog(LOG_WARN, "syscall",
             "SYSCALL/SYSRET deferred: GDT order incompatible "
             "(UCODE=0x%x, UDATA=0x%x -- need UCODE = UDATA + 8)",
             (uint32_t)GDT_USER_CODE, (uint32_t)GDT_USER_DATA);
        return;
    }

    /* Set up STAR.
     * Bits 47:32 = SYSCALL selectors: CS = this value, SS = this+8.
     *   -> GDT_KERNEL_CODE (0x08): CS=0x08 (kernel code), SS=0x10 (kernel data)
     * Bits 63:48 = SYSRET selectors: SS = this+8, CS = this+16.
     *   -> GDT_USER_DATA - 8 = 0x10: SS=0x18 (user data), CS=0x20 (user code) */
    star = ((uint64_t)GDT_KERNEL_CODE << 32) |
           ((uint64_t)(GDT_USER_DATA - 8) << 48);
    msr_write(MSR_IA32_STAR, star);

    /* Set LSTAR to assembly entry point */
    msr_write(MSR_IA32_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* FMASK: clear IF on entry (bit 9) */
    msr_write(MSR_IA32_FMASK, 0x200);

    /* Enable SYSCALL/SYSRET in EFER */
    efer = msr_read(MSR_IA32_EFER);
    efer |= EFER_SCE;
    msr_write(MSR_IA32_EFER, efer);

    /* --- MSR readback verification (canary) ---
     * Read back all written MSRs and verify. If any mismatch,
     * disable EFER_SCE and fall back to INT 0x80. */
    {
        uint64_t rb_star  = msr_read(MSR_IA32_STAR);
        uint64_t rb_lstar = msr_read(MSR_IA32_LSTAR);
        uint64_t rb_fmask = msr_read(MSR_IA32_FMASK);
        uint64_t rb_efer  = msr_read(MSR_IA32_EFER);
        int ok = 1;

        if (rb_star != star) {
            klog(LOG_ERROR, "syscall",
                 "STAR readback mismatch: wrote 0x%x, read 0x%x",
                 star, rb_star);
            ok = 0;
        }
        if (rb_lstar != (uint64_t)(uintptr_t)syscall_entry) {
            klog(LOG_ERROR, "syscall",
                 "LSTAR readback mismatch: wrote 0x%x, read 0x%x",
                 (uint64_t)(uintptr_t)syscall_entry, rb_lstar);
            ok = 0;
        }
        if (rb_fmask != 0x200) {
            klog(LOG_ERROR, "syscall",
                 "FMASK readback mismatch: wrote 0x200, read 0x%x",
                 rb_fmask);
            ok = 0;
        }
        if (!(rb_efer & EFER_SCE)) {
            klog(LOG_ERROR, "syscall", "EFER.SCE not set after write");
            ok = 0;
        }

        if (!ok) {
            /* Disable SYSCALL and fall back to INT 0x80 */
            efer = msr_read(MSR_IA32_EFER);
            efer &= ~EFER_SCE;
            msr_write(MSR_IA32_EFER, efer);
            klog(LOG_WARN, "syscall",
                 "SYSCALL/SYSRET DISABLED due to MSR readback failure -- using INT 0x80");
            return;
        }
    }

    klog(LOG_INFO, "syscall",
         "SYSCALL/SYSRET enabled: LSTAR=0x%x, STAR=0x%x, FMASK=0x200",
         (uint64_t)(uintptr_t)syscall_entry, star);
}

/* C dispatcher called from syscall_entry.asm */
NTSTATUS syscall_dispatch_fast(uint64_t number,
                               uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4,
                               uint64_t a5)
{
    return ssdt_dispatch((uint32_t)number, a1, a2, a3, a4, a5, 0);
}
