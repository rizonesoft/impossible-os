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

    /* MSR_IA32_STAR layout:
     * Bits 63:48 = SYSRET CS/SS base selector (user code CS = base+16, SS = base+8)
     * Bits 47:32 = SYSCALL CS/SS base selector (kernel code CS = base, SS = base+8)
     * Bits 31:0  = reserved (EIP for 32-bit SYSCALL, unused in long mode)
     *
     * GDT layout: 0x08=KCODE, 0x10=KDATA, 0x18=UCODE, 0x20=UDATA
     *
     * SYSCALL: CS = STAR[47:32] = 0x08 (kernel code)
     *          SS = STAR[47:32]+8 = 0x10 (kernel data)
     *
     * SYSRET:  CS = STAR[63:48]+16 = 0x08+16 = 0x18 (user code) -- only if STAR[63:48]=0x08
     *          SS = STAR[63:48]+8  = 0x08+8  = 0x10 ... WRONG, that's kernel data.
     *
     * Actually for SYSRET in long mode:
     *   CS = STAR[63:48]+16 with RPL=3
     *   SS = STAR[63:48]+8 with RPL=3
     *
     * So STAR[63:48] = GDT_USER_CODE - 16 = 0x18 - 16 = 0x08
     * That gives: SYSRET CS = 0x08+16 = 0x18 | 3 = user code ring 3
     *             SYSRET SS = 0x08+8  = 0x10 | 3 = ... kernel data ring 3?
     *
     * Actually the standard trick: STAR[63:48] should be such that +8 = user data.
     * GDT_USER_DATA = 0x20. So STAR[63:48] = 0x20 - 8 = 0x18.
     * Then: SYSRET SS = 0x18+8 = 0x20 | 3 = user data ring 3 (correct)
     *       SYSRET CS = 0x18+16 = 0x28 ... but 0x28 is the TSS!
     *
     * The issue: in the Intel/AMD spec, SYSRET in 64-bit mode:
     *   CS.sel = STAR[63:48]+16, SS.sel = STAR[63:48]+8
     * But GDT_USER_CODE=0x18, GDT_USER_DATA=0x20.
     * Standard solution: GDT must be ordered: KCODE(0x08), KDATA(0x10),
     *   UDATA(0x18), UCODE(0x20) -- then STAR[63:48]=0x10:
     *   SYSRET CS = 0x10+16 = 0x20 = UCODE, SS = 0x10+8 = 0x18 = UDATA
     *
     * BUT our GDT is KCODE(0x08), KDATA(0x10), UCODE(0x18), UDATA(0x20).
     * This is the WRONG order for SYSRET. We need UDATA before UCODE.
     *
     * For now: do NOT enable SYSCALL/SYSRET. The GDT needs to be reordered
     * first. Log a warning and keep INT 0x80.
     *
     * TODO: Reorder GDT entries so UDATA comes before UCODE (swap 0x18/0x20),
     * update all ring-3 transitions that hardcode selectors, then enable.
     */

    /* Check if GDT order is compatible with SYSRET */
    if (GDT_USER_DATA != GDT_USER_CODE + 8) {
        /* GDT order incompatible with SYSRET (need UDATA = UCODE + 8).
         * Current: UCODE=0x18, UDATA=0x20 (reversed).
         * SYSRET requires: UCODE = STAR[63:48]+16, UDATA = STAR[63:48]+8.
         * This means UDATA must be at a LOWER selector than UCODE.
         * Keep INT 0x80 path until GDT is reordered. */
        klog(LOG_WARN, "syscall",
             "SYSCALL/SYSRET deferred: GDT order incompatible "
             "(UCODE=0x%x, UDATA=0x%x -- need UDATA before UCODE)",
             (uint32_t)GDT_USER_CODE, (uint32_t)GDT_USER_DATA);
        return;
    }

    /* Set up STAR */
    star = ((uint64_t)GDT_KERNEL_CODE << 32) |
           ((uint64_t)(GDT_USER_CODE - 16) << 48);
    msr_write(MSR_IA32_STAR, star);

    /* Set LSTAR to assembly entry point */
    msr_write(MSR_IA32_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* FMASK: clear IF on entry (bit 9) */
    msr_write(MSR_IA32_FMASK, 0x200);

    /* Enable SYSCALL/SYSRET in EFER */
    efer = msr_read(MSR_IA32_EFER);
    efer |= EFER_SCE;
    msr_write(MSR_IA32_EFER, efer);

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
