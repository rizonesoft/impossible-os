/* ============================================================================
 * boot_audit.c -- Per-boot policy audit JSONL publisher (kernel-side)
 *
 * Phase-3 single-shot. Composes one JSON Lines record from boot_info v20
 * (selection block + sticky audit surface) and appends it to
 * X:\Boot\history.jsonl. After a successful append, ACKs any
 * sticky triggers that were consumed this boot by writing back the
 * `ImpossibleOS-BootSticky` UEFI variable with the trigger bits cleared.
 *
 * Two-phase ack semantics: bootloader is read-only on the sticky var.
 * The ack happens here, AFTER the durable JSONL line lands. A reset
 * before this point leaves the trigger pending, which is the correct
 * sticky behavior -- a recovery request must persist across crashes
 * until the recovery boot durably records its outcome.
 *
 * Failure modes (all best-effort; never block boot):
 *   - BlackBox not mounted (klog_using_blackbox==0) -- no-op WARN
 *   - vfs_open / vfs_write failure -- WARN, no ack
 *   - JSON buffer overflow -- WARN, skip write, no ack
 *   - 4 MiB rotation rename failure -- WARN, write to current file anyway
 *   - uefi_var_set failure on ack -- WARN, trigger persists (idempotent
 *     -- next boot sees the same trigger and tries again)
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "kernel/util/json_builder.h"
#include "kernel/uefi_vars.h"
#include "kernel/time/wall_clock.h"
#include "kernel/nt/filetime.h"
#include "boot/boot_audit_codes.h"
#include "boot/boot_policy.h"

extern int klog_using_blackbox;
extern struct boot_info g_boot_info;

/* UEFI variable name for the sticky record (UCS-2 NUL-terminated) and
 * its vendor GUID. Pinned to match the bootloader's reader. */
static const uint16_t s_sticky_var_name[] = u"ImpossibleOS-BootSticky";
static const efi_guid_t s_sticky_guid = IMPOSSIBLE_OS_VENDOR_GUID_INIT;

/* JSON line buffer. Sized for the worst-case v19/v20 ABI input:
 * 64 rejected entries (each ~120 bytes JSON-escaped: 64-byte id +
 * numeric reason + 25-char reason_name + key overhead) plus ~700 bytes
 * for the top-level + sticky + selection block. 16 KiB headroom is
 * 2x the ABI max so future enum names cannot push the line past the
 * buffer without static_assert review. Static BSS allocation -- the
 * publisher is single-shot from BSP Phase 3 so the cost amortizes. */
#define BOOT_AUDIT_JSON_LINE_CAP  (16u * 1024u)

/* Reason name for the JSONL `reason_name` field. Matches
 * enum boot_selection_reason in include/boot/boot_policy.h. Returns
 * "UNKNOWN" for values outside the enum so the line is still parseable. */
static const char *
audit_selection_reason_name(uint32_t r)
{
    switch (r) {
        case BOOT_SELECTION_UNSET:                 return "UNSET";
        case BOOT_SELECTION_STORE_DEFAULT:         return "STORE_DEFAULT";
        case BOOT_SELECTION_BOOTNEXT_HINT:         return "BOOTNEXT_HINT";
        case BOOT_SELECTION_HOTKEY:                return "HOTKEY";
        case BOOT_SELECTION_WATCHDOG_ROLLBACK:     return "WATCHDOG_ROLLBACK";
        case BOOT_SELECTION_AB_TRY_STATE:          return "AB_TRY_STATE";
        case BOOT_SELECTION_RECOVERY_REQUEST:      return "RECOVERY_REQUEST";
        case BOOT_SELECTION_FALLBACK_NO_VIABLE:    return "FALLBACK_NO_VIABLE";
        case BOOT_SELECTION_FALLBACK_STORE_INVALID:return "FALLBACK_STORE_INVALID";
        case BOOT_SELECTION_UNKNOWN_BOOTCURRENT:   return "UNKNOWN_BOOTCURRENT";
        default:                                   return "UNKNOWN";
    }
}

static const char *
audit_reject_reason_name(uint32_t r)
{
    switch (r) {
        case BOOT_REJECT_REASON_NONE:                return "NONE";
        case BOOT_REJECT_REASON_KIND_SKIPPED:        return "KIND_SKIPPED";
        case BOOT_REJECT_REASON_NOT_ACTIVE:          return "NOT_ACTIVE";
        case BOOT_REJECT_REASON_HIDDEN:              return "HIDDEN";
        case BOOT_REJECT_REASON_MACHINE_ID_MISMATCH: return "MACHINE_ID_MISMATCH";
        case BOOT_REJECT_REASON_TRIES_EXHAUSTED:     return "TRIES_EXHAUSTED";
        case BOOT_REJECT_REASON_PATH_ESCAPE:         return "PATH_ESCAPE";
        case BOOT_REJECT_REASON_KIND_UNAVAILABLE:    return "KIND_UNAVAILABLE";
        default:                                     return "UNKNOWN";
    }
}

/* Parse one ASCII-decimal sequence file. Returns the value, saturating
 * at UINT32_MAX. Returns 0 on absent / unreadable / parse-error. */
static uint32_t
audit_read_sequence_one(const char *path)
{
    struct vfs_node *f = vfs_open(path, VFS_O_READ);
    if (!f) return 0;

    uint8_t buf[BOOT_AUDIT_SEQUENCE_LEN_MAX + 1];
    int n = vfs_read(f, 0, BOOT_AUDIT_SEQUENCE_LEN_MAX, buf);
    vfs_close(f);
    if (n <= 0 || (uint32_t)n > BOOT_AUDIT_SEQUENCE_LEN_MAX) return 0;
    buf[n] = 0;

    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = buf[i];
        if (c >= '0' && c <= '9') {
            uint32_t digit = (uint32_t)(c - '0');
            /* Saturate at UINT32_MAX rather than wrap. Two-limb check:
             * a file containing 4294967296 has v == 429496729
             * (UINT32_MAX/10) at the final digit, then v*10+6 wraps
             * to 0 in u32 arithmetic. Check digit-on-boundary as well
             * as v-above-boundary so a corrupt over-u32 file cannot
             * silently reset the counter. */
            if (v > 0xFFFFFFFFu / 10u ||
                (v == 0xFFFFFFFFu / 10u && digit > 0xFFFFFFFFu % 10u)) {
                v = 0xFFFFFFFFu;
                break;
            }
            v = v * 10u + digit;
        } else if (c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == 0) {
            break;
        } else {
            /* Garbage byte -- file is corrupt. Treat as 0 so we do
             * not skip a chunk of seq space on a stray edit. */
            return 0;
        }
    }
    return v;
}

/* Read the persistent boot_seq counter, taking the MAX of the canonical
 * and shadow files. Dual-file durability: as long as ONE of the two
 * writes succeeded last boot, the new value is recoverable here. A
 * partial write that leaves one file corrupt cannot regress the
 * counter (the other file still has the true value). */
static uint32_t
audit_read_sequence(void)
{
    uint32_t canon = audit_read_sequence_one(BOOT_AUDIT_SEQUENCE_PATH);
    uint32_t shadow = audit_read_sequence_one(BOOT_AUDIT_SEQUENCE_TEMP_PATH);
    return (canon > shadow) ? canon : shadow;
}

/* Write a single sequence file. Returns 1 on full-write success. */
static int
audit_write_sequence_one(const char *path, const char *buf, int n)
{
    struct vfs_node *f = vfs_open(path,
                                  VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!f) return 0;
    int wr = vfs_write(f, 0, (uint32_t)n, (const uint8_t *)buf);
    vfs_close(f);
    return (wr == n) ? 1 : 0;
}

/* Persist `seq` by writing BOTH the canonical and shadow files. As
 * long as at least one write succeeds, the counter is durable -- the
 * reader takes the max of both files and recovers the new value next
 * boot.
 *
 * Why two files instead of an atomic rename? The kernel-side FAT32
 * driver does not safely rewrite LFN chains during rename (only the
 * SFN field updates, leaving the LFN encoding the old name).
 * Dual-file write sidesteps that limitation entirely.
 *
 * Defense-in-depth limitation: a SIMULTANEOUS failure of both writes
 * after VFS_O_TRUNC could leave both files empty and the next boot
 * would read max(0,0)=0, restarting the counter. The realistic
 * probability is near-zero on healthy media (16-byte writes do not
 * exhaust cluster pools); the early-return guard prevents in-boot
 * duplicates, but cross-boot regression after a true dual-write
 * failure on failing media is the accepted residual risk. A
 * read-back-verify protocol was attempted but the FAT32 driver does
 * not flush the dirent size update before re-open, so the read
 * always returns 0 -- making verify a hard boot dependency would
 * skip every publish on healthy hardware.
 *
 * Returns 1 on success (at least one file durably contains seq);
 * 0 if BOTH writes failed. */
static int
audit_write_sequence(uint32_t seq)
{
    char buf[BOOT_AUDIT_SEQUENCE_LEN_MAX];
    /* Build decimal string in reverse, then flip. */
    uint32_t v = seq;
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[BOOT_AUDIT_SEQUENCE_LEN_MAX];
        int t = 0;
        while (v && t < (int)sizeof(tmp)) { tmp[t++] = (char)('0' + (v % 10u)); v /= 10u; }
        while (t > 0) buf[n++] = tmp[--t];
    }
    if (n < (int)BOOT_AUDIT_SEQUENCE_LEN_MAX) buf[n++] = '\n';

    int ok_shadow = audit_write_sequence_one(BOOT_AUDIT_SEQUENCE_TEMP_PATH, buf, n);
    int ok_canon = audit_write_sequence_one(BOOT_AUDIT_SEQUENCE_PATH, buf, n);

    if (!ok_shadow && !ok_canon) {
        klog(LOG_WARN, "BOOT",
             "audit: sequence persist failed (canon + shadow both rejected)");
        return 0;
    }
    if (!ok_shadow || !ok_canon) {
        klog(LOG_WARN, "BOOT",
             "audit: sequence partial persist (canon=%d shadow=%d); "
             "next boot recovers via max-of-both",
             (uint64_t)ok_canon, (uint64_t)ok_shadow);
    }
    return 1;
}

/* Compose the JSONL line into `buf`. Returns the number of bytes
 * written (excluding NUL). Returns 0 on truncation -- caller MUST
 * skip the file write. Takes `bi` as a parameter (rather than reading
 * the global) so unit tests can drive worst-case-shaped synthetic
 * inputs without touching live boot state. `boot_seq` is supplied by
 * the caller because it is persisted on the BlackBox partition (NOT
 * derived from the sticky var, which is exceptional-only). */
size_t
boot_audit_compose_line(const struct boot_info *bi, uint32_t boot_seq,
                        char *buf, size_t cap)
{
    struct json_builder j;
    jb_init(&j, buf, cap);

    /* Determine the audit event code. Special cases override the
     * selection_reason mapping (e.g. audit_degraded supersedes the
     * normal mapping because we cannot trust the selection inputs).
     * The bootloader's degradation-detection precedes the ladder, so
     * audit_degraded == 1 means the ladder ran without sticky-trigger
     * visibility; the JSONL line records the chain-of-degradation. */
    uint16_t event_code;
    if (bi->audit_degraded) {
        event_code = BOOT_AUDIT_EVENT_AUDIT_DEGRADED;
    } else if (bi->sticky_last_event_code == BOOT_AUDIT_EVENT_FIRST_BOOT &&
               bi->sticky_present == 0) {
        event_code = BOOT_AUDIT_EVENT_FIRST_BOOT;
    } else {
        event_code = boot_audit_event_from_selection_reason(bi->selection_reason);
        if (event_code == BOOT_AUDIT_EVENT_UNSET)
            event_code = BOOT_AUDIT_EVENT_NORMAL;
    }

    /* Wall time as unix seconds. wall_clock_ready() guards against
     * pre-Phase-2 calls; ts=0 is the documented "no clock" sentinel. */
    uint64_t ts = 0;
    if (wall_clock_ready())
        ts = filetime_to_unix_seconds(KeQuerySystemTime());

    jb_putc(&j, '{');
    jb_puts(&j, "\"schema\":");
    jb_u32_dec(&j, BOOT_AUDIT_JSONL_SCHEMA_VERSION);
    jb_puts(&j, ",\"ts\":");
    jb_u64_dec(&j, ts);
    jb_puts(&j, ",\"boot_seq\":");
    jb_u32_dec(&j, boot_seq);

    jb_puts(&j, ",\"event\":");
    jb_str(&j, boot_audit_event_name(event_code));
    jb_puts(&j, ",\"event_code\":");
    jb_u32_dec(&j, event_code);

    jb_puts(&j, ",\"selection_reason\":");
    jb_u32_dec(&j, bi->selection_reason);
    jb_puts(&j, ",\"selection_reason_name\":");
    jb_str(&j, audit_selection_reason_name(bi->selection_reason));

    jb_puts(&j, ",\"selected_entry\":");
    jb_str(&j, bi->selected_entry_id);

    jb_puts(&j, ",\"audit_degraded\":");
    jb_u32_dec(&j, bi->audit_degraded ? 1u : 0u);

    /* Sticky surface as a nested object so consumers can ignore it
     * entirely if they only care about the ladder result. */
    jb_puts(&j, ",\"sticky\":{");
    jb_puts(&j, "\"present\":");
    jb_u32_dec(&j, bi->sticky_present ? 1u : 0u);
    jb_puts(&j, ",\"recovery_trigger\":");
    jb_u32_dec(&j, bi->sticky_recovery_trigger ? 1u : 0u);
    jb_puts(&j, ",\"watchdog_rollback_request\":");
    jb_u32_dec(&j, bi->sticky_watchdog_rollback_request ? 1u : 0u);
    jb_puts(&j, ",\"last_outcome\":");
    jb_u32_dec(&j, bi->sticky_last_outcome ? 1u : 0u);
    jb_puts(&j, ",\"audit_degraded_last_boot\":");
    jb_u32_dec(&j, bi->sticky_audit_degraded_last_boot ? 1u : 0u);
    jb_puts(&j, ",\"last_event_code\":");
    jb_u32_dec(&j, bi->sticky_last_event_code);
    jb_puts(&j, ",\"last_event_name\":");
    jb_str(&j, boot_audit_event_name(bi->sticky_last_event_code));
    jb_puts(&j, ",\"last_boot_seq\":");
    jb_u32_dec(&j, bi->sticky_last_boot_seq);
    jb_puts(&j, ",\"consumed_trigger_seq\":");
    jb_u32_dec(&j, bi->sticky_consumed_trigger_seq);
    jb_putc(&j, '}');

    /* Rejected entries array. Cap = rejected_entry_count, packed
     * prefix matches the v19 ABI. Truncates cleanly via jb_*. */
    jb_puts(&j, ",\"rejected\":[");
    uint32_t rcount = bi->rejected_entry_count;
    if (rcount > 64u) rcount = 64u;
    for (uint32_t i = 0; i < rcount; i++) {
        if (i > 0) jb_putc(&j, ',');
        jb_putc(&j, '{');
        jb_puts(&j, "\"id\":");
        jb_str(&j, bi->rejected_entries[i].id);
        jb_puts(&j, ",\"reason\":");
        jb_u32_dec(&j, bi->rejected_entries[i].reason);
        jb_puts(&j, ",\"reason_name\":");
        jb_str(&j, audit_reject_reason_name(bi->rejected_entries[i].reason));
        jb_putc(&j, '}');
    }
    jb_puts(&j, "],\"rejected_overflow\":");
    jb_u32_dec(&j, bi->rejected_entry_overflow ? 1u : 0u);

    jb_puts(&j, ",\"boot_info_version\":");
    jb_u32_dec(&j, BOOT_INFO_VERSION);
    jb_puts(&j, "}\n");

    if (jb_truncated(&j))
        return 0;
    return jb_pos(&j);
}

/* Write `len` bytes from `line` to `path` as an append. If the file
 * already exists and its size + len would exceed
 * BOOT_AUDIT_ROTATE_THRESHOLD, rename the current file to
 * `rotated_path` (overwriting any prior rotation) and start fresh.
 * Returns 1 on success, 0 on any failure. */
static int
audit_append_file(const char *path, const char *rotated_path,
                  const char *line, size_t len)
{
    if (!path || !line || len == 0) return 0;

    /* Rotate if needed. vfs_stat returns 0 on success. */
    struct vfs_stat st;
    uint32_t append_offset = 0;
    if (vfs_stat(path, &st) == 0) {
        if (st.size + (uint64_t)len > BOOT_AUDIT_ROTATE_THRESHOLD) {
            /* Rotate. Use vfs_rename_ex with REPLACE_EXISTING so a
             * pre-existing rotated file is removed atomically. Plain
             * vfs_rename leaves a stale rotated dirent in place,
             * breaking the one-generation rotation contract.
             * On any rename failure we still attempt the append at
             * offset st.size -- bounded spillover is preferable to
             * losing the record entirely. */
            int rr = vfs_rename_ex(path, rotated_path,
                                   VFS_RENAME_REPLACE_EXISTING);
            if (rr == 0) {
                append_offset = 0;
            } else {
                klog(LOG_WARN, "BOOT",
                     "audit: rotation rename failed; appending past 4 MiB");
                append_offset = (uint32_t)st.size;
            }
        } else {
            append_offset = (uint32_t)st.size;
        }
    } else {
        /* Stat failed -- file may not exist yet. Open with CREATE. */
        append_offset = 0;
    }

    struct vfs_node *f = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE);
    if (!f) {
        klog(LOG_WARN, "BOOT", "audit: cannot open %s", path);
        return 0;
    }
    int wr = vfs_write(f, append_offset, (uint32_t)len, (const uint8_t *)line);
    vfs_close(f);
    if (wr < 0) {
        klog(LOG_WARN, "BOOT", "audit: write failed");
        return 0;
    }
    /* Two-phase ack contract: a partial write is NOT a durable record.
     * vfs_write may return bytes-written on disk-full / media-error
     * paths (positive but less than len). Acking the sticky trigger
     * after a short write would clear it without persisting the JSONL
     * line, breaking the "trigger persists across crashes until
     * durable publish" guarantee. Treat short write as failure. */
    if ((uint32_t)wr != (uint32_t)len) {
        klog(LOG_WARN, "BOOT",
             "audit: short write %d/%u; skip ack",
             (uint64_t)wr, (uint64_t)len);
        return 0;
    }
    return 1;
}

/* Pure helper: classify whether each sticky trigger bit was ACTUALLY
 * consumed by the ladder this boot. A bit is consumed only when
 * selection_reason matches the corresponding ladder branch:
 *   - sticky_recovery_trigger consumed iff selection_reason == RECOVERY_REQUEST
 *   - sticky_watchdog_rollback_request consumed iff selection_reason ==
 *     WATCHDOG_ROLLBACK
 *
 * Unconsumed bits MUST persist (return 0) so the next boot retries.
 * Exposed for unit testing without touching live boot infrastructure. */
void
boot_audit_classify_ack(uint32_t selection_reason,
                        uint8_t sticky_recovery_trigger,
                        uint8_t sticky_watchdog_rollback_request,
                        int *out_recovery_consumed,
                        int *out_watchdog_consumed)
{
    if (out_recovery_consumed)
        *out_recovery_consumed = (sticky_recovery_trigger != 0) &&
                                 (selection_reason == BOOT_SELECTION_RECOVERY_REQUEST);
    if (out_watchdog_consumed)
        *out_watchdog_consumed = (sticky_watchdog_rollback_request != 0) &&
                                 (selection_reason == BOOT_SELECTION_WATCHDOG_ROLLBACK);
}

/* Ack consumed triggers post-publish. A trigger bit is "consumed" only
 * when the policy ladder actually selected based on that signal --
 * i.e. selection_reason matches BOOT_SELECTION_RECOVERY_REQUEST for
 * recovery, or BOOT_SELECTION_WATCHDOG_ROLLBACK for watchdog. If the
 * sticky carried a request but the ladder fell through to default
 * (no recovery entry viable, no watchdog target available), the
 * unconsumed bit MUST persist for the next boot to retry. Clearing
 * indiscriminately would silently lose recovery requests on
 * misconfigured systems.
 *
 * Best-effort: a uefi_var_set failure leaves the triggers pending so
 * the next boot retries. */
static void
audit_ack_consumed_triggers(const struct boot_info *bi, uint32_t boot_seq)
{
    if (!bi->sticky_present) {
        /* No record to ack against -- if a trigger was forced via
         * audit_degraded path, we don't know what state the record is
         * in. Skip. */
        return;
    }

    int recovery_consumed = 0;
    int watchdog_consumed = 0;
    boot_audit_classify_ack(bi->selection_reason,
                            bi->sticky_recovery_trigger,
                            bi->sticky_watchdog_rollback_request,
                            &recovery_consumed, &watchdog_consumed);

    if (!recovery_consumed && !watchdog_consumed) {
        /* No bit was actually consumed. Either no triggers were set,
         * or triggers were set but the ladder fell through (e.g.
         * recovery requested but no recovery entry viable). Leave
         * the sticky var untouched -- the next boot retries. */
        return;
    }

    /* Build a fresh record. Writing a complete record (CRC + magic)
     * is simpler than read-modify-write because we already have the
     * current state via the bootloader's surfaced snapshot, and the
     * bootloader is read-only so no concurrent writer can race us.
     * Each unconsumed trigger is preserved at its observed value. */
    struct boot_sticky_record rec;
    uint8_t *p = (uint8_t *)&rec;
    for (size_t i = 0; i < sizeof(rec); i++) p[i] = 0;

    rec.magic   = BOOT_STICKY_RECORD_MAGIC;
    rec.version = BOOT_STICKY_RECORD_VERSION;
    rec.size    = BOOT_STICKY_VAR_SIZE;
    rec.recovery_trigger          = recovery_consumed ? 0 :
                                        (bi->sticky_recovery_trigger ? 1 : 0);
    rec.watchdog_rollback_request = watchdog_consumed ? 0 :
                                        (bi->sticky_watchdog_rollback_request ? 1 : 0);
    rec.last_outcome              = 0;       /* unconfirmed until mark-good */
    rec.audit_degraded_last_boot  = bi->audit_degraded ? 1u : 0u;
    rec.last_event_code           = boot_audit_event_from_selection_reason(
                                        bi->selection_reason);
    rec.last_boot_seq             = boot_seq;
    rec.consumed_trigger_seq      = boot_seq;
    rec.crc32                     = boot_sticky_compute_crc(&rec);

    NTSTATUS s = uefi_var_set(s_sticky_var_name, &s_sticky_guid,
                              &rec, sizeof(rec), UEFI_VAR_NV_BOOT_RUNTIME);
    if (s != STATUS_SUCCESS) {
        klog(LOG_WARN, "BOOT",
             "audit: sticky ack write failed (status=0x%x); trigger remains",
             (uint64_t)s);
        return;
    }

    klog(LOG_INFO, "BOOT", "audit: sticky triggers acked (boot_seq=%u)",
         (uint64_t)boot_seq);
}

/* Phase-3 entry point. Compose JSONL line, write to BlackBox, ack
 * consumed triggers. Single-shot: callers should invoke once per boot
 * after BlackBox is mounted. */
void
boot_audit_publish(void)
{
    if (!klog_using_blackbox) {
        klog(LOG_WARN, "BOOT",
             "audit: BlackBox not mounted; skip JSONL publish");
        return;
    }

    const struct boot_info *bi = &g_boot_info;

    /* Read the persisted boot_seq counter from BlackBox (NOT NVRAM --
     * the sticky var is exceptional-only). Increment unconditionally
     * so every boot gets a unique seq. Saturate at UINT32_MAX so the
     * counter does not wrap to 0; a wrap would let consumers parse
     * older records as "newer" by seq comparison. */
    uint32_t prev_seq = audit_read_sequence();
    uint32_t boot_seq = (prev_seq == 0xFFFFFFFFu) ? 0xFFFFFFFFu : prev_seq + 1u;

    /* Persist the new seq BEFORE composing/writing the JSONL line.
     * If the JSONL append later fails, the next boot will read the
     * advanced seq and skip past the failed value -- skipped seqs
     * are tolerable; duplicate seqs would violate the schema's
     * monotonic contract. Sequence persistence failure is FATAL to
     * the publish: if we wrote the JSONL line with this boot_seq
     * but the counter file is empty/stale, the next boot reads the
     * old value and emits a duplicate seq for a different boot --
     * the exact monotonic violation the sequence file was added to
     * prevent. Skip the publish entirely and skip ack; the trigger
     * persists for next boot to retry. */
    if (!audit_write_sequence(boot_seq)) {
        klog(LOG_WARN, "BOOT",
             "audit: sequence persist failed; skip JSONL publish to "
             "avoid duplicate boot_seq=%u next boot",
             (uint64_t)boot_seq);
        return;
    }

    static char line[BOOT_AUDIT_JSON_LINE_CAP];
    size_t len = boot_audit_compose_line(bi, boot_seq, line, sizeof(line));
    if (len == 0) {
        klog(LOG_WARN, "BOOT",
             "audit: JSON build truncated (cap=%u); skip publish",
             (uint64_t)BOOT_AUDIT_JSON_LINE_CAP);
        return;
    }

    int ok = audit_append_file(BOOT_AUDIT_HISTORY_PATH,
                               BOOT_AUDIT_HISTORY_ROTATED,
                               line, len);
    if (!ok) {
        /* JSONL not durable -- do NOT ack consumed triggers. Next boot
         * will see the trigger again and retry the publish. */
        return;
    }

    klog(LOG_INFO, "BOOT", "audit: history.jsonl appended (%u bytes, seq=%u)",
         (uint64_t)len, (uint64_t)boot_seq);

    audit_ack_consumed_triggers(bi, boot_seq);
}
