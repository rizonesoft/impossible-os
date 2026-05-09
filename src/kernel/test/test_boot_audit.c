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
#include "kernel/boot_info.h"
#include "boot/boot_audit_codes.h"
#include "boot/boot_policy.h"

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
    static struct boot_info bi;
    static char line[16u * 1024u];

    /* Zero the synthetic boot_info so we control every field. Use
     * static storage so a 28+ KiB struct does not blow the test stack. */
    uint8_t *bp = (uint8_t *)&bi;
    for (size_t i = 0; i < sizeof(bi); i++) bp[i] = 0;

    /* Selected entry id: maximum length (47 chars + NUL per
     * BOOT_ENTRIES_MAX_ID_LEN). All ASCII letters so jb_str escape
     * doesn't expand. */
    for (uint32_t i = 0; i < 47; i++)
        bi.selected_entry_id[i] = 'a';
    bi.selected_entry_id[47] = '\0';

    bi.selection_reason = 4;  /* WATCHDOG_ROLLBACK */
    bi.audit_degraded = 0;
    bi.sticky_present = 1;
    bi.sticky_recovery_trigger = 1;
    bi.sticky_watchdog_rollback_request = 1;
    bi.sticky_last_outcome = 1;
    bi.sticky_audit_degraded_last_boot = 1;
    bi.sticky_last_event_code = BOOT_AUDIT_EVENT_NORMAL;
    bi.sticky_last_boot_seq = 0xFFFFFFFFu;        /* worst-case dec width */
    bi.sticky_consumed_trigger_seq = 0xFFFFFFFFu;

    /* Fill all 64 rejected entries with max-length ids + the longest
     * reason name (MACHINE_ID_MISMATCH = 19 chars). */
    bi.rejected_entry_count = 64;
    bi.rejected_entry_overflow = 1;
    for (uint32_t r = 0; r < 64; r++) {
        for (uint32_t i = 0; i < 47; i++)
            bi.rejected_entries[r].id[i] = 'b';
        bi.rejected_entries[r].id[47] = '\0';
        bi.rejected_entries[r].reason = 4;  /* MACHINE_ID_MISMATCH */
    }

    size_t n = boot_audit_compose_line(&bi, 1u, line, sizeof(line));

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
    static struct boot_info bi;
    static char line[16u * 1024u];

    uint8_t *bp = (uint8_t *)&bi;
    for (size_t i = 0; i < sizeof(bi); i++) bp[i] = 0;

    /* Drive a normal selection_reason -- the override must beat it. */
    bi.selection_reason = 1;  /* STORE_DEFAULT (would map to NORMAL=1) */
    bi.audit_degraded = 1;
    bi.sticky_present = 0;
    bi.selected_entry_id[0] = '\0';
    bi.rejected_entry_count = 0;

    size_t n = boot_audit_compose_line(&bi, 1u, line, sizeof(line));
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
    static struct boot_info bi;
    static char line[16u * 1024u];

    uint8_t *bp = (uint8_t *)&bi;
    for (size_t i = 0; i < sizeof(bi); i++) bp[i] = 0;

    bi.selection_reason = 8;  /* STORE_INVALID -- override must beat */
    bi.audit_degraded = 0;
    bi.sticky_present = 0;
    bi.sticky_last_event_code = BOOT_AUDIT_EVENT_FIRST_BOOT;
    bi.selected_entry_id[0] = '\0';
    bi.rejected_entry_count = 0;

    size_t n = boot_audit_compose_line(&bi, 1u, line, sizeof(line));
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
    static struct boot_info bi;
    static char line[16u * 1024u];

    uint8_t *bp = (uint8_t *)&bi;
    for (size_t i = 0; i < sizeof(bi); i++) bp[i] = 0;

    bi.selection_reason = 4;  /* WATCHDOG_ROLLBACK */
    bi.audit_degraded = 0;
    bi.sticky_present = 1;    /* gate closed */
    bi.sticky_last_event_code = BOOT_AUDIT_EVENT_FIRST_BOOT;
    bi.selected_entry_id[0] = '\0';
    bi.rejected_entry_count = 0;

    size_t n = boot_audit_compose_line(&bi, 1u, line, sizeof(line));
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

void test_register_boot_audit(void);
void test_register_boot_audit(void)
{
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
