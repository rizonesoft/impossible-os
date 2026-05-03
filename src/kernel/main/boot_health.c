/* ============================================================================
 * boot_health.c -- consolidated boot-health audit JSON publisher
 *
 * Single-shot Phase-3 publisher for X:\Diag\boot-health.json. Reads from
 * canonical accessor functions in each owning subsystem (no copy-paste of
 * subsystem state) and emits a schema_version=1 JSON document with every
 * "what's wrong on this boot" signal collapsed into one operator-facing
 * dashboard.
 *
 * Wired AFTER firmware_tables_publish_json + firmware_advisor_init +
 * boot_history_kernel_mark_phase3 in boot_desktop.c so recent_boot_times
 * carries the current-boot Phase-3 sentinel and per-artifact health is
 * not self-incomplete.
 *
 * Failure is LOG_WARN only -- a missing or truncated boot-health.json
 * never blocks userland entry. fail-closed truncation: if json_builder
 * fills its 8 KiB buffer, the disk write is SKIPPED rather than emitting
 * malformed JSON consumers cannot parse.
 * ============================================================================ */

#include "kernel/boot_health.h"
#include "kernel/boot_init.h"
#include "kernel/boot_info.h"
#include "kernel/boot_perf_budget.h"
#include "kernel/boot_timing.h"
#include "kernel/firmware_quirks.h"
#include "kernel/fs/vfs.h"
#include "kernel/klog.h"
#include "kernel/mm/pmm.h"
#include "kernel/tpm.h"
#include "kernel/uefi_config.h"
#include "kernel/uefi_runtime.h"
#include "kernel/util/json_builder.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/drivers/nvme.h"

/* ---- Pinned constants -------------------------------------------------- */

#define BOOT_HEALTH_PATH       "X:\\Diag\\boot-health.json"
#define BOOT_HEALTH_BUF_PAGES  2u
#define BOOT_HEALTH_BUF_SIZE   (BOOT_HEALTH_BUF_PAGES * 4096u)

/* JSON-local cap on mat_wx_violations[] length.  Operator UX prefers a
 * brief triage list; the full-detail list lives in firmware-tables.json
 * (the firmware-table inventory writer owns that surface).
 * mat_overflowed=1 is set when the kernel observed more W+X violations
 * than this cap (distinct from MAT_MAX_ENTRIES cache-overflow which is
 * the firmware-tables side concern). */
#define BOOT_HEALTH_MAT_VIOLATION_CAP 16u

/* Last-N boot times to emit. boot_history rings BOOT_HIST_RING_LEN=8 entries
 * total; the 3 most recent give an operator quick triage context. */
#define BOOT_HEALTH_RECENT_BOOTS 3u

/* ---- Pure classifier (testable) ---------------------------------------- */

enum boot_health_secureboot_state
boot_health_classify_secureboot(int state_valid, int setup_mode, int enabled)
{
    /* Priority ordering: UNKNOWN beats SETUP beats ENABLED beats DISABLED.
     * Unreadable state must not be reported as DISABLED -- that is a
     * security false statement an operator could mistake for "Secure Boot
     * is off, that's fine" when the real answer is "we could not read
     * the variable at all". */
    if (!state_valid)
        return BOOT_HEALTH_SB_UNKNOWN;
    if (setup_mode)
        return BOOT_HEALTH_SB_SETUP;
    if (enabled)
        return BOOT_HEALTH_SB_ENABLED;
    return BOOT_HEALTH_SB_DISABLED;
}

const char *boot_health_secureboot_name(enum boot_health_secureboot_state s)
{
    switch (s) {
    case BOOT_HEALTH_SB_ENABLED:  return "ENABLED";
    case BOOT_HEALTH_SB_DISABLED: return "DISABLED";
    case BOOT_HEALTH_SB_SETUP:    return "SETUP";
    default:                      return "UNKNOWN";
    }
}

/* ---- Internal builders -------------------------------------------------- */

static void emit_degraded_caps(struct json_builder *jb)
{
    jb_puts(jb, ",\"degraded_caps\":[");
    int first = 1;
    uint64_t deg = g_boot_info.caps_degraded;
    /* Walk every bit position currently in the degraded mask. caps_degraded
     * is a 64-bit field; iterating bits 0..63 is bounded and cheap.  Emit
     * canonical bit names per docs/boot/boot-health-schema.md (string array,
     * not {bit,name} objects) -- consumers index by name, not by ordinal. */
    for (uint64_t i = 0; i < 64; i++) {
        uint64_t bit = (uint64_t)1 << i;
        if (!(deg & bit))
            continue;
        const char *name = boot_caps_bit_name(bit);
        if (!first) jb_putc(jb, ',');
        first = 0;
        jb_str(jb, name ? name : "unknown");
    }
    jb_putc(jb, ']');
}

static void emit_degraded_subsystems(struct json_builder *jb)
{
    jb_puts(jb, ",\"degraded_subsystems\":[");
    int first = 1;
    /* Iterate the canonical degraded_mask bitmap, NOT readiness state:
     * kernel_subsystem_apply_result(BOOT_DEGRADED) marks the subsystem
     * READY (so it's available to dependents) AND sets degraded_mask.
     * Reading kernel_subsystem_ready() would silently omit ready-but-
     * degraded subsystems (Secure Boot, TPM, UEFI runtime) -- exactly
     * the cases this audit file exists to consolidate.  Iterate slot=0;
     * slot < SUBSYS_COUNT so new entries before the sentinel are picked
     * up automatically. */
    uint32_t mask = g_boot_info.degraded_mask;
    for (uint32_t slot = 0; slot < SUBSYS_COUNT; slot++) {
        if (!(mask & ((uint32_t)1u << slot)))
            continue;
        const char *name = kernel_subsystem_name((kernel_subsys_t)slot);
        if (!first) jb_putc(jb, ',');
        first = 0;
        jb_str(jb, name ? name : "unknown");
    }
    jb_putc(jb, ']');
}

static void emit_missing_capabilities(struct json_builder *jb)
{
    /* Each capability is queried via the canonical accessor in its owning
     * subsystem.  Adding a new check is a one-liner here; the accessor
     * does the actual presence detection. */
    jb_puts(jb, ",\"missing_capabilities\":[");
    int first = 1;

    if (!tpm_available()) {
        jb_str(jb, "TPM");
        first = 0;
    }
    if (g_boot_info.usb_device_count == 0) {
        if (!first) jb_putc(jb, ',');
        jb_str(jb, "USB");
        first = 0;
    }
    if (nvme_controller_count() == 0) {
        if (!first) jb_putc(jb, ',');
        jb_str(jb, "NVME");
        first = 0;
    }
    /* Block devices total: 0 implies a totally headless boot (no SATA, no
     * NVMe, no virtio-blk).  An OVMF + AHCI boot reports >= 1.  This is a
     * coarse network-stack heuristic stand-in; the network subsystem does
     * not yet expose a "stack-up" oracle.  When net_ready() lands, swap
     * BLOCK for NETWORK here. */
    if (blkdev_count() == 0) {
        if (!first) jb_putc(jb, ',');
        jb_str(jb, "STORAGE");
        first = 0;
    }
    (void)first;
    jb_putc(jb, ']');
}

/* Walk live boot timeline + per-step budgets, emit only entries that breach
 * SOFT or HARD.  Mirrors boot_perf_budget_check() output but as machine-
 * readable JSON. */
static void emit_perf_breaches(struct json_builder *jb)
{
    const boot_timing_step_t *steps = (const boot_timing_step_t *)0;
    uint32_t step_count = boot_timing_get_steps(&steps);

    jb_puts(jb, ",\"perf_breaches\":[");
    int first = 1;

    /* Mirror boot_perf_budget_check() semantics exactly: for each step i,
     * the budget applies to the elapsed time from step i to step i+1, and
     * the breach is labeled with steps[i].step.  Loop bound `i + 1 <
     * step_count` skips the final marker (no successor to subtract). */
    if (step_count >= 2) {
        for (uint32_t i = 0; i + 1 < step_count; i++) {
            const struct boot_phase_budget *b =
                boot_perf_budget_lookup(steps[i].step);
            if (!b)
                continue;
            uint64_t delta_ticks = (steps[i + 1].tsc > steps[i].tsc)
                                 ? steps[i + 1].tsc - steps[i].tsc : 0;
            uint32_t elapsed_ms = boot_timing_tsc_delta_ms(delta_ticks);
            enum boot_perf_budget_class cls =
                boot_perf_budget_classify(elapsed_ms, b->target_ms);
            if (cls == BUDGET_OK)
                continue;
            if (!first) jb_putc(jb, ',');
            first = 0;
            jb_putc(jb, '{');
            jb_puts(jb, "\"step\":");         jb_str(jb, b->step);
            jb_puts(jb, ",\"elapsed_ms\":");  jb_u32_dec(jb, elapsed_ms);
            jb_puts(jb, ",\"budget_ms\":");   jb_u32_dec(jb, b->target_ms);
            jb_puts(jb, ",\"severity\":");
            jb_str(jb, (cls == BUDGET_HARD) ? "HARD" : "SOFT");
            jb_putc(jb, '}');
        }
    }
    jb_putc(jb, ']');
}

/* Map mat_class_t enum values to short stable strings consumers can match
 * on without re-deriving the enum values. */
static const char *mat_class_name(mat_class_t cls)
{
    switch (cls) {
    case MAT_CLASS_GUARD:        return "GUARD";
    case MAT_CLASS_CODE:         return "CODE";
    case MAT_CLASS_DATA:         return "DATA";
    case MAT_CLASS_RODATA:       return "RODATA";
    case MAT_CLASS_WX_VIOLATION: return "WX_VIOLATION";
    default:                     return "UNKNOWN";
    }
}

/* Emit per-violation MAT records (capped) plus a sibling mat_overflowed
 * boolean. mat_overflowed = 1 iff the kernel observed MORE W+X violations
 * than BOOT_HEALTH_MAT_VIOLATION_CAP -- distinct from the inventory's
 * cache-overflow condition (mat_get_count() == MAT_MAX_ENTRIES) which is
 * the firmware-tables JSON publisher's concern. */
static void emit_mat_wx_violations(struct json_builder *jb)
{
    jb_puts(jb, ",\"mat_wx_violations\":[");
    int first = 1;
    uint32_t emitted = 0;
    uint32_t total_wx = 0;
    uint32_t mat_n = mat_get_count();
    for (uint32_t i = 0; i < mat_n; i++) {
        mat_entry_t e;
        if (mat_get_entry(i, &e) != 1)
            continue;
        if (e.cls != MAT_CLASS_WX_VIOLATION)
            continue;
        total_wx++;
        if (emitted >= BOOT_HEALTH_MAT_VIOLATION_CAP)
            continue;
        if (!first) jb_putc(jb, ',');
        first = 0;
        emitted++;
        jb_putc(jb, '{');
        jb_puts(jb, "\"phys_base\":");   jb_hex64(jb, e.phys_addr);
        jb_puts(jb, ",\"page_count\":"); jb_u64_dec(jb, e.num_pages);
        jb_puts(jb, ",\"type\":");       jb_str(jb, mat_class_name(e.cls));
        jb_puts(jb, ",\"attr_hex\":");   jb_hex64(jb, e.attribute);
        jb_putc(jb, '}');
    }
    jb_putc(jb, ']');

    /* sibling boolean: true iff the per-entry list omitted at least one
     * violation due to the JSON-local cap. False when total_wx fits. */
    jb_puts(jb, ",\"mat_overflowed\":");
    jb_puts(jb, (total_wx > BOOT_HEALTH_MAT_VIOLATION_CAP) ? "true" : "false");
}

static void emit_firmware_quirks_active(struct json_builder *jb)
{
    jb_puts(jb, ",\"firmware_quirks_active\":[");
    int first = 1;
    /* Iterator contract: pass the previous bit (0 to start), get the next
     * active bit or 0 when exhausted.  Emit canonical quirk name string per
     * docs/boot/boot-health-schema.md (string array, not {bit,name} object).
     * Consumers match on stable name; bit ordinals are an implementation
     * detail and could renumber without breaking the schema. */
    for (uint32_t bit = firmware_quirks_iter_next(0);
         bit != 0;
         bit = firmware_quirks_iter_next(bit)) {
        const char *name = firmware_quirks_name(bit);
        if (!first) jb_putc(jb, ',');
        first = 0;
        jb_str(jb, name ? name : "unknown");
    }
    jb_putc(jb, ']');
}

/* Emit the last N unix timestamps from the boot history ring, newest first
 * (descending by boot_seq).  Populated-slot sentinel is `boot_seq != 0`;
 * an entry with boot_seq set but unix_time == 0 (RTC failed mid-boot) is
 * still a real boot and emits `0` so consumers see the RTC-failed signal
 * rather than dropping the slot.  When the Phase-3 mark for this boot
 * could not be persisted to NVRAM (committed_seq == 0), the array reflects
 * only prior boots' entries; the current boot is absent from the list, by
 * design (top-level generated_at_utc/boot_seq carry the same 0/0 signal). */
static void emit_recent_boot_times(struct json_builder *jb)
{
    struct boot_error_history_entry ring[BOOT_HIST_RING_LEN];
    (void)boot_history_read(ring);

    /* boot_history_read returns the ring as stored; we must walk it newest-
     * first.  The ring tracks a head index implicitly via the highest
     * boot_seq -- pick the top BOOT_HEALTH_RECENT_BOOTS by seq, in
     * descending order. */
    jb_puts(jb, ",\"recent_boot_times_unix\":[");
    int first = 1;
    uint32_t emitted = 0;
    for (uint32_t pass = 0; pass < BOOT_HEALTH_RECENT_BOOTS; pass++) {
        /* Find the entry with the highest boot_seq we haven't emitted yet. */
        uint32_t best_seq = 0;
        int best_idx = -1;
        for (uint32_t j = 0; j < BOOT_HIST_RING_LEN; j++) {
            /* Populated-slot sentinel: boot_seq != 0.  An entry with
             * boot_seq set but unix_time == 0 (RTC failed mid-boot) is
             * still a real boot -- emit `0` so consumers can see the
             * RTC-failed signal rather than dropping the slot. */
            if (ring[j].boot_seq == 0)
                continue;
            if (ring[j].boot_seq > best_seq) {
                best_seq = ring[j].boot_seq;
                best_idx = (int)j;
            }
        }
        if (best_idx < 0)
            break;
        if (!first) jb_putc(jb, ',');
        first = 0;
        jb_u32_dec(jb, ring[best_idx].unix_time);
        emitted++;
        /* Mark consumed by zeroing the local copy's seq (the populated
         * sentinel) so the next pass'\''s max-seq scan skips this slot. */
        ring[best_idx].boot_seq = 0;
        ring[best_idx].unix_time = 0;
    }
    (void)emitted;
    jb_putc(jb, ']');
}

/* ---- Public publisher --------------------------------------------------- */

void boot_health_publish_json(void)
{
    /* 8 KiB pmm-backed buffer.  Typical boot emits ~2 KiB (6 cap bits + ~25
     * subsys names + ~18 budget rows + 8 quirk bits + a few MAT entries +
     * 3 boot times + small fixed prefix); 8 KiB gives 4x headroom. */
    uintptr_t phys = pmm_alloc_contiguous(BOOT_HEALTH_BUF_PAGES);
    if (!phys) {
        klog(LOG_WARN, "boot_health",
             "JSON: cannot alloc %u pages for boot-health.json",
             (uint64_t)BOOT_HEALTH_BUF_PAGES);
        return;
    }

    struct json_builder jb;
    jb_init(&jb, (char *)phys, BOOT_HEALTH_BUF_SIZE);

    jb_putc(&jb, '{');
    jb_puts(&jb, "\"schema_version\":1");

    /* Resolve current boot's wall-clock + sequence from boot_history.  The
     * publisher is called AFTER boot_history_kernel_mark_phase3() (see
     * boot_desktop.c), but that mark is best-effort -- if NVRAM writes
     * fail (no RuntimeServices, EFI_OUT_OF_RESOURCES, etc.) the ring still
     * contains older entries from prior boots.  Selecting "max boot_seq in
     * the ring" then mis-attributes this boot's JSON to a previous boot.
     *
     * Anchor: ask boot_history for the seq it COMMITTED this boot.  If
     * the mark succeeded, that's the authoritative current-boot seq, and
     * its matching ring entry's unix_time is correct.  If the mark
     * failed (committed_seq == 0), emit `0`/`0` -- consumers treat that
     * as "history unavailable this boot" rather than getting a stale
     * timestamp + stale seq from a different boot. */
    uint32_t now_unix = 0;
    uint32_t cur_seq  = boot_history_kernel_phase3_committed_seq();
    if (cur_seq != 0) {
        struct boot_error_history_entry ring[BOOT_HIST_RING_LEN];
        (void)boot_history_read(ring);
        for (uint32_t j = 0; j < BOOT_HIST_RING_LEN; j++) {
            if (ring[j].boot_seq == cur_seq) {
                now_unix = ring[j].unix_time;
                break;
            }
        }
    }

    jb_puts(&jb, ",\"generated_at_utc\":");
    jb_u32_dec(&jb, now_unix);

    /* boot_seq is informational; downstream consumers correlate this JSON
     * with the boot_history ring entries by seq. */
    jb_puts(&jb, ",\"boot_seq\":");
    jb_u32_dec(&jb, cur_seq);

    jb_puts(&jb, ",\"secureboot_state\":");
    {
        enum boot_health_secureboot_state s =
            boot_health_classify_secureboot(uefi_secureboot_state_valid(),
                                            uefi_secureboot_setup_mode(),
                                            uefi_secureboot_enabled());
        jb_str(&jb, boot_health_secureboot_name(s));
    }

    emit_degraded_caps(&jb);
    emit_degraded_subsystems(&jb);
    emit_missing_capabilities(&jb);
    emit_perf_breaches(&jb);
    emit_mat_wx_violations(&jb);
    emit_firmware_quirks_active(&jb);
    emit_recent_boot_times(&jb);

    jb_putc(&jb, '}');

    if (jb_truncated(&jb)) {
        klog(LOG_WARN, "boot_health",
             "JSON: boot-health.json truncated; skipping disk write");
        for (uint32_t p = 0; p < BOOT_HEALTH_BUF_PAGES; p++)
            pmm_free_frame(phys + p * 4096u);
        return;
    }

    struct vfs_node *f = vfs_open(BOOT_HEALTH_PATH,
                                  VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!f) {
        klog(LOG_WARN, "boot_health",
             "JSON: could not open %s for write", BOOT_HEALTH_PATH);
    } else {
        int wrote = vfs_write(f, 0, (uint32_t)jb_pos(&jb),
                              (uint8_t *)jb_buf(&jb));
        vfs_close(f);
        if (wrote == (int)jb_pos(&jb)) {
            klog(LOG_INFO, "boot_health",
                 "JSON: wrote %s (%u bytes)",
                 BOOT_HEALTH_PATH, (uint64_t)jb_pos(&jb));
        } else {
            /* Short write: VFS_O_TRUNC zeroed the file before we started,
             * so the on-disk state is now a partial JSON prefix that no
             * consumer can parse.  Re-open with TRUNC and immediately
             * close so the file becomes 0 bytes -- "no data this run" is
             * a recoverable signal; "malformed JSON prefix" makes
             * cJSON_Parse + sysinfo + CI gates fail with confusing errors.
             * Best-effort: a second open failure leaves the partial file,
             * but that path is already deeply degraded and the WARN
             * documents it. */
            klog(LOG_WARN, "boot_health",
                 "JSON: short write to %s (wrote=%d, expected=%u); truncating to empty",
                 BOOT_HEALTH_PATH, (uint64_t)wrote, (uint64_t)jb_pos(&jb));
            struct vfs_node *trunc =
                vfs_open(BOOT_HEALTH_PATH,
                         VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
            if (trunc)
                vfs_close(trunc);
        }
    }

    for (uint32_t p = 0; p < BOOT_HEALTH_BUF_PAGES; p++)
        pmm_free_frame(phys + p * 4096u);
}
