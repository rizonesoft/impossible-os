/* ============================================================================
 * boot_hw.c -- Phase 0: Critical Init (Interrupts Disabled)
 *
 * Runs with interrupts off. Only serial, memory, and logging. No drivers,
 * VFS, or network. Any failure in Phase 0 calls boot_halt() on serial --
 * framebuffer is not yet available.
 *
 * Provides: boot_phase0() and the legacy boot_hw_init() wrapper.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/cpuid.h"
#include "kernel/boot_info.h"
#include "kernel/boot_version.h"
#include "registry.h"
#include "gfx_simd.h"
#include "kernel/security/pku.h"
#include "kernel/drivers/serial.h"
#include "kernel/klog.h"
#include "kernel/boot_timing.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/heap.h"
#include "kernel/version.h"
#include "kernel/uefi_runtime.h"
#include "kernel/uefi_vars.h"
#include "kernel/tpm.h"
#include "kernel/tpm_attest_report.h"   /* Phase-0 handoff snapshot for attestation */
#include "kernel/boot_init.h"
#include "kernel/boot_halt.h"
#include "kernel/config.h"
#include "kernel/cpu_security.h"
#include "kernel/smp.h"
#include "kernel/msr.h"
#include "main/main_internal.h"

/* Global boot info -- the canonical, kernel-side handoff structure.
 * Populated by the UEFI bootloader via memcpy from the bootloader's
 * mirror at BOOT_INFO_PHYS_ADDR (0x10000). Storage lives here (not in
 * a protocol-specific file) so the kernel link does not depend on any
 * bootloader-protocol parser. Alt-boot protocol = unsupported per the
 * alternate-boot-protocol policy doc. */
struct boot_info g_boot_info;

/* BSP kernel boot stack. The UEFI bootloader calls kernel_main directly
 * on its own EfiLoaderData stack; this static array provides a stable
 * kernel-owned stack region whose top is exposed as the label `stack_top`
 * for TSS.rsp0 (used on ring 3 -> ring 0 transitions) and IST setup.
 *
 * `stack_top` is declared via module-scope inline asm as a TRUE LABEL
 * (symbol address == bsp_boot_stack + BSP_BOOT_STACK_SIZE), preserving
 * the ABI that the previous entry.asm-defined `global stack_top`
 * provided. `gdt.c` imports it as `extern char stack_top[]` and casts
 * the symbol address directly into kernel_tss.rsp0; declaring it as a
 * C `char *const` would put the pointer in .rodata and TSS.rsp0 would
 * land at the address of the pointer object rather than the stack top.
 *
 * `used` + `retain` prevent LTO / --gc-sections from discarding the
 * stack region (no direct C reads -- only the assembly-side label
 * arithmetic and the gdt.c symbol cast). 16 KB matches AP_STACK_SIZE
 * in include/kernel/smp.h. */
#define BSP_BOOT_STACK_SIZE 16384
__attribute__((aligned(16), used, retain))
char bsp_boot_stack[BSP_BOOT_STACK_SIZE];
/* Define `stack_top` as a true label symbol so its address arithmetic
 * matches the original entry.asm `global stack_top` ABI exactly. Keep
 * the literal size in sync with BSP_BOOT_STACK_SIZE above; the
 * _Static_assert below pins the contract at compile time. */
__asm__ (
    ".globl stack_top\n\t"
    ".set stack_top, bsp_boot_stack + 16384\n\t"
);
_Static_assert(BSP_BOOT_STACK_SIZE == 16384,
    "BSP_BOOT_STACK_SIZE must match the literal in the stack_top inline asm above");

/* UEFI boot magic */
#define UEFI_BOOT_MAGIC 0x55454649ULL  /* "UEFI" */

/* ---- Phase 0 ------------------------------------------------------------ */

void boot_phase0(uint64_t magic, uint64_t mbi)
{
    uint32_t i;

    /* --- BSP per-CPU data: GS_BASE must be valid before ANY interrupt fires.
     * On bare metal, GS_BASE defaults to 0; gs:0 reads IVT garbage instead
     * of NULL, crashing the IRQL tracking in isr_handler. */
    smp_early_bsp_init();

    /* --- Serial: absolute first call, no dependencies --- */
    POST16(POST16_SERIAL);
    serial_init();
    POST16(POST16_SERIAL_OK);
    kernel_subsystem_set_ready(SUBSYS_SERIAL, true);
    boot_progress(0, "SERIAL", POST16_SERIAL_OK);

    klog(LOG_DEBUG, "", "========================================================================");
    klog(LOG_DEBUG, "", "  Impossible OS -- Boot Log");
    klog(LOG_DEBUG, "", "========================================================================");
    version_print();

    /* --- Boot info parse: UEFI or Multiboot2 --- */
    if (magic == UEFI_BOOT_MAGIC) {
        const void *mbi_p = (const void *)(uintptr_t)mbi;

        /* S16 phase 1: address validation.  Rejects NULL, misaligned,
         * out-of-map, and wraparound handoff pointers BEFORE touching
         * any bytes at *mbi.  g_boot_info is still zero-initialized
         * here, so if we halt below, boot_halt()'s framebuffer path
         * checks fb_available == 0 and skips the fb dereference -- no
         * chance of writing through garbage-copied fb.addr. */
        if (boot_info_validate_addr(mbi_p, sizeof(struct boot_info),
                                    BOOT_INFO_EARLY_MAP_END) != BOOT_OK) {
            klog(LOG_ERROR, "UEFI",
                 "boot_info: unsafe mbi pointer 0x%lx (expected 0x%lx..0x%lx, 8-byte aligned)",
                 (uint64_t)mbi,
                 (uint64_t)BOOT_INFO_MIN_ADDR,
                 (uint64_t)(BOOT_INFO_EARLY_MAP_END - sizeof(struct boot_info)));
            boot_halt("boot_info: unsafe mbi pointer");
        }

        /* S16 phase 2: header field validation via boot_version
         * classifier. Observed + expected magic/version/size go into
         * a boot_version_fault record; on mismatch boot_version
         * _render_fatal prints the record (LOG_FATAL) and halts.
         * The NVRAM persist for next-boot BlackBox transcription is
         * best-effort and silently fails when Phase 0 has not yet
         * brought up uefi_runtime (which is the common case at this
         * call site); see include/kernel/boot_version.h for the full
         * contract. Do not return from render_fatal. */
        {
            const struct boot_info_header *h = (const struct boot_info_header *)mbi_p;
            struct boot_version_fault fault;
            if (boot_version_classify(h, sizeof(struct boot_info), &fault) != BOOT_OK)
                boot_version_render_fatal(&fault);
        }

        /* Pre-copy validation passed -- the handoff is safe to copy.
         * Copy only header.size bytes; the validator guaranteed
         * header.size == sizeof(struct boot_info), so this is equivalent
         * to copying sizeof but uses the bootloader's declared length. */
        {
            struct boot_info *src = (struct boot_info *)(uintptr_t)mbi;
            uint8_t *d = (uint8_t *)&g_boot_info;
            const uint8_t *s = (const uint8_t *)src;
            uint16_t copy_len = src->header.size;
            for (i = 0; i < copy_len; i++)
                d[i] = s[i];
        }

        klog(LOG_INFO, "UEFI",
             "Boot info v%d, size=%u, magic=0x%08X",
             (uint64_t)g_boot_info.header.version,
             (uint64_t)g_boot_info.header.size,
             (uint64_t)g_boot_info.header.magic);
    } else {
        boot_halt("Unknown bootloader magic (UEFI is the only supported boot path)");
    }
    boot_progress(0, "BOOT_INFO", 0x0026);

    /* Defense in depth: verify boot_config.cmdline begins with ASCII.
     * The bootloader fills boot_config at a fixed layout and the S16
     * pre-copy validator pins total struct size; this check still
     * catches subtle internal corruption (e.g. cmdline region partially
     * clobbered after ExitBootServices) that a size match would miss. */
    {
        const char *cmd = g_boot_info.config.cmdline;
        int j;
        for (j = 0; j < 4 && cmd[j] != '\0'; j++) {
            if (cmd[j] < 0x20 || cmd[j] > 0x7E) {
                klog(LOG_ERROR, "CONF",
                     "boot_config.cmdline non-ASCII at byte %d (0x%02X) -- post-copy corruption?",
                     (uint64_t)j, (uint64_t)(uint8_t)cmd[j]);
                boot_halt("boot_config.cmdline corrupted after validated copy");
            }
        }
    }

    /* Parse + validate the boot-argument schema before any Phase-0 consumer
     * reads raw config. An unknown kernel.* key (without boot.allow_unknown=1)
     * or a malformed value halts here -- Phase 0 has no degraded-continue. */
    if (boot_args_init(&g_boot_info.config) != BOOT_OK)
        boot_halt("boot args: unknown or invalid kernel boot key");

    /* typed payload descriptor array: validate the packed-prefix,
     * overlap, range, alignment, total-bytes, and unknown-required
     * invariants before any subsystem consumes payloads. Empty array
     * (zero producers in the UEFI boot path today) short-circuits
     * cleanly. */
    {
        enum boot_payload_error err = BOOT_PAYLOAD_ERR_OK;
        if (boot_payload_validate(&g_boot_info, &err) != BOOT_OK) {
            klog(LOG_ERROR, "boot",
                 "boot_payload_validate failed (err=%u); halting before consumers run",
                 (uint64_t)err);
            boot_halt("boot_payload: typed payload array invariants violated");
        }
        if (g_boot_info.payload_count > 0u)
            klog(LOG_INFO, "boot",
                 "boot_payload: %u descriptor(s) validated, %lu bytes total",
                 (uint64_t)g_boot_info.payload_count,
                 (uint64_t)g_boot_info.payload_total_bytes);
    }

    /* Freeze the loader-to-kernel handoff snapshot for the attestation report
     * BEFORE any capability refinement (boot_caps_mark_present, runtime-services
     * degradation) mutates the live caps words, so the report attributes the
     * loader handoff, not later kernel policy. */
    tpm_attest_handoff_snapshot_init(&g_boot_info);

    /* Capability negotiation: reject stale-kernel-on-newer-loader
     * (unknown required bits) and producer contradictions (required &
     * degraded, present & degraded) before any capability-gated
     * subsystem inspects caps_present / caps_degraded. MUST run BEFORE
     * warm-update consume below: the consume path is gated on
     * BOOT_CAP_PAYLOAD_DESCRIPTORS, so running consume first would let
     * warm-update descriptors get ACCEPTED before capability
     * validation ran. */
    {
        enum boot_caps_error cerr = BOOT_CAPS_ERR_OK;
        if (boot_caps_validate(&g_boot_info, &cerr) != BOOT_OK) {
            klog(LOG_ERROR, "boot",
                 "boot_caps_validate failed (err=%u); halting before capability consumers run",
                 (uint64_t)cerr);
            boot_halt("boot_caps: capability negotiation invariants violated");
        }
    }

    /* Warm-kernel-update consume: scan the typed-payload-descriptor
     * array for BOOT_PAYLOAD_WARM_UPDATE_STATE entries. Each descriptor
     * gets validated; ACCEPTED means the caller (runtime live-update
     * owned by todo/03-memory-concurrency/TODO-11) may proceed with
     * reattach. COLD_FALLBACK on any rejection -- the validator logged
     * the reason, the caller logs the decision, boot continues with
     * cold init.
     *
     * Gated on BOOT_CAP_PAYLOAD_DESCRIPTORS (capability negotiation
     * said the descriptor array is populated + validated) AND on
     * BOOT_FLAG_WARM_UPDATE (the global handoff signal).  Both gates
     * are required: skipping the caps_present gate lets a producer
     * with caps_present clear ship a type-9 descriptor that the
     * boot_reserved.c reservation pass (which IS gated on caps_present)
     * would skip, returning preserved memory to PMM; skipping the
     * BOOT_FLAG_WARM_UPDATE gate lets a stale producer's type-9
     * descriptor get ACCEPTED even though the boot-wide handoff flag
     * never announced a warm update, weakening the closed-mask ABI. */
    {
        int caps_ok = (int)((g_boot_info.caps_present
                             & BOOT_CAP_PAYLOAD_DESCRIPTORS) != 0u);
        int flag_set = (int)((g_boot_info.flags
                              & BOOT_FLAG_WARM_UPDATE) != 0u);
        uint32_t j;
        uint32_t accepted_count = 0u;
        for (j = 0u; j < g_boot_info.payload_count; j++) {
            const struct boot_payload_desc *d = &g_boot_info.payload_descriptors[j];
            if (d->type != (uint32_t)BOOT_PAYLOAD_WARM_UPDATE_STATE)
                continue;
            if (!caps_ok || !flag_set) {
                /* Missing precondition: cold-fallback the descriptor
                 * without invoking the warm-update validator. The
                 * descriptor may be stale state from an earlier image;
                 * treating it as ACCEPTED would leave PMM-reservation
                 * skew (H1) or accept handoff data with no global
                 * signal (H3). */
                klog(LOG_WARN, "boot",
                     "boot_warm_update: descriptor[%u] COLD_FALLBACK "
                     "(caps_payload=%u flag_warm_update=%u; both required)",
                     (uint64_t)j, (uint64_t)caps_ok, (uint64_t)flag_set);
                continue;
            }
            enum boot_warm_update_error werr = BOOT_WARM_UPDATE_ERR_OK;
            enum boot_warm_update_decision wd =
                boot_warm_update_consume(d, &werr);
            if (wd == BOOT_WARM_UPDATE_ACCEPTED) {
                klog(LOG_INFO, "boot",
                     "boot_warm_update: descriptor[%u] ACCEPTED; reattach owned by runtime TODO",
                     (uint64_t)j);
                accepted_count++;
            } else {
                klog(LOG_INFO, "boot",
                     "boot_warm_update: descriptor[%u] COLD_FALLBACK (err=%u); proceeding with cold init",
                     (uint64_t)j, (uint64_t)werr);
            }
        }
        /* Inverse invariant: BOOT_FLAG_WARM_UPDATE set but no descriptor
         * accepted. Stale flag from a producer that never published the
         * descriptor (or all descriptors fell back). Log for audit. */
        if (flag_set && accepted_count == 0u) {
            klog(LOG_WARN, "boot",
                 "boot_warm_update: BOOT_FLAG_WARM_UPDATE set but no "
                 "descriptor accepted; cold init proceeds");
        }
    }

    /* Boot-path provenance and decision record: reject stale/unknown
     * enum values, unknown flag bits, and runaway fallback depths
     * before any Registry/BlackBox/attestation consumer reads
     * boot_path / boot_reason / boot_source_flags / boot_fallback
     * _depth. */
    {
        enum boot_decision_error derr = BOOT_DECISION_ERR_OK;
        if (boot_decision_validate(&g_boot_info, &derr) != BOOT_OK) {
            klog(LOG_ERROR, "boot",
                 "boot_decision_validate failed (err=%u); halting before decision consumers run",
                 (uint64_t)derr);
            boot_halt("boot_decision: provenance record invariants violated");
        }
    }

    /* Publish the immutable kernel configuration snapshot now that the boot
     * decision provenance (boot_reason / selected_entry_id) has been validated.
     * Later phases consume kernel_config_get() instead of re-reading boot_args /
     * boot_config / boot_info. */
    kernel_config_publish(&g_boot_info.config);

    /* Anti-rollback: sanity-check flags + security-version fields.
     * The downgrade refusal itself was enforced pre-jump by the
     * bootloader; this validator catches producer bugs (unknown flag
     * bits, versions > BOOT_SECURITY_VERSION_MAX) before any consumer
     * trusts the values. */
    {
        enum boot_rollback_error rerr = BOOT_ROLLBACK_ERR_OK;
        if (boot_rollback_validate(&g_boot_info, &rerr) != BOOT_OK) {
            klog(LOG_ERROR, "boot",
                 "boot_rollback_validate failed (err=%u); halting before anti-rollback consumers run",
                 (uint64_t)rerr);
            boot_halt("boot_rollback: anti-rollback invariants violated");
        }
    }

    /* S13: Log previous boot error if one was persisted in NVRAM.
     * 8 hex digits to match the UINT32 boot_fatal/NVRAM/boot_info
     * contract -- the bootloader's serial CRIT line and graphical
     * BSOD already render 8. */
    if (g_boot_info.last_boot_error != 0)
        klog(LOG_WARN, "UEFI",
             "Previous boot failed: code=0x%08X",
             (uint64_t)g_boot_info.last_boot_error);

    /*: Log boot device info from boot_info */
    if (g_boot_info.boot_device_path[0] != '\0')
        klog(LOG_INFO, "UEFI", "Booted from: %s (type=%u)",
             g_boot_info.boot_device_path,
             (uint64_t)g_boot_info.boot_device_type);

    /* boot_config_parse: boot.conf is parsed by the UEFI bootloader before
     * kernel entry and delivered in g_boot_info.config.  Log the values. */
    klog(LOG_INFO, "CONF",
         "boot.conf: debug=%d verbose=%d serial=%d mode=%d "
         "splash=%ds heartbeat=%d postcode=%d test=%d%s",
           g_boot_info.config.debug, g_boot_info.config.verbose,
           g_boot_info.config.serial_debug, g_boot_info.config.boot_mode,
           g_boot_info.config.splash_timeout, g_boot_info.config.heartbeat,
           g_boot_info.config.postcode, g_boot_info.config.test,
           g_boot_info.config.config_found ? "" : " (defaults)");

    /* --- UEFI runtime services (SetVirtualAddressMap + RT props) --- */
    POST16(POST16_UEFI_RT);
    {
        boot_result_t r = uefi_runtime_init();
        if (r == BOOT_DEGRADED)
            klog(LOG_WARN, "UEFI", "Runtime services unavailable -- degraded");
    }

    /* --- Read prior boot POST code, then mark "booting" in NVRAM ---
     * Read MUST happen before the first NVRAM write -- otherwise we read
     * our own value.  NVRAM writes per boot (5 total, flash-safe):
     *   1. Here: mark "booting" (POST16_SERIAL = 0x0010)
     *   2. Phase 0 complete (POST16_SIMD_OK)
     *   3. Phase 1 complete (POST16_TIMER_OK)
     *   4. Phase 2 complete (POST16_REGISTRY_OK)
     *   5. Phase 3 complete (POST16_BOOT_OK) */
    {
        /* When boot.conf postcode=0 disables POST persistence, do NOT consume
         * the prior NVRAM value either: the write is gated off below, so a
         * stale code would otherwise be reported as the previous boot outcome
         * on every later boot. Read/banner only when persistence is enabled. */
        int post_persist = !(g_boot_info.config.config_found &&
                             !g_boot_info.config.postcode);
        int last_post = post_persist ? boot_post_read16() : -1;
        boot_post_nvram_write16(POST16_SERIAL);  /* NVRAM: booting (gated inside) */
        POST16(POST16_UEFI_RT_OK);

        if (last_post >= 0) {
            extern void vpd_crash_banner(uint16_t);
            if (last_post == (int)POST16_BOOT_OK)
                serial_write("[BOOT] Last boot succeeded\n");
            else
                serial_write("[BOOT] Last boot failed\n");
            vpd_crash_banner((uint16_t)last_post);
        }
        boot_progress(0, "post-code-log", 0x11);
    }

    /* Read previous boot perf data from NVRAM (before we overwrite it) */
    boot_perf_read_prev();

    /* Initialize VPD Tier 1 -- after crash banner so s_banner_shown is set */
    {
        extern void vpd_init(void);
        vpd_init();
    }

    /* A/B slot handoff milestone (TODO-21 sec6). The slot was selected pre-EBS
     * in the bootloader (a different timing domain); this records when the
     * kernel observed + surfaced the result, so the boot timeline shows the
     * A/B resolution point. Only meaningful on an A/B disk. */
    if (g_boot_info.ab_meta_lba != 0)
        boot_timing_record_step(1, "ab-slot-handoff", 0);

    /* --- UEFI variable services (NVRAM enumeration) ---
     * Both readiness oracle channels are updated in lockstep via
     * kernel_subsystem_apply_result(): set_ready (gates BOOT_REQUIRE)
     * and degraded_mask (feeds the desktop summary). */
    POST16(POST16_UEFI_VARS);
    {
        boot_result_t r_vars = uefi_vars_init();
        if (!kernel_subsystem_apply_result(SUBSYS_UEFI_VARS, r_vars))
            klog(LOG_WARN, "UEFI", "Variable services unavailable (r=%d)", (uint64_t)r_vars);
        else if (r_vars == BOOT_DEGRADED)
            klog(LOG_WARN, "UEFI", "Variable services degraded");

        boot_result_t r_time = uefi_time_init();
        if (!kernel_subsystem_apply_result(SUBSYS_UEFI_TIME, r_time))
            klog(LOG_WARN, "UEFI", "RTC wall-clock seed unavailable (r=%d)", (uint64_t)r_time);
        else if (r_time == BOOT_DEGRADED)
            klog(LOG_WARN, "UEFI", "RTC wall-clock seed degraded");
    }
    POST16(POST16_UEFI_VARS_OK);

    /* S13: Clear previous boot error in NVRAM now that the kernel is alive
     * and variable services are initialized.  Clearing HERE (not in the
     * bootloader) ensures that a crash between ExitBootServices and kernel
     * Phase 0 preserves the error evidence for next-boot diagnostics. */
    if (g_boot_info.last_boot_error != 0) {
        static const uint16_t var_name[] = { 'B','o','o','t','E','r','r','o','r', 0 };
        efi_guid_t guid = IMPOSSIBLE_OS_VENDOR_GUID_INIT;
        uint32_t zero = 0;
        uefi_var_set(var_name, &guid, &zero, sizeof(zero), UEFI_VAR_NV_BOOT_RUNTIME);
    }

    /* Render the boot-error history ring.  Reads the 8-entry NVRAM ring
     * populated by the producer-side append sites (bootloader fatal,
     * EBS-success, kernel Phase-3) and emits a klog block listing the
     * recent attempts oldest-first.  Silent no-op on first-ever boot. */
    boot_history_render();

    /* --- Secure Boot state detection --- */
    POST16(POST16_SECUREBOOT);
    {
        boot_result_t r_sb = uefi_secureboot_init();
        if (!kernel_subsystem_apply_result(SUBSYS_SECUREBOOT, r_sb))
            klog(LOG_WARN, "UEFI", "Secure Boot detection unavailable (r=%d)", (uint64_t)r_sb);
        else if (r_sb == BOOT_DEGRADED)
            klog(LOG_WARN, "UEFI", "Secure Boot detection degraded");
    }
    POST16(POST16_SECUREBOOT_OK);

    /* --- TPM measured boot (parse event log) ---
     * tpm_init() establishes presence; tpm_integrity_init() runs PCR
     * checks. The TPM subsystem is "ready" iff BOTH return OK/DEGRADED.
     * Use the worst result (FATAL > DEGRADED > OK) when applying. */
    POST16(POST16_TPM);
    {
        boot_result_t r_tpm = tpm_init();
        boot_result_t r_int = BOOT_OK;
        if (r_tpm == BOOT_OK || r_tpm == BOOT_DEGRADED)
            r_int = tpm_integrity_init();
        /* tpm_init/tpm_integrity_init only return OK/DEGRADED/FATAL
         * (never DEFERRED). For these, numeric enum order matches severity
         * (FATAL=2 > DEGRADED=1 > OK=0), so max() picks the worst. */
        boot_result_t worst = (r_tpm > r_int) ? r_tpm : r_int;
        if (!kernel_subsystem_apply_result(SUBSYS_TPM, worst))
            klog(LOG_WARN, "TPM", "Measured boot unavailable (init=%d integrity=%d)",
                 (uint64_t)r_tpm, (uint64_t)r_int);
        else if (worst == BOOT_DEGRADED)
            klog(LOG_WARN, "TPM", "Measured boot degraded (init=%d integrity=%d)",
                 (uint64_t)r_tpm, (uint64_t)r_int);
    }
    POST16(POST16_TPM_OK);

    /* --- Physical memory manager: BOOT_FATAL if fails --- */
    POST16(POST16_PMM);
    if (pmm_init() != BOOT_OK)
        boot_halt("PMM init failed -- no usable physical memory");
    POST16(POST16_PMM_OK);
    kernel_subsystem_set_ready(SUBSYS_PMM, true);
    boot_progress(0, "PMM", POST16_PMM_OK);

    /* --- Virtual memory manager: BOOT_FATAL if PMM not ready --- */
    if (!kernel_subsystem_ready(SUBSYS_PMM))
        boot_halt("PMM not ready -- cannot init VMM");
    POST16(POST16_VMM);
    if (vmm_init() != BOOT_OK)
        boot_halt("VMM init failed -- cannot set up page tables");
    POST16(POST16_VMM_OK);
    kernel_subsystem_set_ready(SUBSYS_VMM, true);
    boot_progress(0, "VMM", POST16_VMM_OK);

    /* Validate vmm_map_mmio_uc: map LAPIC base as UC, compare with identity-mapped
     * read. Development smoke test for the MMIO mapping path, not required init --
     * gated behind debug so production boots skip the extra pre-sti page-table
     * churn (map/flush/unmap). The 0xD1xx POST states are dev-range and never
     * surface on a production POST card. */
    if (g_boot_info.config.debug) {
        POST16(0xD100);
        volatile uint32_t *lapic_id = (volatile uint32_t *)0xFEE00020;
        uint32_t id_identity = *lapic_id;
        void *uc = vmm_map_mmio_uc(0xFEE00000, 4096);
        POST16(0xD101);
        if (uc) {
            volatile uint32_t *uc_id = (volatile uint32_t *)((uintptr_t)uc + 0x20);
            uint32_t id_uc = *uc_id;
            if (id_identity == id_uc)
                klog(LOG_INFO, "mm", "vmm_map_mmio_uc: LAPIC ID match (%p -> 0x%x)",
                     (uint64_t)(uintptr_t)uc, (uint64_t)id_uc);
            else
                klog(LOG_WARN, "mm", "vmm_map_mmio_uc: LAPIC ID mismatch (0x%x vs 0x%x)",
                     (uint64_t)id_identity, (uint64_t)id_uc);
            vmm_unmap_mmio(uc, 4096);
        } else {
            klog(LOG_WARN, "mm", "vmm_map_mmio_uc: LAPIC test failed (NULL)");
        }
    }

    /* --- Kernel heap: BOOT_FATAL if VMM not ready --- */
    if (!kernel_subsystem_ready(SUBSYS_VMM))
        boot_halt("VMM not ready -- cannot init heap");
    POST16(POST16_HEAP);
    if (heap_init() != BOOT_OK)
        boot_halt("Heap init failed -- cannot allocate kernel heap");
    POST16(POST16_HEAP_OK);
    kernel_subsystem_set_ready(SUBSYS_HEAP, true);
    boot_progress(0, "HEAP", POST16_HEAP_OK);

    /* --- klog early init: ring buffer + serial only (no disk yet) --- */
    POST16(POST16_KLOG);
    klog_early_init();
    POST16(POST16_KLOG_OK);
    kernel_subsystem_set_ready(SUBSYS_KLOG, true);
    boot_progress(0, "KLOG", POST16_KLOG_OK);

    /* Recover crash log from previous boot (needs PMM + UEFI runtime) */
    klog_crash_recover();

    /* --- CPUID: probe CPU features --- */
    POST16(POST16_CPUID);
    cpuid_init();
    POST16(POST16_CPUID_OK);
    boot_progress(0, "CPUID", POST16_CPUID_OK);

    /* --- CPU feature minimum requirements --- */
    {
        /* The BSP gate enforces the SAME baseline that AP feature validation
         * (cpu_validate_ap_features, CPU_FEATURES_REQUIRED_MASK) bug-checks
         * APs against. Without this, a BSP missing a required bit (e.g.
         * SYSCALL) boots past the clear minimum-requirements halt while an
         * AP with the same gap would bug-check -- a divergent contract. */
        /* Rows generated from CPU_FEATURES_REQUIRED_LIST (cpuid.h): the
         * same X-macro that generates CPU_FEATURES_REQUIRED_MASK, so the
         * gate table and the mask cannot drift apart. */
#define CPU_FEATURE_REQ_ROW_(f, diag) { (f), (diag) },
        static const struct {
            enum cpu_feature feat;
            const char      *msg;
        } req[] = {
            CPU_FEATURES_REQUIRED_LIST(CPU_FEATURE_REQ_ROW_)
        };
#undef CPU_FEATURE_REQ_ROW_
        int missing = 0;
        /* LOG_ERROR, not LOG_FATAL: klog(LOG_FATAL) halts in its own
         * for(;;) loop and never returns, which would defeat the
         * accumulate-all-features pattern AND make the boot_halt below
         * unreachable (no styled halt screen / POST16_BOOT_FAILED). Let
         * boot_halt render the single fatal boot failure. */
        for (size_t i = 0; i < sizeof(req) / sizeof(req[0]); i++) {
            if (!cpu_has(req[i].feat)) {
                klog(LOG_ERROR, "cpu", "MINIMUM: %s", req[i].msg);
                missing = 1;
            }
        }
        if (missing)
            boot_halt("CPU does not meet minimum requirements (see MINIMUM lines on serial)");

        /* Recommended features: warn if missing, continue */
        if (!cpu_has(CPU_FEATURE_SMEP))
            klog(LOG_WARN, "cpu", "RECOMMENDED: SMEP not available");
        if (!cpu_has(CPU_FEATURE_SMAP))
            klog(LOG_WARN, "cpu", "RECOMMENDED: SMAP not available");
        if (!cpu_has(CPU_FEATURE_RDRAND))
            klog(LOG_WARN, "cpu", "RECOMMENDED: RDRAND not available");
    }

    /* --- BSP: write IA32_TSC_AUX = 0 for RDTSCP CPU identification ---
     * Probe first via msr_try_read: WHPX may expose RDTSCP in CPUID
     * but trap TSC_AUX MSR access. If probe fails, skip TSC_AUX setup
     * (RDTSCP still works for TSC reads, just ECX is unpredictable). */
    if (cpu_has(CPU_FEATURE_RDTSCP)) {
        extern int g_tsc_aux_available;
        uint64_t probe;
        if (msr_try_read(MSR_IA32_TSC_AUX, &probe) == 0) {
            msr_write(MSR_IA32_TSC_AUX, 0);
            g_tsc_aux_available = 1;
            klog(LOG_INFO, "cpu", "RDTSCP: IA32_TSC_AUX = 0 (BSP)");
        } else {
            klog(LOG_WARN, "cpu",
                 "RDTSCP: TSC_AUX MSR probe failed; CPU ID tagging disabled");
        }
    }

    /* --- CPU security hardening: NX now; SMEP/SMAP skipped (see below) --- */
    POST16(POST16_CPU_HARDEN);
    cpu_harden();
    cpu_security_log_state("pre-VMM");
    POST16(POSTCODE_CPU_HARDEN_DONE);

    /* --- Apply NX policy (per-page NX on kernel non-text mappings) ---
     * NOTE: this applies NX only -- it does NOT clear the User bit from
     * kernel pages. The boot PML4 (entry.asm flag 0x87) still carries User
     * on all kernel 2 MiB pages, which is why cpu_harden_post_pagetable()
     * below still skips SMEP/SMAP on EVERY platform (hv_supports_cr4_smep_smap
     * returns 0). The User-bit clear is the KPTI clean-kernel-PML4 work in
     * 02-kernel-core/TODO-10 (KPTI sections). */
    POST16(POST16_NX_POLICY);
    vmm_apply_nx_policy();

    /* --- Promote upper GiBs to 1 GiB pages (after NX policy) --- */
    vmm_promote_to_1g();
    POST16(POST16_NX_POLICY_OK);

    /* --- PAT: program entry 1 = WC for framebuffer VRAM ---
     * PAT is per-CPU and is owned by a single authoritative write per CPU
     * (TODO-09-boot S8): this is the BSP's, placed AFTER the page-table
     * takeover above because a CR3 reload can reset PAT to the Intel default
     * on some hypervisors. (APs program PAT once via the MSR-profile replay in
     * ap_cpu_harden(); cpu_harden() no longer touches PAT.) Readback verify:
     * if WHPX traps the write (returns Intel default), degrade to WT (still
     * functional, ~2-5x slower than WC for FB). */
    POST16(POST16_PAT);
    {
        cpu_configure_pat();
        uint64_t pat = msr_read(MSR_IA32_PAT);
        uint8_t entry1 = (uint8_t)((pat >> 8) & 0xFF);
        if (entry1 == 0x01) {
            klog(LOG_INFO, "mm", "PAT: entry 1 = WC (0x%016llx)", pat);
        } else {
            klog(LOG_WARN, "mm",
                 "PAT: entry 1 = 0x%02x (expected WC=0x01); hypervisor may trap PAT writes. "
                 "Framebuffer mapped WT instead of WC (functional, slower)",
                 (uint64_t)entry1);
        }
    }
    POST16(POST16_PAT_OK);

    /* --- SMEP/SMAP enable attempt (skipped on all platforms today) ---
     * cpu_harden_post_pagetable() calls cpu_enable_smep/smap, but both bail
     * via hv_supports_cr4_smep_smap()==0 because kernel pages still carry the
     * User bit (see the NX-policy note above). cpu_verify_hardening() reads
     * back EFER/CR4 and logs NX enabled + SMEP/SMAP skipped. They activate
     * once the KPTI clean kernel PML4 lands (02-kernel-core/TODO-10). */
    cpu_harden_post_pagetable();
    cpu_security_log_state("post-pagetable");
    cpu_verify_hardening();

    /* --- KPTI trampoline infrastructure (S3) --- */
    {
        extern void kpti_init(void);
        kpti_init();
    }

    /* --- SIMD: enable AVX2 or fall back to SSE2 ---
     * AVX-512 opt-in is deferred to phase 1 because its throttle guard
     * probes IA32_MPERF/APERF via msr_try_read(), which requires the
     * kernel IDT to be loaded to catch a #GP safely. Phase 0 splash and
     * memops only need AVX2 / SSE2. */
    POST16(POST16_SIMD);
    simd_enable_avx();
    if (simd_avx2_ok)
        klog(LOG_INFO, "simd", "AVX2 enabled (8 pixels/iter)");
    else
        klog(LOG_WARN, "simd", "AVX2 not available, using SSE2 fallback");
    boot_progress(0, "SIMD", POST16_SIMD_OK);

    /* PKU key allocator (must run after cpu_configure_xcr0 sets XCR0 bit 9) */
    if (cpu_has(CPU_FEATURE_PKU))
        pku_init();

    klog(LOG_DEBUG, "", "");
    klog(LOG_DEBUG, "", "[PHASE0] complete -- serial, memory, klog, CPUID, SIMD ready");

    /* NVRAM write: Phase 0 complete (0x0091 = SIMD_OK) */
    boot_post_nvram_write16(POST16_SIMD_OK);
}

/* ---- Legacy wrapper (until main.c switches to boot_phase0) -------------- */

void boot_hw_init(uint64_t magic, uint64_t mbi)
{
    boot_phase0(magic, mbi);
}

/* ----: Boot device Registry population --------------------------------
 * Called from registry_populate_defaults() in Phase 2 after registry_init().
 * Populates HKLM\SYSTEM\Boot\Device\ with boot provenance fields from
 * g_boot_info (set by bootloader pre-ExitBootServices). */

void boot_device_populate_registry(void)
{
    HKEY hKey = (HKEY)0;
    uint32_t disp = 0;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Boot\\Device", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) != ERROR_SUCCESS) {
        klog(LOG_WARN, "boot", "Failed to create HKLM\\SYSTEM\\Boot\\Device key");
        return;
    }

    RegSetDword(hKey, "Type", (uint32_t)g_boot_info.boot_device_type);
    RegSetString(hKey, "Path", g_boot_info.boot_device_path);
    RegSetDword(hKey, "PartitionStyle", (uint32_t)g_boot_info.boot_partition_style);
    RegSetDword(hKey, "Removable", (uint32_t)g_boot_info.boot_device_removable);
    RegSetDword(hKey, "BootCurrent", (uint32_t)g_boot_info.uefi_boot_current);
    RegSetDword(hKey, "BootNext",
                g_boot_info.uefi_boot_next_valid
                    ? (uint32_t)g_boot_info.uefi_boot_next : 0xFFFF);

    /* v15: Extended boot-variable capability surface. UEFI 2.10 spec 3.1.3
     * (Attributes), 3.1.4 (BootOptionSupport), 8.5.4 (OsIndicationsSupported). */
    RegSetDword(hKey, "BootCurrentAttributes", g_boot_info.boot_current_attrs);
    RegSetDword(hKey, "BootOptionSupport", g_boot_info.boot_option_support);
    RegSetDword(hKey, "OsIndicationsSupportedLo",
                (uint32_t)(g_boot_info.os_indications_supported & 0xFFFFFFFFU));
    RegSetDword(hKey, "OsIndicationsSupportedHi",
                (uint32_t)(g_boot_info.os_indications_supported >> 32));
    RegSetString(hKey, "Description", g_boot_info.boot_description);

    /* v16: Local boot device path detail. UEFI 2.10 spec 10.3.2.1 (PCI
     * Hardware DP) + 10.3.4.21 (NVMe Messaging DP). NSID always written;
     * EUI-64 written only when at least one byte is non-zero (some NVMe
     * firmware reports zero EUI-64 -- skipping the value matches Win11
     * MSFT_Disk.UniqueId behavior of omitting absent identifiers). PCI
     * sentinel 0xFF preserved verbatim. */
    RegSetDword(hKey, "NamespaceId", g_boot_info.boot_nvme_nsid);
    {
        int eui_nonzero = 0;
        int i;
        for (i = 0; i < 8; i++) {
            if (g_boot_info.boot_nvme_eui64[i] != 0) {
                eui_nonzero = 1;
                break;
            }
        }
        if (eui_nonzero) {
            RegSetValueEx(hKey, "NamespaceEui64", 0, REG_BINARY,
                          g_boot_info.boot_nvme_eui64, 8);
        }
    }
    RegSetDword(hKey, "PciDevice", (uint32_t)g_boot_info.boot_pci_device);
    RegSetDword(hKey, "PciFunction", (uint32_t)g_boot_info.boot_pci_function);

    /* Format partition GUID as string for REG_SZ */
    if (g_boot_info.boot_partition_style == 2) {
        const uint8_t *g = g_boot_info.boot_partition_guid;
        static const char hex[] = "0123456789ABCDEF";
        char guid_str[37]; /* XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX + NUL */
        int p = 0;
        /* Data1 (4 bytes LE) */
        guid_str[p++] = hex[g[3] >> 4]; guid_str[p++] = hex[g[3] & 0xF];
        guid_str[p++] = hex[g[2] >> 4]; guid_str[p++] = hex[g[2] & 0xF];
        guid_str[p++] = hex[g[1] >> 4]; guid_str[p++] = hex[g[1] & 0xF];
        guid_str[p++] = hex[g[0] >> 4]; guid_str[p++] = hex[g[0] & 0xF];
        guid_str[p++] = '-';
        /* Data2 (2 bytes LE) */
        guid_str[p++] = hex[g[5] >> 4]; guid_str[p++] = hex[g[5] & 0xF];
        guid_str[p++] = hex[g[4] >> 4]; guid_str[p++] = hex[g[4] & 0xF];
        guid_str[p++] = '-';
        /* Data3 (2 bytes LE) */
        guid_str[p++] = hex[g[7] >> 4]; guid_str[p++] = hex[g[7] & 0xF];
        guid_str[p++] = hex[g[6] >> 4]; guid_str[p++] = hex[g[6] & 0xF];
        guid_str[p++] = '-';
        /* Data4 (8 bytes, big-endian) */
        guid_str[p++] = hex[g[8] >> 4]; guid_str[p++] = hex[g[8] & 0xF];
        guid_str[p++] = hex[g[9] >> 4]; guid_str[p++] = hex[g[9] & 0xF];
        guid_str[p++] = '-';
        int i;
        for (i = 10; i < 16; i++) {
            guid_str[p++] = hex[g[i] >> 4];
            guid_str[p++] = hex[g[i] & 0xF];
        }
        guid_str[p] = '\0';
        RegSetString(hKey, "PartitionGUID", guid_str);
    } else if (g_boot_info.boot_partition_style == 1) {
        /* MBR: 4-byte signature as hex */
        const uint8_t *g = g_boot_info.boot_partition_guid;
        static const char hex[] = "0123456789ABCDEF";
        char sig_str[9];
        sig_str[0] = hex[g[3] >> 4]; sig_str[1] = hex[g[3] & 0xF];
        sig_str[2] = hex[g[2] >> 4]; sig_str[3] = hex[g[2] & 0xF];
        sig_str[4] = hex[g[1] >> 4]; sig_str[5] = hex[g[1] & 0xF];
        sig_str[6] = hex[g[0] >> 4]; sig_str[7] = hex[g[0] & 0xF];
        sig_str[8] = '\0';
        RegSetString(hKey, "PartitionGUID", sig_str);
    } else {
        /* Unknown partition style -- write empty string so the key
         * always has all expected values (no silent omission). */
        RegSetString(hKey, "PartitionGUID", "");
    }

    /* MediaRole: the boot-media role-detection feature wrote
     * boot_info.boot_media_role from /IPOS/role.txt on the ESP
     * (cross-checked against BlackBox). Surfacing the name here under
     * HKLM\SYSTEM\Boot\Device gives installer / recovery / diagnostics
     * shell launchers a single read path. Mismatch is published as a
     * DWORD so attestation tooling can detect "media role is normal
     * because the markers disagreed" vs "media role is normal because
     * the markers said so". */
    RegSetString(hKey, "MediaRole",
                 boot_media_role_name(g_boot_info.boot_media_role));
    RegSetDword(hKey, "MediaRoleEnum",     g_boot_info.boot_media_role);
    RegSetDword(hKey, "MediaRoleMismatch",
                (uint32_t)g_boot_info.boot_media_role_mismatch);

    RegCloseKey(hKey);

    /* Non-normal roles imply a downstream shell-launch handler that
     * lives outside this section; emit a single line that names the
     * owning TODO so an operator reading the serial log can see the
     * handoff plan. The handler itself is not a media-role-handoff
     * deliverable: the installer shell is owned by the unattended-
     * install feature (todo/15-installer-release/TODO-02), the
     * recovery shell by the recovery-partition feature (todo/01-
     * boot-platform/TODO-22); this kernel never assumes those shells
     * exist. */
    if (g_boot_info.boot_media_role != BOOT_MEDIA_ROLE_UNSET &&
        g_boot_info.boot_media_role != BOOT_MEDIA_ROLE_NORMAL) {
        const char *who = "(no owner registered)";
        switch (g_boot_info.boot_media_role) {
            case BOOT_MEDIA_ROLE_INSTALLER:
                who = "todo/15-installer-release/TODO-02 (unattended install mode)";
                break;
            case BOOT_MEDIA_ROLE_RECOVERY:
                who = "todo/01-boot-platform/TODO-22 (recovery partition)";
                break;
            case BOOT_MEDIA_ROLE_DIAGNOSTICS:
                who = "todo/01-boot-platform/TODO-22 (diagnostics shell)";
                break;
            case BOOT_MEDIA_ROLE_LIVE:
            case BOOT_MEDIA_ROLE_MANUFACTURING:
                who = "(no override -- run as normal cold boot)";
                break;
            default: break;
        }
        klog(LOG_INFO, "boot",
             "Media role %s requested -- shell launch handler in %s",
             (uint64_t)(uintptr_t)boot_media_role_name(g_boot_info.boot_media_role),
             (uint64_t)(uintptr_t)who);
    }

    klog(LOG_INFO, "boot",
         "Boot device Registry populated: %s type=%u removable=%u media_role=%s",
         g_boot_info.boot_device_path,
         (uint64_t)g_boot_info.boot_device_type,
         (uint64_t)g_boot_info.boot_device_removable,
         (uint64_t)(uintptr_t)boot_media_role_name(g_boot_info.boot_media_role));

    /* HKLM\HARDWARE\BOOT\ESP -- ESP identity surfaced from the
     * bootloader's pre-load integrity check (esp_integrity_check in
     * src/boot/uefi/bootx64.c). Mirrors what Linux exposes via
     * efibootmgr UUID and what Win11 exposes via msinfo32 boot disk
     * fields, with the added FilesystemType / TypeGuidValid bits so
     * post-boot tools can tell at-a-glance whether the integrity gate
     * passed (split-path) or was deliberately skipped (UKI). The Uuid
     * value is the partition UNIQUE GUID (already populated above as
     * boot_partition_guid); the same string is duplicated here under
     * the BOOT\ESP key so callers querying the ESP-by-name path do
     * not need to know about HKLM\SYSTEM\Boot\Device internals. */
    {
        HKEY hEsp = (HKEY)0;
        uint32_t edisp = 0;
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\BOOT\\ESP", 0,
                           (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                           &hEsp, &edisp) == ERROR_SUCCESS) {
            RegSetDword(hEsp, "SizeMB", g_boot_info.esp_size_mb);
            RegSetDword(hEsp, "FilesystemType",
                        (uint32_t)g_boot_info.esp_filesystem_type);
            RegSetDword(hEsp, "TypeGuidValid",
                        (uint32_t)g_boot_info.esp_type_guid_valid);
            /* Uuid: format the unique GUID identically to PartitionGUID
             * above (UEFI mixed-endian -> string). Empty string when
             * the partition style is not GPT, so the key always has a
             * value (no silent omission). */
            if (g_boot_info.boot_partition_style == 2) {
                const uint8_t *g = g_boot_info.boot_partition_guid;
                static const char hex[] = "0123456789ABCDEF";
                char guid_str[37];
                int p = 0;
                guid_str[p++] = hex[g[3] >> 4]; guid_str[p++] = hex[g[3] & 0xF];
                guid_str[p++] = hex[g[2] >> 4]; guid_str[p++] = hex[g[2] & 0xF];
                guid_str[p++] = hex[g[1] >> 4]; guid_str[p++] = hex[g[1] & 0xF];
                guid_str[p++] = hex[g[0] >> 4]; guid_str[p++] = hex[g[0] & 0xF];
                guid_str[p++] = '-';
                guid_str[p++] = hex[g[5] >> 4]; guid_str[p++] = hex[g[5] & 0xF];
                guid_str[p++] = hex[g[4] >> 4]; guid_str[p++] = hex[g[4] & 0xF];
                guid_str[p++] = '-';
                guid_str[p++] = hex[g[7] >> 4]; guid_str[p++] = hex[g[7] & 0xF];
                guid_str[p++] = hex[g[6] >> 4]; guid_str[p++] = hex[g[6] & 0xF];
                guid_str[p++] = '-';
                guid_str[p++] = hex[g[8] >> 4]; guid_str[p++] = hex[g[8] & 0xF];
                guid_str[p++] = hex[g[9] >> 4]; guid_str[p++] = hex[g[9] & 0xF];
                guid_str[p++] = '-';
                int gi;
                for (gi = 10; gi < 16; gi++) {
                    guid_str[p++] = hex[g[gi] >> 4];
                    guid_str[p++] = hex[g[gi] & 0xF];
                }
                guid_str[p] = '\0';
                RegSetString(hEsp, "Uuid", guid_str);
            } else {
                RegSetString(hEsp, "Uuid", "");
            }
            RegCloseKey(hEsp);
            klog(LOG_INFO, "boot",
                 "ESP Registry populated: SizeMB=%u FsType=%u GuidValid=%u",
                 g_boot_info.esp_size_mb,
                 (uint64_t)g_boot_info.esp_filesystem_type,
                 (uint64_t)g_boot_info.esp_type_guid_valid);
        } else {
            klog(LOG_WARN, "boot",
                 "Failed to create HKLM\\HARDWARE\\BOOT\\ESP key");
        }
    }

    /* HKLM\SYSTEM\Boot\Trust -- firmware trust-landscape surface (v18,
     * artifact-signing partial-ship subset). Populated from the v18
     * boot_info fields written by uefi_secureboot_init(). Operators
     * and attestation tooling read these to see SecureBoot/SBAT/dbx
     * posture without re-querying UEFI. SbatLevel is empty when the
     * variable was absent; DbxSize=0 means dbx absent or unreadable.
     * DegradedTrustFlags is the BOOT_DEGRADED_TRUST_* bitmask, and
     * DegradedTrustNames is a space-separated list of bit names from
     * boot_degraded_trust_bit_name() for direct human reading. */
    {
        HKEY hTrust = (HKEY)0;
        uint32_t tdisp = 0;
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Boot\\Trust", 0,
                           (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                           &hTrust, &tdisp) == ERROR_SUCCESS) {
            RegSetString(hTrust, "SbatLevel", g_boot_info.sbat_level);
            RegSetDword(hTrust, "SbatLevelSize",       g_boot_info.sbat_level_size);
            RegSetDword(hTrust, "DbxSize",             g_boot_info.dbx_size);
            RegSetDword(hTrust, "DegradedTrustFlags",  g_boot_info.degraded_trust_flags);

            char names_buf[160];
            int p = 0;
            uint32_t tflags = g_boot_info.degraded_trust_flags;
            uint32_t bit;
            for (bit = 1u;
                 bit != 0u && bit <= (uint32_t)BOOT_DEGRADED_TRUST_MASK_KNOWN;
                 bit <<= 1) {
                if ((tflags & bit) == 0u) continue;
                const char *n = boot_degraded_trust_bit_name(bit);
                if (p > 0 && p < (int)sizeof(names_buf) - 1)
                    names_buf[p++] = ' ';
                while (*n && p < (int)sizeof(names_buf) - 1)
                    names_buf[p++] = *n++;
            }
            names_buf[p] = '\0';
            RegSetString(hTrust, "DegradedTrustNames", names_buf);
            RegCloseKey(hTrust);

            klog(LOG_INFO, "boot",
                 "Trust Registry populated: sbat_size=%u dbx_size=%u flags=0x%x (%s)",
                 g_boot_info.sbat_level_size,
                 g_boot_info.dbx_size,
                 g_boot_info.degraded_trust_flags,
                 (uint64_t)(uintptr_t)(p > 0 ? names_buf : "clean"));
        } else {
            klog(LOG_WARN, "boot",
                 "Failed to create HKLM\\SYSTEM\\Boot\\Trust key");
        }
    }
}

/* ---- Boot decision Registry population ----------------------------------
 * Populates HKLM\SYSTEM\Boot\Decision\ with the provenance fields
 * boot_path / boot_reason / boot_source_flags / boot_fallback_depth so
 * recovery, attestation, and rollback logic can read one authoritative
 * record instead of re-deriving the decision from scattered fields.
 * Path and reason are written in two forms: the enum value as a DWORD
 * for programmatic consumers, and the short name as a REG_SZ for
 * operator-readable tools. */

void boot_decision_populate_registry(void)
{
    HKEY hKey = (HKEY)0;
    uint32_t disp = 0;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Boot\\Decision", 0,
                       (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hKey, &disp) != ERROR_SUCCESS) {
        klog(LOG_WARN, "boot",
             "Failed to create HKLM\\SYSTEM\\Boot\\Decision key");
        return;
    }

    RegSetDword(hKey, "Path",          g_boot_info.boot_path);
    RegSetString(hKey, "PathName",     boot_path_name(g_boot_info.boot_path));
    RegSetDword(hKey, "Reason",        g_boot_info.boot_reason);
    RegSetString(hKey, "ReasonName",   boot_reason_name(g_boot_info.boot_reason));
    RegSetDword(hKey, "SourceFlags",   g_boot_info.boot_source_flags);
    RegSetDword(hKey, "FallbackDepth", g_boot_info.boot_fallback_depth);

    /* Bootloader build identity: surfaces the producer's git_sha +
     * build time + label so post-boot triage tools can attribute the
     * handoff to a specific BOOTX64.EFI image. Mirrors what
     * boot_info.loader_identity carries. The value comes from
     * compile-time constants the bootloader baked into its image
     * via tools/boot-info-manifest/gen-loader-identity.sh. */
    {
        /* git_sha as a hex string for REG_SZ; raw bytes available
         * via boot_info.loader_identity.git_sha for tools that need
         * the binary form. */
        char sha_hex[41];
        const char *xd = "0123456789abcdef";
        uint32_t i;
        int sha_zero = 1;
        for (i = 0; i < 20; i++) {
            uint8_t b = g_boot_info.loader_identity.git_sha[i];
            if (b != 0) sha_zero = 0;
            sha_hex[i * 2]     = xd[(b >> 4) & 0xFu];
            sha_hex[i * 2 + 1] = xd[b & 0xFu];
        }
        sha_hex[40] = '\0';
        if (sha_zero) {
            RegSetString(hKey, "LoaderGitSha",
                         "0000000000000000000000000000000000000000");
        } else {
            RegSetString(hKey, "LoaderGitSha", sha_hex);
        }
        /* build_unix_time is a uint64; registry has no native QWORD
         * setter exposed today (only DWORD), so split into HIGH/LOW
         * pair until a QWORD wrapper lands. */
        RegSetDword(hKey, "LoaderBuildTimeLow",
                    (uint32_t)(g_boot_info.loader_identity.build_unix_time
                               & 0xFFFFFFFFu));
        RegSetDword(hKey, "LoaderBuildTimeHigh",
                    (uint32_t)(g_boot_info.loader_identity.build_unix_time
                               >> 32));
        /* build_label[24] is NUL-terminated; safe for REG_SZ as-is. */
        char label[25];
        for (i = 0; i < 24; i++)
            label[i] = g_boot_info.loader_identity.build_label[i];
        label[24] = '\0';
        RegSetString(hKey, "LoaderBuildLabel", label);
    }

    RegCloseKey(hKey);

    klog(LOG_INFO, "boot",
         "Boot decision Registry populated: path=%s reason=%s flags=0x%x fallback=%u",
         (uint64_t)(uintptr_t)boot_path_name(g_boot_info.boot_path),
         (uint64_t)(uintptr_t)boot_reason_name(g_boot_info.boot_reason),
         (uint64_t)g_boot_info.boot_source_flags,
         (uint64_t)g_boot_info.boot_fallback_depth);
}
