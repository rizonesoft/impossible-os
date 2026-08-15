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
#include "kernel/entropy.h"
#include "kernel/csprng.h"
#include "kernel/cpuid_platform.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/boot_reserved.h"
#include "kernel/boot_version.h"
#include "kernel/boot_load_status.h"
#include "kernel/panic.h"
#include "kernel/timer.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/drivers/ahci.h"
#include "kernel/drivers/watchdog.h"
#include "kernel/sched/task.h"
#include "kernel/sched/workqueue.h"
#include "kernel/sched/syscall.h"
#include "kernel/env.h"
#include "kernel/tunables.h"
#include "kernel/quota/quota.h"
#include "kernel/policy_lock.h"
#include "kernel/boot_status.h"
#include "kernel/config.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/nt_misc.h"   /* nt_misc_atoms_init -- atom table backing */
#include "kernel/pe.h"
#include "kernel/boot_halt.h"
#include "kernel/etw.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_timing.h"
#include "kernel/sched/dpc.h"
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
#include "libc/string.h"   /* snprintf for the flush-progress splash line */
#include "kernel/ipc/alpc.h"
#include "kernel/main/shell_loader.h"
#include "main/main_internal.h"

/* ---- Phase 3 ------------------------------------------------------------ */

/* Splash progress callback for the boot-end forced klog flush: shows
 * "Writing boot log... N/M entries" on the splash diagnostic line so a slow USB
 * drain looks alive. boot_splash_diag() no-ops once the splash is finished. */
static void boot_flush_splash_progress(uint32_t written, uint32_t total)
{
    char msg[64];
    if (snprintf(msg, sizeof(msg), "Writing boot log... %u/%u entries",
                 (unsigned)written, (unsigned)total) > 0)
        boot_splash_diag(msg);
}

void boot_phase3(void)
{
    /* --- Scheduler: requires HEAP + TIMER --- */
    if (!kernel_subsystem_ready(SUBSYS_HEAP) ||
        !kernel_subsystem_ready(SUBSYS_TIMER)) {
        boot_recovery_info_t ri = { SUBSYS_SCHED, POST16_SCHED_OK, BOOT_FATAL, 3 };
        kernel_subsystem_dump();
        boot_recovery_action_t act = boot_recovery_show(&ri);
        boot_recovery_act(act);   /* POWEROFF->shutdown, RETRY->reboot; CONSOLE/other fall to halt */
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

    /* Assign the SYSTEM primary token to PID 0 now that the scheduler and PID 0
     * exist and ObpTokenType is registered (ob_init, prior phase). This must
     * precede any later task creation (DPC worker, kworker pool) so descendants
     * inherit a non-NULL primary token at fork/create time. */
    task_assign_initial_token();

    /* Seed PID 0's environment block from the Registry (or a hardcoded fallback
     * if registry population failed). Registry is up (Phase 2) by now, so the
     * full default set lands. (Descendants are DESIGNED to inherit it via
     * env_copy at creation, but that wiring is not live yet -- owned by
     * TODO-12 s7; children do not inherit today.) */
    env_init_kernel_task();

    ahci_enable_events();  /* safe now: yield handler registered */

    /* Start the threaded DPC worker (needs scheduler). Unconditional at boot so
     * a threaded DPC queued from any IRQL always has a worker to drain it. */
    {
        extern void dpc_start_threads(void);
        dpc_start_threads();
    }

    /* Start the system worker thread pool (long-period periodic monitors) and,
     * ONLY if the worker actually started, register the post-boot SecureBoot
     * drift monitor (5-minute cadence) -- otherwise a registered monitor with no
     * worker would falsely report "armed" while never running. */
    {
        extern int kworker_init(void);
        extern void uefi_secureboot_register_monitor(void);
        extern void mono_clock_watchdog_init(void);
        extern void ke_ntp_discipline_init(void);
        if (kworker_init() == 0) {
            uefi_secureboot_register_monitor();
            /* Clocksource drift watchdog: demotes an unstable TSC to HPET/PMTMR.
             * No-op unless TSC is active with an independent reference. */
            mono_clock_watchdog_init();
            /* NTP continuous wall-time discipline applier: consumes stored
             * freq/slew into the wall clock. No-op unless a correction is set. */
            ke_ntp_discipline_init();
        }
    }

    /* Boot-time self-benchmark + auto-tune (TODO-09 S14): measures memory/SIMD/
     * scheduling characteristics, persists them in HKLM\SYSTEM\HwProfile, and
     * tunes SIMD dispatch to the host. Scheduler is armed (task_init) and the
     * registry is up (Phase 2); the compositor has not started. BSP-only,
     * never fails the boot. */
    {
        extern void hw_profile_init(void);
        hw_profile_init();
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

    /* Register the runtime tunable registry's core knobs now that the work
     * queue exists (deferred tunable callbacks enqueue onto sys_wq). */
    kernel_tunables_register_core();

    /* Per-user quota default caps. These live with the tunables (not with the
     * Phase 2 taxonomy validation) because registration must follow the work
     * queue: a change callback re-limits every live USER block and may be
     * dispatched deferred. USER blocks created before this point keep the
     * taxonomy default; a later boot.conf or runtime set fires the callback and
     * re-limits them, so the window costs no enforcement. */
    quota_config_register_tunables();

    /* Arm quota pressure publication. Must follow BOTH knf_init (Phase 2, the
     * publication states live in the notification namespace) and
     * dpc_start_threads above (the deferred publisher is a threaded DPC, and
     * arming before a worker exists would leave records queued with nothing to
     * drain them). The pressure state machine itself has been recording since
     * the first charge; this is the point transport becomes available. */
    {
        extern void quota_pressure_init(void);
        quota_pressure_init();
    }

    /* Arm deferred obligation completion. Same dependency as pressure
     * publication and for the same reason -- the drain is a threaded DPC, so
     * arming before dpc_start_threads would leave obligations queued with no
     * worker to complete them. Until this point a raised-IRQL return completes in
     * place, which is correct but unbounded; quota_ledger_deferrals_forced()
     * counts how many did. */
    {
        extern void quota_ledger_init(void);
        quota_ledger_init();
    }

    /* Register the core feature flags (resolution uses the cmdline + cohort +
     * Secure Boot state, all available by this point). */
    {
        extern void kernel_features_register_core(void);
        kernel_features_register_core();
    }

    /* Register the core security policies and seal the phases whose boot
     * milestones have already passed by Phase 3: the security reference monitor
     * and the registry-sourced policy merge are both complete here, so lockdown
     * level, KASLR/SMEP-SMAP/KPTI, Secure Boot, CI mode, and the boot verifier
     * become immutable (downgrade-blocked). The debugger lockout seals later at
     * POST_USER_MODE, just before the compositor reaches user mode. */
    if (kernel_policy_register_core() != 0)
        boot_halt("policy lock core registration failed -- refusing to seal an incomplete policy plane");
    /* Resolve the boot-status policy + acceptance ledger and register its
     * policy rows BEFORE the POST_REGISTRY seal so they lock with the rest of
     * the boot-critical policy (stricter-only at runtime). Loads the prior-boot
     * durable record for the rollback / recovery-escalation consumers. */
    boot_status_init();
    kernel_policy_lock_phase_advance(POLICY_PHASE_POST_SECURITY_INIT);
    kernel_policy_lock_phase_advance(POLICY_PHASE_POST_REGISTRY);

    /* --- IPC init (pipe, shmem, signal, alpc) --- */
    /* pipe_create/shmem_create init lazily; explicit pipe_init gives the
     * sequencing a uniform entry point. Entry POST is written before init so
     * a hang/fault inside pipe_init/alpc_init shows the IPC stage rather than
     * the prior SCHED milestone. Fatal-severity policy for both is documented
     * at the ipc_rc computation below. */
    {
        POST16(POST16_IPC);
        boot_result_t pipe_rc = pipe_init();
        boot_result_t alpc_rc = alpc_init();
        /* pipe and ALPC are both all-or-nothing for boot: pipe is the global
         * IPC invariant, and alpc_port_init() declares its failures
         * non-recoverable (a partial ObpAlpcPortType / \RPC Control state
         * that the NtAlpc handlers registered unconditionally below would
         * then expose to callers). A fatal from either halts via the
         * recovery screen rather than advertising a half-initialized IPC
         * subsystem as ready. */
        boot_result_t ipc_rc =
            (pipe_rc == BOOT_FATAL || alpc_rc == BOOT_FATAL) ? BOOT_FATAL
          : (pipe_rc == BOOT_DEGRADED || alpc_rc == BOOT_DEGRADED) ? BOOT_DEGRADED
          : BOOT_OK;
        if (ipc_rc == BOOT_FATAL) {
            boot_recovery_info_t ri = { SUBSYS_IPC, POST16_IPC, BOOT_FATAL, 3 };
            kernel_subsystem_dump();
            boot_recovery_action_t act = boot_recovery_show(&ri);
            boot_recovery_act(act);   /* POWEROFF->shutdown, RETRY->reboot; CONSOLE/other fall to halt */
            boot_halt("IPC init failed -- pipe or ALPC unrecoverable");
        }
        if (ipc_rc == BOOT_DEGRADED)
            klog(LOG_WARN, "boot", "IPC subsystem degraded");
        kernel_subsystem_apply_result(SUBSYS_IPC, ipc_rc);
    }
    POST16(POST16_IPC_OK);
    boot_progress(3, "IPC", POST16_IPC_OK);

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
        extern void nt_misc_register_ssdt(void);
        extern int  nt_lpc_register_ssdt(void);
        extern int  nt_alpc_register_ssdt(void);
        extern int  nt_audit_register_ssdt(void);
        extern int  csprng_register_ssdt(void);
        extern int  nt_env_register_ssdt(void);
        extern void pledge_register_ssdt(void);
        extern void nt_job_register_ssdt(void);
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
        /* Back the atom table BEFORE publishing the atom syscall handlers just
         * below: nt_misc_register_ssdt() registers them unconditionally, so an
         * unbacked table would leave callable handlers over a NULL pointer.
         * A failure here is not fatal to boot -- the handlers stay registered
         * and fail closed with STATUS_INSUFFICIENT_RESOURCES -- but it is a
         * real degradation, so say so on the log. */
        if (nt_misc_atoms_init() != STATUS_SUCCESS)
            klog(LOG_ERROR, "boot", "Atom table unbacked -- atom syscalls degraded");
        nt_misc_register_ssdt();
        pledge_register_ssdt();
        nt_job_register_ssdt();
        /* Normalize every registrar to a 0/1 failure flag before summing: some
         * return raw ssdt_register() results (-1 on failure) while others return
         * nonnegative failure counts, so a mixed-sign sum could otherwise cancel
         * to zero and let boot continue with missing SSDT handlers. */
        reg_failures  = (nt_lpc_register_ssdt()   != 0);
        reg_failures += (nt_alpc_register_ssdt()  != 0);
        reg_failures += (nt_audit_register_ssdt() != 0);
        reg_failures += (csprng_register_ssdt()   != 0);
        reg_failures += (nt_env_register_ssdt()   != 0);
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

    /* --- Exec loader (ELF/PE/EIF format handlers) --- */
    /* Entry POST before init so a fault inside exec_init attributes to the
     * EXEC stage. ELF-registration failure is fatal (no native loader = no
     * userspace); an optional-format failure (EIF/PE) only degrades. */
    {
        extern boot_result_t exec_init(void);
        POST16(POST16_EXEC);
        boot_result_t exec_rc = exec_init();
        if (exec_rc == BOOT_FATAL) {
            boot_recovery_info_t ri = { SUBSYS_EXEC, POST16_EXEC, BOOT_FATAL, 3 };
            kernel_subsystem_dump();
            boot_recovery_action_t act = boot_recovery_show(&ri);
            boot_recovery_act(act);   /* POWEROFF->shutdown, RETRY->reboot; CONSOLE/other fall to halt */
            boot_halt("exec init failed -- no binary loader available");
        }
        if (exec_rc == BOOT_DEGRADED)
            klog(LOG_WARN, "boot",
                 "exec subsystem degraded (some formats unavailable)");
        kernel_subsystem_apply_result(SUBSYS_EXEC, exec_rc);
    }
    POST16(POST16_EXEC_OK);
    boot_progress(3, "EXEC", POST16_EXEC_OK);

    /* --- Boot tests (debug=1 only) --- */
    boot_tests_run();

    /* --- Fonts, icons, cursors --- */
    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Desktop ----------------------------------------------------------------");

    boot_splash_status("Flushing boot log...");
    klog_disk_flush();

    /* Runtime crash test -- crash_test=1 in boot.conf. Matched exactly, not
     * for truthiness: crash_test=2 selects the LATE pre-compositor site
     * further down, and a truthy test here would fire first and never let
     * that boot reach it. */
    if (g_boot_info.config.crash_test == 1) {
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
    /* Re-post the degraded-randomness warning LAST on the splash diag
     * line: Phase-2 storage diagnostics reuse and clear that line, so
     * the Phase-1 post from early_entropy_init() may already be gone.
     * The credited class is final (Phase-3 carryover absorbs at Q_LOW). */
    if (csprng_credited_class() == ENTROPY_CLASS_DEGRADED)
        boot_splash_diag("WARNING: degraded randomness -- no hardware "
                         "entropy source credited");
    /* Surface an unexpected-shutdown notice if the previous boot crashed
     * (section 5). No toast infra exists; the splash diag line is the visible
     * surface and serial klog is authoritative. The full record is written to
     * X:\Crash\last-panic.txt below. */
    if (panic_had_previous_crash()) {
        /* Point the operator at the SAME path panic.c actually wrote to:
         * X:\Crash\ when BlackBox is mounted, else the C:\ fallback. A
         * hardcoded X:\ hint would misdirect on a BlackBox-absent boot. */
        extern int klog_using_blackbox;
        const char *pp = klog_using_blackbox
            ? "X:\\Crash\\last-panic.txt"
            : "C:\\Impossible\\System\\Logs\\last-panic.txt";
        char msg[96];
        const char *pre = "NOTICE: system shut down unexpectedly -- see ";
        int mp = 0, j;
        for (j = 0; pre[j] && mp < (int)sizeof(msg) - 1; j++) msg[mp++] = pre[j];
        for (j = 0; pp[j] && mp < (int)sizeof(msg) - 1; j++) msg[mp++] = pp[j];
        msg[mp] = '\0';
        boot_splash_diag(msg);
        klog(LOG_WARN, "boot",
             "[PANIC] previous boot crashed; evidence -> %s", pp);
    }
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

    /* Surface FAILED/DEGRADED subsystem loads (section 11) AFTER deferred init
     * so the summary reflects the COMPLETE record -- deferred network/input
     * outcomes are recorded inside boot_run_deferred(). Serial klog is the
     * authoritative triage channel; boot_splash_diag is best-effort (a no-op
     * once the splash is inactive). The full per-entry log lands in
     * X:\Diag\boot-load-status.txt below. */
    boot_load_status_report_summary();

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

    /* One-shot admin external-entropy consume (Win11 ExternalEntropy
     * parity): for an HKLM\SYSTEM\Boot\Entropy\ExternalEntropy value it
     * ABSORBS (destroying the value first) only when registry persistence is
     * inactive (in-memory-only, no on-disk recovery source); a persistent
     * on-disk offering is left untouched and NOT absorbed (deferred to the
     * hive-load wiring). The diagnostics mirror is a separate sub-key, so a
     * deferred offering never appears in it. Runs BEFORE the summary +
     * diagnostics surfaces, so the registry/JSON mirror reflects the
     * post-consume-or-defer counters, not a guaranteed injection. */
    {
        extern void entropy_external_consume(void);
        entropy_external_consume();
    }

    /* Entropy source summary -- one line, WARN when degraded. Collectors
     * land with the early-entropy TODO sections; until then this honestly
     * reports all-none rather than implying randomness we do not have. */
    {
        extern void entropy_report(void);
        entropy_report();
    }

    /* Diagnostics surfaces (mask/quality/class only -- never seed bytes):
     * HKLM\SYSTEM\Boot\Entropy registry mirror + X:\Diag\entropy.json. */
    {
        extern void entropy_populate_registry(void);
        extern void entropy_publish_json(void);
        entropy_populate_registry();
        entropy_publish_json();
    }

    boot_timing_print_steps();
    boot_perf_dump();
    boot_perf_compare();
    /* One line per DPC that crossed the watchdog threshold, with its count and
     * worst case. The per-occurrence warning is burst-limited (3 per routine),
     * so without this a chronic offender would be under-reported rather than
     * over-reported -- suppressing spam must not suppress signal. */
    dpc_watchdog_report();
    boot_perf_save();
    boot_timing_write_report();
    boot_postcode_write_log();
    boot_timeline_dump_json();

    /* Write hardware inventory to X:\Diag\hwdump.txt */
    hw_dump_write_file();

    /* Export the measured-boot TCG event log (parsed in Phase 0) as a
     * CEL-JSON subset now that X:\ is mounted. No-op without a clean parse. */
    {
        extern void tpm_evlog_export_cel(void);
        tpm_evlog_export_cel();
    }

    /* Export the TPM-rooted boot attestation report (identity + handoff + the
     * SHA-256 quoted PCR bank + quote/AK/EK) as X:\Diag\attestation.json. */
    {
        extern void tpm_attest_report_export(void);
        tpm_attest_report_export();
    }

    /* Boot protocol section 6: dump the boot_reserved region table to
     * X:\Diag\boot-reserved.json so a post-boot user can audit
     * exactly which physical ranges the PMM kept reserved. */
    boot_reserved_blackbox_dump();

    /* Boot protocol section 7: if the prior boot halted on a protocol
     * mismatch, transcribe the NVRAM fault record to
     * X:\Diag\boot-proto-fault.txt and clear the NVRAM slot. No-op
     * if no record exists. */
    boot_version_blackbox_transcribe();

    /* Dump the healthy-boot bootloader build identity (git_sha + build time +
     * label) to X:\Diag\boot-loader-identity.txt for offline triage. */
    boot_loader_identity_dump_to_blackbox();

    /* Per-subsystem boot load/status log (section 11, ntbtlog parity). */
    boot_load_status_dump_to_blackbox();

    /* If the previous boot crashed, emit its forensic record (section 5). */
    panic_evidence_write_blackbox();

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

    /* Forced final log flush -- the LAST pre-userland point: AFTER all Phase-3
     * publishers and BEFORE task_create(cmd.exe)/scheduler_enable() so NO userland
     * thread runs or races the drain. On slow media the per-subsystem flushes
     * auto-switched to deferred (RAM batching); this single guard-aware forced flush
     * drains the whole boot tail in one batched write and latches deferral off. */
    /* Show flush progress on the splash diagnostic line during the slow-media drain,
     * then clear the cb so post-boot live-mode flushes never touch the finished splash. */
    klog_disk_set_flush_progress_cb(boot_flush_splash_progress);
    klog_disk_flush_all();
    klog_disk_set_flush_progress_cb((klog_flush_progress_fn)0);

    /* Disarm the hardware watchdog at the boot->userland handoff: AFTER the
     * final log flush, BEFORE task_create/scheduler_enable. No runtime petter
     * exists yet, so the desktop must never run with the watchdog armed
     * (no-op unless a WDAT watchdog was armed; readback-verified inside). If
     * disarm cannot be confirmed, halt -- entering the desktop armed would
     * reboot the machine with nothing to pet it. */
    if (hw_watchdog_boot_handoff() != 0)
        boot_halt("HW watchdog disarm failed at boot handoff");

    /* --- Load cmd.exe via task_exec (single PEB allocation path) --- */
    task_create(shell_loader_func, "cmd.exe");

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

    /* Boot init complete: seal boot-only tunables before steady state. */
    kernel_tunable_lock_phase_advance(TUNABLE_PHASE_RUNTIME);

    /* User mode is now reachable via the compositor: seal the final policy
     * phase so the debugger lockout and any POST_USER_MODE policies freeze. */
    kernel_policy_lock_phase_advance(POLICY_PHASE_POST_USER_MODE);

    /* On a debug boot, dump the whole config plane now that EVERY registry is at
     * its final sealed state (tunables RUNTIME-sealed, policy POST_USER_MODE) --
     * a single steady-state, serial-visible support snapshot before the
     * compositor takes over. Secrets redacted; not a panic-path call. */
    {
        const kernel_config_t *cfg_snap = kernel_config_get();
        if (cfg_snap && cfg_snap->debug_enabled)
            config_dump();
    }

    /* crash_test=2 does NOT fire here. Its site is inside the compositor
     * loop, immediately before the first composite -- see compositor.c.
     * Stopping the boot ahead of compositor_run() would leave the entry
     * into the compositor untested, and that is precisely where a
     * relocated anti-rollback raise would hide. */

    /* --- Compositor event loop (never returns) --- */
    POST16(POST16_COMPOSITOR);  /* attribute a crash/hang entering the compositor */
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
