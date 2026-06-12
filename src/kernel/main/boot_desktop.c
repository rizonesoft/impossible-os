/* ============================================================================
 * boot_desktop.c -- Phase 3: User Platform
 *
 * Scheduler, IPC, exec loader, boot tests (debug-only), fonts, icons,
 * cursors, window manager, desktop, cmd.exe, compositor.
 * The kernel is fully operational before this phase; failures fall back
 * to a text console, not BSOD.
 *
 * Provides: boot_phase3() and the legacy boot_desktop_init() wrapper.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/klog.h"
#include "kernel/cpuid_platform.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/boot_reserved.h"
#include "kernel/boot_version.h"
#include "kernel/timer.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/drivers/ahci.h"
#include "kernel/sched/task.h"
#include "kernel/sched/workqueue.h"
#include "kernel/sched/syscall.h"
#include "kernel/nt/ssdt.h"
#include "kernel/pe.h"
#include "kernel/boot_halt.h"
#include "kernel/etw.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_timing.h"
#include "kernel/hw_dump.h"
#include "kernel/boot_init.h"
#include "kernel/boot_progress.h"
#include "kernel/boot_halt.h"
#include "kernel/boot_recovery.h"
#include "kernel/acpi.h"
#include "kernel/boot_info.h"
#include "kernel/uefi_runtime.h"
#include "kernel/seed_file.h"
#include "desktop/wm.h"
#include "desktop/font.h"
#include "font_mgr.h"
#include "icon_store.h"
#include "gfx.h"
#include "cursor.h"
#include "desktop/desktop.h"
#include "desktop/terminal.h"
#include "desktop/gallery.h"
#include "kernel/fs/vfs.h"
#include "kernel/elf.h"
#include "kernel/ipc/pipe.h"
#include "kernel/ipc/alpc.h"
#include "main/main_internal.h"

/* ---- Phase 3 ------------------------------------------------------------ */

void boot_phase3(void)
{
    /* --- Scheduler: requires HEAP + TIMER --- */
    if (!kernel_subsystem_ready(SUBSYS_HEAP) ||
        !kernel_subsystem_ready(SUBSYS_TIMER)) {
        boot_recovery_info_t ri = { SUBSYS_SCHED, POST16_SCHED_OK, BOOT_FATAL, 3 };
        kernel_subsystem_dump();
        boot_recovery_action_t act = boot_recovery_show(&ri);
        if (act == RECOVERY_POWEROFF) { acpi_shutdown(); }
        boot_halt("HEAP or TIMER not ready -- cannot init scheduler");
    }
    /* --- DPC subsystem: per-CPU queues for deferred ISR work --- */
    {
        extern void dpc_init(void);
        dpc_init();
    }

    boot_splash_status("Initializing scheduler...");
    POST16(POST16_SCHED);
    task_init();
    POST16(POST16_SCHED_OK);
    kernel_subsystem_set_ready(SUBSYS_SCHED, true);
    boot_progress(3, "SCHED", POST16_SCHED_OK);

    ahci_enable_events();  /* safe now: yield handler registered */

    /* Start threaded DPC worker thread (needs scheduler) */
    {
        extern void dpc_start_threads(void);
        dpc_start_threads();
    }

    /* Create the system work queue (needed by NIC driver) */
    {
        extern workqueue_t *sys_wq;
        scheduler_enable();
        sys_wq = workqueue_create("sys_wq");
        scheduler_disable();
        if (sys_wq)
            klog(LOG_DEBUG, "wq", "sys_wq created");
        else
            klog(LOG_ERROR, "wq", "sys_wq creation FAILED");
    }

    /* --- IPC init (pipe, shmem, signal, alpc) --- */
    /* pipe_create/shmem_create already init lazily; the explicit pipe_init
     * call here is idempotent but gives the init sequencing a uniform
     * entry point. alpc_init emits the ABI-present marker for the ALPC
     * header; port objects and syscalls arrive in later work. */
    (void)pipe_init();
    (void)alpc_init();
    kernel_subsystem_set_ready(SUBSYS_IPC, true);
    boot_progress(3, "IPC", 0x0061);

    /* --- Syscall handler + SSDT --- */
    boot_splash_status("Initializing syscalls...");
    ssdt_init();
    {
        extern void syscall_init_fast(void);
        syscall_init_fast();  /* attempt SYSCALL/SYSRET; falls back if GDT incompatible */
    }
    syscall_init();  /* keep INT 0x80 path active */

    /* --- NT syscall migration (NtXxx wrappers for existing SYS_* calls) --- */
    {
        extern void nt_syscall_register_ssdt(void);
        extern void nt_process_register_ssdt(void);
        extern void nt_sync_register_ssdt(void);
        extern void nt_memory_register_ssdt(void);
        extern void nt_registry_register_ssdt(void);
        extern void nt_token_register_ssdt(void);
        extern void nt_namespace_register_ssdt(void);
        extern void nt_section_register_ssdt(void);
        extern void nt_timer_register_ssdt(void);
        extern int  nt_lpc_register_ssdt(void);
        extern int  nt_alpc_register_ssdt(void);
        extern int  csprng_register_ssdt(void);
        int reg_failures;
        nt_syscall_register_ssdt();
        nt_process_register_ssdt();
        nt_sync_register_ssdt();
        nt_memory_register_ssdt();
        nt_section_register_ssdt();
        nt_registry_register_ssdt();
        nt_token_register_ssdt();
        nt_namespace_register_ssdt();
        nt_timer_register_ssdt();
        reg_failures  = nt_lpc_register_ssdt();
        reg_failures += nt_alpc_register_ssdt();
        reg_failures += csprng_register_ssdt();
        if (reg_failures != 0) {
            klog(LOG_ERROR, "boot",
                 "SSDT registration failures: %d -- aborting boot",
                 (uint64_t)reg_failures);
            boot_halt("SSDT registration failed");
        }

        /* PE export tables feed binary-search import resolution; a
         * single unsorted entry silently breaks NTAPI lookup for every
         * user-mode process. Assert at boot (not just under KERNEL_TESTS)
         * so release builds cannot ship with a regressed ordering. */
        {
            int pe_violations = pe_exports_sorted_check();
            if (pe_violations != 0) {
                klog(LOG_ERROR, "boot",
                     "PE export table unsorted (%d violations) -- aborting",
                     (uint64_t)pe_violations);
                boot_halt("PE export table unsorted");
            }
        }
    }

    /* --- ETW tracing subsystem --- */
    etw_init();
    etw_register_ssdt();

    /* --- Timer resolution management --- */
    {
        extern void timer_resolution_init(void);
        extern void timer_resolution_register_ssdt(void);
        timer_resolution_init();
        timer_resolution_register_ssdt();
    }

    /* --- Time syscalls --- */
    {
        extern void wall_clock_register_ssdt(void);
        wall_clock_register_ssdt();
    }

    /* --- UEFI firmware variable syscalls --- */
    {
        extern void uefi_register_ssdt(void);
        uefi_register_ssdt();
    }

    /* --- Exec loader (ELF/PE format handlers) --- */
    {
        extern void exec_init(void);
        exec_init();
    }
    kernel_subsystem_set_ready(SUBSYS_EXEC, true);
    boot_progress(3, "EXEC", 0x0062);

    /* --- Boot tests (debug=1 only) --- */
    boot_tests_run();

    /* --- Fonts, icons, cursors --- */
    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Desktop ----------------------------------------------------------------");

    boot_splash_status("Flushing boot log...");
    klog_disk_flush();

    /* Runtime crash test -- triggered by crash_test=1 in boot.conf */
    if (g_boot_info.config.crash_test) {
        extern void panic_screen(struct interrupt_frame *frame,
                                 uint64_t error_code,
                                 const char *description,
                                 const char *file, uint32_t line);
        klog(LOG_WARN, "boot", "crash_test=1 -- triggering deliberate BSOD");
        panic_screen((struct interrupt_frame *)0, 0xDEAD,
                     "CRASH_TEST: Deliberate panic for testing",
                     __FILE__, __LINE__);
    }

    boot_splash_tick();
    boot_splash_status("Loading fonts...");
    POST16(POST16_FONTS);
    ttf_mgr_init();
    POST16(POST16_FONTS_OK);

    boot_splash_tick();
    boot_splash_status("Loading resources...");
    POST16(POST16_ICONS);
    icon_store_init();
    POST16(POST16_ICONS_OK);

    POST16(POST16_CURSORS);
    cursor_init();
    POST16(POST16_CURSORS_OK);

    /* --- Window manager + desktop --- */
    klog(LOG_DEBUG, "boot", "--- Phase: desktop & WM ---");
    boot_splash_tick();
    boot_splash_status("Almost ready...");
    wm_init();
    POST16(POST16_DESKTOP);
    desktop_init();
    POST16(POST16_DESKTOP_OK);

    /* Finish boot splash and stop timer-driven spinner */
    timer_unregister_tick_callback();
    boot_splash_finish();

    /* Boot complete timing */
    {
        uint64_t ms = system_get_ticks() * 10;
        klog(LOG_INFO, "boot",
             "Boot complete in %u.%03us (PIT uptime from interrupt init)",
             (uint64_t)(ms / 1000), (uint64_t)(ms % 1000));
    }

    boot_progress(3, "DESKTOP_READY", POST16_DESKTOP_OK);
    kernel_subsystem_set_ready(SUBSYS_DESKTOP, true);

    /* Run deferred non-critical inits inline. These include VirtIO/VBox
     * mouse drivers needed for absolute cursor positioning. Running on a
     * thread caused them to never execute (thread starved by compositor). */
    boot_run_deferred();

    /* NVRAM write: Phase 3 complete -- boot succeeded.
     * Must be here, not later -- on bare metal the compositor may crash
     * (timer interrupt issue), and we need this written before that.
     * This marker is independent of the anti-rollback raise (which now
     * waits for the compositor-steady signal; see boot_rollback.c). */
    boot_post_nvram_write16(POST16_BOOT_OK);

    /* Random-seed carryover: read + verify + reseed + rotate
     * X:\Boot\random-seed.bin (Phase-3 reseed via csprng_add_entropy;
     * the FIRST seed is the Phase-1 staged transcript, and the early
     * boot_info read of the same file format is the seed-handoff
     * section's job). Runs before entropy_report() so the summary line
     * reflects the seed-file source. */
    seed_file_phase3();

    /* Entropy source summary -- one line, WARN when degraded. Collectors
     * land with the early-entropy TODO sections; until then this honestly
     * reports all-none rather than implying randomness we do not have. */
    {
        extern void entropy_report(void);
        entropy_report();
    }

    boot_timing_print_steps();
    boot_perf_dump();
    boot_perf_compare();
    boot_perf_save();
    boot_timing_write_report();
    boot_postcode_write_log();
    boot_timeline_dump_json();

    /* Write hardware inventory to X:\Diag\hwdump.txt */
    hw_dump_write_file();

    /* Boot protocol section 6: dump the boot_reserved region table to
     * X:\Diag\boot-reserved.json so a post-boot user can audit
     * exactly which physical ranges the PMM kept reserved. */
    boot_reserved_blackbox_dump();

    /* Boot protocol section 7: if the prior boot halted on a protocol
     * mismatch, transcribe the NVRAM fault record to
     * X:\Diag\boot-proto-fault.txt and clear the NVRAM slot. No-op
     * if no record exists. */
    boot_version_blackbox_transcribe();

    /* Per-boot policy audit JSONL publish + sticky-trigger ack. Reads
     * the v20 audit surface in g_boot_info, composes one line under
     * X:\Boot\history.jsonl, and clears any consumed sticky triggers
     * via uefi_var_set AFTER the file write succeeds. */
    extern void boot_audit_publish(void);
    boot_audit_publish();

    /* Per-entry health-gated mark-good gate. Reads the bootloader's
     * CurBootCtr handoff, runs registered required + wanted checks,
     * writes a one-line record to X:\Boot\health.jsonl, and on a clean
     * pass composes the MarkGood UEFI variable so the next bootloader
     * run can delete the per-entry tries counter file. Layered above
     * the A/B-rollback slot-level mark (unshipped today). */
    {
        extern void boot_health_check_register_defaults(void);
        extern int boot_health_check_run(void);
        boot_health_check_register_defaults();
        (void)boot_health_check_run();
    }

    /* Report degraded subsystems.  Names come from kernel_subsystem_name() --
     * single source of truth in boot_init.c.  Loop bound is SUBSYS_COUNT,
     * not a hard-coded constant, so new SUBSYS_* slots are reported
     * automatically. (Drift caught by Codex 2026-04-08: this used to have
     * a 20-entry copy of the names table and an `i < 20` cap, silently
     * dropping SUBSYS_OB and any slot added after it.) */
    if (g_boot_info.degraded_mask) {
        uint32_t mask = g_boot_info.degraded_mask;
        uint32_t i;
        klog(LOG_WARN, "boot", "Degraded subsystems:");
        for (i = 0; i < SUBSYS_COUNT && mask; i++) {
            if (mask & (1u << i)) {
                klog(LOG_WARN, "boot", "  - %s",
                     kernel_subsystem_name((kernel_subsys_t)i));
                mask &= ~(1u << i);
            }
        }
    }

    /* Boot-time heap stats */
    {
        uint64_t h_used  = heap_get_used();
        uint64_t h_total = heap_get_total();
        uint64_t pct     = h_total ? (h_used * 100) / h_total : 0;
        klog(LOG_INFO, "heap", "Heap: %u KB used / %u KB total (%u%%)",
               (h_used + 1023) / 1024,
               (h_total + 1023) / 1024,
               pct);
        if (pct > 75)
            klog(LOG_WARN, "heap",
                 "Heap pressure: %u%% used -- risk of silent exhaustion", pct);
    }

    /* --- Demo window --- */
    {
        int demo = wm_create_window("Welcome", 100, 80, 460, 300,
                                     WM_DEFAULT_FLAGS);
        if (demo >= 0) {
            uint32_t *fb = wm_get_framebuffer(demo);
            uint32_t cw = wm_get_client_width(demo);
            uint32_t ch = wm_get_client_height(demo);
            if (fb && cw && ch) {
                gfx_surface_t ws;
                ttf_font_t *fnt;
                gfx_surface_init(&ws, fb, cw, ch, cw);

                wm_fill_rect(demo, 0, 0, cw, ch, 0xFF202020);

                /* Toolbar strip */
                {
                    static const system_icon_t toolbar_icons[] = {
                        ICON_CUT, ICON_COPY, ICON_PASTE,
                        ICON_UNDO, ICON_REDO, ICON_SAVE,
                        ICON_SEARCH, ICON_SETTINGS
                    };
                    int icon_count = 8;
                    int icon_size  = 20;
                    int padding    = 8;
                    int toolbar_y  = 8;
                    int ix;
                    uint32_t toolbar_bg = 0xFF2D2D2D;
                    uint32_t icon_color = 0xFFFFFFFF;

                    gfx_fill_rect(&ws, 0, 0, cw,
                                   icon_size + padding * 2,
                                   toolbar_bg);

                    for (ix = 0; ix < icon_count; ix++) {
                        icon_bitmap_t *bmp = icon_get_variant(
                            toolbar_icons[ix], icon_size,
                            icon_color, ICON_FONT_REGULAR);
                        if (bmp) {
                            int icon_x =
                                padding + ix * (icon_size + padding);
                            icon_draw(&ws, bmp, icon_x, toolbar_y);
                        }
                    }

                    gfx_fill_rect(&ws, 0,
                                   icon_size + padding * 2,
                                   cw, 1, 0xFF383838);
                }

                fnt = ttf_get(FONT_UI_BOLD, 20);
                if (fnt)
                    ttf_draw_string(&ws, fnt, 20, 56,
                                    "Welcome to Impossible OS!",
                                    0xFF60CDFF);

                fnt = ttf_get(FONT_UI, 14);
                if (fnt)
                    ttf_draw_string(&ws, fnt, 20, 86,
                                    "Fluent System Icons loaded from TTF",
                                    0xFFB0B0B0);
            }
        }
    }

    /* --- Terminal + Gallery --- */
    terminal_open();
    gallery_open();

    /* Firmware-table inventory JSON publish to X:\Diag\firmware-
     * tables.json runs BEFORE task_create + scheduler_enable so its
     * file I/O happens on the BSP main thread (not racing against
     * cmd.exe loading) AND its diagnostic logs are captured by
     * scripts/test-smoke.sh which tears down once "Boot complete"
     * appears.  BlackBox VFS+IXFS are mounted by Phase 3 when this
     * runs.  Failure is logged but does not block userland entry. */
    {
        extern void firmware_tables_publish_json(void);
        firmware_tables_publish_json();
    }

    /* Firmware-update advisor: load X:\Diag\lvfs-metadata.json (offline
     * cache), join with ESRT, populate HKLM\SOFTWARE\Impossible\
     * FirmwareAdvisor for the desktop notification UX + sysinfo CLI.
     * Read-only -- no UpdateCapsule, no OsIndications, no ESP staging
     * (refusal enforced by firmware_capsule_refused.c sentinel).  Runs
     * here so the registry surface is populated before cmd.exe loads
     * sysinfo.exe.  Cache absent -> degrades to status=unknown. */
    {
        extern void firmware_advisor_init(void);
        firmware_advisor_init();
    }

    /* Boot-error history ring: append the kernel-Phase-3 sentinel.
     * Subsystem _init calls have all returned successfully and userland
     * threads are not yet running; this is the canonical "kernel
     * reached steady state" mark.  Best-effort: a SetVariable failure
     * is logged but does not block userland entry. */
    boot_history_kernel_mark_phase3();

    /* Consolidated boot-health audit JSON: writes X:\Diag\boot-health.json
     * with degraded caps / subsystems / missing capabilities / perf
     * breaches / MAT W^X violations / firmware quirks / recent boot times /
     * Secure Boot state.  Runs LAST among Phase-3 publishers (after
     * firmware-tables, advisor, and the kernel Phase-3 history sentinel)
     * so recent_boot_times reflects the current-boot mark and per-artifact
     * health is not self-incomplete.  Failure is LOG_WARN only -- never
     * blocks userland entry. */
    {
        extern void boot_health_publish_json(void);
        boot_health_publish_json();
    }

    /* Boot-trend rolling regression file -- writes X:\Perf\boot-trend.json
     * with the last 16 boots' per-phase durations and emits BOOT-TREND
     * WARN when a phase's 3-run median grew >15%.  Atomic via
     * vfs_rename_ex.  Failure is LOG_WARN only. */
    {
        extern void boot_trend_publish_json(void);
        boot_trend_publish_json();
    }

    /* --- Load cmd.exe via task_exec (single PEB allocation path) --- */
    {
        extern void shell_loader_func(void);
        task_create(shell_loader_func, "cmd.exe");
    }

    scheduler_enable();

    /* Yield to give cmd.exe CPU time to print its banner */
    {
        int i;
        for (i = 0; i < 200; i++)
            yield();
    }

    /* --- Compositor mode gating (TODO-05 headless compositor) ---
     * `compositor=headless` in boot.conf suppresses real VSYNC + fb_swap
     * so tests can drive frames deterministically via
     * compositor_step_frames(N). Refuse the flag on bare metal: the
     * user expects the monitor to show the desktop and headless would
     * silently leave them with a black screen. Hypervisors (QEMU /
     * VBox / WHPX) are the only supported headless platforms. */
    if (g_boot_info.config.compositor == 1) {
        if (platform_get() == PLATFORM_BARE_METAL) {
            /* Route through boot_halt() so a physical monitor actually
             * shows the misconfig message. klog(LOG_FATAL, ...) already
             * halts internally -- calling boot_halt first paints the
             * framebuffer halt screen before klog's own halt trips.
             * Codex [M] review: the prior klog-then-hlt path left
             * the screen black on the only platform where the user
             * needs to see the reason. */
            boot_halt("boot.conf compositor=headless rejected on bare metal "
                      "(physical display present; use compositor=normal)");
        }
        klog(LOG_INFO, "compositor",
             "headless mode enabled via boot.conf (no fb_swap, no VSYNC)");
        compositor_set_headless(1);
    }

    /* --- Compositor event loop (never returns) --- */
    compositor_run();

    /* Unreachable under normal operation */
    klog(LOG_FATAL, "boot", "compositor_run() returned -- halting");
    for (;;)
        __asm__ volatile ("hlt");
}

/* ---- Legacy wrapper ----------------------------------------------------- */

void boot_desktop_init(void)
{
    boot_phase3();
}
