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
#include "kernel/nt/zw.h"
#include "kernel/sched/task.h"
#include "kernel/ob/teb.h"

/* Assembly entry point defined in syscall_entry.asm */
extern void syscall_entry(void);

/* RFLAGS bits cleared on SYSCALL entry (written to IA32_FMASK).
 * Clearing only IF (0x200) leaves user-controlled TF/DF/IOPL/NT/AC live in
 * ring 0 until SYSRET: TF single-steps the kernel entry, DF violates the SysV
 * C ABI (DF=0 on entry) so compiler/string ops can run backwards, IOPL/AC/NT
 * carry hostile state. Mask the Linux/Windows-parity set:
 *   TF 0x100 | IF 0x200 | DF 0x400 | IOPL 0x3000 | NT 0x4000 | AC 0x40000 */
#define SYSCALL_FMASK  0x47700ULL

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

    /* FMASK: clear hostile user RFLAGS on entry (TF|IF|DF|IOPL|NT|AC) */
    msr_write(MSR_IA32_FMASK, SYSCALL_FMASK);

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
        if (rb_fmask != SYSCALL_FMASK) {
            klog(LOG_ERROR, "syscall",
                 "FMASK readback mismatch: wrote 0x%x, read 0x%x",
                 (uint64_t)SYSCALL_FMASK, rb_fmask);
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

/* C dispatcher called from syscall_entry.asm.
 * Always called from ring 3 (SYSCALL instruction), so previous mode = UserMode.
 * On error, propagates Win32 error code to TEB->LastErrorValue (gs:[0x68]). */
NTSTATUS syscall_dispatch_fast(uint64_t number,
                               uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4,
                               uint64_t a5)
{
    NTSTATUS result;
    /* Ring-3 execution evidence for the user-mode test launcher. This path
     * has no saved frame to inspect, but SYSCALL reaches it only from ring
     * 3 (the same property ssdt_syscall_enter() below relies on to set
     * previous_mode unconditionally), and SYSRET returns to GDT_USER_CODE
     * with RPL 3 -- so that selector IS the caller's CS. */
    TASK_UTEST_NOTE_USER_ENTRY(GDT_USER_CODE | SEL_RPL_USER);
    ssdt_syscall_enter();   /* previous_mode=UserMode + in_system_service, one lookup */
    result = ssdt_dispatch((uint32_t)number, a1, a2, a3, a4, a5, 0);
    ssdt_syscall_leave();   /* previous_mode=KernelMode + clear in_system_service */

    /* Propagate Win32 error code to TEB->LastErrorValue on failure.
     * Only write when the caller is user mode (which it always is here)
     * and the current thread has a TEB (user task, not kernel). */
    if (NT_ERROR(result)) {
        struct thread *thr = thread_current();
        if (thr && thr->teb) {
            TEB *teb = (TEB *)thr->teb;
            teb->LastErrorValue = RtlNtStatusToDosError(result);
        }
    }

    return result;
}
