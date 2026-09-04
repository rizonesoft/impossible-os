/* ============================================================================
 * boot_storage.c -- Phase 2: System Services
 *
 * PCI, peripherals, disk drivers, VFS, partition mount, registry, SMP,
 * network, symbol table, mmap. BOOT_FATAL only if VFS or registry are
 * completely broken; everything else degrades.
 *
 * Provides: boot_phase2() and the legacy boot_storage_init() wrapper.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/boot_media.h"
#include "kernel/printk.h"
#include "kernel/hw_dump.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/mmap.h"
#include "kernel/fs/vfs.h"
#include "kernel/nt/nls.h"
#include "kernel/wer.h"
#include "kernel/fs/fat32.h"
#include "kernel/fs/partition.h"
#include "kernel/smp.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_init.h"
#include "kernel/cpu_security.h"   /* cpu_audit_ensure_bsp (S9 per-CPU audit) */
#include "kernel/boot_halt.h"
#include "kernel/boot_recovery.h"
#include "kernel/boot_load_status.h"   /* ntbtlog-parity load/status log (section 11) */
#include "kernel/acpi.h"
#include "registry.h"
#include "kernel/symtab.h"
#include "kernel/cpuid_platform.h"
#include "kernel/config.h"   /* boot_arg_resolved_ival -- S27 injection keys */
#include "kernel/drivers/pci.h"
#include "kernel/drivers/xhci.h"
#include "kernel/drivers/usb_legacy.h"
#include "kernel/drivers/usb_boot_report.h"
#include "kernel/drivers/xhci_dev.h"
#include "kernel/drivers/rtl8139.h"
#include "kernel/net/net.h"
#include "kernel/drivers/virtio_input.h"
#include "kernel/drivers/mouse.h"
#include "kernel/drivers/usb_input_diag.h"
#include "kernel/drivers/vbox_mouse.h"
#include "kernel/drivers/ata.h"
#include "kernel/drivers/virtio_blk.h"
#include "kernel/drivers/ahci.h"
#include "kernel/drivers/nvme.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/timer.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_trace.h"
#include "kernel/quota/quota.h"
#include "kernel/knf/knf.h"
#include "kernel/ex.h"
#include "main/main_internal.h"
#include "kernel/pm.h"

/* ---- Deferred init wrappers --------------------------------------------- */

/* Network: rtl8139 + net stack + DHCP -- not needed for desktop */
static boot_result_t deferred_net_init(void)
{
    POST16(POST16_DEFERRED_NET);
    int nic = rtl8139_init();
    if (nic < 0) {
        POST16(POST16_DEFERRED_NET_OK);
        /* rtl8139 is an OPTIONAL NIC; its absence (the common <0 case) is not a
         * degraded boot -- record SKIPPED, not DEGRADED, so a NIC-less machine
         * does not emit a spurious "[BOOT-LOAD] degraded" on every clean boot.
         * (rtl8139_init returns -1 for absent AND found-but-failed; splitting
         * those into SKIPPED vs FAILED is the granular-coverage follow-up.) */
        boot_load_record("network", BOOT_LOAD_CLASS_NET, BOOT_LOAD_SKIPPED,
                         0u, POST16_DEFERRED_NET_OK);
        return BOOT_DEGRADED;  /* no NIC found -- not fatal */
    }
    net_init();
    dhcp_discover();
    POST16(POST16_DEFERRED_NET_OK);
    boot_load_record("network", BOOT_LOAD_CLASS_NET, BOOT_LOAD_LOADED,
                     0u, POST16_DEFERRED_NET_OK);
    return BOOT_OK;
}

/* Optional input: VirtIO tablet + VBox absolute mouse -- PS/2 is sufficient */
static boot_result_t deferred_input_init(void)
{
    POST16(POST16_DEFERRED_INPUT);
    /* PS/2 mouse init moved from Phase 1 to deferred: the PS/2 BAT
     * (Basic Assurance Test) takes 300ms-2s on real hardware, which
     * froze the splash spinner. Running it here means the splash
     * is already animating when the wait happens. */
    mouse_init();                  /* PS/2 (void): the always-attempted baseline */
    int vi = virtio_input_init();  /* 0 = up, <0 = absent-or-failed (optional) */
    int vb = vbox_mouse_init();    /* 0 = up, <0 = absent-or-failed (optional) */
    /* Record the input load status so it is no longer silently reported as a
     * blanket success: the optional absolute-pointer devices are SKIPPED when
     * both are absent/failed (PS/2 mouse is the baseline, so never fatal),
     * LOADED when either came up. Distinguishing absent vs detected-but-failed
     * needs per-driver granularity -- the same follow-up as the rtl8139 path. */
    boot_load_record("input", BOOT_LOAD_CLASS_INPUT,
                     (vi == 0 || vb == 0) ? BOOT_LOAD_LOADED : BOOT_LOAD_SKIPPED,
                     0u, POST16_DEFERRED_INPUT_OK);
    /* All input sources are now up (keyboard in Phase 1, USB HID enumerated in
     * xhci_init, PS/2 mouse + VirtIO + VBox just now) -- emit the diagnostic
     * source summary + per-USB-HID identity + HID poll error stats. */
    usb_input_diag_report();
    POST16(POST16_DEFERRED_INPUT_OK);
    return BOOT_OK;
}

/* ---- Async init wrappers (boot_result_t) -------------------------------- */

static boot_result_t async_ata_init(void)
{
    /* ata_init() is void -- legacy PIO driver; absence is not failure
     * (modern systems use AHCI/NVMe). Always BOOT_OK. */
    ata_init();
    return BOOT_OK;
}

static boot_result_t async_ahci_init(void)
{
    int rc = ahci_init();
    if (rc != 0) {
        klog(LOG_WARN, "boot", "AHCI init failed (rc=%d)", (uint64_t)rc);
        return BOOT_DEGRADED;
    }
    return BOOT_OK;
}

static boot_result_t async_nvme_init(void)
{
    /* nvme_init() returns the COUNT of initialized controllers (>= 0), not a
     * 0/-1 status: a positive count is SUCCESS, and zero (no NVMe present) is
     * not a failure on a SATA/AHCI system -- same "absence is not failure"
     * contract as async_ata_init. Treating nonzero as degraded falsely marked
     * the whole storage span DEGRADED on a healthy multi-controller NVMe boot. */
    int n = nvme_init();
    if (n > 0)
        klog(LOG_INFO, "boot", "NVMe: %d controller(s) ready", (uint64_t)n);
    return BOOT_OK;
}

static boot_result_t async_virtio_blk_init(void)
{
    int rc = virtio_blk_init();
    if (rc != 0) {
        klog(LOG_WARN, "boot", "VirtIO-blk init failed (rc=%d)", (uint64_t)rc);
        return BOOT_DEGRADED;
    }
    return BOOT_OK;
}

/* ---- Phase 2 ------------------------------------------------------------ */

/* Substring test for the BlackBox cleanup's rotated-log matcher. */
static int bb_name_contains(const char *s, const char *sub)
{
    for (; *s; s++) {
        const char *a = s, *b = sub;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

void boot_phase2(void)
{
    uint32_t i;
    uint64_t total_ram = 0;

    /* --- Resource quota registry: validate the resource-type taxonomy at the
     * very start of Phase 2 -- before SMP, storage, VFS, and any subsystem that
     * will later charge quota. The table is static const (valid at link time);
     * this validates it and halts boot on a malformed table (the full table is
     * dumped to serial only in test boots -- see below). --- */
    POST16(POST16_QUOTA);
    if (quota_register_types() == BOOT_FATAL)
        boot_halt("Resource quota registry validation failed");
    /* The one-line validated-count summary above ships on every boot; the full
     * 14-row table is diagnostic metadata (~1 KB of polled serial), so emit it
     * only in test boots to keep production boot latency down. */
    if (g_boot_info.config.test)
        quota_types_dump();
    POST16(POST16_QUOTA_OK);
    boot_progress(2, "QUOTA", POST16_QUOTA_OK);

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Storage & Filesystem ---------------------------------------------------");

    /* --- PCI + peripherals (moved from Phase 1) --- */
    klog(LOG_DEBUG, "boot", "--- Phase: PCI & peripherals ---");
    boot_splash_tick();
    boot_splash_status("Scanning PCI bus...");
    POST16(POST16_PCI);
    pci_scan();
    POST16(POST16_PCI_OK);
    /* Detect legacy USB host controllers (EHCI/UHCI/OHCI) for the no-xHCI
     * fallback diagnostic below. Detection only -- the legacy HCD is pending. */
    usb_legacy_scan();
    POST16(POST16_XHCI);
    int xhci_n = xhci_init();
    POST16(POST16_XHCI_OK);
    /* If no xHCI but a legacy USB controller exists, log which one and skip
     * gracefully (no hang) rather than silently leaving USB boot unavailable. */
    usb_legacy_announce(xhci_n);
    if (g_boot_info.config.deferred) {
        /* Defer non-critical peripherals until after desktop is up */
        boot_defer("network", deferred_net_init);
        boot_defer("input",   deferred_input_init);
        /* network load-status is recorded from deferred_net_init() with the
         * real post-defer outcome, not here at registration time. */
        boot_progress(2, "PCI_NET_DEFERRED", POST16_PCI_OK);
    } else {
        /* Legacy: all subsystems init in-phase */
        boot_splash_status("Initializing network...");
        POST16(POST16_NIC);
        int nic = rtl8139_init();
        POST16(POST16_NIC_OK);
        /* Only bring up the network stack when a NIC is actually present.
         * net_init()/dhcp_discover() reach rtl8139_send(), which transmits
         * through tx_buffers[] -- on a NIC-less machine (nic < 0) those
         * buffers were never allocated, so transmitting is a wild write.
         * The deferred path gates identically (deferred_net_init early-returns
         * on nic < 0). Absent optional NIC -> SKIPPED, not a fake LOADED. */
        if (nic >= 0) {
            POST16(POST16_NET);
            net_init();
            POST16(POST16_NET_OK);
            boot_load_record("network", BOOT_LOAD_CLASS_NET, BOOT_LOAD_LOADED,
                             0u, POST16_NET_OK);
        } else {
            /* Emit the terminal network POST before recording (mirrors the
             * deferred path's POST-then-record order); net_init()/dhcp are
             * skipped entirely on a NIC-less boot. */
            POST16(POST16_NET_OK);
            boot_load_record("network", BOOT_LOAD_CLASS_NET, BOOT_LOAD_SKIPPED,
                             0u, POST16_NET_OK);
        }
        mouse_init();   /* legacy in-phase: init PS/2 mouse here (the deferred
                         * path does it in deferred_input_init) so the diagnostic
                         * runs after every input source, like the deferred path */
        virtio_input_init();
        vbox_mouse_init();
        usb_input_diag_report();   /* input-source summary (legacy in-phase path) */
        boot_progress(2, "PCI_NET", POST16_PCI_OK);

        /* DHCP fire-and-forget -- only when a NIC actually came up. Do not
         * claim a DHCP milestone on a NIC-less boot (it would post a code for
         * work that never ran). */
        if (nic >= 0) {
            dhcp_discover();
            boot_progress(2, "DHCP", POST16_NET_OK);
        }
    }

    /* Spectre v2 eIBRS (TODO-10 S8): set the BSP's IA32_SPEC_CTRL.IBRS once,
     * post-IDT, BEFORE smp_init() so the BSP's SPEC_CTRL audit (captured in
     * cpu_record_bsp_profile) reflects the eIBRS bit -- consistent with the APs,
     * which set + audit their own during smp bringup. No-op without Enhanced IBRS
     * (legacy CPUs use retpoline). Unconditional so a no-ACPI boot is covered. */
    cpu_program_bsp_eibrs();

    /* --- SMP: moved before storage so async init can use APs --- */
    if (g_boot_info.acpi_available) {
        boot_splash_status("Initializing SMP...");
        POST16(POST16_SMP);
        smp_init();
        POST16(POST16_SMP_OK);
        kernel_subsystem_set_ready(SUBSYS_SMP, true);
        boot_progress(2, "SMP", POST16_SMP_OK);
    }

    /* Per-CPU register audit (TODO-09-boot S9). smp_init() emits the [CPU%u
     * AUDIT] lines + consistency verdict on the ACPI path; this fallback (a
     * no-op when smp_init already audited the BSP) guarantees the BSP audit on
     * a non-ACPI/degraded boot that skipped smp_init entirely. */
    cpu_audit_ensure_bsp();
    boot_progress(2, "CPU_AUDIT", POSTCODE_CPU_AUDIT);

    /* WAITPKG anti-DoS bound (TODO-09 S19): program the BSP's IA32_UMWAIT_CONTROL
     * here, unconditionally and post-IDT, so the bound applies even on a
     * no-ACPI/degraded boot that skipped smp_init() (idempotent on the ACPI
     * path). APs program their own copy during smp bringup. */
    cpu_program_bsp_umwait();

    /* IBPB writability latch (TODO-10 S8): probe PRED_CMD on the BSP post-IDT so
     * the scheduler IBPB hot path is a fault-free atomic check (each AP probes at
     * its ap_cpu_harden tail). */
    cpu_probe_ibpb();

    /* MDS VERW gate + TAA TSX-disable (TODO-10 S19): decide on the BSP post-IDT
     * (reads IA32_ARCH_CAPABILITIES); each AP re-decides at its ap_cpu_harden tail. */
    cpu_decide_mds();

    /* CPU topology: Zen CCD/NUMA + Intel hybrid P/E-core detection */
    {
        extern void topology_init(void);
        topology_init();
    }

#ifdef KERNEL_TESTS
    /* Degraded-configuration injection (TODO-10 S27): park an already-online
     * CPU and check the live-versus-present relations on a live boot. Placed
     * AFTER topology_init() deliberately -- the point is that a CPU going away
     * does NOT move the discovery-scoped counts, which is only a claim worth
     * testing once those counts have been established. */
    {
        int64_t park = boot_arg_resolved_ival("test_park_cpu");
        /* The RESULT is the verdict, and it is checked. Discarding it let a
         * rejected target or a violated relation pass as a healthy boot, which
         * is exactly the false-coverage shape this injection exists to
         * eliminate: a harness keying on "Boot complete" would go green having
         * proven no live/present/topology relation at all. */
        if (park > 0 && !smp_test_park_cpu((uint32_t)park))
            klog(LOG_ERROR, "boot",
                 "[S27] test_park_cpu=%u did NOT establish the degraded "
                 "configuration -- treat this boot as FAILED coverage",
                 (uint32_t)park);
    }
#endif

    /* Performance monitoring counters (Intel PMU / AMD PMC) */
    {
        extern void pmc_init(void);
        pmc_init();
    }

    /* Register async init IPI handler (safe even with 1 CPU) */
    boot_async_init();

    /* --- Disk drivers --- */
    klog(LOG_DEBUG, "boot", "--- Phase: disk drivers ---");
    boot_splash_status("Initializing storage...");

    /* Load/status log (section 11): measured-duration span around the storage
     * probe block. begin..finish gives a real duration even when the async
     * group fans the four drivers across CPUs in parallel. */
    int storage_tok = boot_load_begin("storage", BOOT_LOAD_CLASS_STORAGE);
    uint8_t  storage_state = BOOT_LOAD_LOADED;
    uint16_t storage_err   = 0u;
    uint16_t storage_post  = POST16_NVME_OK;   /* sequential terminal (last driver) */
    /* Drivers the recovery path could not make safe (TODO-10 S27). Carried out
     * of the async branch because the consumers it gates -- interrupt setup and
     * block registration -- run below for BOTH branches. Zero on the sequential
     * path: every initializer there ran to completion on this CPU. */
    uint32_t storage_unsafe = 0;
#ifdef KERNEL_TESTS
    /* A REQUESTED HOLD THAT NOBODY HONOURS MUST SAY SO (TODO-10 S27). Only the
     * target AP reads `test_hold_async_cpu`, and it only reads it inside the
     * async IPI handler -- so on a single-CPU boot, with async_init=0, or when
     * the named slot is simply never claimed as a worker for this group, the
     * key is silently ignored, storage initializes sequentially, and the boot
     * reaches "Boot complete" clean. A harness keying on completion would then
     * record degraded-path coverage for a run in which no worker was ever
     * held. Validated here and confirmed after dispatch, below. */
    int64_t hold_req = boot_arg_resolved_ival("test_hold_async_cpu");
#endif
    if (g_boot_info.config.async_init && smp_cpu_count() > 1) {
        storage_post = POST16_ASYNC_DONE;      /* async group terminal */
        /* Async: probe storage drivers in parallel across CPUs */
        boot_async_step_t storage_steps[] = {
            { "ATA",       async_ata_init },
            { "AHCI",      async_ahci_init },
            { "NVMe",      async_nvme_init },
            { "VirtIO-blk", async_virtio_blk_init },
        };
        /* Per-driver POST pairs, indexed to match storage_steps[] above, so the
         * fallback emits the same crash/recovery telemetry as the sequential
         * branch for exactly the drivers it actually re-ran. VirtIO-blk has no
         * POST pair of its own in the sequential branch either; 0 means "emit
         * nothing" rather than inventing a code the decoder has never seen. */
        static const uint16_t storage_post_pre[4] = {
            POST16_ATA, POST16_AHCI, POST16_NVME, 0
        };
        static const uint16_t storage_post_ok[4] = {
            POST16_ATA_OK, POST16_AHCI_OK, POST16_NVME_OK, 0
        };
        /* Which driver each step index degrades, NAMED rather than derived.
         * This was `(1u << i)` guarded by a _Static_assert that the
         * BLKDEV_UNSAFE_* bits equal 1<<0..1<<3 -- which is a tautology over
         * constants and could not observe storage_steps[] at all, so the
         * reorder it claimed to catch would still have mapped a degraded NVMe
         * onto the AHCI bit. A table cannot detect a reorder either; what it
         * does is put the mapping ON the line being reordered, so moving a step
         * without its bit is visible in the diff instead of inferred. */
        static const uint32_t storage_unsafe_bit[] = {
            BLKDEV_UNSAFE_ATA, BLKDEV_UNSAFE_AHCI,
            BLKDEV_UNSAFE_NVME, BLKDEV_UNSAFE_VIRTIO
        };
        /* One step count for the dispatch, the fallback loop and every parallel
         * table. The literal 4 was repeated at each of those, so adding a fifth
         * storage driver would have dispatched it and gated it nowhere while
         * every check stayed green. */
        enum { STORAGE_STEP_COUNT =
                   (uint32_t)(sizeof storage_steps / sizeof storage_steps[0]) };
        _Static_assert(sizeof storage_post_pre == sizeof storage_post_ok,
                       "storage POST pre/ok tables must stay the same length");
        _Static_assert(sizeof storage_post_pre / sizeof storage_post_pre[0]
                           == STORAGE_STEP_COUNT &&
                       sizeof storage_unsafe_bit / sizeof storage_unsafe_bit[0]
                           == STORAGE_STEP_COUNT,
                       "every per-step storage table must cover every step");
        _Static_assert(STORAGE_STEP_COUNT <= BOOT_ASYNC_MAX_STEPS,
                       "storage group must fit the outcome record");
        boot_async_outcome_t outcome;
        boot_result_t async_rc = boot_async_group_ex("storage", storage_steps,
                                                     STORAGE_STEP_COUNT,
                                                     &outcome);
#ifdef KERNEL_TESTS
        /* Confirm the requested worker was actually DISPATCHED. The group only
         * claims as many APs as it can, and a slot that is offline, already
         * busy, or beyond the step count is never woken -- in which case the
         * hold never happened, whatever the key said. */
        if (hold_req > 0) {
            int held = 0;
            for (uint32_t i = 0; i < STORAGE_STEP_COUNT; i++)
                if (outcome.step[i].worker_cpu == (uint32_t)hold_req)
                    held = 1;
            if (!held)
                klog(LOG_ERROR, "boot",
                     "[S27] FAILED coverage: test_hold_async_cpu=%u was never "
                     "dispatched a storage step, so no worker was held",
                     (uint32_t)hold_req);
        }
#endif
        if (async_rc == BOOT_FATAL) {
            uint32_t skipped = 0;

            klog(LOG_ERROR, "boot",
                 "Async storage init FATAL -- falling back to sequential");

            /* OWNERSHIP, not blanket re-entry (TODO-10 S27). The old fallback
             * re-ran all four initializers unconditionally, which is wrong in
             * two separate directions: a driver whose worker overran the
             * deadline is STILL INSIDE its controller reset, so re-running it
             * puts two CPUs into the same DMA and MMIO programming; and a
             * driver that already succeeded gets reset and re-queued for
             * nothing, since none of these initializers is idempotent. Ask each
             * step what actually happened to it, re-reading the worker's live
             * claim so a worker that finished late is credited with finishing.  */
            for (uint32_t i = 0; i < STORAGE_STEP_COUNT; i++) {
                const boot_async_step_outcome_t *o = &outcome.step[i];
                int decision = boot_async_step_fallback(o);

                if (decision == BOOT_ASYNC_FALLBACK_KEEP) {
                    klog(LOG_INFO, "boot",
                         "Storage fallback: %s already initialized -- not re-run",
                         storage_steps[i].name);
                    continue;
                }
                if (decision == BOOT_ASYNC_FALLBACK_SKIP) {
                    skipped++;
                    /* DEGRADED IS A STATE, NOT A LOG LINE (TODO-10 S27). SKIP
                     * means the initializer never completed -- the worker is
                     * still inside it, or was cut down inside it -- and the BSP
                     * deliberately did not re-run it. Nothing downstream may
                     * build on that driver's globals, so record it rather than
                     * only reporting it. */
                    storage_unsafe |= storage_unsafe_bit[i];
                    klog(LOG_ERROR, "boot",
                         "Storage fallback: %s NOT re-run on the BSP -- its "
                         "worker on CPU%u has not released the step; driver degraded",
                         storage_steps[i].name, (uint64_t)o->worker_cpu);
                    continue;
                }
                if (storage_post_pre[i])
                    POST16(storage_post_pre[i]);
                storage_steps[i].fn();
                if (storage_post_ok[i])
                    POST16(storage_post_ok[i]);
            }

            /* SKIP kept the BSP out of the initializer; it did NOT stop the
             * worker. The consumers below (ahci_setup_interrupts,
             * blkdev_register_all) read the very driver globals that worker may
             * still be writing, so not re-entering the initializer is only half
             * the safety property. Wait, bounded, for those workers to release
             * their steps before anything downstream builds on them. */
            if (skipped) {
                uint32_t stuck = boot_async_quiesce(&outcome,
                                                    BOOT_ASYNC_QUIESCE_MS);
                if (stuck)
                    klog(LOG_ERROR, "boot",
                         "Storage: %u async worker(s) never released their step "
                         "-- those drivers stay excluded from interrupt setup "
                         "and device registration",
                         (uint64_t)stuck);
            }

#ifdef KERNEL_TESTS
            /* The injected worker has now been observed still-running by every
             * stage that had to see it: the barrier recorded RUNNING, the
             * fallback read its claim and chose SKIP, and quiescence spent its
             * whole deadline on it. Releasing here rather than on a timer is
             * what makes those three observations deterministic (TODO-10 S27).
             * Inert unless test_hold_async_cpu named a CPU. */
            boot_async_test_release_hold();
#endif

            /* THE GATE, not a warning (TODO-10 S27). Quiescence bounds how long
             * a still-running worker keeps writing; it does not finish the
             * initializer, and it does not touch a POISONED step at all (that
             * worker is already gone, having left the driver's globals
             * arbitrarily partial). Either way `storage_unsafe` names a driver
             * nothing completed, so the consumers below are suppressed for it
             * rather than allowed to run on half-written state.
             *
             * EXCLUSION IS NOT CONTAINMENT, and the difference is not closed
             * here. Suppressing registration and interrupt setup stops the
             * KERNEL from building on partial driver state; it does not undo
             * what the initializer already did to the HARDWARE. Each of these
             * initializers enables PCI bus mastering and can program MSI-X
             * before it finishes (VirtIO-blk does both inside virtio_blk_init),
             * so a worker still inside its step, or one cut down between
             * arming MSI-X and registering its handler, can leave a device able
             * to DMA or raise an interrupt no matter what this mask says.
             * Closing that needs either driver-specific quarantine (clear bus
             * master + MSI/MSI-X/INTx on the degraded controller) or the
             * cooperative-cancellation protocol the drivers do not have -- both
             * driver-owner decisions, not barrier decisions
             * -> XREF: 02-kernel-core/TODO-01 "Async Subsystem Init (SMP
             * Parallel)", item "Contain a degraded storage driver at the
             * HARDWARE, not just in bookkeeping". */

            storage_state = BOOT_LOAD_DEGRADED;   /* async failed; recovered serially */
            storage_err   = (uint16_t)BOOT_FATAL;
            storage_post  = skipped ? POST16_ASYNC_DONE : POST16_NVME_OK;
        } else if (async_rc == BOOT_DEGRADED) {
            klog(LOG_WARN, "boot",
                 "Async storage init degraded -- some drivers may be unavailable");
            storage_state = BOOT_LOAD_DEGRADED;
            storage_err   = (uint16_t)BOOT_DEGRADED;
        }
#ifdef KERNEL_TESTS
        /* Backstop release for every async path, not just the FATAL one: a
         * dispatched-and-held worker whose group did NOT end FATAL would
         * otherwise sit until its post-barrier cap. Idempotent -- it sets a
         * latch -- so the deterministic release after quiescence above still
         * owns the ordering on the path that matters. */
        boot_async_test_release_hold();
#endif
    } else {
#ifdef KERNEL_TESTS
        /* The async branch was not taken at all (single CPU, or async_init=0),
         * so nothing could have honoured the hold. Say so rather than letting a
         * clean sequential boot read as degraded-path coverage. */
        if (hold_req > 0)
            klog(LOG_ERROR, "boot",
                 "[S27] FAILED coverage: test_hold_async_cpu=%u requested but "
                 "storage init took the SEQUENTIAL path (async_init=%u, %u live "
                 "CPU(s)) -- no async worker exists to hold",
                 (uint32_t)hold_req, (uint32_t)g_boot_info.config.async_init,
                 smp_cpu_count());
#endif
        /* Sequential: original order */
        POST16(POST16_ATA);
        ata_init();
        POST16(POST16_ATA_OK);
        virtio_blk_init();
        POST16(POST16_AHCI);
        ahci_init();
        POST16(POST16_AHCI_OK);
        POST16(POST16_NVME);
        nvme_init();
        POST16(POST16_NVME_OK);
    }

    boot_load_finish(storage_tok, storage_state, storage_err, storage_post);

    /* Arming AHCI's MSI/INTx reads the port map and command-list addresses the
     * initializer publishes. On a degraded AHCI those are partial, so an
     * interrupt would fire into a half-built ISR context. */
    if (storage_unsafe & BLKDEV_UNSAFE_AHCI)
        klog(LOG_ERROR, "boot",
             "AHCI degraded -- interrupt setup suppressed, controller left polled");
    else
        ahci_setup_interrupts();
    xhci_setup_interrupts();  /* After enumeration -- ISR would steal events from polling loops */
    blkdev_register_all(storage_unsafe);
    boot_progress(2, "STORAGE_DRV", POST16_AHCI_OK);

    /* --- VFS: requires HEAP --- */
    if (!kernel_subsystem_ready(SUBSYS_HEAP)) {
        boot_recovery_info_t ri = { SUBSYS_VFS, POST16_VFS_OK, BOOT_FATAL, 2 };
        kernel_subsystem_dump();
        boot_recovery_action_t act = boot_recovery_show(&ri);
        boot_recovery_act(act);   /* POWEROFF->shutdown, RETRY->reboot; CONSOLE/other fall to halt */
        boot_halt("HEAP not ready -- cannot init VFS");
    }
    klog(LOG_DEBUG, "boot", "--- Phase: storage & VFS ---");
    boot_splash_status("Initializing VFS...");
    POST16(POST16_VFS);
    vfs_init();
    POST16(POST16_VFS_OK);
    kernel_subsystem_set_ready(SUBSYS_VFS, true);
    boot_progress(2, "VFS", POST16_VFS_OK);

    /* --- Partition scan + filesystem mount --- */
    klog(LOG_DEBUG, "boot", "--- Phase: partition & filesystem mount ---");
    boot_splash_tick();
    boot_splash_status("Scanning partitions...");
    POST16(POST16_PARTITION);
    partition_scan_all();
    boot_splash_status("Mounting filesystems...");
    /* A/B dual-slot (TODO-21): mount the root slot the bootloader selected. */
    partition_mount_filesystems((int)g_boot_info.active_slot);
    /* Pet the watchdog after the (disk-I/O-bound) mount and before the
     * BlackBox X:\ skeleton + low-space cleanup, which can do further slow
     * FAT32 I/O on a full volume. boot_progress() is the primary WDAT pet. */
    boot_progress(2, "PARTITION", POST16_PARTITION_OK);

    /* --- BlackBox directory skeleton (X:\) ---
     * Try every dir on every boot. vfs_create on an existing FAT32
     * directory is a silent no-op (fat32_create_dir_vol returns -1
     * when the dirent already exists), so re-running is safe. The
     * old "probe Logs and skip everything if it exists" gate left
     * Diag/Perf/Crash/Tools missing whenever Logs survived but a
     * sibling didn't (firmware crash mid-skeleton, manual deletion,
     * mismatch between BlackBox builds). */
    {
        static const char *bb_dirs[] = {
            "X:\\Logs", "X:\\Logs\\Serial", "X:\\Boot", "X:\\Crash",
            "X:\\Crash\\WER", "X:\\Perf", "X:\\Diag", "X:\\Tools"
        };
        uint32_t d;
        for (d = 0; d < sizeof(bb_dirs) / sizeof(bb_dirs[0]); d++) {
            if (vfs_create(bb_dirs[d], VFS_DIRECTORY) == 0)
                klog(LOG_INFO, "boot", "BlackBox: created %s",
                     bb_dirs[d]);
        }
    }

    /* --- BlackBox disk space management --- */
    if (vfs_is_mounted('X')) {
        struct vfs_node *x_root = vfs_get_drive_root('X');
        struct fat32_volume *bb_vol = x_root ?
            fat32_volume_from_root(x_root) : (struct fat32_volume *)0;

        if (bb_vol) {
            uint64_t free_bytes = fat32_get_free_bytes(bb_vol);
            uint64_t total_bytes = fat32_get_total_bytes(bb_vol);
            uint32_t pct = total_bytes > 0
                         ? (uint32_t)((free_bytes * 100) / total_bytes) : 0;
            if (pct > 100) pct = 100;  /* clamp: FSInfo can over-report */

            klog(LOG_INFO, "boot", "BlackBox: %u MiB free (%u%%)",
                 (uint32_t)(free_bytes / (1024 * 1024)), (uint64_t)pct);

            /* WER retention: always-enforced cap once per boot at mount
             * (Win11 MaxArchiveCount style). Runs outside the low-space gate
             * so a crash loop cannot accumulate reports between low-space
             * events. Self-logs the pruned count. */
            wer_prune_reports("X:\\Crash\\WER", WER_MAX_REPORTS);

            /* Cleanup if < 10% free */
            if (pct < 10) {
                uint32_t deleted = 0;
                uint32_t max_sessions = 10;  /* TODO: registry HKLM\SYSTEM\BlackBox\MaxBootSessions */
                struct vfs_node *boot_dir;

                /* Delete oldest files in Boot\ beyond max_sessions */
                boot_dir = vfs_open("X:\\Boot", VFS_O_READ);
                if (boot_dir) {
                    uint32_t file_count = 0;
                    struct vfs_dirent *de;
                    uint32_t idx = 0;

                    /* Count files */
                    while ((de = vfs_readdir(boot_dir, idx++)) != 0)
                        if (de->type == VFS_FILE) file_count++;

                    /* Delete oldest if over limit */
                    if (file_count > max_sessions) {
                        idx = 0;
                        while ((de = vfs_readdir(boot_dir, idx)) != 0
                               && file_count > max_sessions) {
                            if (de->type == VFS_FILE) {
                                char path[64];
                                int p = 0, j;
                                const char *pfx = "X:\\Boot\\";
                                for (j = 0; pfx[j]; j++) path[p++] = pfx[j];
                                for (j = 0; de->name[j] && p < 60; j++)
                                    path[p++] = de->name[j];
                                path[p] = '\0';
                                if (vfs_unlink(path) == 0) {
                                    deleted++;
                                    file_count--;
                                    continue;  /* re-read same index */
                                }
                            }
                            idx++;
                        }
                    }
                    vfs_close(boot_dir);
                }

                /* Delete rotated logs (.1, .2, .3) in Logs\ */
                {
                    struct vfs_node *logs_dir = vfs_open("X:\\Logs", VFS_O_READ);
                    if (logs_dir) {
                        uint32_t idx2 = 0;
                        struct vfs_dirent *de2;
                        while ((de2 = vfs_readdir(logs_dir, idx2)) != 0) {
                            /* Match only actual rotated logs: a regular file
                             * named *.log.N or *.jsonl.N (the rotate_log_file
                             * naming). Requiring VFS_FILE + the .log/.jsonl
                             * infix avoids deleting unrelated artifacts (or a
                             * directory) that merely end in ".<digit>". */
                            int len = 0;
                            while (de2->name[len]) len++;
                            if (de2->type == VFS_FILE
                                && len >= 2 && de2->name[len-2] == '.'
                                && de2->name[len-1] >= '1'
                                && de2->name[len-1] <= '9'
                                && (bb_name_contains(de2->name, ".log")
                                    || bb_name_contains(de2->name, ".jsonl"))) {
                                char path[64];
                                int p = 0, j;
                                const char *pfx = "X:\\Logs\\";
                                for (j = 0; pfx[j]; j++) path[p++] = pfx[j];
                                for (j = 0; de2->name[j] && p < 60; j++)
                                    path[p++] = de2->name[j];
                                path[p] = '\0';
                                if (vfs_unlink(path) == 0) {
                                    deleted++;
                                    continue;
                                }
                            }
                            idx2++;
                        }
                        vfs_close(logs_dir);
                    }
                }

                /* Emergency WER prune in the low-space path (the mount-time
                 * cap above usually already satisfied this). */
                deleted += wer_prune_reports("X:\\Crash\\WER", WER_MAX_REPORTS);

                if (deleted > 0) {
                    uint64_t new_free = fat32_get_free_bytes(bb_vol);
                    /* Clamp: concurrent klog writes to X:\Logs can consume
                     * space during cleanup, so new_free may dip below the
                     * pre-cleanup value -- avoid a uint64 underflow that
                     * would report a nonsense freed amount. */
                    uint64_t freed = (new_free >= free_bytes)
                                   ? new_free - free_bytes : 0;
                    klog(LOG_WARN, "boot",
                         "BlackBox: cleanup freed %u KiB (%u files removed)",
                         (uint64_t)(freed / 1024), (uint64_t)deleted);
                }

                /* If still critically low, fall back to C:\ */
                {
                    uint64_t recheck = fat32_get_free_bytes(bb_vol);
                    uint32_t min_free_mib = 16;  /* TODO: registry HKLM\SYSTEM\BlackBox\MinFreeMiB */
                    if (recheck < (uint64_t)min_free_mib * 1024 * 1024) {
                        klog(LOG_ERROR, "boot",
                             "BlackBox: critically low (%u MiB free, min %u) "
                             "-- falling back to C:\\ for this session",
                             (uint32_t)(recheck / (1024*1024)),
                             (uint64_t)min_free_mib);
                        /* Force klog to C:\ by marking X:\ unavailable
                         * for log resolution */
                        extern int klog_using_blackbox;
                        extern const char *klog_dir;
                        klog_using_blackbox = 0;
                        klog_dir = KLOG_DIR_FALLBACK;
                    }
                }
            }
        }
    }

    /* End of the BlackBox X:\ skeleton + cleanup window -- pet the watchdog
     * (boot_progress supersets the raw POST16 port-0x80 write with a WDAT pet
     * + timing record), so a slow full-volume cleanup is not mistaken for a
     * hung boot by an armed watchdog. */
    boot_progress(2, "BLACKBOX", POST16_PARTITION_OK);

    /* --- Debug diagnostic (shows storage state on serial + optionally splash) ---
     * diag_splash=1: render each message on the splash diag line and pause.
     *                If diag_delay=0, default to 5s so the text is readable
     *                on bare metal without serial access. */
    if (g_boot_info.config.debug) {
        char diag[256];
        int p = 0;
        int blk_n = blkdev_count();
        const char *s;
        uint8_t show_splash = g_boot_info.config.diag_splash;
        uint8_t delay = g_boot_info.config.diag_delay;
        if (show_splash && delay == 0)
            delay = 5;  /* bare metal default: 5s per screen */

        /* Diag 1: xHCI, USB devices, mount status */
        {
            int usb_ports = 0, usb_msc = 0;
            if (xhci_controller_count() > 0) {
                const struct xhci_controller *hc = xhci_get_controller(0);
                if (hc) usb_ports = (int)hc->max_ports;
            }
            usb_msc = xhci_msc_device_count();

            s = "xHCI="; while (*s) diag[p++] = *s++;
            diag[p++] = '0' + (char)xhci_controller_count();
            s = " ports="; while (*s) diag[p++] = *s++;
            diag[p++] = '0' + (char)(usb_ports / 10);
            diag[p++] = '0' + (char)(usb_ports % 10);
            s = " msc="; while (*s) diag[p++] = *s++;
            diag[p++] = '0' + (char)usb_msc;
            s = " blk="; while (*s) diag[p++] = *s++;
            diag[p++] = '0' + (char)(blk_n > 9 ? 9 : blk_n);
            s = " C:="; while (*s) diag[p++] = *s++;
            s = vfs_is_mounted('C') ? "Y" : "N"; while (*s) diag[p++] = *s++;
            s = " ccs="; while (*s) diag[p++] = *s++;
            diag[p++] = '0' + (char)xhci_get_ccs_count();
            s = " stg="; while (*s) diag[p++] = *s++;
            diag[p++] = '0' + (char)xhci_get_enum_stage();
            s = " C:="; while (*s) diag[p++] = *s++;
            s = vfs_is_mounted('C') ? "Y" : "N"; while (*s) diag[p++] = *s++;

            /* Scan PCI for all USB controllers: show prog-if codes */
            s = " USB:"; while (*s) diag[p++] = *s++;
            {
                uint16_t bus; uint8_t dv, fn;
                int usb_found = 0;
                for (bus = 0; bus < 256 && p < 230; bus++) {
                    for (dv = 0; dv < 32; dv++) {
                        for (fn = 0; fn < 8; fn++) {
                            uint16_t vid = pci_read16((uint8_t)bus, dv, fn, 0x00);
                            if (vid == 0xFFFF) continue;
                            uint8_t cls = pci_read8((uint8_t)bus, dv, fn, 0x0B);
                            uint8_t sub = pci_read8((uint8_t)bus, dv, fn, 0x0A);
                            if (cls == 0x0C && sub == 0x03) {
                                uint8_t pi = pci_read8((uint8_t)bus, dv, fn, 0x09);
                                if (usb_found > 0) diag[p++] = ',';
                                /* Show prog-if as hex: 00=UHCI 10=OHCI 20=EHCI 30=xHCI */
                                diag[p++] = "0123456789ABCDEF"[pi >> 4];
                                diag[p++] = "0123456789ABCDEF"[pi & 0xF];
                                usb_found++;
                            }
                            if (fn == 0) {
                                uint8_t hdr = pci_read8((uint8_t)bus, dv, fn, 0x0E);
                                if (!(hdr & 0x80)) break;
                            }
                        }
                    }
                }
                if (usb_found == 0) { s = "none"; while (*s) diag[p++] = *s++; }
            }
        }
        diag[p] = '\0';
        klog(LOG_WARN, "boot", "DIAG: %s", diag);
        if (show_splash) {
            boot_splash_diag(diag);
            boot_splash_delay(delay);
        }

        /* Diag 2: list block device names and sizes */
        p = 0;
        {
            int i;
            for (i = 0; i < blk_n && i < 6; i++) {
                const struct blkdev *bd = blkdev_get_by_index(i);
                if (!bd) continue;
                if (i > 0) diag[p++] = ' ';
                int j;
                for (j = 0; bd->name[j] && p < 240; j++)
                    diag[p++] = bd->name[j];
                diag[p++] = ':';
                /* Show size in MB */
                uint32_t mb = (uint32_t)(bd->sector_count * bd->sector_size / (1024*1024));
                if (mb >= 1000) {
                    diag[p++] = '0' + (char)(mb/1000 % 10);
                    diag[p++] = '0' + (char)(mb/100 % 10);
                    diag[p++] = '0' + (char)(mb/10 % 10);
                    diag[p++] = '0' + (char)(mb % 10);
                } else if (mb >= 100) {
                    diag[p++] = '0' + (char)(mb/100 % 10);
                    diag[p++] = '0' + (char)(mb/10 % 10);
                    diag[p++] = '0' + (char)(mb % 10);
                } else {
                    diag[p++] = '0' + (char)(mb/10 % 10);
                    diag[p++] = '0' + (char)(mb % 10);
                }
                diag[p++] = 'M';
            }
        }
        diag[p] = '\0';
        klog(LOG_WARN, "boot", "DIAG: %s", diag);
        if (show_splash) {
            boot_splash_diag(diag);
            boot_splash_delay(delay);
            boot_splash_diag("");  /* clear diag line before resuming */
        }
    }

    boot_splash_status("Checking boot flags...");

    /* --- Boot media speed detection: BEFORE both the C:\DEBUG live-log enable AND
     * klog_disk_enable, so a SLOW classification arms deferred klog (klog_set_deferred)
     * before ANY disk-log flush -- live-mode klog() and klog_disk_enable()'s own first
     * klog_disk_flush() -- pays the slow-media cost. VFS is mounted above; the timed
     * 4 KiB probe read is clean here, free of klog disk activity. --- */
    boot_media_probe();

    /* USB boot diagnostic report: emit AFTER boot_media_probe so the media-speed
     * line is real, not BOOT_MEDIA_UNKNOWN. USB enumeration completed in
     * xhci_init earlier this phase, so the device/controller data is stable. */
    usb_boot_report();

    /* Check for debug boot flag on C:\ */
    if (vfs_is_mounted('C')) {
        struct vfs_node *c_root = vfs_get_drive_root('C');
        if (c_root && c_root->ops && c_root->ops->finddir) {
            struct vfs_node *dbg = c_root->ops->finddir(c_root, "DEBUG");
            if (dbg) {
                klog(LOG_INFO, "boot",
                     "DEBUG flag found on C:\\ -- disabling splash");
                boot_splash_abort();
                printk("\n=== DEBUG BOOT MODE ===\n");
                printk("Splash disabled. Showing live boot output.\n\n");
                klog_disk_set_live(1);
            }
        }
    }

    /* --- klog disk enable: Phase 2 VFS-backed logging --- */
    klog_resolve_dir();  /* always resolve X:\ vs C:\ -- even on TCG */
    if (!platform_is_tcg()) {
        /* POST16 bracket so a hang/triple-fault inside the first disk flush
         * (slow/degraded media) leaves a breadcrumb at POST16_KLOG_DISK. The
         * _OK code + boot_progress fire only when disk logging is ACTUALLY live
         * (klog_disk_enable returns 1: buffer allocated + log target mounted);
         * a silent alloc-fail / no-mount keeps the breadcrumb at the entry code
         * instead of falsely reporting success. */
        POST16(POST16_KLOG_DISK);
        int disk_live = klog_disk_enable();
        POST16(disk_live ? POST16_KLOG_DISK_OK : POST16_KLOG_DISK);
        boot_progress(2, "KLOG_DISK", disk_live ? POST16_KLOG_DISK_OK : POST16_KLOG_DISK);
    } else {
        boot_progress(2, "KLOG_DISK", POST16_KLOG_OK); /* TCG: ring+serial only, no disk */
    }
    boot_splash_tick();

    /* --- System summary --- */
    boot_splash_status("Configuring system...");

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- System Summary ---------------------------------------------------------");

    klog(LOG_INFO, "boot", "Running in 64-bit Long Mode");

    if (g_boot_info.fb_available) {
        klog(LOG_INFO, "gfx", "Framebuffer: %ux%ux%u at %p",
               (uint64_t)g_boot_info.fb.width,
               (uint64_t)g_boot_info.fb.height,
               (uint64_t)g_boot_info.fb.bpp,
               (uint64_t)g_boot_info.fb.addr);
    }

    /* Memory summary */
    for (i = 0; i < g_boot_info.mmap_count; i++) {
        if (g_boot_info.mmap[i].type == 1)
            total_ram += g_boot_info.mmap[i].length;
    }
    klog(LOG_INFO, "mm", "Total RAM: %u MiB (%u entries)",
         (uint64_t)(total_ram / (1024 * 1024)),
         (uint64_t)g_boot_info.mmap_count);

    /* (SMP moved earlier -- before disk drivers for async init support) */

    /* Hardware dump (only in live debug mode) */
    if (klog_disk_live_active()) {
        boot_splash_status("Dumping hardware info...");
        hw_dump_to_log();
    }

    /* Heap self-test */
    boot_splash_status("Heap self-test...");
    {
        uint8_t *a = (uint8_t *)kmalloc(64);
        uint8_t *b = (uint8_t *)kmalloc(128);
        uint8_t *c = (uint8_t *)kmalloc(256);
        uint32_t ok = 1;

        if (a) { uint32_t j; for (j = 0; j < 64; j++) a[j] = (uint8_t)j; }
        if (b) { uint32_t j; for (j = 0; j < 128; j++) b[j] = (uint8_t)(j ^ 0xAA); }
        if (c) { uint32_t j; for (j = 0; j < 256; j++) c[j] = (uint8_t)(j ^ 0x55); }

        if (a) { uint32_t j; for (j = 0; j < 64; j++) if (a[j] != (uint8_t)j) ok = 0; }
        if (b) { uint32_t j; for (j = 0; j < 128; j++) if (b[j] != (uint8_t)(j ^ 0xAA)) ok = 0; }

        kfree(b);
        b = (uint8_t *)krealloc(a, 512);
        if (b) { uint32_t j; for (j = 0; j < 64; j++) if (b[j] != (uint8_t)j) ok = 0; }

        kfree(b);
        kfree(c);

        klog(ok ? LOG_INFO : LOG_ERROR, "mm",
             "Heap: %s (used: %u, free: %u bytes)",
             ok ? "OK" : "FAIL", heap_get_used(), heap_get_free());
    }

    /* --- Object Manager: requires HEAP --- */
    boot_splash_status("Initializing Object Manager...");
    POST16(POST16_OB);
    {
        boot_result_t r = ob_init();
        kernel_subsystem_set_ready(SUBSYS_OB, r != BOOT_FATAL);
        if (r == BOOT_FATAL) {
            boot_recovery_info_t ri = { SUBSYS_OB, POST16_OB_OK, BOOT_FATAL, 2 };
            kernel_subsystem_dump();
            boot_recovery_action_t act = boot_recovery_show(&ri);
            boot_recovery_act(act);   /* POWEROFF->shutdown, RETRY->reboot; CONSOLE/other fall to halt */
            boot_halt("Object Manager init failed");
        }
    }
    POST16(POST16_OB_OK);
    boot_progress(2, "OB", POST16_OB_OK);

    /* Wire OB handle event tracing from boot.conf (S15) */
    if (g_boot_info.config.ob_handle_trace)
        g_ob_handle_trace = 1;

    /* --- Executive Support Runtime: after OB (callback objects bind an OM
     * type), before registry/security (they consume ERESOURCE/push locks). --- */
    POST16(POST16_EX);
    {
        boot_result_t r = ex_init();
        kernel_subsystem_set_ready(SUBSYS_EX, r != BOOT_FATAL);
        if (r == BOOT_FATAL) {
            boot_recovery_info_t ri = { SUBSYS_EX, POST16_EX_OK, BOOT_FATAL, 2 };
            kernel_subsystem_dump();
            boot_recovery_action_t act = boot_recovery_show(&ri);
            boot_recovery_act(act);
            boot_halt("Executive support runtime init failed");
        }
    }
    POST16(POST16_EX_OK);
    boot_progress(2, "EX", POST16_EX_OK);

    /* --- Registry: requires VFS --- */
    if (!kernel_subsystem_ready(SUBSYS_VFS)) {
        boot_recovery_info_t ri = { SUBSYS_REGISTRY, POST16_REGISTRY_OK, BOOT_FATAL, 2 };
        kernel_subsystem_dump();
        boot_recovery_action_t act = boot_recovery_show(&ri);
        boot_recovery_act(act);   /* POWEROFF->shutdown, RETRY->reboot; CONSOLE/other fall to halt */
        boot_halt("VFS not ready -- cannot init registry");
    }
    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- Registry ---------------------------------------------------------------");

    boot_splash_status("Loading registry...");
    boot_splash_tick();
    klog_disk_flush();

    boot_splash_status("Initializing registry...");
    POST16(POST16_REGISTRY);
    {
        /* NOTE: do NOT publish SUBSYS_REGISTRY ready here. The panic path
         * (KeBugCheckEx, panic.c) gates its LastBugCheck registry writes on
         * kernel_subsystem_ready(SUBSYS_REGISTRY); the registry tree has no
         * SMP lock yet (registry SMP synchronization is unimplemented) and
         * registry_populate_defaults() below mutates it while APs are already
         * running. Publishing readiness before population would let an AP
         * panic re-enter RegCreateKeyEx / RegSetValueEx against a tree the BSP
         * is mid-mutating. Readiness is published after population below. */
        boot_result_t r = registry_init();
        if (r == BOOT_FATAL) {
            boot_recovery_info_t ri = { SUBSYS_REGISTRY, POST16_REGISTRY_OK, BOOT_FATAL, 2 };
            kernel_subsystem_dump();
            boot_recovery_action_t act = boot_recovery_show(&ri);
            boot_recovery_act(act);   /* POWEROFF->shutdown, RETRY->reboot; CONSOLE/other fall to halt */
            boot_halt("Registry init failed -- cannot allocate root keys");
        }
    }
    POST16(POST16_REGISTRY_OK);
    boot_splash_status("Populating registry defaults...");
    registry_populate_defaults();
    /* Cache the PowerIdleEnable policy value now that the registry is up and
     * populated. pm_idle_c1() must never read the registry itself: it runs at
     * timer rate on an idle machine, and the registry has no SMP lock yet.
     * Before this call the cached value reads as enabled, which is exactly the
     * unconditional HLT every idle site did previously (todo/02-kernel-core/TODO-26-power-management.md section 2). */
    pm_idle_init();
    /* Apply admin-configured per-subsystem log verbosity + rate limits now that the
     * registry is up (HKLM\SYSTEM\Logs\Levels\<tag> + \RateLimit\<tag>). No-op until
     * an admin sets the keys; without this call the registry log config never loads. */
    klog_load_levels_from_registry();
    /* Publish readiness only now -- tree is fully populated + log-config
     * applied, so the panic-path registry write (gated on this flag) is safe. */
    kernel_subsystem_set_ready(SUBSYS_REGISTRY, true);
    boot_progress(2, "REGISTRY", POST16_REGISTRY_OK);

    /* --- Kernel Notification Facility: \Notifications namespace + type.
     * After Ob + registry (persistent-state metadata will live in the
     * registry); still single-threaded here (pre-scheduler), so the tree
     * build races nothing. --- */
    knf_init();
    /* Report the KNF step by its readiness result, not an unconditional OK: a
     * fatal init (no type/root namespace) leaves SUBSYS_KNF not-ready, so the
     * boot step shows POST16_KNF (entry) rather than a false POST16_KNF_OK. */
    boot_progress(2, "KNF",
                  kernel_subsystem_ready(SUBSYS_KNF) ? POST16_KNF_OK : POST16_KNF);

    /* --- NLS table loader: requires VFS + C: mount (nls_init sets readiness) --- */
    POST16(POST16_NLS);
    nls_init();
    POST16(POST16_NLS_OK);
    boot_progress(2, "NLS", POST16_NLS_OK);

    /* --- Symbol table --- */
    symtab_init();

    /* --- mmap --- */
    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "--- System Services --------------------------------------------------------");

    boot_splash_status("Initializing mmap...");
    mmap_init();

    /* --- Time subsystem: wall clock, timezone, KUSER_SHARED_DATA --- */
    {
        extern int  hpet_init(void);
        extern int  acpi_is_ready(void);
        extern void mono_clock_init(void);
        extern void wall_clock_init(void);
        extern void timezone_init(void);
        extern void kusd_init(void);

        /* Probe HPET (UC-maps the counter via the ACPI HPET base) BEFORE
         * mono_clock_init so the TSC > HPET > PMTMR > LAPIC selection can
         * actually pick HPET when there is no invariant TSC. Gate on
         * acpi_is_ready(): acpi_get_hpet_base() walks raw ACPI roots off the
         * boot_info RSDP pointer alone, so on a failed acpi_init (bad RSDP /
         * missing FADT) skipping the probe degrades cleanly to PMTMR/LAPIC
         * instead of walking + mapping invalid firmware tables. */
        if (acpi_is_ready())
            hpet_init();
        mono_clock_init();
        wall_clock_init();
        timezone_init();
        kusd_init();
    }

    /* --- ACPI power init: S-state discovery + SCI handler --- */
    {
        extern void acpi_power_init(void);
        extern void acpi_enable_fixed_events(void);
        extern void acpi_register_sci(void);
        /* POST16 around the DSDT AML walk: it is the one step in this block
         * that can fault or hang on malformed firmware, and it is exactly the
         * walk the watchdog note below is about. Every other Phase 2 subsystem
         * in this file carries a breadcrumb; this block had none. */
        POST16(POST16_ACPI_PM);
        acpi_power_init();
        acpi_enable_fixed_events();
        acpi_register_sci();
        POST16(POST16_ACPI_PM_OK);
    }

    /* --- ACPI Embedded Controller: ECDT discovery + polled transactions ---
     * Phase 2, not inside acpi_init(), and the placement is load-bearing: every
     * EC handshake wait bounds itself against mono_ns(), which reads 0 until
     * mono_clock_init() above has run. Discovering the EC in Phase 1 would give
     * each wait a meaningless deadline and let a wedged controller hang boot. */
    {
        extern void acpi_ec_init(void);
        POST16(POST16_ACPI_EC);
        acpi_ec_init();
        POST16(POST16_ACPI_EC_OK);
    }

    /* Pet after the post-registry system-services tail (symtab, mmap, time,
     * ACPI power/DSDT S-state parse) so the gap to Phase 3 does not leave the
     * watchdog unpetted across acpi_power_init's DSDT walk. */
    boot_progress(2, "SYS_SVC", POST16_REGISTRY_OK);

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "[PHASE2] complete -- VFS, registry, SMP, network ready");

    /* NVRAM write: Phase 2 complete */
    boot_post_nvram_write16(POST16_REGISTRY_OK);
}

/* ---- Legacy wrapper ----------------------------------------------------- */

void boot_storage_init(uint64_t magic)
{
    (void)magic;
    boot_phase2();
}
