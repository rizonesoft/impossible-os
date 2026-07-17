/* ============================================================================
 * boot_health_check.c -- per-entry health-gated mark-good gate
 *
 * Phase-3 single-shot gate. Reads bootloader-supplied CurBootCtr + (optional)
 * HealthSubset UEFI variables, runs registered health checks, writes a JSONL
 * report to X:\Boot\health.jsonl, and on a clean pass composes the MarkGood
 * UEFI variable so the next bootloader run can delete the per-entry tries
 * counter file.
 *
 * Default check set:
 *   Required:
 *     desktop_ready          -- SUBSYS_DESKTOP must be ready (proves Phase 3
 *                                completed the WM init).
 *     no_boot_err            -- klog ring must not contain LOG_ERROR or
 *                                LOG_FATAL entries from any subsystem whose
 *                                tag starts with "boot" (boot_audit,
 *                                boot_history, boot_caps, etc).
 *     no_panic               -- panic_screen halts; reaching the gate proves
 *                                no panic. Returns OK at this code point.
 *   Wanted:
 *     x_mountable            -- vfs_is_mounted('X') for BlackBox.
 *     network_reachable      -- SKIPPED today (no NIC stack); follow-up.
 *     no_service_crash_60s   -- SKIPPED today (no service supervisor);
 *                                follow-up.
 *
 * Failure model: every uefi_var_set / vfs_open / vfs_write failure is a
 * LOG_WARN no-op. The counter stays decremented; the next boot retries.
 * Never blocks userland entry.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_health_check.h"
#include "kernel/boot_status.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"
#include "kernel/util/json_builder.h"
#include "kernel/uefi_vars.h"
#include "kernel/uefi_runtime.h"
#include "kernel/time/wall_clock.h"
#include "kernel/nt/filetime.h"
#include "boot/boot_health_handoff.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int c, size_t n);
extern int   memcmp(const void *a, const void *b, size_t n);
extern size_t strlen(const char *s);
extern int klog_using_blackbox;
extern struct boot_info g_boot_info;

/* ---- Pinned constants -------------------------------------------------- */

#define BOOT_HEALTH_REPORT_PATH         "X:\\Boot\\health.jsonl"
#define BOOT_HEALTH_REPORT_ROTATED_PATH "X:\\Boot\\health.jsonl.1"
#define BOOT_HEALTH_REPORT_ROTATE_BYTES (4u * 1024u * 1024u)

#define BOOT_HEALTH_JSON_LINE_CAP 4096u

/* UEFI variable names. UCS-2 wide form for uefi_var_get / uefi_var_set;
 * ASCII mirror lives in boot_health_handoff.h. */
static const uint16_t s_mark_good_var_name[]    = u"ImpossibleOS-MarkGood";
static const uint16_t s_cur_boot_ctr_var_name[] = u"ImpossibleOS-CurBootCtr";
static const uint16_t s_subset_var_name[]       = u"ImpossibleOS-HealthSubset";
static const efi_guid_t s_vendor_guid           = IMPOSSIBLE_OS_VENDOR_GUID_INIT;

/* ---- Registry --------------------------------------------------------- */

struct registered_check {
    char                            name[BOOT_HEALTH_CHECK_NAME_LEN];
    enum boot_health_check_kind     kind;
    boot_health_check_fn            fn;
};

static struct registered_check     s_checks[BOOT_HEALTH_CHECK_MAX];
static unsigned int                s_check_count;
static int                         s_defaults_registered;
static enum boot_health_aggregate  s_cached_aggregate = BOOT_HEALTH_AGG_PENDING;
static int                         s_ran_once;

/* ---- Pure helpers (also exported for unit tests) ---------------------- */

/* String length capped at a hard limit. Returns the offset of the first
 * NUL or `limit`. Used in lieu of strlen() so a malformed input (no NUL
 * within `limit` bytes) cannot run off the end. */
static unsigned int
strn_count(const char *s, unsigned int limit)
{
    unsigned int n = 0;
    if (!s) return 0;
    while (n < limit && s[n] != '\0') n++;
    return n;
}

static int
str_eq_n(const char *a, const char *b, unsigned int n)
{
    unsigned int i;
    if (!a || !b) return 0;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

int
boot_health_check_in_subset(const char *name,
                             const char (*subset_names)[BOOT_HEALTH_CHECK_NAME_LEN],
                             unsigned int subset_count)
{
    unsigned int i;
    if (!name || name[0] == '\0') return 0;
    if (subset_count == 0u) return 1;   /* empty subset -> run everything */
    if (!subset_names) return 0;
    for (i = 0; i < subset_count; i++) {
        if (str_eq_n(name, subset_names[i], BOOT_HEALTH_CHECK_NAME_LEN))
            return 1;
    }
    return 0;
}

enum boot_health_aggregate
boot_health_check_aggregate(unsigned int req_ok, unsigned int req_soft,
                             unsigned int req_hard, unsigned int req_skipped)
{
    /* A required check that returned SKIPPED is treated as a hard-fail:
     * we cannot prove the boot is good if a required check could not run.
     * SOFT_FAIL on a required check is also a hard-fail (the boot did not
     * cleanly pass even if the failure was "soft"). */
    if (req_hard > 0u) return BOOT_HEALTH_AGG_INDETERMINATE;
    if (req_skipped > 0u) return BOOT_HEALTH_AGG_INDETERMINATE;
    if (req_soft > 0u) return BOOT_HEALTH_AGG_INDETERMINATE;
    if (req_ok == 0u) return BOOT_HEALTH_AGG_INDETERMINATE;
    return BOOT_HEALTH_AGG_PASS;
}

/* ---- Registry mechanics ------------------------------------------------ */

int
boot_health_check_register(const char *name,
                            enum boot_health_check_kind kind,
                            boot_health_check_fn fn)
{
    unsigned int nlen, i;
    if (!name || !fn) return 0;
    if (kind != BOOT_HEALTH_KIND_REQUIRED && kind != BOOT_HEALTH_KIND_WANTED)
        return 0;
    nlen = strn_count(name, BOOT_HEALTH_CHECK_NAME_LEN);
    if (nlen == 0u || nlen >= BOOT_HEALTH_CHECK_NAME_LEN) return 0;
    /* Reject duplicates. */
    for (i = 0; i < s_check_count; i++) {
        if (str_eq_n(s_checks[i].name, name, BOOT_HEALTH_CHECK_NAME_LEN))
            return 0;
    }
    if (s_check_count >= BOOT_HEALTH_CHECK_MAX) return 0;
    memset(&s_checks[s_check_count], 0, sizeof(s_checks[s_check_count]));
    memcpy(s_checks[s_check_count].name, name, nlen);
    s_checks[s_check_count].kind = kind;
    s_checks[s_check_count].fn   = fn;
    s_check_count++;
    return 1;
}

unsigned int
boot_health_check_registered_count(void)
{
    return s_check_count;
}

#ifdef KERNEL_TESTS
void
boot_health_check_test_reset(void)
{
    memset(s_checks, 0, sizeof(s_checks));
    s_check_count = 0;
    s_defaults_registered = 0;
    s_cached_aggregate = BOOT_HEALTH_AGG_PENDING;
    s_ran_once = 0;
}
#endif

/* ---- Default check implementations ------------------------------------- */

static enum boot_health_check_result
chk_desktop_ready(void)
{
    return kernel_subsystem_ready(SUBSYS_DESKTOP) ?
        BOOT_HEALTH_OK : BOOT_HEALTH_HARD_FAIL;
}

/* True iff the tag is exactly "boot" or "BOOT" (NUL-terminated at
 * index 4), or starts with "boot_" (lowercase init-subsystem
 * convention: boot_init, boot_audit, boot_history, boot_caps, etc).
 * Excludes hyphenated perf-reporter tags like "BOOT-BUDGET" and
 * "BOOT-TREND" so timing warnings do not fail the gate. */
static int boot_health_tag_is_init_subsystem(const char *tag)
{
    if (!tag) return 0;
    /* Exact "boot" or "BOOT". */
    if ((tag[0] == 'b' || tag[0] == 'B') &&
        (tag[1] == 'o' || tag[1] == 'O') &&
        (tag[2] == 'o' || tag[2] == 'O') &&
        (tag[3] == 't' || tag[3] == 'T') &&
        tag[4] == '\0')
        return 1;
    /* "boot_" lowercase prefix only -- the init-subsystem convention. */
    if (tag[0] == 'b' && tag[1] == 'o' && tag[2] == 'o' &&
        tag[3] == 't' && tag[4] == '_' && tag[5] != '\0')
        return 1;
    return 0;
}

/* Scan the klog ring for LOG_ERROR / LOG_FATAL entries from
 * init-subsystem tags. Returns HARD_FAIL on first match, OK otherwise.
 * Excludes hyphenated reporter tags via boot_health_tag_is_init_subsystem
 * so BOOT-BUDGET / BOOT-TREND perf warnings do not block the gate
 * (those are advisory, not init failures). */
static enum boot_health_check_result
chk_no_boot_err(void)
{
    uint32_t count = 0;
    uint32_t head  = 0;
    const klog_entry_t *ring = klog_get_ring(&count, &head);
    uint32_t i;
    if (!ring || count == 0u) return BOOT_HEALTH_OK;
    for (i = 0; i < count; i++) {
        const klog_entry_t *e = &ring[i];
        if (e->level != LOG_ERROR && e->level != LOG_FATAL) continue;
        if (boot_health_tag_is_init_subsystem(e->subsystem))
            return BOOT_HEALTH_HARD_FAIL;
    }
    return BOOT_HEALTH_OK;
}

/* panic_screen halts the kernel before returning; reaching this code
 * proves no panic. The check exists as an explicit registry entry so a
 * future panic-counter persistence path can swap it for a real query
 * without changing the gate semantics. */
static enum boot_health_check_result
chk_no_panic(void)
{
    return BOOT_HEALTH_OK;
}

static enum boot_health_check_result
chk_x_mountable(void)
{
    return vfs_is_mounted('X') ? BOOT_HEALTH_OK : BOOT_HEALTH_SOFT_FAIL;
}

static enum boot_health_check_result
chk_network_reachable(void)
{
    /* No NIC stack at Phase 3 today; network reachability is not
     * something we can answer. Report SKIPPED so the aggregator records
     * the gap rather than silently treating it as OK. */
    return BOOT_HEALTH_SKIPPED;
}

static enum boot_health_check_result
chk_no_service_crash_60s(void)
{
    /* No service supervisor today; nothing to crash. SKIPPED until a
     * supervisor lands. */
    return BOOT_HEALTH_SKIPPED;
}

void
boot_health_check_register_defaults(void)
{
    if (s_defaults_registered) return;
    s_defaults_registered = 1;
    (void)boot_health_check_register("desktop_ready",
        BOOT_HEALTH_KIND_REQUIRED, chk_desktop_ready);
    (void)boot_health_check_register("no_boot_err",
        BOOT_HEALTH_KIND_REQUIRED, chk_no_boot_err);
    (void)boot_health_check_register("no_panic",
        BOOT_HEALTH_KIND_REQUIRED, chk_no_panic);
    (void)boot_health_check_register("x_mountable",
        BOOT_HEALTH_KIND_WANTED, chk_x_mountable);
    (void)boot_health_check_register("network_reachable",
        BOOT_HEALTH_KIND_WANTED, chk_network_reachable);
    (void)boot_health_check_register("no_service_crash_60s",
        BOOT_HEALTH_KIND_WANTED, chk_no_service_crash_60s);
}

/* ---- CurBootCtr + HealthSubset read ------------------------------------ */

/* Canonical attrs for the per-boot handoff vars. BS+RT means
 * volatile across power-cycles -- the bootloader rewrites them every
 * boot, so a stale record from a prior boot survives only on
 * non-spec-compliant firmware. Enforced on every read so a pre-OS
 * tool cannot preseed a CRC-valid record with NV attrs and steer
 * the gate. */
#define BOOT_HEALTH_VOLATILE_ATTRS \
    (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

/* Read the CurBootCtr handoff written by the bootloader post-decrement.
 * Returns 1 on success, 0 on missing / wrong attrs / wrong size /
 * invalid record (caller must treat as "no binding available"). */
static int
read_cur_boot_ctr(struct boot_health_cur_boot_ctr_record *out)
{
    size_t sz = sizeof(*out);
    uint32_t attrs = 0;
    NTSTATUS s;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    s = uefi_var_get(s_cur_boot_ctr_var_name, &s_vendor_guid,
                     out, &sz, &attrs);
    if (s != STATUS_SUCCESS) return 0;
    if (sz != sizeof(*out)) return 0;
    if (attrs != BOOT_HEALTH_VOLATILE_ATTRS) {
        klog(LOG_WARN, "BOOT",
             "health: CurBootCtr attrs mismatch (got 0x%x, want 0x%x) -- ignoring",
             (uint64_t)attrs,
             (uint64_t)BOOT_HEALTH_VOLATILE_ATTRS);
        return 0;
    }
    if (!boot_health_cur_boot_ctr_is_valid(out)) return 0;
    return 1;
}

/* Read the optional HealthSubset record. Returns 1 if a valid subset
 * was found, 0 if absent / wrong attrs / invalid (treat as empty
 * subset = run all checks). Same BS+RT attr binding as CurBootCtr. */
static int
read_health_subset(struct boot_health_subset_record *out)
{
    size_t sz = sizeof(*out);
    uint32_t attrs = 0;
    NTSTATUS s;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    s = uefi_var_get(s_subset_var_name, &s_vendor_guid,
                     out, &sz, &attrs);
    if (s != STATUS_SUCCESS) return 0;
    if (sz != sizeof(*out)) return 0;
    if (attrs != BOOT_HEALTH_VOLATILE_ATTRS) {
        klog(LOG_WARN, "BOOT",
             "health: HealthSubset attrs mismatch (got 0x%x) -- ignoring",
             (uint64_t)attrs);
        return 0;
    }
    if (!boot_health_subset_is_valid(out)) return 0;
    return 1;
}

/* ---- mark_entry_successful --------------------------------------------- */

/* Internal helper: compose + write the MarkGood record from an already-
 * validated CurBootCtr record. Avoids a redundant firmware GetVariable
 * + CRC pass on the gate's PASS path, where boot_health_check_run has
 * already read and validated the same record. */
static NTSTATUS
mark_entry_successful_from_ctr(const struct boot_health_cur_boot_ctr_record *ctr)
{
    struct boot_health_mark_good_record rec;
    NTSTATUS s;

    if (!ctr) return STATUS_INVALID_PARAMETER;

    memset(&rec, 0, sizeof(rec));
    rec.magic   = BOOT_HEALTH_MARK_GOOD_MAGIC;
    rec.version = BOOT_HEALTH_VAR_VERSION;
    memcpy(rec.entry_id, ctr->entry_id, BOOT_HEALTH_HANDOFF_ID_LEN);
    rec.tries_left = ctr->tries_left;
    rec.tries_done = ctr->tries_done;
    rec.reserved   = 0u;
    rec.crc32      = boot_health_handoff_compute_crc(&rec,
                        (unsigned int)sizeof(rec));

    s = uefi_var_set(s_mark_good_var_name, &s_vendor_guid,
                     &rec, sizeof(rec), UEFI_VAR_NV_BOOT_RUNTIME);
    if (s != STATUS_SUCCESS) {
        klog(LOG_WARN, "BOOT",
             "health: MarkGood SetVariable failed (status=0x%x)",
             (uint64_t)s);
        return s;
    }

    klog(LOG_INFO, "BOOT",
         "health: entry marked good (id=%s tries_left=%u tries_done=%u)",
         (uint64_t)rec.entry_id,
         (uint64_t)rec.tries_left,
         (uint64_t)rec.tries_done);
    /* Slot-level mark_boot_successful is the A/B-rollback mark-good
     * hook; that feature is unshipped today. When it lands, the
     * kernel-side call will fire here after the per-entry mark
     * succeeds. */
    return STATUS_SUCCESS;
}

NTSTATUS
mark_entry_successful(const char *entry_id)
{
    struct boot_health_cur_boot_ctr_record ctr;
    unsigned int idlen;

    if (!entry_id || entry_id[0] == '\0')
        return STATUS_INVALID_PARAMETER;
    idlen = strn_count(entry_id, BOOT_HEALTH_HANDOFF_ID_LEN);
    if (idlen == 0u || idlen >= BOOT_HEALTH_HANDOFF_ID_LEN)
        return STATUS_INVALID_PARAMETER;

    if (!read_cur_boot_ctr(&ctr)) {
        /* No binding available. Without CurBootCtr we cannot state-bind
         * the mark-good record -- stale replay defense requires the
         * {entry_id, tries_left, tries_done} triple. */
        klog(LOG_WARN, "BOOT",
             "health: CurBootCtr absent/invalid -- skipping mark_entry_successful");
        return STATUS_NOT_FOUND;
    }

    /* Caller must agree with bootloader on which entry is being marked.
     * Mismatch = producer bug (kernel saw a different selected_entry_id
     * than the bootloader wrote into CurBootCtr). Refuse so the mismatch
     * surfaces loudly. */
    if (!str_eq_n(ctr.entry_id, entry_id, BOOT_HEALTH_HANDOFF_ID_LEN)) {
        klog(LOG_WARN, "BOOT",
             "health: mark_entry_successful id mismatch -- CurBootCtr says different entry");
        return STATUS_INVALID_PARAMETER;
    }

    return mark_entry_successful_from_ctr(&ctr);
}

/* ---- JSONL report ------------------------------------------------------ */

static const char *
result_name(enum boot_health_check_result r)
{
    switch (r) {
        case BOOT_HEALTH_OK:        return "ok";
        case BOOT_HEALTH_SOFT_FAIL: return "soft_fail";
        case BOOT_HEALTH_HARD_FAIL: return "hard_fail";
        case BOOT_HEALTH_SKIPPED:   return "skipped";
        default:                    return "unknown";
    }
}

static const char *
aggregate_name(enum boot_health_aggregate a)
{
    switch (a) {
        case BOOT_HEALTH_AGG_PASS:          return "pass";
        case BOOT_HEALTH_AGG_INDETERMINATE: return "indeterminate";
        case BOOT_HEALTH_AGG_PENDING:       return "pending";
        default:                            return "unknown";
    }
}

static int
append_health_record(const char *line, size_t len)
{
    struct vfs_stat st;
    uint32_t append_offset = 0;
    struct vfs_node *f;
    int wr;

    if (!line || len == 0u) return 0;
    if (!klog_using_blackbox) {
        klog(LOG_WARN, "BOOT",
             "health: BlackBox not mounted -- skipping report write");
        return 0;
    }

    if (vfs_stat(BOOT_HEALTH_REPORT_PATH, &st) == 0) {
        if (st.size + (uint64_t)len > BOOT_HEALTH_REPORT_ROTATE_BYTES) {
            int rr = vfs_rename_ex(BOOT_HEALTH_REPORT_PATH,
                                    BOOT_HEALTH_REPORT_ROTATED_PATH,
                                    VFS_RENAME_REPLACE_EXISTING);
            if (rr == 0) {
                append_offset = 0;
            } else {
                klog(LOG_WARN, "BOOT",
                     "health: rotation rename failed; appending past 4 MiB");
                append_offset = (uint32_t)st.size;
            }
        } else {
            append_offset = (uint32_t)st.size;
        }
    }

    f = vfs_open(BOOT_HEALTH_REPORT_PATH, VFS_O_WRITE | VFS_O_CREATE);
    if (!f) {
        klog(LOG_WARN, "BOOT", "health: cannot open report path");
        return 0;
    }
    wr = vfs_write(f, append_offset, (uint32_t)len, (const uint8_t *)line);
    vfs_close(f);
    if (wr < 0 || (uint32_t)wr != (uint32_t)len) {
        klog(LOG_WARN, "BOOT",
             "health: report short write %d/%u",
             (uint64_t)wr, (uint64_t)len);
        return 0;
    }
    return 1;
}

/* ---- The gate ---------------------------------------------------------- */

struct check_outcome {
    unsigned int                    idx;
    enum boot_health_check_result   result;
    int                             ran;
};

enum boot_health_aggregate
boot_health_check_run(void)
{
    struct boot_health_cur_boot_ctr_record ctr;
    struct boot_health_subset_record       subset;
    int have_ctr;
    int have_subset;
    struct check_outcome                   outcomes[BOOT_HEALTH_CHECK_MAX];
    unsigned int                           req_ok = 0, req_soft = 0;
    unsigned int                           req_hard = 0, req_skipped = 0;
    unsigned int                           want_ok = 0, want_soft = 0;
    unsigned int                           want_hard = 0, want_skipped = 0;
    unsigned int                           i;
    char                                   linebuf[BOOT_HEALTH_JSON_LINE_CAP];
    struct json_builder                    j;
    enum boot_health_aggregate             agg;

    /* Exactly-once latch (SMP-safe): the first caller wins and runs the gate;
     * concurrent losers wait for the published aggregate rather than re-running
     * the checks and re-emitting the success side effects. s_ran_once states:
     * 0 = idle, 1 = running, 2 = aggregate published. */
    int prev = 0;
    if (!__atomic_compare_exchange_n(&s_ran_once, &prev, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&s_ran_once, __ATOMIC_ACQUIRE) != 2)
            ;   /* await the winner's publish (no concurrent caller today) */
        return __atomic_load_n(&s_cached_aggregate, __ATOMIC_ACQUIRE);
    }

    if (s_check_count == 0u) {
        klog(LOG_WARN, "BOOT",
             "health: no checks registered -- aggregate=indeterminate");
        s_cached_aggregate = BOOT_HEALTH_AGG_INDETERMINATE;
        __atomic_store_n(&s_ran_once, 2, __ATOMIC_RELEASE);
        return s_cached_aggregate;
    }

    have_ctr    = read_cur_boot_ctr(&ctr);
    have_subset = read_health_subset(&subset);

    /* Run the registered checks. Apply the subset filter if present. */
    memset(outcomes, 0, sizeof(outcomes));
    for (i = 0; i < s_check_count; i++) {
        outcomes[i].idx = i;
        if (have_subset && !boot_health_check_in_subset(
                s_checks[i].name, subset.names, subset.count)) {
            /* Filtered out by per-entry subset; do not run. */
            outcomes[i].ran    = 0;
            outcomes[i].result = BOOT_HEALTH_SKIPPED;
            continue;
        }
        outcomes[i].ran = 1;
        outcomes[i].result = s_checks[i].fn ?
            s_checks[i].fn() : BOOT_HEALTH_HARD_FAIL;
    }

    /* Tally. Required checks that were filtered-out by the subset do
     * NOT count toward the required tally (the entry explicitly opted
     * out of them). Required checks that ran contribute to the
     * aggregate. */
    for (i = 0; i < s_check_count; i++) {
        if (!outcomes[i].ran) continue;
        if (s_checks[i].kind == BOOT_HEALTH_KIND_REQUIRED) {
            switch (outcomes[i].result) {
                case BOOT_HEALTH_OK:        req_ok++; break;
                case BOOT_HEALTH_SOFT_FAIL: req_soft++; break;
                case BOOT_HEALTH_HARD_FAIL: req_hard++; break;
                case BOOT_HEALTH_SKIPPED:   req_skipped++; break;
            }
        } else {
            switch (outcomes[i].result) {
                case BOOT_HEALTH_OK:        want_ok++; break;
                case BOOT_HEALTH_SOFT_FAIL: want_soft++; break;
                case BOOT_HEALTH_HARD_FAIL: want_hard++; break;
                case BOOT_HEALTH_SKIPPED:   want_skipped++; break;
            }
        }
    }

    agg = boot_health_check_aggregate(req_ok, req_soft, req_hard, req_skipped);
    s_cached_aggregate = agg;

    /* Build JSONL record. */
    jb_init(&j, linebuf, sizeof(linebuf));
    jb_puts(&j, "{\"schema_version\":1");
    jb_puts(&j, ",\"event\":\"health_gate\"");
    jb_puts(&j, ",\"aggregate\":\"");
    jb_puts(&j, aggregate_name(agg));
    jb_puts(&j, "\"");
    jb_puts(&j, ",\"selected_entry_id\":");
    jb_str(&j, g_boot_info.selected_entry_id);
    jb_puts(&j, ",\"cur_boot_ctr_present\":");
    jb_puts(&j, have_ctr ? "true" : "false");
    if (have_ctr) {
        jb_puts(&j, ",\"tries_left\":");
        jb_u32_dec(&j, ctr.tries_left);
        jb_puts(&j, ",\"tries_done\":");
        jb_u32_dec(&j, ctr.tries_done);
    }
    jb_puts(&j, ",\"subset_present\":");
    jb_puts(&j, have_subset ? "true" : "false");
    jb_puts(&j, ",\"checks\":[");
    for (i = 0; i < s_check_count; i++) {
        if (i > 0u) jb_putc(&j, ',');
        jb_puts(&j, "{\"name\":");
        jb_str(&j, s_checks[i].name);
        jb_puts(&j, ",\"kind\":\"");
        jb_puts(&j, s_checks[i].kind == BOOT_HEALTH_KIND_REQUIRED ?
                "required" : "wanted");
        jb_puts(&j, "\",\"result\":\"");
        jb_puts(&j, result_name(outcomes[i].result));
        jb_puts(&j, "\",\"ran\":");
        jb_puts(&j, outcomes[i].ran ? "true" : "false");
        jb_puts(&j, "}");
    }
    jb_puts(&j, "],\"counts\":{");
    jb_puts(&j, "\"required_ok\":");      jb_u32_dec(&j, req_ok);
    jb_puts(&j, ",\"required_soft\":");   jb_u32_dec(&j, req_soft);
    jb_puts(&j, ",\"required_hard\":");   jb_u32_dec(&j, req_hard);
    jb_puts(&j, ",\"required_skipped\":");jb_u32_dec(&j, req_skipped);
    jb_puts(&j, ",\"wanted_ok\":");       jb_u32_dec(&j, want_ok);
    jb_puts(&j, ",\"wanted_soft\":");     jb_u32_dec(&j, want_soft);
    jb_puts(&j, ",\"wanted_hard\":");     jb_u32_dec(&j, want_hard);
    jb_puts(&j, ",\"wanted_skipped\":");  jb_u32_dec(&j, want_skipped);
    jb_puts(&j, "}}\n");

    if (jb_truncated(&j)) {
        klog(LOG_WARN, "BOOT",
             "health: JSON line truncated -- skipping report write");
    } else {
        (void)append_health_record(linebuf, jb_pos(&j));
    }

    klog(LOG_INFO, "BOOT",
         "health: gate %s (required ok=%u soft=%u hard=%u skipped=%u; wanted ok=%u soft=%u hard=%u skipped=%u)",
         (uint64_t)aggregate_name(agg),
         (uint64_t)req_ok, (uint64_t)req_soft,
         (uint64_t)req_hard, (uint64_t)req_skipped,
         (uint64_t)want_ok, (uint64_t)want_soft,
         (uint64_t)want_hard, (uint64_t)want_skipped);

    /* Route the health verdict through the boot-status ledger instead of
     * marking the boot good here. The single accepted transition owns the
     * per-entry MarkGood (plus the A/B mark + durable record), exactly once,
     * so the gate and the compositor no longer bless from two independent
     * authorities. The verdict alone is recorded; the actual MarkGood (which
     * re-reads + validates CurBootCtr, handling an absent counter itself) fires
     * when the boot reaches its acceptance stage. */
    if (agg == BOOT_HEALTH_AGG_PASS)
        boot_status_note_health_pass();

    __atomic_store_n(&s_ran_once, 2, __ATOMIC_RELEASE);   /* aggregate published */
    return agg;
}

enum boot_health_aggregate
boot_health_check_last_aggregate(void)
{
    return s_cached_aggregate;
}
