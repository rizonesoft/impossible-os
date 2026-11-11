/* ============================================================================
 * test_boot_audit.c -- unit tests for the boot policy audit feature
 *
 * PURE-HELPER TESTS ONLY. Per CLAUDE.md "Test Code Policy", these tests
 * must NEVER call uefi_var_set/get, RuntimeServices, vfs_*, klog (live
 * subsystems), boot_audit_publish, or any other live boot infrastructure.
 *
 * Allowed surface:
 *   - boot_sticky_record struct layout (compile-time + runtime asserts)
 *   - boot_sticky_compute_crc()           pure function
 *   - boot_sticky_record_is_valid()       pure function
 *   - boot_audit_event_from_selection_reason() pure function
 *   - boot_audit_event_name()             pure function
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/test/test.h"
#include "kernel/test/scratch.h"  /* TEST_SCRATCH_KBUF */

/* The compose tests' line buffer: the worst-case audit line is ~8.4 KiB, so
 * 12 KiB keeps a >1.4x margin. These fixtures and their 28 KiB boot_info
 * come from the test scratch allocator, not .bss: the kernel image is at
 * its user-base page budget (higher-half kernel relocation TODO). */
#define AUDIT_TEST_LINE_CAP (12u * 1024u)
#include "kernel/boot_info.h"
#include "boot/boot_audit_codes.h"
#include "boot/boot_policy.h"
#include "kernel/boot_audit.h"
#include "kernel/nt/ntstatus.h"

extern size_t boot_audit_compose_line(const struct boot_info *bi,
                                      uint32_t boot_seq,
                                      char *buf, size_t cap);

extern void boot_audit_classify_ack(uint32_t selection_reason,
                                    uint8_t sticky_recovery_trigger,
                                    uint8_t sticky_watchdog_rollback_request,
                                    int *out_recovery_consumed,
                                    int *out_watchdog_consumed);

/* ---- Schema asserts (the static_asserts in the header are duplicated
 *      here at runtime so a regression in the on-disk wire format is
 *      caught even when the header itself is mistakenly relaxed). */

static void test_sticky_record_size(void)
{
    TEST_ASSERT_EQ(sizeof(struct boot_sticky_record), 256,
        "boot_sticky_record must be exactly 256 bytes");
    TEST_ASSERT_EQ((uint32_t)BOOT_STICKY_VAR_SIZE, 256u,
        "BOOT_STICKY_VAR_SIZE must equal 256");
}

static void test_sticky_record_offsets(void)
{
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_sticky_record, magic), 0,
        "magic must live at offset 0");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_sticky_record, recovery_trigger), 8,
        "recovery_trigger must live at offset 8");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_sticky_record, last_event_code), 12,
        "last_event_code must live at offset 12");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_sticky_record, crc32), 252,
        "crc32 must live at offset 252 -- changes here break NVRAM compat");
}

/* ---- CRC + validity round-trip ---- */

static void zero_record(struct boot_sticky_record *r)
{
    uint8_t *p = (uint8_t *)r;
    for (size_t i = 0; i < sizeof(*r); i++) p[i] = 0;
}

static void seed_record_v1(struct boot_sticky_record *r)
{
    zero_record(r);
    r->magic   = BOOT_STICKY_RECORD_MAGIC;
    r->version = BOOT_STICKY_RECORD_VERSION;
    r->size    = BOOT_STICKY_VAR_SIZE;
    r->last_boot_seq = 42;
    r->last_event_code = BOOT_AUDIT_EVENT_NORMAL;
    r->crc32 = boot_sticky_compute_crc(r);
}

static void test_sticky_crc_roundtrip_valid(void)
{
    struct boot_sticky_record r;
    seed_record_v1(&r);
    TEST_ASSERT_EQ((uint32_t)boot_sticky_record_is_valid(&r), 1u,
        "freshly-stamped record must validate");
}

static void test_sticky_crc_detects_byte_flip(void)
{
    struct boot_sticky_record r;
    seed_record_v1(&r);
    /* Flip one bit in a payload field; CRC must reject. */
    r.recovery_trigger ^= 1;
    TEST_ASSERT_EQ((uint32_t)boot_sticky_record_is_valid(&r), 0u,
        "byte flip must invalidate the CRC");
}

static void test_sticky_rejects_bad_magic(void)
{
    struct boot_sticky_record r;
    seed_record_v1(&r);
    r.magic = 0xDEADBEEFu;
    /* Magic is part of the CRC payload, so the CRC is also wrong, but
     * the validator short-circuits on magic before checking CRC -- this
     * test pins that early-out. */
    TEST_ASSERT_EQ((uint32_t)boot_sticky_record_is_valid(&r), 0u,
        "wrong magic must reject before CRC check");
}

static void test_sticky_rejects_wrong_version(void)
{
    struct boot_sticky_record r;
    seed_record_v1(&r);
    r.version = 99;
    r.crc32 = boot_sticky_compute_crc(&r);
    TEST_ASSERT_EQ((uint32_t)boot_sticky_record_is_valid(&r), 0u,
        "future version must reject (not silently treat as v1)");
}

static void test_sticky_rejects_wrong_size(void)
{
    struct boot_sticky_record r;
    seed_record_v1(&r);
    r.size = 128;
    r.crc32 = boot_sticky_compute_crc(&r);
    TEST_ASSERT_EQ((uint32_t)boot_sticky_record_is_valid(&r), 0u,
        "wrong size must reject");
}

static void test_sticky_zeroed_record_invalid(void)
{
    struct boot_sticky_record r;
    zero_record(&r);
    TEST_ASSERT_EQ((uint32_t)boot_sticky_record_is_valid(&r), 0u,
        "all-zero record (UEFI_NOT_FOUND fill pattern) must reject");
}

/* ---- selection_reason -> audit event mapping ---- */

static void test_audit_event_mapping(void)
{
    TEST_ASSERT_EQ((uint32_t)boot_audit_event_from_selection_reason(1),
        (uint32_t)BOOT_AUDIT_EVENT_NORMAL,
        "STORE_DEFAULT (1) -> NORMAL");
    TEST_ASSERT_EQ((uint32_t)boot_audit_event_from_selection_reason(4),
        (uint32_t)BOOT_AUDIT_EVENT_WATCHDOG_ROLLBACK,
        "selection_reason 4 -> WATCHDOG_ROLLBACK");
    TEST_ASSERT_EQ((uint32_t)boot_audit_event_from_selection_reason(8),
        (uint32_t)BOOT_AUDIT_EVENT_STORE_INVALID,
        "selection_reason 8 -> STORE_INVALID");
    TEST_ASSERT_EQ((uint32_t)boot_audit_event_from_selection_reason(9),
        (uint32_t)BOOT_AUDIT_EVENT_UNKNOWN_BOOTCURRENT,
        "selection_reason 9 -> UNKNOWN_BOOTCURRENT");
}

static void test_audit_event_mapping_out_of_range(void)
{
    TEST_ASSERT_EQ((uint32_t)boot_audit_event_from_selection_reason(0),
        (uint32_t)BOOT_AUDIT_EVENT_UNSET,
        "selection_reason 0 (UNSET) -> EVENT_UNSET");
    TEST_ASSERT_EQ((uint32_t)boot_audit_event_from_selection_reason(99),
        (uint32_t)BOOT_AUDIT_EVENT_UNSET,
        "out-of-range selection_reason -> EVENT_UNSET (caller fixes up)");
}

/* ---- event name table coverage ---- */

static int strs_equal(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return *a == *b;
}

static void test_audit_event_name_known(void)
{
    TEST_ASSERT_EQ((uint32_t)strs_equal(
        boot_audit_event_name(BOOT_AUDIT_EVENT_WATCHDOG_ROLLBACK),
        "WATCHDOG_ROLLBACK"), 1u,
        "WATCHDOG_ROLLBACK -> name string match");
    TEST_ASSERT_EQ((uint32_t)strs_equal(
        boot_audit_event_name(BOOT_AUDIT_EVENT_AUDIT_DEGRADED),
        "AUDIT_DEGRADED"), 1u,
        "AUDIT_DEGRADED -> name string match");
}

static void test_audit_event_name_unknown(void)
{
    /* Future codes outside the enum return "UNKNOWN" so consumers can
     * still emit the line without hard-coded string-table breakage. */
    TEST_ASSERT_EQ((uint32_t)strs_equal(
        boot_audit_event_name(0xFFFF), "UNKNOWN"), 1u,
        "out-of-range event code -> 'UNKNOWN'");
}

/* ---- Worst-case JSONL composition ---- */

/* Synthesizes a worst-case-shaped boot_info: max-length selected_entry_id,
 * 64 max-length rejected entries, all-bits-set sticky surface. Confirms
 * the static buffer in boot_audit_publish() is large enough for the
 * worst input the v19/v20 ABI permits.
 *
 * Catches the regression where BOOT_AUDIT_JSON_LINE_CAP was 4 KiB but
 * a full 64-reject store needs >5 KiB of JSON, causing jb_truncated()
 * to skip publish AND skip the sticky ack -- replaying the trigger
 * forever on machines with many filtered entries. */
static void test_audit_compose_line_worst_case(void)
{
    TEST_SCRATCH_KBUF(bi_buf, sizeof(struct boot_info));
    struct boot_info *bi = (struct boot_info *)bi_buf;
    /* AUDIT_TEST_LINE_CAP; the non-truncation assertion below is the real gate. */
    TEST_SCRATCH_KBUF(line_buf, AUDIT_TEST_LINE_CAP);
    char *line = (char *)line_buf;

    /* Zero the synthetic boot_info so we control every field (scratch, not
     * the stack: the struct is 28+ KiB). */
    uint8_t *bp = (uint8_t *)bi;
    for (size_t i = 0; i < sizeof(*bi); i++) bp[i] = 0;

    /* Selected entry id: maximum length (47 chars + NUL per
     * BOOT_ENTRIES_MAX_ID_LEN). All ASCII letters so jb_str escape
     * doesn't expand. */
    for (uint32_t i = 0; i < 47; i++)
        bi->selected_entry_id[i] = 'a';
    bi->selected_entry_id[47] = '\0';

    bi->selection_reason = 4;  /* WATCHDOG_ROLLBACK */
    bi->audit_degraded = 0;
    bi->sticky_present = 1;
    bi->sticky_recovery_trigger = 1;
    bi->sticky_watchdog_rollback_request = 1;
    bi->sticky_last_outcome = 1;
    bi->sticky_audit_degraded_last_boot = 1;
    bi->sticky_last_event_code = BOOT_AUDIT_EVENT_NORMAL;
    bi->sticky_last_boot_seq = 0xFFFFFFFFu;        /* worst-case dec width */
    bi->sticky_consumed_trigger_seq = 0xFFFFFFFFu;

    /* Fill all 64 rejected entries with max-length ids + the longest
     * reason name (MACHINE_ID_MISMATCH = 19 chars). */
    bi->rejected_entry_count = 64;
    bi->rejected_entry_overflow = 1;
    for (uint32_t r = 0; r < 64; r++) {
        for (uint32_t i = 0; i < 47; i++)
            bi->rejected_entries[r].id[i] = 'b';
        bi->rejected_entries[r].id[47] = '\0';
        bi->rejected_entries[r].reason = 4;  /* MACHINE_ID_MISMATCH */
    }

    size_t n = boot_audit_compose_line(bi, 1u, line, AUDIT_TEST_LINE_CAP);

    /* Non-zero return == compose succeeded without truncation.
     * boot_audit_publish skips the file write AND the sticky ack on
     * truncation, so this is the difference between "trigger acked"
     * and "trigger replays forever". */
    TEST_ASSERT_EQ((uint32_t)(n != 0), 1u,
        "worst-case (max id, 64 max-length rejects) must NOT truncate");

    /* Spot-check the line shape: starts with '{', ends with "}\n". */
    TEST_ASSERT_EQ((uint32_t)(line[0] == '{'), 1u,
        "JSONL line must start with '{'");
    TEST_ASSERT_EQ((uint32_t)(n >= 2 && line[n - 2] == '}' && line[n - 1] == '\n'), 1u,
        "JSONL line must end with '}\\n'");
}

/* ---- compose_line override branches ---- */

/* Returns 1 when `needle` appears anywhere inside the first `len`
 * bytes of `haystack`. Linear search; the test inputs are <=200
 * bytes so cost is negligible. Substring match is enough to confirm
 * the JSONL line carries the expected event_code keyword without
 * pulling in a JSON parser. */
static int contains_substring(const char *haystack, size_t len, const char *needle)
{
    size_t nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0 || nlen > len) return 0;
    for (size_t i = 0; i + nlen <= len; i++) {
        size_t j = 0;
        while (j < nlen && haystack[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

/* audit_degraded=1 must override the normal selection_reason mapping
 * and emit BOOT_AUDIT_EVENT_AUDIT_DEGRADED. Without this branch a
 * degraded boot would mis-classify as NORMAL on disk. */
static void test_audit_compose_audit_degraded_override(void)
{
    TEST_SCRATCH_KBUF(bi_buf, sizeof(struct boot_info));
    struct boot_info *bi = (struct boot_info *)bi_buf;
    /* AUDIT_TEST_LINE_CAP; the non-truncation assertion below is the real gate. */
    TEST_SCRATCH_KBUF(line_buf, AUDIT_TEST_LINE_CAP);
    char *line = (char *)line_buf;

    uint8_t *bp = (uint8_t *)bi;
    for (size_t i = 0; i < sizeof(*bi); i++) bp[i] = 0;

    /* Drive a normal selection_reason -- the override must beat it. */
    bi->selection_reason = 1;  /* STORE_DEFAULT (would map to NORMAL=1) */
    bi->audit_degraded = 1;
    bi->sticky_present = 0;
    bi->selected_entry_id[0] = '\0';
    bi->rejected_entry_count = 0;

    size_t n = boot_audit_compose_line(bi, 1u, line, AUDIT_TEST_LINE_CAP);
    TEST_ASSERT_EQ((uint32_t)(n != 0), 1u,
        "audit_degraded compose must not truncate");
    TEST_ASSERT_EQ((uint32_t)contains_substring(line, n,
        "\"event\":\"AUDIT_DEGRADED\""), 1u,
        "audit_degraded=1 must force event=AUDIT_DEGRADED, overriding selection_reason mapping");
    TEST_ASSERT_EQ((uint32_t)contains_substring(line, n,
        "\"audit_degraded\":1"), 1u,
        "audit_degraded=1 must surface the flag in the JSONL line");
}

/* sticky_last_event_code == FIRST_BOOT && sticky_present == 0 must
 * emit BOOT_AUDIT_EVENT_FIRST_BOOT regardless of selection_reason --
 * the bootloader's "absent (first boot)" path is the documented
 * happy state on a fresh image. */
static void test_audit_compose_first_boot_override(void)
{
    TEST_SCRATCH_KBUF(bi_buf, sizeof(struct boot_info));
    struct boot_info *bi = (struct boot_info *)bi_buf;
    /* AUDIT_TEST_LINE_CAP; the non-truncation assertion below is the real gate. */
    TEST_SCRATCH_KBUF(line_buf, AUDIT_TEST_LINE_CAP);
    char *line = (char *)line_buf;

    uint8_t *bp = (uint8_t *)bi;
    for (size_t i = 0; i < sizeof(*bi); i++) bp[i] = 0;

    bi->selection_reason = 8;  /* STORE_INVALID -- override must beat */
    bi->audit_degraded = 0;
    bi->sticky_present = 0;
    bi->sticky_last_event_code = BOOT_AUDIT_EVENT_FIRST_BOOT;
    bi->selected_entry_id[0] = '\0';
    bi->rejected_entry_count = 0;

    size_t n = boot_audit_compose_line(bi, 1u, line, AUDIT_TEST_LINE_CAP);
    TEST_ASSERT_EQ((uint32_t)(n != 0), 1u,
        "first-boot compose must not truncate");
    TEST_ASSERT_EQ((uint32_t)contains_substring(line, n,
        "\"event\":\"FIRST_BOOT\""), 1u,
        "FIRST_BOOT sticky carrier + sticky_present=0 must force event=FIRST_BOOT");
}

/* Negative case: when sticky_present=1, the FIRST_BOOT carrier must
 * NOT trigger the override (the override is gated on sticky_present==0).
 * A valid sticky record still carrying FIRST_BOOT from the very first
 * boot must use selection_reason mapping, not the override. */
static void test_audit_compose_first_boot_gated_on_present(void)
{
    TEST_SCRATCH_KBUF(bi_buf, sizeof(struct boot_info));
    struct boot_info *bi = (struct boot_info *)bi_buf;
    /* AUDIT_TEST_LINE_CAP; the non-truncation assertion below is the real gate. */
    TEST_SCRATCH_KBUF(line_buf, AUDIT_TEST_LINE_CAP);
    char *line = (char *)line_buf;

    uint8_t *bp = (uint8_t *)bi;
    for (size_t i = 0; i < sizeof(*bi); i++) bp[i] = 0;

    bi->selection_reason = 4;  /* WATCHDOG_ROLLBACK */
    bi->audit_degraded = 0;
    bi->sticky_present = 1;    /* gate closed */
    bi->sticky_last_event_code = BOOT_AUDIT_EVENT_FIRST_BOOT;
    bi->selected_entry_id[0] = '\0';
    bi->rejected_entry_count = 0;

    size_t n = boot_audit_compose_line(bi, 1u, line, AUDIT_TEST_LINE_CAP);
    TEST_ASSERT_EQ((uint32_t)(n != 0), 1u,
        "compose must not truncate");
    TEST_ASSERT_EQ((uint32_t)contains_substring(line, n,
        "\"event\":\"WATCHDOG_ROLLBACK\""), 1u,
        "sticky_present=1 must close the FIRST_BOOT override gate");
}

/* ---- ack-classification gate ---- */

/* Recovery trigger set + ladder selected RECOVERY_REQUEST -> consumed. */
static void test_ack_classify_recovery_consumed(void)
{
    int rec = -1, wd = -1;
    boot_audit_classify_ack(BOOT_SELECTION_RECOVERY_REQUEST, 1, 0, &rec, &wd);
    TEST_ASSERT_EQ((uint32_t)rec, 1u,
        "recovery_trigger=1 + RECOVERY_REQUEST -> recovery_consumed=1");
    TEST_ASSERT_EQ((uint32_t)wd, 0u,
        "watchdog bit clear -> watchdog_consumed=0");
}

/* Recovery trigger set but ladder fell through to STORE_DEFAULT
 * (e.g. no recovery entry viable) -> NOT consumed; trigger persists. */
static void test_ack_classify_recovery_unconsumed_fallthrough(void)
{
    int rec = -1, wd = -1;
    boot_audit_classify_ack(BOOT_SELECTION_STORE_DEFAULT, 1, 0, &rec, &wd);
    TEST_ASSERT_EQ((uint32_t)rec, 0u,
        "recovery_trigger=1 + STORE_DEFAULT -> recovery_consumed=0 (preserve)");
    TEST_ASSERT_EQ((uint32_t)wd, 0u,
        "watchdog bit clear -> watchdog_consumed=0");
}

/* Watchdog trigger set + ladder selected WATCHDOG_ROLLBACK -> consumed. */
static void test_ack_classify_watchdog_consumed(void)
{
    int rec = -1, wd = -1;
    boot_audit_classify_ack(BOOT_SELECTION_WATCHDOG_ROLLBACK, 0, 1, &rec, &wd);
    TEST_ASSERT_EQ((uint32_t)wd, 1u,
        "watchdog_rollback_request=1 + WATCHDOG_ROLLBACK -> watchdog_consumed=1");
    TEST_ASSERT_EQ((uint32_t)rec, 0u,
        "recovery bit clear -> recovery_consumed=0");
}

/* Watchdog trigger set but ladder selected an unrelated entry
 * (e.g. BOOTNEXT_HINT overrode) -> watchdog NOT consumed. */
static void test_ack_classify_watchdog_unconsumed_other_reason(void)
{
    int rec = -1, wd = -1;
    boot_audit_classify_ack(BOOT_SELECTION_BOOTNEXT_HINT, 0, 1, &rec, &wd);
    TEST_ASSERT_EQ((uint32_t)wd, 0u,
        "watchdog_rollback_request=1 + BOOTNEXT_HINT -> watchdog_consumed=0 (preserve)");
}

/* Both triggers set but ladder picked HOTKEY -> NEITHER consumed.
 * Reproduces the regression where ack-on-presence cleared both bits
 * regardless of selection_reason. */
static void test_ack_classify_both_set_neither_consumed(void)
{
    int rec = -1, wd = -1;
    boot_audit_classify_ack(BOOT_SELECTION_HOTKEY, 1, 1, &rec, &wd);
    TEST_ASSERT_EQ((uint32_t)rec, 0u,
        "recovery+watchdog set but HOTKEY consumed neither -> recovery preserved");
    TEST_ASSERT_EQ((uint32_t)wd, 0u,
        "recovery+watchdog set but HOTKEY consumed neither -> watchdog preserved");
}

/* ---- 4 MiB rotation threshold sanity ---- */

static void test_rotate_threshold(void)
{
    /* The kernel-side publisher reads this constant; locking it here
     * catches accidental redefinition that would change rotation
     * behavior on next ship. 4 MiB matches the existing BlackBox
     * convention (Perf log + boot-profile.log). */
    TEST_ASSERT_EQ((uint32_t)BOOT_AUDIT_ROTATE_THRESHOLD,
        4u * 1024u * 1024u,
        "history.jsonl rotation must trigger at 4 MiB");
}

/* ---- boot_seq counter: dual-file persistence under write faults ----
 *
 * boot_audit_sequence_advance() runs against an in-memory pair of files. A
 * write fault is one of: the open is refused (contents kept), the file is
 * truncated and the write fails (empty), or a short write keeps only the
 * first byte. The property: every published boot_seq is strictly greater
 * than every earlier one, whatever faults hit any boot. */

enum { FK_OK = 0, FK_FAIL_OPEN = 1, FK_FAIL_TRUNC = 2, FK_SHORT = 3, FK_MODES = 4 };

struct fk_file { uint8_t data[BOOT_AUDIT_SEQUENCE_LEN_MAX]; int len; int exists; int read_error; };
struct fk_fs {
    struct fk_file canon, shadow;
    const uint8_t *faults;     /* fault per write call, in call order */
    int nfaults;
    int nwrites;
    int last_target;           /* 0 canon, 1 shadow */
};

static int fk_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static struct fk_file *fk_file_for(struct fk_fs *fs, const char *path)
{
    if (fk_streq(path, BOOT_AUDIT_SEQUENCE_PATH)) return &fs->canon;
    if (fk_streq(path, BOOT_AUDIT_SEQUENCE_TEMP_PATH)) return &fs->shadow;
    return (struct fk_file *)0;
}

static int fk_read(void *ctx, const char *path, uint8_t *buf, uint32_t cap)
{
    struct fk_file *f = fk_file_for((struct fk_fs *)ctx, path);
    if (!f || !f->exists) return BOOT_AUDIT_SEQ_READ_ABSENT;
    if (f->read_error) return BOOT_AUDIT_SEQ_READ_ERROR;
    int n = f->len < (int)cap ? f->len : (int)cap;
    for (int i = 0; i < n; i++) buf[i] = f->data[i];
    return n;
}

static int fk_write(void *ctx, const char *path, const char *buf, uint32_t n)
{
    struct fk_fs *fs = (struct fk_fs *)ctx;
    struct fk_file *f = fk_file_for(fs, path);
    if (!f || n > sizeof(f->data)) return 0;
    int mode = fs->nwrites < fs->nfaults ? fs->faults[fs->nwrites] : FK_OK;
    fs->nwrites++;
    fs->last_target = (f == &fs->shadow);
    if (mode == FK_FAIL_OPEN) return 0;
    f->exists = 1;
    f->len = 0;
    if (mode == FK_FAIL_TRUNC) return 0;
    uint32_t keep = (mode == FK_SHORT) ? 1u : n;
    for (uint32_t i = 0; i < keep; i++) f->data[i] = (uint8_t)buf[i];
    f->len = (int)keep;
    return mode == FK_OK ? 1 : 0;
}

static void fk_init(struct fk_fs *fs, struct boot_audit_seq_io *io)
{
    uint8_t *p = (uint8_t *)fs;
    for (size_t i = 0; i < sizeof(*fs); i++) p[i] = 0;
    io->read = fk_read;
    io->write = fk_write;
    io->ctx = fs;
}

static void fk_set(struct fk_file *f, const char *text)
{
    int n = 0;
    while (text[n] && n < (int)sizeof(f->data)) { f->data[n] = (uint8_t)text[n]; n++; }
    f->len = n;
    f->exists = 1;
}

static uint32_t fk_value(const struct fk_file *f)
{
    uint32_t v = 0;
    if (f->exists) (void)boot_audit_seq_classify(f->data, f->len, &v);
    return v;
}

static uint32_t fk_class(const char *text, int n, uint32_t *v)
{
    return (uint32_t)boot_audit_seq_classify((const uint8_t *)text, n, v);
}

static uint32_t fk_high_water(const struct fk_fs *fs)
{
    uint32_t c = fk_value(&fs->canon), sh = fk_value(&fs->shadow);
    return c > sh ? c : sh;
}

/* One boot: returns 1 and the published seq when the caller would publish. */
static int fk_boot(const struct boot_audit_seq_io *io, uint32_t *seq)
{
    enum boot_audit_seq_result r = boot_audit_sequence_advance(io, seq);
    return r == BOOT_AUDIT_SEQ_OK || r == BOOT_AUDIT_SEQ_PARTIAL;
}

static void test_audit_seq_classify(void)
{
    uint32_t v = 99u;
    TEST_ASSERT_EQ(fk_class("17\n", 3, &v), (uint32_t)BOOT_AUDIT_SEQ_VALID, "the writer's format is valid");
    TEST_ASSERT_EQ(v, 17u, "and reads its value");
    TEST_ASSERT_EQ(fk_class("17\r\n", 4, &v), (uint32_t)BOOT_AUDIT_SEQ_VALID, "CRLF is valid");
    TEST_ASSERT_EQ(fk_class("4294967295\n", 11, &v), (uint32_t)BOOT_AUDIT_SEQ_VALID, "UINT32_MAX is valid");
    TEST_ASSERT_EQ(v, 0xFFFFFFFFu, "and reads exactly");
    /* Residue of this writer's interrupted writes: safe to read as 0. */
    TEST_ASSERT_EQ(fk_class("4", 0, &v), (uint32_t)BOOT_AUDIT_SEQ_STALE, "an empty file is stale");
    TEST_ASSERT_EQ(fk_class("4", BOOT_AUDIT_SEQ_READ_ABSENT, &v), (uint32_t)BOOT_AUDIT_SEQ_STALE,
                   "an absent file is stale");
    TEST_ASSERT_EQ(fk_class("42", 2, &v), (uint32_t)BOOT_AUDIT_SEQ_STALE,
                   "digits with no terminator (a short write) are stale");
    TEST_ASSERT_EQ(v, 0u, "and read as 0");
    /* Anything else may have been the high-water copy. */
    TEST_ASSERT_EQ(fk_class("4", BOOT_AUDIT_SEQ_READ_ERROR, &v), (uint32_t)BOOT_AUDIT_SEQ_UNKNOWN,
                   "a read error is unknown");
    TEST_ASSERT_EQ(fk_class("4294967296\n", 11, &v), (uint32_t)BOOT_AUDIT_SEQ_UNKNOWN,
                   "one past UINT32_MAX is unknown");
    TEST_ASSERT_EQ(fk_class("12x\n", 4, &v), (uint32_t)BOOT_AUDIT_SEQ_UNKNOWN, "a garbage byte is unknown");
    TEST_ASSERT_EQ(fk_class("12 \n", 4, &v), (uint32_t)BOOT_AUDIT_SEQ_UNKNOWN, "trailing junk is unknown");
    TEST_ASSERT_EQ(fk_class("\n", 1, &v), (uint32_t)BOOT_AUDIT_SEQ_UNKNOWN, "no digits is unknown");
    TEST_ASSERT_EQ(fk_class("12345678901234567", 17, &v), (uint32_t)BOOT_AUDIT_SEQ_UNKNOWN,
                   "longer than the file cap is unknown");
}

static void test_audit_seq_format(void)
{
    char b[BOOT_AUDIT_SEQUENCE_LEN_MAX];
    TEST_ASSERT_EQ(boot_audit_seq_format(0u, b, sizeof(b)), 2u, "0 formats as two bytes");
    TEST_ASSERT_EQ((uint32_t)(b[0] == '0' && b[1] == '\n'), 1u, "0 formats as \"0\\n\"");
    uint32_t n = boot_audit_seq_format(0xFFFFFFFFu, b, sizeof(b));
    TEST_ASSERT_EQ(n, 11u, "UINT32_MAX formats as eleven bytes");
    uint32_t v = 0;
    TEST_ASSERT_EQ((uint32_t)boot_audit_seq_classify((const uint8_t *)b, (int)n, &v),
                   (uint32_t)BOOT_AUDIT_SEQ_VALID, "the formatted counter classifies valid");
    TEST_ASSERT_EQ(v, 0xFFFFFFFFu, "format and classify round-trip at UINT32_MAX");
    TEST_ASSERT_EQ(boot_audit_seq_format(1234567890u, b, 10u), 0u,
                   "a buffer without room for the newline is refused");
}

static void test_audit_seq_clean_boots(void)
{
    struct fk_fs fs; struct boot_audit_seq_io io; fk_init(&fs, &io);
    uint32_t seq = 0;
    for (uint32_t want = 1; want <= 3; want++) {
        TEST_ASSERT_EQ((uint32_t)boot_audit_sequence_advance(&io, &seq),
                       (uint32_t)BOOT_AUDIT_SEQ_OK, "clean boot persists both files");
        TEST_ASSERT_EQ(seq, want, "clean boots count 1, 2, 3");
    }
    TEST_ASSERT_EQ((uint32_t)fs.nwrites, 6u, "two writes per clean boot");
    TEST_ASSERT_EQ(fk_value(&fs.canon), 3u, "canonical file holds 3");
    TEST_ASSERT_EQ(fk_value(&fs.shadow), 3u, "shadow file holds 3");
}

/* The case the old write order lost: the shadow is the only copy of the last
 * published value, and the next boot's writes both fail after truncating.
 * The old code wrote the shadow first, emptied both files and restarted the
 * counter at 1; the low file (canonical, empty) must be written first. */
static void test_audit_seq_high_water_survives_dual_failure(void)
{
    struct fk_fs fs; struct boot_audit_seq_io io; fk_init(&fs, &io);
    fk_set(&fs.shadow, "2\n");                 /* last boot: canonical write failed */
    static const uint8_t faults[] = { FK_FAIL_TRUNC, FK_FAIL_TRUNC };
    fs.faults = faults; fs.nfaults = 2;
    uint32_t seq = 0;
    TEST_ASSERT_EQ((uint32_t)boot_audit_sequence_advance(&io, &seq),
                   (uint32_t)BOOT_AUDIT_SEQ_FAILED, "first write failed: not durable");
    TEST_ASSERT_EQ((uint32_t)fs.nwrites, 1u, "the high-water file is not touched");
    TEST_ASSERT_EQ((uint32_t)fs.last_target, 0u, "the low (canonical) file was written first");
    TEST_ASSERT_EQ(fk_value(&fs.shadow), 2u, "the shadow still holds the published 2");
    fs.nfaults = 0;
    TEST_ASSERT_EQ((uint32_t)fk_boot(&io, &seq), 1u, "the next clean boot publishes");
    TEST_ASSERT_EQ(seq, 3u, "and continues at 3, not 1");
}

/* Every fault pattern over three boots after a clean first boot: 4^6 runs. */
static void test_audit_seq_exhaustive_faults(void)
{
    uint32_t runs = 0, violations = 0, published_total = 0, hw_violations = 0;
    uint8_t faults[6];
    for (uint32_t combo = 0; combo < 4096u; combo++) {
        uint32_t c = combo;
        for (int i = 0; i < 6; i++) { faults[i] = (uint8_t)(c % FK_MODES); c /= FK_MODES; }
        struct fk_fs fs; struct boot_audit_seq_io io; fk_init(&fs, &io);
        uint32_t seq = 0, last = 0;
        if (!fk_boot(&io, &seq)) { violations++; continue; }
        last = seq;
        published_total++;
        for (int b = 0; b < 3; b++) {
            /* Each boot draws its two faults; an unused second fault is skipped. */
            fs.faults = &faults[b * 2]; fs.nfaults = 2; fs.nwrites = 0;
            if (fk_boot(&io, &seq)) {
                if (seq <= last) violations++;
                last = seq;
                published_total++;
            }
            if (fk_high_water(&fs) < last) hw_violations++;
        }
        runs++;
    }
    TEST_ASSERT_EQ(runs, 4096u, "every fault pattern ran");
    TEST_ASSERT_EQ(violations, 0u, "no published boot_seq repeats or goes backwards");
    TEST_ASSERT_EQ(hw_violations, 0u, "a file always holds the last published boot_seq");
    /* Not vacuous: each boot publishes unless its first write faulted, so
     * 4096 first boots + 3 * 4096 * (1/4 of first-write patterns OK) * 4. */
    TEST_ASSERT_EQ(published_total, 4096u + 3u * 4096u / 4u, "publishes exactly when the first write lands");
}

static void test_audit_seq_exhausted(void)
{
    struct fk_fs fs; struct boot_audit_seq_io io; fk_init(&fs, &io);
    fk_set(&fs.canon, "4294967294\n");
    uint32_t seq = 0;
    TEST_ASSERT_EQ((uint32_t)fk_boot(&io, &seq), 1u, "MAX-1 advances");
    TEST_ASSERT_EQ(seq, 0xFFFFFFFFu, "to UINT32_MAX");
    int writes = fs.nwrites;
    seq = 7u;
    TEST_ASSERT_EQ((uint32_t)boot_audit_sequence_advance(&io, &seq),
                   (uint32_t)BOOT_AUDIT_SEQ_EXHAUSTED, "an exhausted counter does not publish");
    TEST_ASSERT_EQ((uint32_t)(fs.nwrites - writes), 0u, "and writes nothing");
    TEST_ASSERT_EQ(seq, 7u, "and leaves the out value alone");
}

/* After a PARTIAL boot the first-written file is the only current copy. If
 * it then cannot be read, or is damaged into something the writer never
 * produces, the stale copy is far behind: advancing from it would re-publish
 * used numbers, so nothing may be written. */
static void test_audit_seq_damaged_high_water_fails_closed(void)
{
    struct fk_fs fs; struct boot_audit_seq_io io; fk_init(&fs, &io);
    fk_set(&fs.canon, "42\n");
    fk_set(&fs.shadow, "4");                   /* short second write of 42 */
    uint32_t seq = 0;
    fs.canon.read_error = 1;
    TEST_ASSERT_EQ((uint32_t)boot_audit_sequence_advance(&io, &seq),
                   (uint32_t)BOOT_AUDIT_SEQ_UNREADABLE, "a read error on the current copy fails closed");
    TEST_ASSERT_EQ((uint32_t)fs.nwrites, 0u, "and writes nothing");
    fs.canon.read_error = 0;
    fk_set(&fs.canon, "4294967296\n");
    TEST_ASSERT_EQ((uint32_t)boot_audit_sequence_advance(&io, &seq),
                   (uint32_t)BOOT_AUDIT_SEQ_UNREADABLE, "an above-range current copy fails closed");
    fk_set(&fs.canon, "4\x7f\n");
    TEST_ASSERT_EQ((uint32_t)boot_audit_sequence_advance(&io, &seq),
                   (uint32_t)BOOT_AUDIT_SEQ_UNREADABLE, "a garbage current copy fails closed");
    TEST_ASSERT_EQ((uint32_t)fs.nwrites, 0u, "none of them wrote anything");
    fk_set(&fs.canon, "42\n");
    TEST_ASSERT_EQ((uint32_t)fk_boot(&io, &seq), 1u, "once readable again the counter publishes");
    TEST_ASSERT_EQ(seq, 43u, "from the high-water mark, not the stale copy");
    TEST_ASSERT_EQ((uint32_t)boot_audit_sequence_advance((const struct boot_audit_seq_io *)0, &seq),
                   (uint32_t)BOOT_AUDIT_SEQ_FAILED, "missing ops fail closed");
}

void test_register_boot_audit(void);

/* A sticky write is read-modify-write: only an authoritative not-found builds
 * a fresh record; every other read outcome refuses, so a transient failure can
 * never erase a pending watchdog request or the sequence fields. */
static void test_audit_sticky_rmw_prepare(void)
{
    struct boot_sticky_record cur, out;
    uint8_t *p = (uint8_t *)&cur;

    for (uint32_t i = 0; i < sizeof cur; i++) p[i] = 0;
    cur.magic = BOOT_STICKY_RECORD_MAGIC;
    cur.version = BOOT_STICKY_RECORD_VERSION;
    cur.size = BOOT_STICKY_VAR_SIZE;
    cur.watchdog_rollback_request = 1;
    cur.last_boot_seq = 77;
    cur.crc32 = boot_sticky_compute_crc(&cur);

    TEST_ASSERT_EQ(boot_sticky_rmw_prepare(STATUS_SUCCESS, sizeof cur, &cur, &out), 0,
                   "a valid record is modifiable");
    TEST_ASSERT_EQ(out.watchdog_rollback_request, 1, "its watchdog request is kept");
    TEST_ASSERT_EQ(out.last_boot_seq, 77u, "and its sequence");

    TEST_ASSERT_EQ(boot_sticky_rmw_prepare(STATUS_NOT_FOUND, 0, &cur, &out), 0,
                   "an absent variable gets a fresh record");
    TEST_ASSERT_EQ(out.magic, BOOT_STICKY_RECORD_MAGIC, "fresh record carries the magic");
    TEST_ASSERT_EQ(out.watchdog_rollback_request, 0, "and nothing else");

    TEST_ASSERT_EQ(boot_sticky_rmw_prepare(STATUS_UNSUCCESSFUL, sizeof cur, &cur, &out), -1,
                   "a failed read refuses");
    TEST_ASSERT_EQ(boot_sticky_rmw_prepare(STATUS_BUFFER_TOO_SMALL, sizeof cur, &cur, &out), -1,
                   "an oversized variable refuses");
    TEST_ASSERT_EQ(boot_sticky_rmw_prepare(STATUS_SUCCESS, sizeof cur - 1u, &cur, &out), -1,
                   "a short record refuses");
    cur.crc32 ^= 1u;
    TEST_ASSERT_EQ(boot_sticky_rmw_prepare(STATUS_SUCCESS, sizeof cur, &cur, &out), -1,
                   "a corrupt record refuses rather than being replaced");
    TEST_ASSERT_EQ(boot_sticky_rmw_prepare(STATUS_SUCCESS, sizeof cur, &cur,
                                           (struct boot_sticky_record *)0), -1,
                   "no output refuses");
}


static void test_audit_sticky_ack_apply(void)
{
    struct boot_sticky_record r;

    r.recovery_trigger = 1; r.watchdog_rollback_request = 1;
    boot_sticky_ack_apply(&r, 0, 1, 1);
    TEST_ASSERT_EQ(r.watchdog_rollback_request, 0, "consumed watchdog cleared");
    TEST_ASSERT_EQ(r.recovery_trigger, 1, "beside it, this boot's arm survives");

    r.recovery_trigger = 1; r.watchdog_rollback_request = 1;
    boot_sticky_ack_apply(&r, 1, 1, 1);
    TEST_ASSERT_EQ(r.recovery_trigger, 1, "a ladder-consumed trigger does not erase a new arm");

    r.recovery_trigger = 1; r.watchdog_rollback_request = 1;
    boot_sticky_ack_apply(&r, 0, 1, 0);
    TEST_ASSERT_EQ(r.recovery_trigger, 0, "a spent trigger is not resurrected by the snapshot");
    TEST_ASSERT_EQ(r.watchdog_rollback_request, 0, "consumed watchdog cleared");

    r.recovery_trigger = 1; r.watchdog_rollback_request = 1;
    boot_sticky_ack_apply(&r, 1, 0, -1);
    TEST_ASSERT_EQ(r.recovery_trigger, 0, "untouched: the ladder's consume clears it");
    TEST_ASSERT_EQ(r.watchdog_rollback_request, 1, "an unconsumed watchdog request persists");
}


static void test_audit_sticky_ack_build(void)
{
    struct boot_sticky_record cur, out;
    uint8_t *p = (uint8_t *)&cur;

    for (uint32_t i = 0; i < sizeof cur; i++) p[i] = 0;
    cur.magic = BOOT_STICKY_RECORD_MAGIC;
    cur.version = BOOT_STICKY_RECORD_VERSION;
    cur.size = BOOT_STICKY_VAR_SIZE;
    cur.watchdog_rollback_request = 1;
    cur.enroll_offer_dismissed = 1;
    cur.crc32 = boot_sticky_compute_crc(&cur);

    TEST_ASSERT_EQ(boot_sticky_ack_build(STATUS_UNSUCCESSFUL, sizeof cur, &cur, 0, 1, -1, &out), -1,
                   "a failed read skips the ack rather than rebuilding the record");
    TEST_ASSERT_EQ(boot_sticky_ack_build(STATUS_SUCCESS, sizeof cur, &cur, 0, 1, -1, &out), 0,
                   "a readable record is acked");
    TEST_ASSERT_EQ(out.watchdog_rollback_request, 0, "the consumed watchdog request is cleared");
    TEST_ASSERT_EQ(out.enroll_offer_dismissed, 1, "the offer dismissal survives the ack");
}

void test_register_boot_audit(void)
{
    test_suite_register_cat("boot_audit sticky read-modify-write", test_audit_sticky_rmw_prepare, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit sticky ack never rebuilds an unreadable record",
                            test_audit_sticky_ack_build, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit sticky ack honours this boot's trigger write",
                            test_audit_sticky_ack_apply, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit seq classify", test_audit_seq_classify, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit seq format", test_audit_seq_format, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit seq clean boots", test_audit_seq_clean_boots, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit seq high-water survives dual failure",
        test_audit_seq_high_water_survives_dual_failure, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit seq exhaustive write faults",
        test_audit_seq_exhaustive_faults, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit seq exhausted counter", test_audit_seq_exhausted, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit seq damaged high-water fails closed",
        test_audit_seq_damaged_high_water_fails_closed, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit sticky record size",
        test_sticky_record_size, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit sticky record offsets",
        test_sticky_record_offsets, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit sticky CRC roundtrip valid",
        test_sticky_crc_roundtrip_valid, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit sticky CRC detects byte flip",
        test_sticky_crc_detects_byte_flip, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit sticky rejects bad magic",
        test_sticky_rejects_bad_magic, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit sticky rejects wrong version",
        test_sticky_rejects_wrong_version, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit sticky rejects wrong size",
        test_sticky_rejects_wrong_size, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit sticky zeroed record invalid",
        test_sticky_zeroed_record_invalid, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit selection_reason -> event mapping",
        test_audit_event_mapping, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit out-of-range selection_reason -> UNSET",
        test_audit_event_mapping_out_of_range, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit event_name known codes",
        test_audit_event_name_known, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit event_name unknown -> 'UNKNOWN'",
        test_audit_event_name_unknown, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit history.jsonl rotation threshold",
        test_rotate_threshold, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit compose line worst-case capacity",
        test_audit_compose_line_worst_case, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit compose audit_degraded override",
        test_audit_compose_audit_degraded_override, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit compose FIRST_BOOT override",
        test_audit_compose_first_boot_override, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit FIRST_BOOT gated on sticky_present",
        test_audit_compose_first_boot_gated_on_present, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit ack: recovery consumed on RECOVERY_REQUEST",
        test_ack_classify_recovery_consumed, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit ack: recovery preserved on fallthrough",
        test_ack_classify_recovery_unconsumed_fallthrough, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit ack: watchdog consumed on WATCHDOG_ROLLBACK",
        test_ack_classify_watchdog_consumed, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit ack: watchdog preserved on other reason",
        test_ack_classify_watchdog_unconsumed_other_reason, TEST_CAT_BOOT);
    test_suite_register_cat("boot_audit ack: both set but neither consumed -> preserve",
        test_ack_classify_both_set_neither_consumed, TEST_CAT_BOOT);
}
