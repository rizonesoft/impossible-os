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
#include "kernel/boot_audit.h"

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

/* Classify one sequence file. See boot_audit.h for the three states. */
enum boot_audit_seq_state
boot_audit_seq_classify(const uint8_t *buf, int n, uint32_t *value)
{
    uint32_t v = 0;
    if (value) *value = 0;
    if (n == BOOT_AUDIT_SEQ_READ_ABSENT || n == 0) return BOOT_AUDIT_SEQ_STALE;
    if (!buf || n < 0 || (uint32_t)n > BOOT_AUDIT_SEQUENCE_LEN_MAX) return BOOT_AUDIT_SEQ_UNKNOWN;

    int i = 0;
    for (; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) {
        uint32_t digit = (uint32_t)(buf[i] - '0');
        /* Two-limb overflow check: 4294967296 has v == UINT32_MAX/10 at its
         * final digit, so test the digit on the boundary too. */
        if (v > 0xFFFFFFFFu / 10u ||
            (v == 0xFFFFFFFFu / 10u && digit > 0xFFFFFFFFu % 10u))
            return BOOT_AUDIT_SEQ_UNKNOWN;
        v = v * 10u + digit;
    }
    if (i == n) return BOOT_AUDIT_SEQ_STALE;   /* digits, no terminator: a short write */
    if (i == 0) return BOOT_AUDIT_SEQ_UNKNOWN; /* starts with a non-digit */
    int rest = n - i;
    int terminated = (rest == 1 && buf[i] == '\n') ||
                     (rest == 2 && buf[i] == '\r' && buf[i + 1] == '\n');
    if (!terminated) return BOOT_AUDIT_SEQ_UNKNOWN;
    if (value) *value = v;
    return BOOT_AUDIT_SEQ_VALID;
}

uint32_t
boot_audit_seq_format(uint32_t seq, char *buf, uint32_t cap)
{
    char tmp[10];
    uint32_t t = 0;
    do { tmp[t++] = (char)('0' + (seq % 10u)); seq /= 10u; } while (seq);
    if (!buf || cap < t + 1u) return 0;
    uint32_t n = 0;
    while (t > 0) buf[n++] = tmp[--t];
    buf[n++] = '\n';
    return n;
}

static enum boot_audit_seq_state
audit_seq_read_one(const struct boot_audit_seq_io *io, const char *path, uint32_t *value)
{
    uint8_t buf[BOOT_AUDIT_SEQUENCE_LEN_MAX];
    int n = io->read(io->ctx, path, buf, (uint32_t)sizeof(buf));
    return boot_audit_seq_classify(buf, n, value);
}

/* Why two files instead of an atomic rename: the FAT32 driver does not
 * rewrite LFN chains on rename (only the SFN field), so a rename would leave
 * the old long name. The ORDER of the two writes is what makes them safe: an
 * open with VFS_O_TRUNC empties a file before the write can fail, so writing
 * the file that holds the high-water mark first could destroy the only copy
 * of the last published value, and two failed writes would then restart the
 * counter at 1. The low file is written first and the high file only after
 * that succeeded, so a durable copy of the high-water mark always survives. */
enum boot_audit_seq_result
boot_audit_sequence_advance(const struct boot_audit_seq_io *io, uint32_t *out_seq)
{
    if (!io || !io->read || !io->write || !out_seq) return BOOT_AUDIT_SEQ_FAILED;

    uint32_t canon = 0, shadow = 0;
    enum boot_audit_seq_state cs = audit_seq_read_one(io, BOOT_AUDIT_SEQUENCE_PATH, &canon);
    enum boot_audit_seq_state ss = audit_seq_read_one(io, BOOT_AUDIT_SEQUENCE_TEMP_PATH, &shadow);
    if (cs == BOOT_AUDIT_SEQ_UNKNOWN || ss == BOOT_AUDIT_SEQ_UNKNOWN)
        return BOOT_AUDIT_SEQ_UNREADABLE;
    uint32_t prev = (canon > shadow) ? canon : shadow;
    if (prev == 0xFFFFFFFFu)
        return BOOT_AUDIT_SEQ_EXHAUSTED;
    uint32_t seq = prev + 1u;

    char buf[BOOT_AUDIT_SEQUENCE_LEN_MAX];
    uint32_t n = boot_audit_seq_format(seq, buf, (uint32_t)sizeof(buf));
    if (n == 0) return BOOT_AUDIT_SEQ_FAILED;

    /* Low file first; on a tie either copy may go first, the other survives. */
    const char *first = (canon > shadow) ? BOOT_AUDIT_SEQUENCE_TEMP_PATH : BOOT_AUDIT_SEQUENCE_PATH;
    const char *second = (canon > shadow) ? BOOT_AUDIT_SEQUENCE_PATH : BOOT_AUDIT_SEQUENCE_TEMP_PATH;

    if (io->write(io->ctx, first, buf, n) != 1)
        return BOOT_AUDIT_SEQ_FAILED;
    *out_seq = seq;
    if (io->write(io->ctx, second, buf, n) != 1)
        return BOOT_AUDIT_SEQ_PARTIAL;
    return BOOT_AUDIT_SEQ_OK;
}

static int
audit_vfs_read(void *ctx, const char *path, uint8_t *buf, uint32_t cap)
{
    (void)ctx;
    struct vfs_node *f = vfs_open(path, VFS_O_READ);
    if (!f) return BOOT_AUDIT_SEQ_READ_ABSENT;
    uint64_t size = f->size;
    int n = (size <= cap) ? vfs_read(f, 0, (uint32_t)size, buf) : BOOT_AUDIT_SEQ_READ_ERROR;
    vfs_close(f);
    /* A read that returns fewer bytes than the file holds is a device error,
     * not an empty file: FAT32 returns 0 when the cluster read fails. */
    if (n < 0 || (uint64_t)n != size) return BOOT_AUDIT_SEQ_READ_ERROR;
    return n;
}

static int
audit_vfs_write(void *ctx, const char *path, const char *buf, uint32_t n)
{
    (void)ctx;
    struct vfs_node *f = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!f) return 0;
    int wr = vfs_write(f, 0, n, (const uint8_t *)buf);
    /* Durable means on the medium: the protocol truncates the other copy next,
     * so the cached FAT and directory sectors and the device cache must be
     * written out first. vfs_flush is that boundary on FAT32. */
    int flushed = (wr == (int)n) && vfs_flush(f) == 0;
    vfs_close(f);
    return flushed ? 1 : 0;
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

    jb_puts(&j, ",\"loader_vars_degraded\":");
    jb_u32_dec(&j, bi->loader_vars_degraded ? 1u : 0u);

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

/* The recovery trigger as THIS boot last wrote it: -1 untouched, 0 spent, 1
 * armed. Phase 1 writes it (single CPU, before smp_init); the Phase-3 ack
 * reads it. The atomics order that handoff without depending on the phases. */
static int s_trigger_written = -1;

void boot_sticky_ack_apply(struct boot_sticky_record *rec, int recovery_consumed,
                           int watchdog_consumed, int trigger_written)
{
    if (!rec)
        return;
    /* A trigger this boot armed is a NEW request, not the one the ladder
     * consumed, so a consumed-by-selection ack must not clear it; a trigger
     * this boot spent stays spent whatever the snapshot said. */
    if (trigger_written >= 0)
        rec->recovery_trigger = (unsigned char)(trigger_written ? 1 : 0);
    if (recovery_consumed && trigger_written != 1)
        rec->recovery_trigger = 0;
    if (watchdog_consumed)
        rec->watchdog_rollback_request = 0;
}

int boot_sticky_ack_build(int32_t read_status, size_t read_size,
                          const struct boot_sticky_record *cur,
                          int recovery_consumed, int watchdog_consumed,
                          int trigger_written, struct boot_sticky_record *out)
{
    if (boot_sticky_rmw_prepare(read_status, read_size, cur, out) != 0)
        return -1;
    boot_sticky_ack_apply(out, recovery_consumed, watchdog_consumed, trigger_written);
    return 0;
}

int boot_sticky_rmw_prepare(int32_t read_status, size_t read_size,
                            const struct boot_sticky_record *cur,
                            struct boot_sticky_record *out)
{
    if (!out)
        return -1;
    if (read_status == (int32_t)STATUS_NOT_FOUND) {
        uint8_t *p = (uint8_t *)out;
        for (size_t i = 0; i < sizeof(*out); i++) p[i] = 0;
        out->magic   = BOOT_STICKY_RECORD_MAGIC;
        out->version = BOOT_STICKY_RECORD_VERSION;
        out->size    = BOOT_STICKY_VAR_SIZE;
        return 0;
    }
    if (read_status != (int32_t)STATUS_SUCCESS || !cur ||
        read_size != sizeof(*cur) || !boot_sticky_record_is_valid(cur))
        return -1;
    *out = *cur;
    return 0;
}

/* Read the live record and prepare it for modification. */
static int sticky_read_prepared(struct boot_sticky_record *out)
{
    struct boot_sticky_record cur;
    size_t sz = sizeof(cur);
    NTSTATUS st = uefi_var_get(s_sticky_var_name, &s_sticky_guid, &cur, &sz,
                               (uint32_t *)0);
    return boot_sticky_rmw_prepare((int32_t)st, sz, &cur, out);
}

static int sticky_write(struct boot_sticky_record *rec)
{
    rec->crc32 = boot_sticky_compute_crc(rec);
    return uefi_var_set(s_sticky_var_name, &s_sticky_guid, rec, sizeof(*rec),
                        UEFI_VAR_NV_BOOT_RUNTIME) == STATUS_SUCCESS ? 0 : -1;
}

int boot_sticky_set_field(enum boot_sticky_field field, uint8_t value)
{
    struct boot_sticky_record rec;
    uint8_t v = value ? 1u : 0u;

    if (sticky_read_prepared(&rec) != 0) {
        klog(LOG_WARN, "BOOT", "sticky: record unreadable; field %u not written",
             (uint64_t)field);
        return -1;
    }
    switch (field) {
    case BOOT_STICKY_FIELD_RECOVERY_TRIGGER: rec.recovery_trigger = v; break;
    case BOOT_STICKY_FIELD_OFFER_DISMISSED:  rec.enroll_offer_dismissed = v; break;
    default: return -1;
    }
    if (sticky_write(&rec) != 0) {
        klog(LOG_WARN, "BOOT", "sticky: write of field %u failed", (uint64_t)field);
        return -1;
    }
    if (field == BOOT_STICKY_FIELD_RECOVERY_TRIGGER)
        __atomic_store_n(&s_trigger_written, (int)v, __ATOMIC_RELEASE);
    return 0;
}

int boot_sticky_offer_dismissed(void)
{
    struct boot_sticky_record rec;

    if (sticky_read_prepared(&rec) != 0)
        return -1;
    return rec.enroll_offer_dismissed ? 1 : 0;
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

    /* Read-modify-write the LIVE record: Phase 1 may already have armed or
     * spent the recovery trigger and dismissed the enrollment offer, and a
     * record rebuilt from the loader's snapshot would undo both. Only when
     * the live record cannot be read is it rebuilt from that snapshot, and
     * then this boot's own trigger write still wins. */
    struct boot_sticky_record cur, rec;
    size_t cur_sz = sizeof(cur);
    NTSTATUS rs = uefi_var_get(s_sticky_var_name, &s_sticky_guid, &cur, &cur_sz,
                               (uint32_t *)0);
    if (boot_sticky_ack_build((int32_t)rs, cur_sz, &cur, recovery_consumed,
                              watchdog_consumed,
                              __atomic_load_n(&s_trigger_written, __ATOMIC_ACQUIRE),
                              &rec) != 0) {
        klog(LOG_WARN, "BOOT", "audit: sticky record unreadable; ack skipped, trigger remains");
        return;
    }
    rec.last_outcome              = 0;       /* unconfirmed until mark-good */
    rec.audit_degraded_last_boot  = bi->audit_degraded ? 1u : 0u;
    rec.last_event_code           = boot_audit_event_from_selection_reason(
                                        bi->selection_reason);
    rec.last_boot_seq             = boot_seq;
    rec.consumed_trigger_seq      = boot_seq;

    if (sticky_write(&rec) != 0) {
        klog(LOG_WARN, "BOOT", "audit: sticky ack write failed; trigger remains");
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

    /* Allocate and persist boot_seq BEFORE composing/writing the JSONL line
     * (the counter lives on BlackBox, NOT NVRAM: the sticky var is
     * exceptional-only). If the JSONL append later fails, the next boot skips
     * past this value: skipped seqs are tolerable, duplicate seqs violate the
     * schema's monotonic contract. When the counter is not durable or is
     * exhausted, skip the publish and the ack; the trigger persists for the
     * next boot to retry. */
    static const struct boot_audit_seq_io vfs_io = {
        audit_vfs_read, audit_vfs_write, (void *)0
    };
    uint32_t boot_seq = 0;
    enum boot_audit_seq_result sr = boot_audit_sequence_advance(&vfs_io, &boot_seq);
    if (sr == BOOT_AUDIT_SEQ_EXHAUSTED) {
        klog(LOG_WARN, "BOOT",
             "audit: boot_seq counter exhausted at 4294967295; skip JSONL publish");
        return;
    }
    if (sr == BOOT_AUDIT_SEQ_UNREADABLE) {
        klog(LOG_WARN, "BOOT",
             "audit: a boot_seq file is unreadable or damaged (%s, %s); "
             "skip JSONL publish until it is repaired",
             (uint64_t)(uintptr_t)BOOT_AUDIT_SEQUENCE_PATH,
             (uint64_t)(uintptr_t)BOOT_AUDIT_SEQUENCE_TEMP_PATH);
        return;
    }
    if (sr == BOOT_AUDIT_SEQ_FAILED) {
        klog(LOG_WARN, "BOOT",
             "audit: sequence persist failed (high-water file left untouched); "
             "skip JSONL publish");
        return;
    }
    if (sr == BOOT_AUDIT_SEQ_PARTIAL)
        klog(LOG_WARN, "BOOT",
             "audit: sequence partial persist; next boot recovers via max-of-both");

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
