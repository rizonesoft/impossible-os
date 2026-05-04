/* ============================================================================
 * boot_interrupts.c -- Phase 1: Platform Services (Interrupts Enabled at End)
 *
 * INIT ORDER CONTRACT (do not reorder without understanding the dependencies):
 *
 *   1. GDT + IST stacks    -- segment selectors + exception stacks
 *   2. IDT + IRQ            -- interrupt vectors registered
 *   3. ACPI (MADT)          -- discover CPUs, LAPIC/IOAPIC addresses
 *   4. LAPIC + IOAPIC       -- interrupt routing ready (PIC disabled)
 *   5. PIC                  -- only if no IOAPIC (legacy fallback)
 *   6. RTC                  -- non-critical, safe after IDT
 *   7. Input (kbd/mouse)    -- needs IRQ routing (IOAPIC or PIC)
 *   8. Framebuffer          -- needs PMM (Phase 0), no IRQ dependency
 *   9. Splash + UEFI info   -- cosmetic, after FB
 *  10. Timer (LAST!)        -- generates interrupts immediately on start;
 *                              must be LAST before sti to prevent timer
 *                              firing into uninitialized subsystems
 *  11. sti                  -- enable interrupts (timer starts ticking)
 *  12. splash animation     -- only after sti (needs timer ticks)
 *
 * UEFI RUNTIME CALLS: All UEFI runtime service calls (SetVariable, GetTime,
 * etc.) may enable interrupts internally via firmware SMI. The LAPIC timer
 * must be masked during these calls if it's running. See boot_post_write16()
 * and uefi_runtime.c for the mask/unmask pattern.
 *
 * Provides: boot_phase1() and the legacy boot_interrupts_init() wrapper.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/irq.h"
#include "kernel/drivers/pic.h"
#include "kernel/drivers/pit.h"
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
#include "kernel/boot_halt.h"
#include "kernel/uefi_config.h"
#include "kernel/uefi_runtime.h"
#include "kernel/smbios.h"
#include "kernel/boot_timing.h"
#include "kernel/smp.h"
#include "kernel/exec.h"
#include "gfx_simd.h"
#include "main/main_internal.h"

/* ---- Phase 1 ------------------------------------------------------------ */

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

    /* --- AVX-512 opt-in with MPERF/APERF throttle guard ---
     * Deferred from phase 0 because the probe needs the kernel IDT to
     * catch a #GP from unavailable MSRs (e.g. KVM traps IA32_MPERF even
     * when CPUID advertises AVX512F via -cpu host). AVX2 was already
     * enabled in phase 0; this only turns on the 512-bit memops path
     * if the CPU measurably does not throttle. */
    simd_enable_avx512();

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
            klog(LOG_INFO, "irq",
                 "PIC: skipped (PCAT_COMPAT=0, APIC-only platform)");
        }
    }
    boot_progress(1, "PIC", 0x1036);

    /* --- RTC --- */
    POST16(POST16_RTC);
    rtc_init();
    POST16(POST16_RTC_OK);
    kernel_subsystem_set_ready(SUBSYS_RTC, true);
    boot_progress(1, "RTC", POST16_RTC_OK);

    /* --- Framebuffer + boot splash (before input so splash messages are visible) --- */
    klog(LOG_DEBUG, "boot", "--- Phase: display & splash ---");

    POST16(POST16_FB);
    fb_init();
    POST16(POST16_FB_OK);
    kernel_subsystem_set_ready(SUBSYS_FB, true);
    boot_progress(1, "FB", POST16_FB_OK);
    POST16(POST16_SPLASH);
    {
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

    __asm__ volatile ("sti");
    boot_splash_start_animation();

    /* Drive splash spinner from timer interrupt so it stays alive during
     * long busy-waits (e.g., PS/2 mouse reset). */
    {
        extern void boot_splash_tick(void);
        timer_register_tick_callback(boot_splash_tick, 5);
    }

    /* --- Input devices (after timer+sti so splash spinner is alive) --- */
    boot_splash_status("Detecting PS/2 keyboard...");
    POST16(POST16_KBD);
    keyboard_init();
    POST16(POST16_KBD_OK);
    boot_progress(1, "KEYBOARD", POST16_KBD_OK);
    /* PS/2 mouse init deferred to boot_run_deferred() -- the PS/2 BAT
     * self-test takes 300ms-2s on real hardware, freezing the splash
     * spinner. Mouse init now runs after the desktop is live. */
    POST16(POST16_MOUSE);
    POST16(POST16_MOUSE_OK);
    boot_progress(1, "MOUSE", POST16_MOUSE_OK);

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
