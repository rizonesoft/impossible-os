/* test_boot_entry_parser.c -- kernel-side unit tests for the boot entry store parser.
 *
 * The parser source lives in src/boot/uefi/boot_entries_parser.c (compiled into
 * the bootloader binary). This test file pulls a private copy of the same source
 * via #include so the kernel test binary can call boot_entries_parse() against
 * static fixture strings without linking against the bootloader binary. The
 * parser is pure C with no UEFI types, kmalloc, or printk dependencies, so the
 * #include is self-contained.
 *
 * XREF: 01-boot-platform/TODO-07-boot-entry-store-menu-policy.md "Boot Entry
 * Parser and Validator"
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"

/* Pull the parser implementation into this translation unit. The functions
 * become local to test_boot_entry_parser.o and do not collide with the
 * bootloader's copy (different binaries). */
#include "../../boot/uefi/boot_entries_parser.c"

#include "kernel/types.h"   /* uint32_t for assertions / size_t */

/* Helper: build a CRC-correct fixture by writing a placeholder, computing the
 * CRC, and patching the 8 hex digits in place. Mirrors validate.py --emit-crc. */
static unsigned int s_fixture_buf_size = 0;
static unsigned char s_fixture_buf[2048];

static unsigned int patch_fixture_crc(unsigned char *buf, unsigned int len)
{
    unsigned int zero_off, expected;
    if (!find_crc_field(buf, len, &zero_off, &expected)) return 0;
    unsigned int crc = crc32_zeroed(buf, len, zero_off);
    /* Write the CRC as 8 uppercase hex digits at offset zero_off. */
    static const char HEX[] = "0123456789ABCDEF";
    unsigned int i;
    for (i = 0; i < 8u; i++) {
        unsigned int nybble = (crc >> ((7u - i) * 4u)) & 0xFu;
        buf[zero_off + i] = (unsigned char)HEX[nybble];
    }
    return crc;
}

static unsigned int load_fixture(const char *json)
{
    unsigned int n = 0;
    while (json[n]) {
        if (n >= sizeof(s_fixture_buf)) break;
        s_fixture_buf[n] = (unsigned char)json[n];
        n++;
    }
    s_fixture_buf_size = n;
    patch_fixture_crc(s_fixture_buf, n);
    return n;
}

/* ---- Test cases ------------------------------------------------------- */

static void test_parser_valid_minimal(void)
{
    static const char JSON[] =
        "{"
        "\"schema_version\":1,"
        "\"crc32\":\"0x00000000\","
        "\"entries\":["
        "{\"id\":\"slot-a\",\"title\":\"Slot A\",\"kind\":\"split\","
        "\"flags\":[\"active\"],\"sort_key\":\"00\","
        "\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],"
        "\"payload\":{\"kernel\":\"\\\\EFI\\\\ImpossibleOS\\\\kernel.exe\"}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK, "valid minimal store accepted");
    TEST_ASSERT_EQ(r.entry_count, 1u, "one entry parsed");
    TEST_ASSERT_EQ(r.entries[0].kind, BOOT_ENTRY_KIND_SPLIT, "kind=split parsed");
    TEST_ASSERT_EQ(r.entries[0].flags & BOOT_ENTRY_FLAG_ACTIVE, BOOT_ENTRY_FLAG_ACTIVE,
                   "active flag set");
}

static void test_parser_bad_schema_version(void)
{
    static const char JSON[] =
        "{\"schema_version\":99,\"crc32\":\"0x00000000\",\"entries\":[]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_SCHEMA_VERSION,
                   "schema_version != 1 rejected");
}

static void test_parser_no_entries(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":[]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_NO_ENTRIES, "empty entries rejected");
}

static void test_parser_missing_payload(void)
{
    /* Entry with all required envelope fields except payload -> reject */
    static const char JSON[] =
        "{"
        "\"schema_version\":1,"
        "\"crc32\":\"0x00000000\","
        "\"entries\":["
        "{\"id\":\"x\",\"title\":\"X\",\"kind\":\"split\",\"flags\":[\"active\"],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[]}"
        "]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_MISSING_FIELD, "missing payload rejected");
}

static void test_parser_bad_crc(void)
{
    static const char JSON[] =
        "{"
        "\"schema_version\":1,"
        "\"crc32\":\"0xDEADBEEF\","   /* deliberately wrong; do NOT patch */
        "\"entries\":["
        "{\"id\":\"a\",\"title\":\"A\",\"kind\":\"split\",\"flags\":[\"active\"],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}}"
        "]}";
    /* Load WITHOUT patching CRC -- 0xDEADBEEF stays as the stored value. */
    unsigned int n = 0;
    while (JSON[n]) { s_fixture_buf[n] = (unsigned char)JSON[n]; n++; }
    s_fixture_buf_size = n;
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_CRC_MISMATCH, "wrong CRC rejected");
}

static void test_parser_unknown_string_kind_skipped(void)
{
    static const char JSON[] =
        "{"
        "\"schema_version\":1,"
        "\"crc32\":\"0x00000000\","
        "\"entries\":["
        "{\"id\":\"x\",\"title\":\"X\",\"kind\":\"future-stable\","
        "\"flags\":[\"active\"],\"sort_key\":\"00\","
        "\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK,
                   "unknown string kind store accepted overall");
    TEST_ASSERT_EQ(r.entry_count, 0u, "skipped entry not in entries[]");
    TEST_ASSERT_EQ(r.skipped_count, 1u, "skipped count incremented");
}

static void test_parser_stable_numeric_unknown_rejected(void)
{
    static const char JSON[] =
        "{"
        "\"schema_version\":1,"
        "\"crc32\":\"0x00000000\","
        "\"entries\":["
        "{\"id\":\"x\",\"title\":\"X\",\"kind\":50,"
        "\"flags\":[\"active\"],\"sort_key\":\"00\","
        "\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_UNKNOWN_KIND_RANGE,
                   "stable-range numeric kind unknown rejected");
}

static void test_parser_duplicate_id(void)
{
    static const char JSON[] =
        "{"
        "\"schema_version\":1,"
        "\"crc32\":\"0x00000000\","
        "\"entries\":["
        "{\"id\":\"dup\",\"title\":\"A\",\"kind\":\"split\",\"flags\":[\"active\"],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}},"
        "{\"id\":\"dup\",\"title\":\"B\",\"kind\":\"split\",\"flags\":[\"active\"],"
        "\"sort_key\":\"01\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_DUPLICATE_ID, "duplicate id rejected");
}

static void test_parser_id_not_kebab(void)
{
    static const char JSON[] =
        "{"
        "\"schema_version\":1,"
        "\"crc32\":\"0x00000000\","
        "\"entries\":["
        "{\"id\":\"Slot-A\",\"title\":\"X\",\"kind\":\"split\",\"flags\":[\"active\"],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_ID, "non-kebab id rejected");
}

static void test_parser_malformed_payload_rejected(void)
{
    /* payload {"k":} is balanced-braces but missing the value -- the new state-
     * machine walker rejects this where the old delimiter balancer accepted it. */
    static const char JSON[] =
        "{"
        "\"schema_version\":1,"
        "\"crc32\":\"0x00000000\","
        "\"entries\":["
        "{\"id\":\"x\",\"title\":\"X\",\"kind\":\"split\",\"flags\":[\"active\"],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{\"k\":}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT(rc != BOOT_ENTRIES_OK, "malformed payload value rejected");
}

static void test_parser_chainload_passes_under_secure_boot(void)
{
    /* The parser no longer hard-rejects an untrusted chainload under
     * Secure Boot -- per-entry demote is now owned by the boot-policy
     * filter so other viable entries are not killed by one bad entry.
     * The store parses cleanly; the policy layer is exercised in
     * test_boot_policy.c::test_ladder_path_escape_under_secure_boot. */
    static const char JSON[] =
        "{"
        "\"schema_version\":1,"
        "\"crc32\":\"0x00000000\","
        "\"entries\":["
        "{\"id\":\"x\",\"title\":\"X\",\"kind\":\"chainload\","
        "\"flags\":[\"active\"],\"sort_key\":\"00\","
        "\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 1, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK,
                   "chainload entry retained under Secure Boot for policy filter");
    TEST_ASSERT_EQ(r.entry_count, 1u, "one entry retained");
    TEST_ASSERT_EQ(r.entries[0].kind, BOOT_ENTRY_KIND_CHAINLOAD,
                   "kind=chainload preserved");
}

static void test_parser_depth_bomb(void)
{
    /* Build a string with deep array nesting beyond MAX_PARSE_DEPTH (8 levels).
     * The entries array element is a deep nested array (not an object) -- the
     * parser will reject either at REJECT_NOT_OBJECT or at depth-limit when
     * skipping the nested value. Either path proves no stack overflow. */
    static unsigned char buf[1024];
    const char *prefix =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":[";
    const char *suffix = "]}";
    unsigned int p = 0, i;
    for (i = 0; prefix[i]; i++) buf[p++] = (unsigned char)prefix[i];
    for (i = 0; i < BOOT_ENTRIES_MAX_PARSE_DEPTH + 2u; i++) buf[p++] = '[';
    for (i = 0; i < BOOT_ENTRIES_MAX_PARSE_DEPTH + 2u; i++) buf[p++] = ']';
    for (i = 0; suffix[i]; i++) buf[p++] = (unsigned char)suffix[i];
    patch_fixture_crc(buf, p);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(buf, p, 0, NULL_PTR, &r);
    TEST_ASSERT(rc != BOOT_ENTRIES_OK, "depth-bomb rejected without crash");
}

static void test_parser_trailing_garbage_rejected(void)
{
    /* CRC-correct store with non-whitespace trailing content -> reject. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"x\",\"title\":\"X\",\"kind\":\"split\",\"flags\":[\"active\"],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}}]}xyz";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_JSON_PARSE,
                   "trailing garbage after root object rejected");
}

static void test_parser_bad_string_escape_rejected(void)
{
    /* `\q` is not a valid JSON escape; lex_string must reject. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"x\",\"title\":\"bad\\qescape\",\"kind\":\"split\",\"flags\":[\"active\"],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}}]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT(rc != BOOT_ENTRIES_OK, "bad string escape \\q rejected");
}

static void test_parser_flags_trailing_comma_rejected(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"x\",\"title\":\"X\",\"kind\":\"split\",\"flags\":[\"active\",],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}}]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_FLAGS,
                   "trailing comma in flags rejected");
}

static void test_parser_entry_object_trailing_comma_rejected(void)
{
    /* Trailing comma after the last entry-object field must be rejected for
     * parity with the host validator's json.loads (RFC 8259 §5). */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"x\",\"title\":\"X\",\"kind\":\"split\",\"flags\":[],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{},}]}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_JSON_PARSE,
                   "trailing comma in entry object rejected");
}

static void test_parser_top_level_trailing_comma_rejected(void)
{
    /* Trailing comma after the last top-level field must be rejected. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"x\",\"title\":\"X\",\"kind\":\"split\",\"flags\":[],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}}],}";
    unsigned int n = load_fixture(JSON);
    boot_entries_parse_result_t r;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, &r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_JSON_PARSE,
                   "trailing comma in top-level object rejected");
}

static void test_parser_fallback_uki(void)
{
    boot_entry_envelope_t e;
    boot_entries_synthesize_fallback(1, &e);
    TEST_ASSERT_EQ(e.kind, BOOT_ENTRY_KIND_UKI, "fallback under UKI mode is kind=uki");
    TEST_ASSERT_EQ(e.flags & BOOT_ENTRY_FLAG_ACTIVE, BOOT_ENTRY_FLAG_ACTIVE,
                   "fallback active flag");
}

static void test_parser_fallback_split(void)
{
    boot_entry_envelope_t e;
    boot_entries_synthesize_fallback(0, &e);
    TEST_ASSERT_EQ(e.kind, BOOT_ENTRY_KIND_SPLIT, "fallback under split mode is kind=split");
}

/* ---- Registration ----------------------------------------------------- */

void test_register_boot_entry_parser(void)
{
    test_suite_register_cat("boot-entries: valid minimal",
                            test_parser_valid_minimal, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: bad schema_version rejected",
                            test_parser_bad_schema_version, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: empty entries rejected",
                            test_parser_no_entries, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: missing payload rejected",
                            test_parser_missing_payload, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: bad CRC rejected",
                            test_parser_bad_crc, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: unknown string kind skipped",
                            test_parser_unknown_string_kind_skipped, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: stable numeric kind unknown rejected",
                            test_parser_stable_numeric_unknown_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: duplicate id rejected",
                            test_parser_duplicate_id, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: non-kebab id rejected",
                            test_parser_id_not_kebab, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: malformed payload value rejected",
                            test_parser_malformed_payload_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: chainload retained under SB (policy demote)",
                            test_parser_chainload_passes_under_secure_boot, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: depth-bomb rejected without crash",
                            test_parser_depth_bomb, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: trailing garbage rejected",
                            test_parser_trailing_garbage_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: bad string escape rejected",
                            test_parser_bad_string_escape_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: flags trailing comma rejected",
                            test_parser_flags_trailing_comma_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: entry-object trailing comma rejected",
                            test_parser_entry_object_trailing_comma_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: top-level trailing comma rejected",
                            test_parser_top_level_trailing_comma_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: fallback UKI synth",
                            test_parser_fallback_uki, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: fallback split synth",
                            test_parser_fallback_split, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
