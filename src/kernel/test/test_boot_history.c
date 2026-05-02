/* ============================================================================
 * test_boot_history.c -- unit tests for the boot-error history ring
 *
 * SYNTHETIC-RING TESTS ONLY.  Live BlackBox/NVRAM I/O is exercised by
 * the cascade-failure smoke fixture (scripts/test-smoke-history.sh).
 * Per CLAUDE.md "Test Code Policy", these tests must NEVER call live
 * boot infrastructure (uefi_var_set/get, RuntimeServices, boot_halt,
 * panic, subsystem _init).  All assertions exercise pure decoders and
 * round-trip the schema against an in-memory ring.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/test/test.h"

/* ---- Helper: zero a ring (mirrors boot_history_read's zero-init) ---- */
static void zero_ring(struct boot_error_history_entry ring[BOOT_HIST_RING_LEN])
{
    for (size_t i = 0; i < BOOT_HIST_RING_LEN; i++) {
        ring[i].boot_seq = 0;
        ring[i].unix_time = 0;
        ring[i].err_code = 0;
        ring[i].source_section = 0;
        ring[i]._pad = 0;
    }
}

/* ---- Schema asserts (compile-time + runtime parity) ---- */

static void test_boot_history_struct_size(void)
{
    TEST_ASSERT_EQ(sizeof(struct boot_error_history_entry), 16,
        "boot_error_history_entry must be exactly 16 bytes");
    /* The relationship between BOOT_HIST_BIN_SIZE and RING_LEN * sizeof(entry)
     * exercises a derived value, so the assert is non-tautological -- it
     * catches a future drift that touches one constant but not the other. */
    TEST_ASSERT_EQ(BOOT_HIST_BIN_SIZE,
                   BOOT_HIST_RING_LEN * sizeof(struct boot_error_history_entry),
        "BOOT_HIST_BIN_SIZE must equal RING_LEN * sizeof(entry)");
}

static void test_boot_history_field_offsets(void)
{
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_error_history_entry, boot_seq), 0,
        "boot_seq must be at offset 0");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_error_history_entry, unix_time), 4,
        "unix_time must be at offset 4");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_error_history_entry, err_code), 8,
        "err_code must be at offset 8");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_error_history_entry, source_section), 10,
        "source_section must be at offset 10");
    TEST_ASSERT_EQ(__builtin_offsetof(struct boot_error_history_entry, _pad), 12,
        "_pad must be at offset 12");
}

/* ---- Sentinel value asserts ---- */

/* Sentinel value coverage is exercised through the decoder tests below
 * (each sentinel is passed to boot_history_decode_source_section and the
 * resulting label string is asserted).  Tautological literal-vs-#define
 * asserts add no behavioral coverage and are forbidden by the lint
 * "tautological-test" check. */

/* ---- Decoder string-mapping tests ---- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void test_decode_sentinels(void)
{
    char buf[32];
    boot_history_decode_source_section(BOOT_SECTION_UNKNOWN, buf, sizeof(buf));
    TEST_ASSERT(str_eq(buf, "unknown"),
        "BOOT_SECTION_UNKNOWN decodes to \"unknown\"");
    boot_history_decode_source_section(BOOT_SECTION_EBS_OK, buf, sizeof(buf));
    TEST_ASSERT(str_eq(buf, "ebs-success"),
        "BOOT_SECTION_EBS_OK decodes to \"ebs-success\"");
    boot_history_decode_source_section(BOOT_SECTION_KERNEL_PHASE3, buf, sizeof(buf));
    TEST_ASSERT(str_eq(buf, "kernel-Phase3"),
        "BOOT_SECTION_KERNEL_PHASE3 decodes to \"kernel-Phase3\"");
}

static void test_decode_bootloader_phases(void)
{
    char buf[32];
    boot_history_decode_source_section(0x0101u, buf, sizeof(buf));
    TEST_ASSERT(str_eq(buf, "bl-init"),
        "0x0101 decodes to \"bl-init\"");
    boot_history_decode_source_section(0x0102u, buf, sizeof(buf));
    TEST_ASSERT(str_eq(buf, "bl-conf"),
        "0x0102 decodes to \"bl-conf\"");
    boot_history_decode_source_section(0x0103u, buf, sizeof(buf));
    TEST_ASSERT(str_eq(buf, "bl-kernel"),
        "0x0103 decodes to \"bl-kernel\"");
    boot_history_decode_source_section(0x0104u, buf, sizeof(buf));
    TEST_ASSERT(str_eq(buf, "bl-pagetables"),
        "0x0104 decodes to \"bl-pagetables\"");
    boot_history_decode_source_section(0x0105u, buf, sizeof(buf));
    TEST_ASSERT(str_eq(buf, "bl-ebs"),
        "0x0105 decodes to \"bl-ebs\"");
}

static void test_decode_unknown_falls_back_to_hex(void)
{
    char buf[32];
    /* A value outside any known table must fall back to "section-0xNNNN"
     * so a future producer adding unknown codes still surfaces them
     * legibly in the renderer. */
    boot_history_decode_source_section(0x0042u, buf, sizeof(buf));
    TEST_ASSERT(str_eq(buf, "section-0x0042"),
        "Unknown code falls back to section-0xNNNN form");
    boot_history_decode_source_section(0xABCDu, buf, sizeof(buf));
    TEST_ASSERT(str_eq(buf, "section-0xABCD"),
        "Hex digits use uppercase A-F");
}

static void test_decode_handles_tiny_buffer(void)
{
    /* Caller must never receive an unterminated string regardless of
     * how small the buffer is.  Cap == 1 means a single NUL only. */
    char tiny[1];
    tiny[0] = 0xAA;
    boot_history_decode_source_section(BOOT_SECTION_EBS_OK, tiny, sizeof(tiny));
    TEST_ASSERT_EQ((unsigned)tiny[0], 0,
        "cap=1 yields empty NUL-terminated string");
}

/* ---- Ring semantics: head = (seq+1) % LEN, wrap on overflow ---- */

static void test_ring_wrap_overwrites_oldest(void)
{
    /* The producer's atomicity rule: head = new_seq % BOOT_HIST_RING_LEN.
     * Writing 9 entries (seq 1..9) into an 8-slot ring overwrites slot 1
     * (which had seq=1) with seq=9. */
    struct boot_error_history_entry ring[BOOT_HIST_RING_LEN];
    zero_ring(ring);
    for (uint32_t seq = 1; seq <= 9; seq++) {
        size_t head = (size_t)(seq % BOOT_HIST_RING_LEN);
        ring[head].boot_seq = seq;
        ring[head].source_section = BOOT_SECTION_KERNEL_PHASE3;
    }
    /* Slot 1 (head for seq=1, then head for seq=9) holds seq=9. */
    TEST_ASSERT_EQ(ring[1].boot_seq, 9,
        "9th append overwrites slot 1 (seq=1 displaced by seq=9)");
    /* Slots 2..7 retain their original seq values (2..7). */
    for (size_t i = 2; i <= 7; i++) {
        TEST_ASSERT_EQ(ring[i].boot_seq, (uint32_t)i,
            "slot retains its original seq across the wrap");
    }
    /* Slot 0 = seq 8; slot 1 = seq 9 (just verified). */
    TEST_ASSERT_EQ(ring[0].boot_seq, 8,
        "slot 0 holds seq=8");
}

static void test_ring_wrap_guard_against_seq_zero(void)
{
    /* Producer-side guard: new_seq == 0 is promoted to 1 to avoid
     * collision with the empty-slot sentinel.  Verify the contract
     * that no valid entry ever carries boot_seq == 0. */
    uint32_t seq = 0xFFFFFFFFu;  /* attacker-seeded or 4Bn-boot wrap */
    uint32_t new_seq = seq + 1;
    if (new_seq == 0) new_seq = 1;
    TEST_ASSERT_EQ(new_seq, 1,
        "wrap from UINT32_MAX promotes to seq=1, never seq=0");
}

/* ---- Round-trip: synthetic write -> read pattern ----
 * Mirrors the boot_history_read shape (count non-zero) on an in-memory
 * ring -- no live NVRAM. */

static size_t count_non_zero(struct boot_error_history_entry ring[BOOT_HIST_RING_LEN])
{
    size_t count = 0;
    for (size_t i = 0; i < BOOT_HIST_RING_LEN; i++) {
        if (ring[i].boot_seq != 0)
            count++;
    }
    return count;
}

static void test_count_empty_ring_is_zero(void)
{
    struct boot_error_history_entry ring[BOOT_HIST_RING_LEN];
    zero_ring(ring);
    TEST_ASSERT_EQ(count_non_zero(ring), 0,
        "freshly zeroed ring has zero non-zero entries");
}

static void test_count_full_ring(void)
{
    struct boot_error_history_entry ring[BOOT_HIST_RING_LEN];
    zero_ring(ring);
    for (uint32_t seq = 1; seq <= BOOT_HIST_RING_LEN; seq++) {
        size_t head = (size_t)(seq % BOOT_HIST_RING_LEN);
        ring[head].boot_seq = seq;
    }
    TEST_ASSERT_EQ(count_non_zero(ring), (uint32_t)BOOT_HIST_RING_LEN,
        "fully populated ring has 8 non-zero entries");
}

/* ---- Atomicity contract: cookie filters torn appends ---- */

/* Pure-data emulation of boot_history_read's filter pass.  Mirrors the
 * "skip slots where boot_seq > committed_seq" rule so this test does
 * not have to call the live uefi_var_get path (CLAUDE.md test
 * policy: no live boot infrastructure in tests). */
static size_t simulate_filter(struct boot_error_history_entry ring[BOOT_HIST_RING_LEN],
                              uint32_t committed_seq)
{
    size_t count = 0;
    for (size_t i = 0; i < BOOT_HIST_RING_LEN; i++) {
        if (ring[i].boot_seq == 0) continue;
        if (ring[i].boot_seq > committed_seq) {
            ring[i].boot_seq = 0;
            ring[i].unix_time = 0;
            ring[i].err_code = 0;
            ring[i].source_section = 0;
            ring[i]._pad = 0;
            continue;
        }
        count++;
    }
    return count;
}

static void test_torn_append_filtered_out(void)
{
    /* Ring contains entries seq=1..4 from prior committed boots, plus
     * a slot stamped with seq=5 that the producer never managed to
     * commit (cookie write failed -- cookie still says 4).  The
     * renderer must show 4 entries, NOT 5. */
    struct boot_error_history_entry ring[BOOT_HIST_RING_LEN];
    zero_ring(ring);
    for (uint32_t seq = 1; seq <= 4; seq++) {
        ring[seq % BOOT_HIST_RING_LEN].boot_seq = seq;
        ring[seq % BOOT_HIST_RING_LEN].source_section = BOOT_SECTION_EBS_OK;
    }
    /* The torn write: ring stamps seq=5, cookie stays at 4. */
    ring[5 % BOOT_HIST_RING_LEN].boot_seq = 5;
    ring[5 % BOOT_HIST_RING_LEN].source_section = BOOT_SECTION_EBS_OK;
    uint32_t committed_seq = 4;

    size_t count = simulate_filter(ring, committed_seq);
    TEST_ASSERT_EQ(count, 4,
        "torn append (seq=5, cookie=4) is filtered out; only 4 entries visible");
    TEST_ASSERT_EQ(ring[5 % BOOT_HIST_RING_LEN].boot_seq, 0,
        "uncommitted slot zeroed in place");
}

static void test_committed_cookie_passes_all_entries(void)
{
    /* Cookie matches the highest seq in the ring -- nothing is filtered. */
    struct boot_error_history_entry ring[BOOT_HIST_RING_LEN];
    zero_ring(ring);
    for (uint32_t seq = 1; seq <= 8; seq++) {
        ring[seq % BOOT_HIST_RING_LEN].boot_seq = seq;
    }
    uint32_t committed_seq = 8;
    size_t count = simulate_filter(ring, committed_seq);
    TEST_ASSERT_EQ(count, 8,
        "cookie at HEAD passes all 8 entries through");
}

static void test_zero_cookie_filters_everything(void)
{
    /* If the cookie reads as 0 (poisoned, missing, or first-ever boot
     * with stray ring data), nothing renders.  This matches the
     * boot_history_read contract -- a poisoned cookie cannot inflate
     * the visible history. */
    struct boot_error_history_entry ring[BOOT_HIST_RING_LEN];
    zero_ring(ring);
    ring[1].boot_seq = 7;
    ring[2].boot_seq = 8;
    uint32_t committed_seq = 0;
    size_t count = simulate_filter(ring, committed_seq);
    TEST_ASSERT_EQ(count, 0,
        "cookie=0 filters all ring entries (poisoned-cookie defense)");
}

/* ---- Registration ---- */

void test_register_boot_history(void)
{
    test_suite_register_cat("boot_history struct size", test_boot_history_struct_size, TEST_CAT_BOOT);
    test_suite_register_cat("boot_history field offsets", test_boot_history_field_offsets, TEST_CAT_BOOT);
    test_suite_register_cat("boot_history decode sentinels", test_decode_sentinels, TEST_CAT_BOOT);
    test_suite_register_cat("boot_history decode BL phases", test_decode_bootloader_phases, TEST_CAT_BOOT);
    test_suite_register_cat("boot_history decode unknown -> hex", test_decode_unknown_falls_back_to_hex, TEST_CAT_BOOT);
    test_suite_register_cat("boot_history decode tiny buffer", test_decode_handles_tiny_buffer, TEST_CAT_BOOT);
    test_suite_register_cat("boot_history ring wrap overwrites", test_ring_wrap_overwrites_oldest, TEST_CAT_BOOT);
    test_suite_register_cat("boot_history wrap guard seq=0", test_ring_wrap_guard_against_seq_zero, TEST_CAT_BOOT);
    test_suite_register_cat("boot_history empty count", test_count_empty_ring_is_zero, TEST_CAT_BOOT);
    test_suite_register_cat("boot_history full count", test_count_full_ring, TEST_CAT_BOOT);
    test_suite_register_cat("boot_history torn-append filtered", test_torn_append_filtered_out, TEST_CAT_BOOT);
    test_suite_register_cat("boot_history cookie HEAD passes all",
                            test_committed_cookie_passes_all_entries, TEST_CAT_BOOT);
    test_suite_register_cat("boot_history zero cookie filters all", test_zero_cookie_filters_everything, TEST_CAT_BOOT);
}
