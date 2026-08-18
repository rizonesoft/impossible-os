/* ============================================================================
 * boot_interrupts.c -- Phase 1: Platform Services (Interrupts Enabled at End)
 *
 * INIT ORDER CONTRACT (do not reorder without understanding the dependencies):
 *
 *   1. GDT + IST stacks    -- segment selectors + exception stacks
 *   2. IDT + IRQ            -- interrupt vectors registered
 *   3. Bugcheck + crash reg -- NMI crash handler wired after IDT
 *   4. ACPI (MADT)          -- discover CPUs, LAPIC/IOAPIC addresses
 *   5. LAPIC + IOAPIC       -- interrupt routing ready (PIC disabled)
 *   6. PIC                  -- only if no IOAPIC (legacy fallback)
 *   7. RTC                  -- non-critical, safe after IDT
 *   8. Framebuffer + splash -- needs PMM (Phase 0), no IRQ dependency;
 *                              before input so splash messages are visible
 *   9. UEFI info gathering  -- uefi_config_init etc., cosmetic
 *  10. Timer (LAST!)        -- generates interrupts immediately on start;
 *                              must be the LAST interrupt source armed
 *                              before sti so it can't fire into an
 *                              uninitialized subsystem
 *  11. DPC queues           -- must be ready before sti: an ISR may queue
 *                              a DPC the instant interrupts are enabled
 *  12. sti                  -- enable interrupts (timer starts ticking)
 *  13. splash animation     -- after sti (needs timer ticks); also
 *                              registers the timer tick callback
 *  14. Input (kbd/mouse)    -- AFTER sti so the splash spinner stays alive
 *                              during slow PS/2 reset busy-waits
 *
 * UEFI RUNTIME CALLS: All UEFI runtime service calls (SetVariable, GetTime,
 * etc.) may enable interrupts internally via firmware SMI. The active timer
 * must be masked during these calls if it's running. See rt_call_enter() /
 * rt_call_exit() in uefi_runtime.c for the LAPIC mask/unmask pattern
 * (boot_post_nvram_write16() in boot_init.c handles the NVRAM-POST path).
 *
 * Provides: boot_phase1() and the legacy boot_interrupts_init() wrapper.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/except.h"      /* except_init -- fault-to-exception handlers */
#include "kernel/mm/vmm.h"      /* vmm_register_page_fault_handler -- re-install ISR 14 */
#include "kernel/irq.h"
#include "kernel/drivers/pic.h"
#include "kernel/drivers/pit.h"
#include "kernel/drivers/watchdog.h"
#include "kernel/timer.h"
#include "kernel/drivers/rtc.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/mouse.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/acpi.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_init.h"
#include "kernel/cpu_security.h"
#include "kernel/entropy.h"
#include "kernel/csprng.h"
#include "kernel/tpm_transport.h"
#include "kernel/tpm.h"
#include "kernel/tpm_replay.h"
#include "kernel/tpm_nv.h"
#include "kernel/tpm_baseline.h"
#include "kernel/tpm_budget.h"
#include "kernel/tpm_enroll_gate.h"
#include "kernel/boot_confirm.h"
#include "kernel/cpuid_platform.h"
#include "kernel/boot_halt.h"
#include "kernel/security/wx.h"
#include "kernel/uefi_config.h"
#include "kernel/uefi_runtime.h"
#include "kernel/smbios.h"
#include "kernel/boot_timing.h"
#include "kernel/smp.h"
#include "kernel/exec.h"
#include "gfx_simd.h"
#include "main/main_internal.h"

/* ---- Phase 1 ------------------------------------------------------------ */

/* CPU-execution jitter sampler (ARCH: x86-64 rdtsc -- boot-path file).
 * 64 samples of rdtsc deltas across variable-length pause loops; the
 * delta low bytes go to the staged entropy transcript as JITTER (LOW --
 * the model clamps it there regardless; deterministic VM timing must
 * never masquerade as high-quality entropy, hence the vm caveat log). */
static void entropy_collect_jitter(void)
{
    uint8_t deltas[64];
    uint64_t prev, now;
    uint32_t i, spin;
    volatile uint32_t sink = 0;

    {
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        prev = ((uint64_t)hi << 32) | lo;
    }
    for (i = 0; i < (uint32_t)sizeof(deltas); i++) {
        /* Variable work keyed on the previous sample so loop length is
         * data-dependent: pipeline + cache noise feeds the deltas. */
        uint32_t work = 16u + ((uint32_t)prev & 0x3Fu);
        for (spin = 0; spin < work; spin++) {
            sink += spin;
            __asm__ volatile ("pause");
        }
        {
            uint32_t lo, hi;
            __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
            now = ((uint64_t)hi << 32) | lo;
        }
        deltas[i] = (uint8_t)(now - prev);
        prev = now;
    }
    (void)sink;

    if (entropy_stage_source(ENTROPY_SRC_JITTER, deltas,
                             (uint32_t)sizeof(deltas), ENTROPY_Q_LOW)) {
        klog(LOG_INFO, "entropy", "jitter: 64 samples staged (LOW%s)",
             platform_get() == PLATFORM_BARE_METAL ? "" : ", vm timing caveat");
    }
    {
        uint32_t k;
        for (k = 0; k < (uint32_t)sizeof(deltas); k++)
            deltas[k] = 0;
    }
}

void boot_phase1(void)
{
    klog(LOG_DEBUG, "boot", "--- Phase: interrupt controllers & timer ---");
    boot_splash_status("Setting up interrupts...");

    /* --- GDT: no prerequisites --- */
    POST16(POST16_GDT);
    gdt_init();
    POST16(POST16_GDT_OK);
    kernel_subsystem_set_ready(SUBSYS_GDT, true);
    boot_progress(1, "GDT", POST16_GDT_OK);

    /* --- IDT + IRQ: requires GDT --- */
    if (!kernel_subsystem_ready(SUBSYS_GDT)) {
        kernel_subsystem_dump();
        boot_halt("GDT not ready -- cannot init IDT");
    }
    POST16(POST16_IDT);
    idt_init();
    irq_init();
    POST16(POST16_IDT_OK);
    kernel_subsystem_set_ready(SUBSYS_IDT, true);
    boot_progress(1, "IDT", POST16_IDT_OK);

    /* Register CPU fault -> Windows exception handlers NOW, after idt_init() has
     * loaded the kernel IDT and (re)zeroed handlers[]. A registration done in
     * phase 0 would be erased by that clear. CPUID ran in phase 0, so the #CP
     * (CET shadow-stack) gate is valid. */
    except_init();

    /* Re-install the #PF handler (ISR 14). except_init() deliberately skips
     * vector 14 (the VMM owns it), and idt_init() above zeroed handlers[],
     * erasing the phase-0 vmm_init() registration. Without this call
     * handlers[14] is NULL and a live #PF hits the generic unhandled panic
     * instead of the user/kernel triage handler. */
    vmm_register_page_fault_handler();

    /* --- AVX-512 opt-in with MPERF/APERF throttle guard ---
     * Deferred from phase 0 because the probe needs the kernel IDT to
     * catch a #GP from unavailable MSRs (e.g. KVM traps IA32_MPERF even
     * when CPUID advertises AVX512F via -cpu host). AVX2 was already
     * enabled in phase 0; this only turns on the 512-bit memops path
     * if the CPU measurably does not throttle. */
    simd_enable_avx512();

    /* --- XSAVE finalize + PCID activation window (TODO-09-boot S5) ---
     * XCR0 was configured in Phase 0 (cpu_configure_xcr0 in cpuid_init), where
     * Phase-0 SIMD + PKU require it; this finalizes AFTER the AVX-512 throttle
     * guard above so it records the final XCR0 mask, and activates CR4.PCIDE
     * now that page tables are established (VMM came up in Phase 0). Both are
     * no-ops on CPUs lacking the feature. */
    /* Three result states for each activation:
     *   BOOT_OK       -> feature actually enabled  -> ready + *_ENABLED marker
     *   BOOT_DEGRADED -> unsupported/skipped (fine) -> ready, NO marker
     *   BOOT_FATAL    -> invariant violation (PCID CR3) -> NOT ready + degraded_mask
     * Emitting POSTCODE_*_ENABLED only on BOOT_OK keeps the progress/POST
     * stream from claiming activation on a CPU that lacks the feature; setting
     * degraded_mask on BOOT_FATAL surfaces a CR3-corruption fault in the
     * end-of-boot degraded summary instead of only a single klog line. */
    {
        boot_result_t xr = cpu_xsave_enable();
        kernel_subsystem_set_ready(SUBSYS_XSAVE, xr == BOOT_OK || xr == BOOT_DEGRADED);
        if (xr == BOOT_OK)
            boot_progress(1, "XSAVE", POSTCODE_XSAVE_ENABLED);

        boot_result_t pr = cpu_pcid_enable();
        kernel_subsystem_set_ready(SUBSYS_PCID, pr == BOOT_OK || pr == BOOT_DEGRADED);
        if (pr == BOOT_FATAL)
            g_boot_info.degraded_mask |= (1u << SUBSYS_PCID);
        else if (pr == BOOT_OK)
            boot_progress(1, "PCID", POSTCODE_PCID_ENABLED);
    }

    /* --- CR0/CR4 safety-bit pinning (TODO-09-boot S7) ---
     * MUST run AFTER the XSAVE/PCID activation above so CR4.OSXSAVE/PCIDE are
     * already set (the pinned mask captures whatever security bits are live).
     * Pins CR0.WP + enabled CR4 SMEP/SMAP/UMIP/FSGSBASE/CET on the BSP and arms
     * enforcement; each AP pins its own bits at the tail of ap_cpu_harden(). */
    POST16(0xD400);
    cpu_pin_control_regs();
    POST16(0xD401);
    boot_progress(1, "CR_PINNED", POSTCODE_CR_PINNED);

    /* --- Kernel-image W^X: clear WRITABLE on .text + .rodata.
     * Runs here -- single-CPU (APs launch in Phase 2 smp_init) and CR0.WP was
     * just pinned, so the read-only PTE bits are enforced immediately and the
     * kernel can no longer patch its own code/constants. Fail CLOSED: a
     * security gate that boots with .text/.rodata still writable (vmm_set_ro
     * can fail open or partially on a split/PMM error) is worse than not
     * booting -- halt before any AP launches. */
    POST16(0xD402);
    /* The read-only PTE bits only bind when CR0.WP is set; cpu_pin_control_regs
     * just forced it on. Verify before protecting -- a WP-clear path would make
     * the RO bits silent no-ops (false STRICT_KERNEL_RWX). */
    if (!cpu_wp_enforced())
        boot_halt("W^X: CR0.WP not set -- read-only kernel PTEs would not be enforced");
    if (kernel_wx_protect() != 0)
        boot_halt("W^X: kernel .text could not be made read-only");
    if (kernel_rodata_protect() != 0)
        boot_halt("W^X: kernel .rodata could not be made read-only");
    POST16(0xD403);

    /* --- Bugcheck: wire NMI crash handler after IDT (TODO-16 S1) --- */
    {
        extern void bugcheck_init(void);
        bugcheck_init();
    }

    /* --- Register kernel as first module in crash registry (TODO-16 S3) ---
     * Uses static array (no heap), spinlock safe after IDT. Must be early
     * so any crash during boot includes the kernel in the module list. */
    {
        extern void kernel_main(uint64_t, uint64_t);
        extern char __kernel_start[];
        extern char __kernel_end[];
        loaded_module_t kmod;
        uint8_t *p = (uint8_t *)&kmod;
        uint32_t ki;
        for (ki = 0; ki < sizeof(kmod); ki++)
            p[ki] = 0;
        kmod.base_address  = (uint64_t)(uintptr_t)__kernel_start;
        kmod.size_of_image = (uint64_t)(uintptr_t)__kernel_end -
                             (uint64_t)(uintptr_t)__kernel_start;
        kmod.entry_point   = (uint64_t)(uintptr_t)kernel_main;
        kmod.format        = EXEC_FMT_PE;  /* kernel presented as PE to WinDbg */
        /* name + full_path */
        {
            const char *n = "kernel.exe";
            const char *fp = "C:\\Impossible\\System32\\kernel.exe";
            uint32_t ni = 0;
            while (n[ni] && ni < EXEC_MODULE_NAME_MAX - 1) {
                kmod.name[ni] = n[ni]; ni++;
            }
            kmod.name[ni] = '\0';
            ni = 0;
            while (fp[ni] && ni < EXEC_MODULE_PATH_MAX - 1) {
                kmod.full_path[ni] = fp[ni]; ni++;
            }
            kmod.full_path[ni] = '\0';
        }
        POST16(POST16_MODULE_REGISTRY);
        if (exec_register_module((process_t *)0, &kmod) != 0) {
            klog(LOG_ERROR, "boot",
                 "FATAL: kernel module self-registration failed -- "
                 "crash dumps will lack kernel symbolization");
            boot_halt("exec_register_module(kernel.exe) failed");
        }
    }

    /* --- ACPI: requires IDT --- */
    if (g_boot_info.acpi_available) {
        klog(LOG_INFO, "acpi", "RSDP v%u at %p",
               (uint64_t)g_boot_info.acpi_version,
               g_boot_info.acpi_rsdp_addr);
        boot_splash_status("Parsing ACPI tables...");
        POST16(POST16_ACPI);
        acpi_init();
        POST16(POST16_ACPI_OK);
        kernel_subsystem_set_ready(SUBSYS_ACPI, true);
        /* Arm the ACPI WDAT hardware watchdog now that ACPI is parsed; no-op
         * (HW_WD_NONE) when no usable WDAT exists, e.g. QEMU. */
        hw_watchdog_init();
        klog(LOG_INFO, "smp", "CPUs discovered: %u",
             (uint64_t)acpi_get_cpu_count());
        boot_progress(1, "ACPI", POST16_ACPI_OK);
    }

    /* --- LAPIC + IOAPIC: APIC-first boot ---
     *
     * With the MADT parsed, we know the IOAPIC base address and whether
     * a PIC exists (PCAT_COMPAT flag).  Initialize LAPIC and IOAPIC NOW
     * so that pit_init() IRQ0 routes through the IOAPIC redirect table
     * instead of the 8259 PIC.  This fixes Hyper-V Gen 2 (no PIC). */
    if (g_boot_info.acpi_available && acpi_get_ioapic_base() != 0) {
        boot_splash_status("Initializing LAPIC...");
        POST16(POST16_LAPIC);
        lapic_init();

        if (lapic_available()) {
            kernel_subsystem_set_ready(SUBSYS_LAPIC, true);
            ioapic_init();

            if (ioapic_available()) {
                kernel_subsystem_set_ready(SUBSYS_IOAPIC, true);

                /* Disable PIC if platform has one -- IOAPIC takes over */
                if (acpi_pcat_compat()) {
                    pic_disable();
                    klog(LOG_INFO, "irq",
                         "Switched to LAPIC/IOAPIC (PIC disabled)");
                } else {
                    klog(LOG_INFO, "irq",
                         "LAPIC/IOAPIC active (APIC-only, no PIC)");
                }

                /* Drain stale ISR bits */
                {
                    uint32_t isr_dirty = 1;
                    uint32_t drain_rounds = 0;
                    while (isr_dirty && drain_rounds < 256) {
                        isr_dirty = 0;
                        uint32_t ri;
                        for (ri = 0; ri < 8; ri++) {
                            uint32_t isr_val =
                                lapic_read(0x100 + ri * 0x10);
                            if (isr_val) {
                                isr_dirty = 1;
                                if (drain_rounds == 0)
                                    klog(LOG_DEBUG, "lapic",
                                         "ISR[%u]=0x%x (stale)",
                                         (uint64_t)ri,
                                         (uint64_t)isr_val);
                            }
                        }
                        if (isr_dirty) {
                            lapic_eoi();
                            drain_rounds++;
                        }
                    }
                    if (drain_rounds > 0)
                        klog(LOG_INFO, "lapic",
                             "ISR drain: %u EOIs sent",
                             (uint64_t)drain_rounds);
                }
            }
        }
        boot_progress(1, "LAPIC_IOAPIC", POST16_LAPIC_OK);

        /* NOTE: ahci_setup_interrupts() moved to Phase 2, after ahci_init()
         * has discovered the PCI device.  LAPIC is ready here, so MSI will
         * work when called from Phase 2. */
    }

    /* --- PIC init: only when PIC exists AND IOAPIC has NOT taken over --- */
    if (!ioapic_available()) {
        if (!g_boot_info.acpi_available || acpi_pcat_compat()) {
            pic_init();
        } else {
            /* APIC-only platform (PCAT_COMPAT=0) with no working IOAPIC:
             * no controller can route external IRQs and the PIC is
             * firmware-declared absent. Booting on would leave timer,
             * input, and device interrupts silently dead. */
            boot_halt("irq: APIC-only platform but LAPIC/IOAPIC init failed"
                      " -- no usable interrupt controller");
        }
    }
    boot_progress(1, "PIC", 0x1036);

    /* --- RTC --- */
    /* Gate CMOS RTC init on ACPI: hardware-reduced platforms (and any FADT
     * with CMOS_RTC_NOT_PRESENT) have no CMOS RTC, so touching ports 0x70/0x71
     * reads garbage. acpi_has_cmos_rtc() safe-defaults to present for legacy
     * PCs (no/short FADT), so this never skips a real RTC. */
    POST16(POST16_RTC);
    if (acpi_has_cmos_rtc()) {
        rtc_init();
        kernel_subsystem_set_ready(SUBSYS_RTC, true);
    } else {
        klog(LOG_INFO, "rtc", "skipped (ACPI: no CMOS RTC -- hardware-reduced)");
    }
    POST16(POST16_RTC_OK);
    boot_progress(1, "RTC", POST16_RTC_OK);

    /* --- Framebuffer + boot splash (before input so splash messages are visible) --- */
    klog(LOG_DEBUG, "boot", "--- Phase: display & splash ---");

    POST16(POST16_FB);
    fb_init();
    POST16(POST16_FB_OK);
    kernel_subsystem_set_ready(SUBSYS_FB, true);
    boot_progress(1, "FB", POST16_FB_OK);
    POST16(POST16_SPLASH);
    /* postbars=diag (2): keep Tier 1 VPD authoritative -- do NOT stop it; the
     * splash then suppresses itself (boot_splash_init early-returns) so the VPD
     * diagnostic layout persists through boot. Any other mode stops Tier 1 and
     * lets the splash background overwrite the VPD area. */
    if (!(g_boot_info.config.config_found && g_boot_info.config.postbars == 2)) {
        extern void vpd_stop_tier1(void);
        vpd_stop_tier1();
    }
    boot_splash_init();
    POST16(POST16_SPLASH_OK);

    /* Input devices moved after timer+sti so splash spinner is alive */

    /* --- Phase 1 info gathering --- */
    uefi_config_init();
    POST16(POST16_SMBIOS);
    smbios_init();
    POST16(POST16_SMBIOS_OK);
    boot_progress(1, "SMBIOS", POST16_SMBIOS_OK);
    /* Initialize firmware quirks immediately after SMBIOS so MAT and
     * ESRT init can consult them for severity-downgrade decisions
     * (firmware_quirks_is_active(FW_QUIRK_BOGUS_MAT) etc.). */
    {
        extern void firmware_quirks_init(void);
        firmware_quirks_init();
    }
    esrt_init();
    mat_init();
    /* W^X enforcement on UEFI runtime regions (TODO-27 sec3). Pinned here in
     * Phase 1 after mat_init (MAT ready) and SVAM (Phase 0), but BEFORE Phase 2
     * smp_init -- the pass uses local TLB flushes valid only single-threaded
     *. It self-guards against SMP-up and degrades cleanly when
     * the MAT is absent. */
    uefi_runtime_enforce_wx();
    uefi_conformance_init();
    {
        extern void firmware_tables_init(void);
        firmware_tables_init();
    }
    {
        extern void firmware_platform_init(void);
        firmware_platform_init();
    }
    uefi_capsule_init();
    uefi_crypto_agility_init();
    secureboot_keys_init();
    POST16(POST16_BOOT_TIMING);
    boot_timing_init();
    POST16(POST16_BOOT_TIMING_OK);

    /* GOP mode enumeration report */
    {
        static const char *pf_names[] = { "RGBX", "BGRX", "BitMask" };
        uint32_t mc = g_boot_info.gop_mode_count;
        uint32_t sel = g_boot_info.gop_mode_selected;
        if (mc > 0) {
            const char *pf = (g_boot_info.fb.pixel_format < 3) ?
                pf_names[g_boot_info.fb.pixel_format] : "Unknown";
            if (sel < mc) {
                klog(LOG_INFO, "GOP",
                     "%ux%u %s (mode %u of %u available)",
                     g_boot_info.fb.width, g_boot_info.fb.height,
                     pf, sel, mc);
            } else {
                klog(LOG_INFO, "GOP",
                     "%ux%u %s (active mode off-table, %u modes enumerated)",
                     g_boot_info.fb.width, g_boot_info.fb.height,
                     pf, mc);
            }
        }
    }

    /* --- Timer + enable interrupts (MUST BE LAST before sti) --- */
    BOOT_ASSERT(kernel_subsystem_ready(SUBSYS_IDT), "IDT must be ready before timer");
    BOOT_ASSERT(kernel_subsystem_ready(SUBSYS_GDT), "GDT must be ready before timer");
    POST16(POST16_TIMER);
    timer_hal_init();
    POST16(POST16_TIMER_OK);
    kernel_subsystem_set_ready(SUBSYS_TIMER, true);
    boot_progress(1, "TIMER", POST16_TIMER_OK);

    /* DPC queues must be ready before sti -- ISRs may queue DPCs immediately */
    {
        extern void dpc_init_queues(void);
        dpc_init_queues();
    }

    /* Kernel timer lists must be ready before sti -- the timer ISR scans them
     * each tick and queues due DPCs. (BSS-zero already, but init explicitly.) */
    {
        extern void ktimer_init_lists(void);
        ktimer_init_lists();
    }

    __asm__ volatile ("sti");

    /* Jitter entropy sample -- earliest post-sti point so the staged
     * bytes are available to the first CSPRNG seed (Phase 3 would be
     * too late for pre-KASLR consumers). CPU-execution jitter, not
     * tick-boundary sampling: 64 tick-boundary samples at 100 Hz would
     * cost 640ms of boot; execution jitter finishes in microseconds and
     * is the same source class Linux jitterentropy uses. Quality is
     * LOW by definition (the entropy model clamps JITTER regardless). */
    entropy_collect_jitter();

    /* TPM2 command transport (TIS/CRB). Phase 1 placement is load-
     * bearing: needs validated ACPI (acpi_init above), UC MMIO maps
     * (VMM, Phase 0), and the calibrated TSC for poll deadlines
     * (boot_timing_init above). The Phase 0 tpm_init() event-log parse
     * is independent. Absent/wedged TPMs degrade; boot continues. */
    /* spinner_start() (via boot_splash_start_animation) registers the
     * singleton timer tick callback at 10 ticks (~10fps, Fluent 100 deg/s).
     * The slot has exactly one owner -- do NOT register another callback
     * here: a second registration silently overwrites the spinner's and
     * historically ran the animation at 2x design speed. Starts BEFORE
     * the TPM work below: a present-but-slow TPM can legally consume
     * the probe + RNG budgets (up to 5s combined) and the spinner must
     * already be animating through that window. */
    boot_splash_start_animation();

    POST16(POST16_TPM_TRANSPORT);
    tpm_transport_init();
    /* TPM RNG entropy rides the fresh transport (cumulative-budget
     * bounded); staged so the first CSPRNG seed sees it. Absent or
     * wedged TPMs degrade and the entropy report shows tpm=none. */
    entropy_collect_tpm();
    /* Eagerly populate the measured-boot PCR cache on the fresh transport
     * (single-threaded here; lock-free reads afterward). No-op without a TPM. */
    tpm_pcr_cache_init();
    /* Structural (unauthenticated) Secure Boot variable reconciliation: decode
     * EV_EFI_VARIABLE_* events + compare against live UEFI vars. Single-threaded;
     * report read-only afterward. Runs after integrity + secureboot init. */
    tpm_secureboot_reconcile();
    /* Replay the measured-boot event log and compare to the live PCRs (SHA-256
     * bank, the universal TPM2 bank); publish the tamper verdict into the boot
     * integrity report. Single-threaded; uses the just-populated PCR cache. */
    {
        struct tpm_replay_report rpt;
        /* Hoisted out of the verify block because the enrollment gate below
         * consumes them: enrolling a boot whose event log already disagrees
         * with the hardware PCRs would promote the tampered measurement to
         * golden. `replay_known` stays 0 when the replay did not run at all,
         * and the gate REFUSES on that -- it whitelists an explicit VERIFIED
         * rather than treating "no verdict" as clean. */
        uint8_t replay_known = 0;
        uint8_t replay_verdict = 0;
        if (tpm_replay_verify(TPM_ALG_SHA256, &rpt) == TPM_REPLAY_OK) {
            tpm_integrity_set_replay_verdict(rpt.verdict);
            replay_known = 1;
            replay_verdict = rpt.verdict;
            if (rpt.verdict == TPM_REPLAY_TAMPER)
                klog(LOG_WARN, "TPM", "PCR replay: event-log TAMPER at PCR %d",
                     (uint64_t)rpt.first_mismatch_pcr);
        }
        /* Measured-boot baseline. Enrollment writes the golden record every
         * later boot is measured against, so it is gated on the enrollment
         * AUTHORITY predicate rather than on boot.conf: `boot_mode` and
         * `tpm_enroll` are both ESP-controlled, so the old gate proved ESP
         * write access and nothing else. See include/kernel/tpm_enroll_gate.h
         * for the trust model. Otherwise verify the current state against the
         * stored baseline and publish the verdict (the replay TAMPER above
         * already outranks a baseline match). */
        /* ONE deadline for every verified NV read this boot performs, armed
         * before the first of them. Each verified read costs two TPM
         * transactions where an unverified read cost one, and every logical NV
         * operation otherwise arms its OWN per-call budget -- so without an
         * aggregate the reads below can each claim the full allowance and blow
         * the boot between them while no individual bound is exceeded. */
        tpm_boot_budget_arm();

        struct tpm_enroll_gate_inputs gi = {
            .boot_path               = g_boot_info.boot_path,
            .boot_reason             = g_boot_info.boot_reason,
            .boot_source_flags       = g_boot_info.boot_source_flags,
            .selection_reason        = g_boot_info.selection_reason,
            .audit_degraded          = g_boot_info.audit_degraded,
            .sticky_present          = g_boot_info.sticky_present,
            .sticky_recovery_trigger = g_boot_info.sticky_recovery_trigger,
            .secure_boot_enabled     = g_boot_info.secure_boot_enabled,
            /* Whole-chain coverage, NOT just "Secure Boot is on": only the
             * UKI path runs a kernel the signature actually covered. The
             * split path loads an unsigned kernel.exe from the ESP even with
             * Secure Boot enabled (bootx64.c:7894-7896). */
            .whole_chain_verified    =
                (g_boot_info.flags & BOOT_FLAG_INVOKED_VIA_UKI) ? 1u : 0u,
            .replay_known            = replay_known,
            .replay_verdict          = replay_verdict,
            .tpm_enroll              = g_boot_info.config.tpm_enroll,
            .confirm                 = TPM_CONFIRM_PENDING,
        };
        struct tpm_enroll_gate_result gr;
        tpm_enroll_gate_evaluate(&gi, &gr);

        /* Prompt ONLY when every non-operator condition already held. That is
         * what keeps a normal boot silent: a machine that never asked to
         * enroll is refused at the config check and no prompt is drawn. */
        if (gr.needs_confirm) {
            boot_confirm_result_t cr =
                boot_confirm_prompt("[TPM] Enroll THIS boot as the trusted "
                                    "measured-boot baseline? [y/N]",
                                    'y', 'n', BOOT_CONFIRM_TIMEOUT_MS,
                                    BOOT_CONFIRM_IRQ_LEAVE_ALONE,
                                    BOOT_CONFIRM_CLOCK_REQUIRED);
            /* IRQ_LEAVE_ALONE is correct here and is load-bearing: this runs
             * BEFORE keyboard_init() below, so nothing has claimed IRQ1 and
             * IOAPIC entries are still masked from reset. Leaving interrupts
             * enabled keeps the timer alive, so the splash spinner keeps
             * animating and the watchdog keeps being petted through the
             * prompt. */
            switch (cr) {
                case BOOT_CONFIRM_YES:         gi.confirm = TPM_CONFIRM_YES; break;
                case BOOT_CONFIRM_DECLINED:    gi.confirm = TPM_CONFIRM_WRONG_KEY; break;
                case BOOT_CONFIRM_TIMEOUT:     gi.confirm = TPM_CONFIRM_TIMEOUT; break;
                case BOOT_CONFIRM_UNAVAILABLE: gi.confirm = TPM_CONFIRM_UNAVAILABLE; break;
            }
            tpm_enroll_gate_evaluate(&gi, &gr);
        }

        if (gr.admit) {
            tpm_baseline_status_t bs =
                tpm_baseline_enroll(TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256);
            klog(LOG_WARN, "TPM",
                 "Baseline enroll (authority %s): status %d",
                 tpm_enroll_authority_label(gr.authority), (uint64_t)bs);
            /* AN ENROLLING BOOT RUNS INSTEAD OF VERIFICATION, so anything it
             * detects is the only chance this boot has to report it -- the
             * verify branch below runs exclusively under !admit. Without this
             * the enrolling boot logged a corrupt baseline, a wrong index
             * definition, or its own corrupt ABI identity and then carried on
             * under whatever status preceded it.
             *
             * The define call's own TPM_NV_MISMATCH is deliberately NOT part of
             * this class: globally that status also covers a benign commit-
             * counter race, and widening it is the regression the write path
             * already paid for. */
            /* UNBOUND is EXCLUDED here and only here: tpm_baseline_enroll
             * returns it as an ordinary configuration refusal on a healthy
             * authority-provisioned machine, so publishing MISMATCH for it
             * would report tamper on a working box. On the verify path below
             * it genuinely is a verdict. */
            if (tpm_baseline_status_is_failure(bs) &&
                bs != TPM_BASELINE_UNBOUND) {
                klog(LOG_ERROR, "TPM",
                     "Baseline enroll detected an integrity failure (status %d): "
                     "not a clean first install",
                     (uint64_t)bs);
                tpm_integrity_publish_baseline(BOOT_INTEGRITY_MISMATCH, 0, 0);
            }
        } else if (g_boot_info.config.tpm_enroll) {
            /* Only report a refusal when enrollment was actually requested;
             * every ordinary boot refuses at CONFIG_DISABLED and saying so
             * each time would be noise. The authority is reported next to the
             * reason so a config-only boot can never read as trusted. */
            klog(LOG_WARN, "TPM",
                 "Baseline enroll REFUSED (%s; authority %s)",
                 tpm_enroll_refusal_label(gr.refusal),
                 tpm_enroll_authority_label(gr.authority));
        }

        if (!gr.admit) {
            uint8_t overall = 0;
            uint8_t pcr_status[BOOT_INTEGRITY_MAX_PCRS];
            uint8_t pcr_n = 0;
            tpm_baseline_status_t bs =
                tpm_baseline_verify(TPM_NV_INDEX_BASELINE, TPM_ALG_SHA256, &overall,
                                    pcr_status, (uint8_t)BOOT_INTEGRITY_MAX_PCRS,
                                    &pcr_n);
            /* One publication carries the overall verdict AND the per-PCR
             * detail, so a reader can never catch the report with a fresh
             * verdict beside stale per-PCR values. A non-verdict return
             * (NO_TPM / TPMERR / BADARG) publishes nothing at all -- a machine
             * that could not measure has not failed to match.
             *
             * EVERY AUTHENTICITY VERDICT IS A VERDICT. TORN, RELABELED and
             * UNBOUND all mean tpm_baseline_verify reached a conclusion and set
             * `overall` to MISMATCH; only the repair differs. Leaving any of
             * them unpublished lets a detected failure boot under whatever
             * Phase 0 published, which is the one outcome that must never read
             * as untampered -- and RELABELED is precisely the attack the bind
             * record exists to catch. */
            if (bs == TPM_BASELINE_OK || bs == TPM_BASELINE_NO_BASELINE ||
                tpm_baseline_status_is_failure(bs))
                tpm_integrity_publish_baseline(overall, pcr_status, pcr_n);

            /* The three repairs are opposite, so they are named separately
             * rather than left for an operator to infer from one status. */
            if (bs == TPM_BASELINE_TORN)
                /* Deliberately does NOT name one repair. This status collapses
                 * three directions -- an uncommitted write, a torn pairing, an
                 * impossible one -- and the enum's own contract says the
                 * direction decides the repair. Completing an interrupted
                 * commit and entering authorized recovery are different
                 * actions, and prescribing the second for the first is
                 * destructive over-recovery. The direction lives in the view's
                 * tpm_pairing_t; tpm_baseline_verify does not yet pass it out,
                 * which is filed. What IS safe to say unconditionally is the
                 * one thing every direction shares. */
                klog(LOG_ERROR, "TPM",
                     "Baseline bind record and its commit counter DISAGREE: "
                     "repair is AUTHORIZED and never a fresh enrollment");
            else if (bs == TPM_BASELINE_RELABELED)
                klog(LOG_ERROR, "TPM",
                     "Baseline blob does NOT match its committed bind record: "
                     "content was replaced under a valid record");
            else if (bs == TPM_BASELINE_UNBOUND)
                klog(LOG_WARN, "TPM",
                     "Baseline has no authenticated bind record (legacy): "
                     "authorized migration required, never auto-binding");
            else if (bs == TPM_BASELINE_IDENTITY)
                klog(LOG_ERROR, "TPM",
                     "Baseline NV index failed its enrolled contract: the index "
                     "answering is NOT the one enrolled");

            /* Budget expiry collapses into NO_TPM by design (tpm_baseline.c
             * nv_to_baseline): a TPM that merely answered too slowly must leave
             * the verdict unpublished rather than report a false tamper. That
             * is right for the VERDICT and useless for DIAGNOSIS, so the one
             * fact the collapse destroys is logged here. Without it an
             * exhausted boot is indistinguishable from a machine with no TPM at
             * all. */
            if (bs == TPM_BASELINE_NO_TPM && tpm_boot_budget_expired())
                klog(LOG_WARN, "TPM",
                     "Boot verified-read budget EXHAUSTED (%u ms aggregate): "
                     "integrity verdict is UNVERIFIED, not clean",
                     (uint64_t)TPM_BOOT_BUDGET_MS);
        }
        {
            struct boot_integrity_report ir;
            tpm_integrity_report_copy(&ir);
            klog(LOG_INFO, "TPM", "Boot integrity status: %s",
                 tpm_integrity_status_label(&ir));
        }

        /* The deadline ends where the boot's verified reads end. Nothing else
         * ends a boot, so leaving it armed would apply Phase 1's spent quota to
         * every verified read the machine performs afterwards -- a runtime
         * mark-good or a later attestation would be refused with a budget the
         * boot, not they, had used up. */
        tpm_boot_budget_disarm();
    }
    POST16(POST16_TPM_TRANSPORT_OK);
    boot_progress(1, "TPM-TRANSPORT", POST16_TPM_TRANSPORT_OK);

    /* Kernel CSPRNG first seed. Placement is load-bearing: AFTER the
     * jitter and TPM collectors above (csprng_init drains their staged
     * transcript), BEFORE any consumer (AT_RANDOM in task_exec, future
     * KASLR). The boot_info seed payload (bootloader EFI RNG / RDSEED /
     * OEM0 / pre-EBS seed-file carryover) is consumed FIRST so it folds
     * into the same initial key -- first-seed-or-nothing, never a
     * post-init reseed a consumer could race past. */
    POST16(POST16_CSPRNG);
    early_entropy_init();
    POST16(POST16_CSPRNG_OK);
    boot_progress(1, "CSPRNG", POST16_CSPRNG_OK);

    /* --- Input devices (after timer+sti so splash spinner is alive) --- */
    boot_splash_status("Detecting PS/2 keyboard...");
    POST16(POST16_KBD);
    keyboard_init();
    POST16(POST16_KBD_OK);
    boot_progress(1, "KEYBOARD", POST16_KBD_OK);
    /* PS/2 mouse init is NOT done in Phase 1 -- the PS/2 BAT self-test
     * takes 300ms-2s on real hardware and would freeze the splash spinner.
     * mouse_init() runs in deferred_input_init() via boot_run_deferred()
     * after the desktop is live (boot_storage.c), which owns the real
     * POST16_DEFERRED_INPUT[_OK] progress. Do NOT emit POST16_MOUSE_OK
     * here: that would falsely certify mouse readiness in the Phase 1 POST
     * / progress stream before the device is actually probed. */

    boot_splash_status("Setting up hardware...");

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "[PHASE1] complete -- interrupts, timer, display ready");

    /* NVRAM write: Phase 1 complete */
    boot_post_nvram_write16(POST16_TIMER_OK);
}

/* ---- Legacy wrapper (until main.c switches to boot_phase1) -------------- */

void boot_interrupts_init(void)
{
    boot_phase1();
}
