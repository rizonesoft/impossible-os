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
#include "kernel/test/scratch.h"  /* TEST_SCRATCH_KBUF: ~36 KB sort fixture */

extern void *memset(void *dst, int c, size_t n);  /* freestanding: no <string.h> */

/* Pull the parser implementation into this translation unit. The functions
 * become local to test_boot_entry_parser.o and do not collide with the
 * bootloader's copy (different binaries). */
#include "../../boot/uefi/boot_entries_parser.c"

#include "kernel/types.h"   /* uint32_t for assertions / size_t */

/* Helper: build a CRC-correct fixture by writing a placeholder, computing the
 * CRC, and patching the 8 hex digits in place. Mirrors validate.py --emit-crc. */
static unsigned char s_fixture_buf[2048];

static unsigned int patch_fixture_crc(unsigned char *buf, unsigned int len)
{
    unsigned int zero_off, expected;
    /* Tri-state since section 20: CRC_LOC_MALFORMED is -1, so a plain `!`
     * test would read a broken fixture as a successful locate. */
    lexer_t L;
    if (find_crc_field(&L, buf, len, &zero_off, &expected) != CRC_LOC_FOUND) return 0;
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
        /* Hard-fail rather than silently truncate: a shortened fixture is a
         * DIFFERENT input than the case author wrote, so the case could pass
         * for the wrong reason with nothing on the log to say so. */
        if (n >= sizeof(s_fixture_buf)) {
            TEST_ASSERT(0, "fixture exceeds s_fixture_buf -- grow the buffer");
            break;
        }
        s_fixture_buf[n] = (unsigned char)json[n];
        n++;
    }
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK, "valid minimal store accepted");
    TEST_ASSERT_EQ(r->entry_count, 1u, "one entry parsed");
    TEST_ASSERT_EQ(r->entries[0].kind, BOOT_ENTRY_KIND_SPLIT, "kind=split parsed");
    TEST_ASSERT_EQ(r->entries[0].flags & BOOT_ENTRY_FLAG_ACTIVE, BOOT_ENTRY_FLAG_ACTIVE,
                   "active flag set");
}

static void test_parser_bad_schema_version(void)
{
    static const char JSON[] =
        "{\"schema_version\":99,\"crc32\":\"0x00000000\",\"entries\":[]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_SCHEMA_VERSION,
                   "schema_version != 1 rejected");
}

/* The converted cases in this file hand boot_entries_parse a scratch buffer
 * instead of the stack local they used to declare, so nothing zeroes the
 * output before the call. That is safe ONLY because the parser clears the whole result as its
 * first act, ahead of any input validation (boot_entries_parser.c: zero_buf on
 * entry). Prove that contract rather than assuming it: poison the buffer, then
 * take an EARLY-REJECT path, where a parser that zeroed late (or only on the
 * success path) would leave the poison visible in entry_count. */
static void test_parser_zeroes_output_before_validation(void)
{
    static const char JSON[] =
        "{\"schema_version\":99,\"crc32\":\"0x00000000\",\"entries\":[]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;

    memset(r, 0xAA, sizeof(boot_entries_parse_result_t));
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);

    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_SCHEMA_VERSION,
                   "poisoned output still rejects schema_version 99");
    TEST_ASSERT_EQ(r->entry_count, 0u,
                   "parser zeroed entry_count before validating, not after");
}

static void test_parser_no_entries(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":[]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK,
                   "unknown string kind store accepted overall");
    TEST_ASSERT_EQ(r->entry_count, 0u, "skipped entry not in entries[]");
    TEST_ASSERT_EQ(r->skipped_count, 1u, "skipped count incremented");
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT(rc != BOOT_ENTRIES_OK, "malformed payload value rejected");
}

static void test_parser_empty_machine_id_accepted(void)
{
    /* The boot-policy filter treats empty machine_id as "match any
     * machine"; the parser must accept the empty string in that field
     * without rejecting the store. Real-parser fixture (vs the manual
     * envelope construction in test_boot_policy.c) so a regression in
     * the parser's machine_id branch is caught here. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"global\",\"title\":\"Global\",\"kind\":\"split\","
        "\"flags\":[\"active\"],\"sort_key\":\"00\","
        "\"machine_id\":\"\","
        "\"policy_tags\":[],\"payload\":{}}]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK,
                   "empty machine_id accepted as wildcard");
    TEST_ASSERT_EQ(r->entry_count, 1u, "one entry retained");
    TEST_ASSERT(r->entries[0].machine_id[0] == '\0',
                "empty machine_id preserved as wildcard sentinel");
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 1, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK,
                   "chainload entry retained under Secure Boot for policy filter");
    TEST_ASSERT_EQ(r->entry_count, 1u, "one entry retained");
    TEST_ASSERT_EQ(r->entries[0].kind, BOOT_ENTRY_KIND_CHAINLOAD,
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(buf, p, 0, NULL_PTR, r);
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_JSON_PARSE,
                   "trailing comma in top-level object rejected");
}

/* ---- Repeated / escaped key names ------------------------------------- *
 *
 * A second top-level `entries` key used to reset the per-occurrence cap counter
 * while out->entry_count kept climbing, so the second array wrote past the
 * 64-slot output array. Every fixture below asserts the REJECT and deliberately
 * uses ONE-entry arrays: a fixture that actually reproduced the overflow would
 * corrupt the test runner executing it.
 */

/* The depth fixtures below hardcode bracket counts (6/7/8 here, 11 in the
 * direct rescan test) against the rescan budget. Pin the budget so a change
 * to it fails HERE, naming the cause, instead of surfacing as an opaque
 * "unwalkable prefix" assertion failure. */
_Static_assert(BOOT_ENTRIES_MAX_SCAN_DEPTH == 10u,
               "rescan budget moved -- update the 6/7/8/11-deep fixtures below with it");

#define TEST_ENTRY_A \
    "{\"id\":\"a\",\"title\":\"A\",\"kind\":\"split\",\"flags\":[]," \
    "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\"," \
    "\"policy_tags\":[],\"payload\":{}}"
#define TEST_ENTRY_B \
    "{\"id\":\"b\",\"title\":\"B\",\"kind\":\"split\",\"flags\":[]," \
    "\"sort_key\":\"01\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\"," \
    "\"policy_tags\":[],\"payload\":{}}"

static void test_parser_repeated_entries_key_rejected(void)
{
    /* The memory-safety case: two `entries` arrays. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\","
        "\"entries\":[" TEST_ENTRY_A "],"
        "\"entries\":[" TEST_ENTRY_B "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_DUPLICATE_KEY,
                   "second top-level entries array rejected");
    /* entry_count is deliberately NOT asserted here. The parser documents it as
     * meaningful on BOOT_ENTRIES_OK only (boot_entries_parser.h), and the first
     * array is already parsed when the repeated key is found. Nothing consumes
     * the partial state: boot_policy_decide() short-circuits to
     * FALLBACK_STORE_INVALID on any reject_code before touching entries
     * (boot_policy.c:340), so the reject code IS the contract under test. */
}

static void test_parser_duplicate_id_across_entries_keys_rejected(void)
{
    /* Same id in two `entries` arrays. Before the fix the block-scoped
     * all_ids_count reset, so the duplicate-id gate never saw the collision.
     * The repeated KEY is caught first, which is what closes both holes. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\","
        "\"entries\":[" TEST_ENTRY_A "],"
        "\"entries\":[" TEST_ENTRY_A "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_DUPLICATE_KEY,
                   "duplicate id split across two entries arrays rejected (by the key guard)");
}

static void test_parser_repeated_schema_version_rejected(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"schema_version\":1,\"crc32\":\"0x00000000\","
        "\"entries\":[" TEST_ENTRY_A "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_DUPLICATE_KEY,
                   "repeated schema_version rejected");
}

static void test_parser_repeated_crc32_rejected(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"crc32\":\"0x00000000\","
        "\"entries\":[" TEST_ENTRY_A "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_DUPLICATE_KEY,
                   "repeated crc32 rejected");
}

static void test_parser_repeated_unknown_top_level_key_rejected(void)
{
    /* The rule covers every key, not only the three known ones: an unknown key
     * is skipped, so a repeat carries no meaning either parser could act on,
     * and rejecting it keeps host and firmware in agreement. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"future\":1,\"future\":2,"
        "\"entries\":[" TEST_ENTRY_A "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_DUPLICATE_KEY,
                   "repeated unknown top-level key rejected");
}

static void test_parser_repeated_entry_object_key_rejected(void)
{
    /* One level down, the same shape: saw_id recorded presence only, so a
     * second `id` silently won. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"a\",\"id\":\"b\",\"title\":\"A\",\"kind\":\"split\",\"flags\":[],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}}]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_DUPLICATE_KEY,
                   "repeated key in entry object rejected");
}

static void test_parser_escaped_top_level_key_rejected(void)
{
    /* `\u0065ntries` decodes to `entries` in the host's json.loads but is an
     * unknown key to a raw-byte comparison, and find_crc_field() cannot locate
     * an escaped `crc32` at all. Both parsers reject the spelling. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\","
        "\"\\u0065ntries\":[" TEST_ENTRY_A "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_ESCAPED_KEY,
                   "escaped top-level key name rejected");
}

static void test_parser_escaped_entry_key_rejected(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"\\u0069d\":\"a\",\"title\":\"A\",\"kind\":\"split\",\"flags\":[],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{}}]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_ESCAPED_KEY,
                   "escaped entry key name rejected");
}

static void test_parser_many_distinct_keys_accepted(void)
{
    /* Regression guard for the shape this fix deliberately did NOT take. A
     * fixed seen-key table would have to hard-fail once an object carried more
     * distinct keys than it had slots, turning forward-compat extension keys
     * into a whole-store rejection at boot. The prefix rescan has no ceiling,
     * so 30 distinct unknown keys must still parse. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\","
        "\"k00\":0,\"k01\":0,\"k02\":0,\"k03\":0,\"k04\":0,\"k05\":0,"
        "\"k06\":0,\"k07\":0,\"k08\":0,\"k09\":0,\"k10\":0,\"k11\":0,"
        "\"k12\":0,\"k13\":0,\"k14\":0,\"k15\":0,\"k16\":0,\"k17\":0,"
        "\"k18\":0,\"k19\":0,\"k20\":0,\"k21\":0,\"k22\":0,\"k23\":0,"
        "\"k24\":0,\"k25\":0,\"k26\":0,\"k27\":0,\"k28\":0,\"k29\":0,"
        "\"entries\":[" TEST_ENTRY_A "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK,
                   "30 distinct unknown top-level keys still accepted");
    TEST_ASSERT_EQ((int)r->entry_count, 1, "the single entry is still retained");
}

static void test_parser_prefix_sharing_keys_accepted(void)
{
    /* Control: the rescan must not reject keys that merely SHARE a prefix.
     *
     * ORDER IS LOAD-BEARING. key_seen_before compares `(ce - cs) == key_len &&
     * bytes_eq(...)`, and only the LENGTH clause separates these two keys. With
     * the shorter key first ("sort" then "sort_key") a length-blind compare
     * reads 8 bytes from "sort"'s content start, gets `sort":1,`, and mismatches
     * anyway -- so that ordering passes with the clause DELETED and pins
     * nothing. Longer-first is the ordering that kills the mutant: testing
     * "sort" (4) against the earlier "sort_key" makes a length-blind compare
     * match its first 4 bytes and report a false DUPLICATE, which would send a
     * perfectly valid store to invalid-store fallback. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"sort_key\":1,\"sort\":2,"
        "\"entries\":[" TEST_ENTRY_A "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK,
                   "distinct top-level keys sharing a prefix accepted");
}

/* Repeated `entries` behind a DEEPLY NESTED entry extension. The first version
 * of key_seen_before() charged the entries array and the entry object to the
 * same depth budget the main parser reserves for the payload alone, so a store
 * the parser accepted could exhaust the rescan -- which then answered "no
 * duplicate" and let the overflow straight back in. Depths 6, 7 and 8 bracket
 * the old limit: 6 passed even before the fix, 7 and 8 did not. */
#define TEST_ENTRY_NEST6 \
    "{\"id\":\"a\",\"title\":\"A\",\"kind\":\"split\",\"flags\":[]," \
    "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\"," \
    "\"policy_tags\":[],\"ext\":[[[[[[0]]]]]],\"payload\":{}}"
#define TEST_ENTRY_NEST7 \
    "{\"id\":\"a\",\"title\":\"A\",\"kind\":\"split\",\"flags\":[]," \
    "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\"," \
    "\"policy_tags\":[],\"ext\":[[[[[[[0]]]]]]],\"payload\":{}}"
#define TEST_ENTRY_NEST8 \
    "{\"id\":\"a\",\"title\":\"A\",\"kind\":\"split\",\"flags\":[]," \
    "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\"," \
    "\"policy_tags\":[],\"ext\":[[[[[[[[0]]]]]]]],\"payload\":{}}"

static void test_parser_repeated_entries_behind_nest6(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\","
        "\"entries\":[" TEST_ENTRY_NEST6 "],"
        "\"entries\":[" TEST_ENTRY_B "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_DUPLICATE_KEY,
                   "repeated entries behind 6-deep extension rejected");
}

static void test_parser_repeated_entries_behind_nest7(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\","
        "\"entries\":[" TEST_ENTRY_NEST7 "],"
        "\"entries\":[" TEST_ENTRY_B "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_DUPLICATE_KEY,
                   "repeated entries behind 7-deep extension rejected");
}

static void test_parser_repeated_entries_behind_nest8(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\","
        "\"entries\":[" TEST_ENTRY_NEST8 "],"
        "\"entries\":[" TEST_ENTRY_B "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_DUPLICATE_KEY,
                   "repeated entries behind 8-deep extension rejected");
}

static void test_parser_deep_extension_single_entries_accepted(void)
{
    /* Control for the three above: the SAME deep extension with only ONE
     * entries array must still be ACCEPTED. Without this, the fix could have
     * "passed" by rejecting deep stores outright, which would be a functional
     * regression wearing a security fix's clothes. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\","
        "\"entries\":[" TEST_ENTRY_NEST8 "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK,
                   "8-deep extension with one entries array still accepted");
    TEST_ASSERT_EQ((int)r->entry_count, 1, "its entry is retained");
}

/* The firmware's value-depth budget is 8 containers measured FROM the skipped
 * value, and the host validator now mirrors exactly that (validate.py
 * validate_value_depths). These two cases pin the boundary from the firmware
 * side so the two implementations cannot drift apart silently: the host has a
 * matching depth-8-accept / depth-9-reject pair. Before this, a nine-deep
 * extension validated on the host and then booted to invalid-store fallback. */
static void test_parser_entry_ext_depth8_accepted(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"a\",\"title\":\"A\",\"kind\":\"split\",\"flags\":[],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"ext\":[[[[[[[[0]]]]]]]],\"payload\":{}}]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK,
                   "entry extension nested 8 deep accepted");
}

static void test_parser_entry_ext_depth9_rejected(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"a\",\"title\":\"A\",\"kind\":\"split\",\"flags\":[],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"ext\":[[[[[[[[[0]]]]]]]]],\"payload\":{}}]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_JSON_PARSE,
                   "entry extension nested 9 deep rejected");
}

/* ---- Guards that ordinary input cannot reach ---------------------------- *
 *
 * The fail-closed rescan result and the bounded append are backstops: through
 * boot_entries_parse() the key guard fires first, so no store fixture exercises
 * them. A backstop no test can fail is one that can be deleted without anyone
 * noticing, so these call the helpers directly. The parser source is #included
 * into this translation unit, which is what makes its statics reachable.
 */
static void test_parser_rescan_reports_failure_not_absence(void)
{
    /* A prefix the rescan cannot walk must answer KEY_SCAN_FAILED, never
     * KEY_SCAN_ABSENT. Conflating the two is what made the first version of the
     * duplicate check bypassable. Here the first value nests deeper than the
     * rescan budget, so the skipper gives up part-way through the prefix. */
    static const char RAW[] =
        "{\"a\":[[[[[[[[[[[0]]]]]]]]]]],\"b\":1}";
    lexer_t L;
    lex_init(&L, (const u8 *)RAW, (unsigned int)(sizeof(RAW) - 1u));
    /* Body starts just past the opening brace; "b" is the key under test. */
    u32 body_pos = 1u;
    u32 b_key_start = 0u;
    {
        u32 i;
        for (i = 0; i + 3u < (u32)(sizeof(RAW) - 1u); i++) {
            if (RAW[i] == '"' && RAW[i + 1] == 'b' && RAW[i + 2] == '"') {
                b_key_start = i;
                break;
            }
        }
    }
    TEST_ASSERT(b_key_start != 0u, "located the second key in the fixture");
    int rc = key_seen_before(&L, body_pos, b_key_start, (const u8 *)"b", 1u);
    TEST_ASSERT_EQ(rc, KEY_SCAN_FAILED,
                   "an unwalkable prefix reports FAILED, not ABSENT");
}

static void test_parser_rescan_finds_and_misses_correctly(void)
{
    /* Controls for the case above: over a WALKABLE prefix the same helper must
     * return DUPLICATE for a repeat and ABSENT for a fresh key. Without these,
     * a helper that returned FAILED unconditionally would pass the test above. */
    static const char RAW[] = "{\"a\":1,\"b\":2,\"a\":3}";
    lexer_t L;
    lex_init(&L, (const u8 *)RAW, (unsigned int)(sizeof(RAW) - 1u));
    u32 second_a = 13u;    /* offset of the third key's opening quote */
    TEST_ASSERT(RAW[second_a] == '"' && RAW[second_a + 1] == 'a',
                "fixture offset points at the repeated key");
    TEST_ASSERT_EQ(key_seen_before(&L, 1u, second_a, (const u8 *)"a", 1u),
                   KEY_SCAN_DUPLICATE, "repeat found in a walkable prefix");
    TEST_ASSERT_EQ(key_seen_before(&L, 1u, second_a, (const u8 *)"z", 1u),
                   KEY_SCAN_ABSENT, "fresh key reported absent");
}

static void test_parser_skip_value_depth_budget(void)
{
    /* The budget is what the rescan fix turns on, so it gets its own test:
     * a container value must be refused at budget 0 and accepted at 1. */
    static const char RAW[] = "[0]";
    lexer_t L;
    lex_init(&L, (const u8 *)RAW, (unsigned int)(sizeof(RAW) - 1u));
    TEST_ASSERT(lex_next(&L) && L.kind == TOK_LBRACKET, "positioned on '['");
    TEST_ASSERT_EQ(skip_value_depth(&L, 0u), 0, "budget 0 refuses a container");

    lex_init(&L, (const u8 *)RAW, (unsigned int)(sizeof(RAW) - 1u));
    TEST_ASSERT(lex_next(&L) && L.kind == TOK_LBRACKET, "positioned on '['");
    TEST_ASSERT_EQ(skip_value_depth(&L, 1u), 1, "budget 1 skips a flat array");
}

static void test_parser_entries_retain_bound(void)
{
    /* 63 -> 64 succeeds, 64 -> refused, and a refusal must not write or count. */
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    memset(r, 0, sizeof(*r));
    boot_entry_envelope_t e;
    memset(&e, 0, sizeof(e));

    r->entry_count = BOOT_ENTRIES_MAX_ENTRIES - 1u;
    e.kind = 7;
    TEST_ASSERT_EQ(entries_retain(r, &e), 1, "the last free slot accepts");
    TEST_ASSERT_EQ((int)r->entry_count, (int)BOOT_ENTRIES_MAX_ENTRIES,
                   "count advanced to the cap");
    TEST_ASSERT_EQ((int)r->entries[BOOT_ENTRIES_MAX_ENTRIES - 1u].kind, 7,
                   "the entry landed in the last slot");

    e.kind = 9;
    TEST_ASSERT_EQ(entries_retain(r, &e), 0, "a full array refuses");
    TEST_ASSERT_EQ((int)r->entry_count, (int)BOOT_ENTRIES_MAX_ENTRIES,
                   "a refused append does not advance the count");
    TEST_ASSERT_EQ((int)r->entries[BOOT_ENTRIES_MAX_ENTRIES - 1u].kind, 7,
                   "a refused append does not overwrite the last slot");
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

/* ---- BLS display order (boot_entries_bls_sort) ------------------------ */

/* Set the three display-order keys of a zeroed envelope. id/sort_key/machine_id
 * are fixed char arrays; copy clamped to capacity, NUL-terminated. */
static void bls_set(boot_entry_envelope_t *e, const char *id,
                    const char *sort_key, const char *machine_id)
{
    unsigned int i;
    for (i = 0; i + 1u < sizeof(e->id) && id[i]; i++) e->id[i] = id[i];
    e->id[i] = '\0';
    for (i = 0; i + 1u < sizeof(e->sort_key) && sort_key[i]; i++) e->sort_key[i] = sort_key[i];
    e->sort_key[i] = '\0';
    for (i = 0; i + 1u < sizeof(e->machine_id) && machine_id[i]; i++) e->machine_id[i] = machine_id[i];
    e->machine_id[i] = '\0';
}

static int bls_streq(const char *a, const char *b)
{
    unsigned int k = 0;
    while (a[k] == b[k] && a[k] != 0) k++;
    return a[k] == b[k];
}

/* sort_key is the primary key; empty sort_key (the "match any"/unset value)
 * sorts before any non-empty key. id breaks ties when sort_key is equal. The
 * entries[] array is deliberately built out of display order. */
/* The bls sort fixture is ~36 KB. It was one shared file-scope static because
 * two function-local statics of that size pushed the image over the 0x800000
 * user-base ceiling; it is now a per-case TEST_SCRATCH_KBUF allocation, which
 * keeps the bytes out of the kernel image entirely and gives each case its own
 * fixture. The explicit zeroing reproduces the old BSS-zero start state, which
 * the sort relies on for every field the cases do not set. */
static void test_bls_sort_by_sort_key_then_id(void)
{
    TEST_SCRATCH_KBUF(fixbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)fixbuf;

    memset(r, 0, sizeof(boot_entries_parse_result_t));
    bls_set(&r->entries[0], "zeta",  "20", "m1");
    bls_set(&r->entries[1], "alpha", "10", "m1");
    bls_set(&r->entries[2], "beta",  "20", "m1");
    bls_set(&r->entries[3], "gamma", "",   "m1");
    r->entry_count = 4;

    unsigned int idx[4] = {0, 1, 2, 3};
    boot_entries_bls_sort(r, idx, 4u);

    TEST_ASSERT(bls_streq(r->entries[idx[0]].id, "gamma"), "empty sort_key sorts first");
    TEST_ASSERT(bls_streq(r->entries[idx[1]].id, "alpha"), "sort_key 10 second");
    TEST_ASSERT(bls_streq(r->entries[idx[2]].id, "beta"),  "sort_key 20 tiebreak id beta before zeta");
    TEST_ASSERT(bls_streq(r->entries[idx[3]].id, "zeta"),  "sort_key 20 tiebreak id zeta last");
}

/* machine_id is the secondary key: equal sort_key falls through to machine_id
 * before id. */
static void test_bls_sort_machine_id_tiebreak(void)
{
    TEST_SCRATCH_KBUF(fixbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)fixbuf;

    memset(r, 0, sizeof(boot_entries_parse_result_t));
    bls_set(&r->entries[0], "id-a", "10", "mb");
    bls_set(&r->entries[1], "id-b", "10", "ma");
    r->entry_count = 2;

    unsigned int idx[2] = {0, 1};
    boot_entries_bls_sort(r, idx, 2u);

    TEST_ASSERT(bls_streq(r->entries[idx[0]].machine_id, "ma"), "machine_id tiebreak: ma first");
    TEST_ASSERT(bls_streq(r->entries[idx[1]].machine_id, "mb"), "machine_id tiebreak: mb second");
}

/* ---- health_check_subset (per-entry health-gate override) ------------- */

static void test_parser_health_subset_valid(void)
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
        "\"health_check_subset\":[\"desktop_ready\",\"no_panic\"],"
        "\"payload\":{\"kernel\":\"\\\\EFI\\\\ImpossibleOS\\\\kernel.exe\"}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK, "valid health_check_subset accepted");
    TEST_ASSERT_EQ(r->entries[0].health_check_subset_count, 2u,
                   "count=2 retained");
    TEST_ASSERT_EQ((unsigned int)r->entries[0].health_check_subset[0][0], (unsigned int)'d',
                   "first name first byte is 'd'");
}

static void test_parser_health_subset_absent_accepted(void)
{
    /* Field is optional; absent => count=0. */
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
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK, "absent subset accepted");
    TEST_ASSERT_EQ(r->entries[0].health_check_subset_count, 0u,
                   "absent => count=0");
}

static void test_parser_health_subset_rejects_non_array(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"slot-a\",\"title\":\"Slot A\",\"kind\":\"split\","
        "\"flags\":[\"active\"],\"sort_key\":\"00\","
        "\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],"
        "\"health_check_subset\":\"not-an-array\","
        "\"payload\":{\"kernel\":\"\\\\EFI\\\\ImpossibleOS\\\\kernel.exe\"}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                   "non-array subset rejected");
}

static void test_parser_health_subset_rejects_overflow(void)
{
    /* 9 names exceeds the 8 cap. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"slot-a\",\"title\":\"Slot A\",\"kind\":\"split\","
        "\"flags\":[\"active\"],\"sort_key\":\"00\","
        "\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],"
        "\"health_check_subset\":[\"n1\",\"n2\",\"n3\",\"n4\",\"n5\",\"n6\",\"n7\",\"n8\",\"n9\"],"
        "\"payload\":{\"kernel\":\"\\\\EFI\\\\ImpossibleOS\\\\kernel.exe\"}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                   "subset count > 8 rejected");
}

static void test_parser_health_subset_rejects_empty_name(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"slot-a\",\"title\":\"Slot A\",\"kind\":\"split\","
        "\"flags\":[\"active\"],\"sort_key\":\"00\","
        "\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],"
        "\"health_check_subset\":[\"\"],"
        "\"payload\":{\"kernel\":\"\\\\EFI\\\\ImpossibleOS\\\\kernel.exe\"}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                   "empty subset name rejected");
}

static void test_parser_health_subset_rejects_oversize_name(void)
{
    /* 24-byte name exceeds the 23-char cap (24 includes terminator). */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"slot-a\",\"title\":\"Slot A\",\"kind\":\"split\","
        "\"flags\":[\"active\"],\"sort_key\":\"00\","
        "\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],"
        "\"health_check_subset\":[\"aaaaaaaaaaaaaaaaaaaaaaaa\"],"
        "\"payload\":{\"kernel\":\"\\\\EFI\\\\ImpossibleOS\\\\kernel.exe\"}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                   "subset name longer than 23 chars rejected");
}

static void test_parser_health_subset_rejects_backslash_in_name(void)
{
    /* A backslash in a name would parse as a JSON escape; "\\u0001"
     * decodes to a control char which the parser must reject under the
     * printable-ASCII grammar. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"slot-a\",\"title\":\"Slot A\",\"kind\":\"split\","
        "\"flags\":[\"active\"],\"sort_key\":\"00\","
        "\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],"
        "\"health_check_subset\":[\"bad\\u0001ctrl\"],"
        "\"payload\":{\"kernel\":\"\\\\EFI\\\\ImpossibleOS\\\\kernel.exe\"}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                   "control char in subset name rejected");
}

static void test_parser_health_subset_rejects_trailing_comma(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":["
        "{\"id\":\"slot-a\",\"title\":\"Slot A\",\"kind\":\"split\","
        "\"flags\":[\"active\"],\"sort_key\":\"00\","
        "\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],"
        "\"health_check_subset\":[\"desktop_ready\",],"
        "\"payload\":{\"kernel\":\"\\\\EFI\\\\ImpossibleOS\\\\kernel.exe\"}}"
        "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                   "subset trailing comma rejected");
}

/* ---- Registration ----------------------------------------------------- */

/* ---- Structural CRC-header locate (section 20) ------------------------- */

/* Independent oracle for "which bytes are the real header": the offset of the 8
 * hex digits of the ONLY `"crc32":"0x` pair written with no space. Every decoy
 * below is spelled differently on purpose (a plain string value, or a nested
 * member written WITH a space), so this scanner cannot match one. Deliberately a
 * different rule from find_crc_field(): an oracle that shared the locator's
 * logic would agree with it even when both are wrong. */
static unsigned int oracle_crc_span(const unsigned char *raw, unsigned int len)
{
    static const char NEEDLE[] = "\"crc32\":\"0x";
    unsigned int nlen = s_len(NEEDLE), i, hits = 0, off = 0;
    if (len < nlen) return 0;
    for (i = 0; i + nlen <= len; i++) {
        if (bytes_eq(raw + i, (const u8 *)NEEDLE, nlen)) { hits++; off = i + nlen; }
    }
    TEST_ASSERT_EQ(hits, 1u, "oracle needle must be unique in the fixture");
    return off;
}

/* Locate must land on the ROOT header, proven against the oracle rather than
 * against a successful parse: the fixture's CRC is patched at whatever span the
 * locator reports, so a consistently-wrong locator still produces a store that
 * verifies. */
static void assert_locates_root(const char *json, const char *what)
{
    unsigned int n = load_fixture(json);
    unsigned int zero_off = 0, expected = 0;
    lexer_t L;
    crc_loc_t loc = find_crc_field(&L, s_fixture_buf, n, &zero_off, &expected);
    TEST_ASSERT_EQ(loc, CRC_LOC_FOUND, what);
    TEST_ASSERT_EQ(zero_off, oracle_crc_span(s_fixture_buf, n), what);
}

/* The pre-section-20 value-blind byte scan, kept HERE (never in the parser) as the
 * control's oracle. Without it the control is a manual revert somebody has to
 * remember to perform, which is not a regression test at all: a decoy fixture that
 * fails to reproduce the old defect looks exactly like one that reproduces it. Both
 * decoy fixtures below were wrong on the first draft and this scan is what says so. */
static crc_loc_t old_string_match_locate(const u8 *raw, u32 len, u32 *zero_off)
{
    static const char KEY[] = "\"crc32\"";
    u32 i, p;
    for (i = 0; i + 7u <= len; i++) {
        u32 k; int match = 1;
        for (k = 0; k < 7u; k++) if (raw[i + k] != (u8)KEY[k]) { match = 0; break; }
        if (!match) continue;
        p = i + 7u;
        while (p < len && (raw[p] == ' ' || raw[p] == '\t' ||
                           raw[p] == '\r' || raw[p] == '\n')) p++;
        if (p >= len || raw[p] != ':') return CRC_LOC_ABSENT;
        p++;
        while (p < len && (raw[p] == ' ' || raw[p] == '\t' ||
                           raw[p] == '\r' || raw[p] == '\n')) p++;
        if (p >= len || raw[p] != '"') return CRC_LOC_ABSENT;
        p++;
        if (p + 10u > len) return CRC_LOC_ABSENT;
        if (raw[p] != '0' || (raw[p + 1] != 'x' && raw[p + 1] != 'X')) return CRC_LOC_ABSENT;
        /* The original validated the eight digits before reporting success. A
         * control that skips this is MORE PERMISSIVE than the code it stands
         * for, and would certify agreement on a malformed value the real
         * predecessor rejected. */
        {
            unsigned int scratch;
            if (!parse_hex8(raw, p + 2u, &scratch)) return CRC_LOC_ABSENT;
        }
        *zero_off = p + 2u;
        return CRC_LOC_FOUND;
    }
    return CRC_LOC_ABSENT;
}

/* A decoy fixture EARNS its place by breaking the old scan. This asserts the old
 * scan either refuses the store or picks a different span, and that it agrees on a
 * store carrying no decoy at all -- otherwise the control is not measuring the
 * decoy. */
static void assert_old_scan_broke(const char *json, const char *what)
{
    unsigned int n = load_fixture(json);
    unsigned int old_off = 0;
    crc_loc_t old_loc = old_string_match_locate(s_fixture_buf, n, &old_off);
    unsigned int real = oracle_crc_span(s_fixture_buf, n);
    TEST_ASSERT(old_loc != CRC_LOC_FOUND || old_off != real, what);
}

static void test_parser_crc_locate_control(void)
{
    /* CONTROL: with no decoy present the locate must succeed and match the
     * oracle. If this fails, every decoy result below means nothing. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0x00000000\",\"entries\":[" TEST_ENTRY_A "]}";
    assert_locates_root(JSON, "plain store locates the root header");
    {   /* The control must AGREE here, or it is not measuring the decoys. */
        unsigned int n0 = load_fixture(JSON), old_off = 0;
        TEST_ASSERT_EQ(old_string_match_locate(s_fixture_buf, n0, &old_off), CRC_LOC_FOUND,
                       "CONTROL: the old scan locates a plain store");
        TEST_ASSERT_EQ(old_off, oracle_crc_span(s_fixture_buf, n0),
                       "CONTROL: the old scan agrees on a plain store");
    }
    {   /* CONTROL FIDELITY: the old scan rejected a non-hex value, and so must
         * the copy of it. Without this the omission above is invisible.
         * Built by MUTATING the fixture already in the buffer rather than by
         * embedding a second store: a whole extra literal costs `.rodata` in
         * the linked image for one assertion, and test strings are the
         * cheapest thing in this kernel to grow without noticing. */
        unsigned int nb = load_fixture(JSON), off = 0;
        unsigned int span = oracle_crc_span(s_fixture_buf, nb);
        s_fixture_buf[span] = (unsigned char)'G';
        s_fixture_buf[span + 1u] = (unsigned char)'G';
        TEST_ASSERT_EQ(old_string_match_locate(s_fixture_buf, nb, &off), CRC_LOC_ABSENT,
                       "CONTROL: the old scan rejected a non-hex crc32 value");
        lexer_t L;
        TEST_ASSERT_EQ(find_crc_field(&L, s_fixture_buf, nb, &off, &span), CRC_LOC_ABSENT,
                       "the structural locate rejects a non-hex value too");
    }
    /* Position among the root keys, and whitespace in the gaps, must not matter:
     * the locate is token-driven, not offset-driven. */
    static const char FIRST_KEY[] =
        "{\"crc32\":\"0x00000000\",\"schema_version\":1,\"entries\":[" TEST_ENTRY_A "]}";
    assert_locates_root(FIRST_KEY, "header as the first root key");
    static const char SPACED[] =
        "{\"schema_version\" : 1 ,\n\t\"crc32\" : \"0x00000000\" ,"
        "\"entries\":[" TEST_ENTRY_A "]}";
    unsigned int n = load_fixture(SPACED);
    unsigned int zero_off = 0, expected = 0;
    lexer_t L;
    TEST_ASSERT_EQ(find_crc_field(&L, s_fixture_buf, n, &zero_off, &expected), CRC_LOC_FOUND,
                   "whitespace around the key, colon and value tolerated");
}

static void test_parser_crc_decoy_string_value(void)
{
    /* The defect: `crc32` inside a string VALUE ahead of the real header. The
     * old byte scan matched here, looked for a colon, found a comma, and the
     * store was rejected as a bad CRC field before any key was parsed. */
    static const char JSON[] =
        "{\"note\":\"crc32\",\"schema_version\":1,"
        "\"crc32\":\"0x00000000\",\"entries\":[" TEST_ENTRY_A "]}";
    assert_locates_root(JSON, "decoy string value before the header");
    assert_old_scan_broke(JSON, "CONTROL: the old scan broke on this decoy");
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK, "store with a decoy value accepted");
    TEST_ASSERT_EQ(r->entry_count, 1u, "decoy store still parses its entry");
}

static void test_parser_crc_decoy_nested_member(void)
{
    /* The same shape one level down: a crc32 member inside `payload`. It sits
     * BEFORE the root header in the byte stream, which is what makes it a decoy
     * -- the old scan took the first `"crc32"` it met. Written with a space
     * after the colon so the oracle needle stays unique. */
    static const char JSON[] =
        "{\"schema_version\":1,\"entries\":["
        "{\"id\":\"a\",\"title\":\"A\",\"kind\":\"split\",\"flags\":[],"
        "\"sort_key\":\"00\",\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],\"payload\":{\"crc32\": \"0x11223344\"}}],"
        "\"crc32\":\"0x00000000\"}";
    assert_locates_root(JSON, "nested crc32 member inside payload");
    assert_old_scan_broke(JSON, "CONTROL: the old scan broke on this decoy");
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK, "store with a nested crc32 accepted");
}

static void test_parser_crc_decoy_escaped_and_array(void)
{
    /* An escaped-quote decoy carries `\"crc32\": \"0x11223344\"` inside a string
     * value; an array-contained decoy hides the same pair one level down in a list
     * rather than an object. Only the ARRAY one breaks the old scan, and the
     * control below says so rather than assuming it: in `\"crc32\"` a backslash
     * sits where the old seven-byte needle wanted the closing quote, so the old
     * scan walked straight past it. The escaped case still matters here because
     * the NEW locator must not match an escaped spelling in a value either. */
    static const char ESCQ[] =
        "{\"q\":\"\\\"crc32\\\": \\\"0x11223344\\\"\",\"schema_version\":1,"
        "\"crc32\":\"0x00000000\",\"entries\":[" TEST_ENTRY_A "]}";
    static const char ARR[] =
        "{\"list\":[{\"crc32\": \"0x11223344\"}],\"schema_version\":1,"
        "\"crc32\":\"0x00000000\",\"entries\":[" TEST_ENTRY_A "]}";
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;

    assert_locates_root(ESCQ, "escaped-quote decoy before the header");
    {   /* CONTROL: the old scan AGREED here, so this fixture is a locate case and
         * not a regression case. Asserting a break would assert a bug that never
         * existed, and the fixture would pass for the wrong reason. */
        unsigned int n0 = load_fixture(ESCQ), old_off = 0;
        TEST_ASSERT_EQ(old_string_match_locate(s_fixture_buf, n0, &old_off), CRC_LOC_FOUND,
                       "CONTROL: the old scan skipped the escaped spelling");
        TEST_ASSERT_EQ(old_off, oracle_crc_span(s_fixture_buf, n0),
                       "CONTROL: the old scan agreed on the escaped-quote store");
    }
    unsigned int n = load_fixture(ESCQ);
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK, "escaped-quote decoy store accepted");

    assert_locates_root(ARR, "array-contained decoy before the header");
    assert_old_scan_broke(ARR, "CONTROL: the old scan broke on the array decoy");
    n = load_fixture(ARR);
    rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK, "array-contained decoy store accepted");
}

static void test_parser_crc_header_after_deep_value(void)
{
    /* The locate skips ROOT values, two container levels further out than the
     * authoritative walk ever skips from, so it gets the RESCAN budget. A root
     * value at that budget must still leave the header findable. */
    static const char JSON[] =
        "{\"pad\":[[[[[[[[[[0]]]]]]]]]],\"schema_version\":1,"
        "\"crc32\":\"0x00000000\",\"entries\":[" TEST_ENTRY_A "]}";
    assert_locates_root(JSON, "header after a 10-container root value");
}

static void test_parser_crc_header_after_over_budget_value(void)
{
    /* One container past the budget: the locate cannot get to the header, and
     * the store is a JSON problem rather than a missing-header one. */
    static const char JSON[] =
        "{\"pad\":[[[[[[[[[[[0]]]]]]]]]]],\"schema_version\":1,"
        "\"crc32\":\"0x00000000\",\"entries\":[" TEST_ENTRY_A "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_JSON_PARSE,
                   "root value over the scan budget rejected as malformed");
}

static void test_parser_crc_uppercase_prefix_rejected(void)
{
    /* `0X` has no agreed 8-byte span: the host validator requires a lowercase
     * `0x` in its whole-store check, so accepting it here would mean the two
     * sides disagree about which bytes carry the CRC. */
    static const char JSON[] =
        "{\"schema_version\":1,\"crc32\":\"0XAABBCCDD\",\"entries\":[" TEST_ENTRY_A "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD,
                   "uppercase 0X prefix rejected");
}

static void test_parser_crc_value_shapes_rejected(void)
{
    /* An over-long value and a non-string value are both "no usable header",
     * not "malformed": the root object itself is fine. */
    static const char LONG_VAL[] =
        "{\"schema_version\":1,\"crc32\":\"0xAABBCCDDEE\",\"entries\":[" TEST_ENTRY_A "]}";
    static const char NUM_VAL[] =
        "{\"schema_version\":1,\"crc32\":2864434397,\"entries\":[" TEST_ENTRY_A "]}";
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    unsigned int n = load_fixture(LONG_VAL);
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD, "over-long crc32 value rejected");
    n = load_fixture(NUM_VAL);
    rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD, "non-string crc32 value rejected");
    /* `0x\u0041ABBCCDD` decodes to `0xAABBCCDD` and is 14 RAW bytes, so there is no
     * agreed 8-byte span for the producer to patch. Absent, not malformed. */
    static const char ESC_VAL[] =
        "{\"schema_version\":1,\"crc32\":\"0x\\u0041ABBCCDD\",\"entries\":["
        TEST_ENTRY_A "]}";
    n = load_fixture(ESC_VAL);
    rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD, "escaped crc32 value rejected");
}

static void test_parser_crc_malformed_before_header(void)
{
    /* Root grammar broken ahead of the header. Before this section a broken
     * prefix could still reach the CRC check and be reported as a mismatch;
     * naming it a parse error is the honest code. */
    static const char JSON[] =
        "{\"note\" \"x\",\"crc32\":\"0x00000000\",\"entries\":[" TEST_ENTRY_A "]}";
    unsigned int n = load_fixture(JSON);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_JSON_PARSE,
                   "malformed root before the header rejected as a parse error");
}

static void test_parser_crc_broken_root_is_malformed(void)
{
    /* The tri-state's whole claim is that it tells a BROKEN root object apart from
     * one that merely carries no crc32 member. These three shapes are where that
     * claim was false: a trailing comma, and truncation after the crc32 colon or
     * after a root comma, all reported ABSENT because `}` was accepted at both the
     * first-key and after-comma positions and because lex_next reports EOF as a
     * successful token read. Found by the boot-quality auditor. */
    static const char TRAILING_COMMA[] = "{\"schema_version\":1,}";
    static const char TRUNC_COLON[] = "{\"crc32\":";
    static const char TRUNC_COMMA[] = "{\"schema_version\":1,";
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    unsigned int n = load_fixture(TRAILING_COMMA);
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_JSON_PARSE, "trailing comma at root is malformed");
    n = load_fixture(TRUNC_COLON);
    rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_JSON_PARSE, "truncation after the crc32 colon");
    n = load_fixture(TRUNC_COMMA);
    rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_JSON_PARSE, "truncation after a root comma");
}

static void test_parser_crc_absent_root_member(void)
{
    static const char JSON[] =
        "{\"schema_version\":1,\"entries\":[" TEST_ENTRY_A "]}";
    static const char EMPTY[] = "{}";
    static const char ESC_KEY[] =
        "{\"\\u0063rc32\":\"0x00000000\",\"schema_version\":1,\"entries\":["
        TEST_ENTRY_A "]}";
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    unsigned int n = load_fixture(JSON);
    int rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD, "no root crc32 member rejected");
    n = load_fixture(EMPTY);
    rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD, "empty root object rejected");
    /* The key is compared RAW, so an escaped spelling is a different key and the
     * header is simply absent -- the same conclusion the host reaches. */
    n = load_fixture(ESC_KEY);
    rc = boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD, "escaped crc32 key rejected");
}


/* ---- Reject vocabulary + the notice's false-positive gate (TODO-03 s22) --- */

/* Local strcmp: the parser TU is freestanding and pulls in no libc. */
static int rn_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* Every reject code must have its own name. A missing switch arm would fall
 * through to "UNKNOWN", which is exactly the failure a user sees as a notice
 * that cannot say what went wrong -- so the control below asserts that an
 * out-of-range code DOES reach "UNKNOWN", proving the probe can detect the
 * fall-through it is looking for. */
static void test_reject_name_table_complete(void)
{
    const int last = (int)BOOT_ENTRIES_REJECT_ESCAPED_KEY;
    for (int c = 0; c <= last; c++) {
        const char *n = boot_entries_reject_name(c);
        TEST_ASSERT_EQ(n != (const char *)0, 1, "reject name never NULL");
        TEST_ASSERT_EQ(n[0] != 0, 1, "reject name never empty");
        TEST_ASSERT_EQ(rn_streq(n, "UNKNOWN"), 0, "in-range code has a real name");
    }
    /* Names are distinct -- a copy-paste arm returning a neighbour's token
     * would misreport the cause on screen. */
    for (int a = 0; a <= last; a++)
        for (int b = a + 1; b <= last; b++)
            TEST_ASSERT_EQ(rn_streq(boot_entries_reject_name(a),
                                    boot_entries_reject_name(b)), 0,
                           "reject names are distinct");
    /* CONTROL: the fall-through arm is reachable, so the assertions above
     * are testing something. */
    TEST_ASSERT_EQ(rn_streq(boot_entries_reject_name(last + 1), "UNKNOWN"), 1,
                   "out-of-range code reports UNKNOWN");
}

/* The cause strings are what the user actually reads, so they must exist for
 * every code and must not be the generic fallback sentence. */
static void test_reject_cause_table_complete(void)
{
    const int last = (int)BOOT_ENTRIES_REJECT_ESCAPED_KEY;
    const char *generic = boot_entries_reject_cause(last + 1);
    TEST_ASSERT_EQ(generic != (const char *)0, 1, "fallback cause never NULL");
    for (int c = 0; c <= last; c++) {
        const char *m = boot_entries_reject_cause(c);
        TEST_ASSERT_EQ(m != (const char *)0, 1, "cause never NULL");
        TEST_ASSERT_EQ(m[0] != 0, 1, "cause never empty");
        TEST_ASSERT_EQ(rn_streq(m, generic), 0, "in-range code has a real cause");
    }
}

/* One rejected store, and where the parser says the problem is. */
static void check_position(const char *json, int code, unsigned int line, unsigned int column,
                           const char *locator)
{
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    char text[BOOT_ENTRIES_REJECT_LOCATOR_LEN];
    unsigned int n = load_fixture(json);
    TEST_ASSERT_EQ(boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r), code, "reject code");
    TEST_ASSERT_EQ(r->reject_line, line, "reject line");
    TEST_ASSERT_EQ(r->reject_column, column, "reject column");
    boot_entries_reject_locator(r, text, sizeof(text));
    TEST_ASSERT_EQ(rn_streq(text, locator), 1, "locator text");
}

/* A rejected store tells the user where to look. */
static void test_reject_position(void)
{
    /* A bad value on its own line. */
    check_position("{\n\"crc32\":\"0x00000000\",\n  \"schema_version\":99,\n\"entries\":[]}",
                   BOOT_ENTRIES_REJECT_BAD_SCHEMA_VERSION, 3u, 20u,
                   "BAD_SCHEMA_VERSION at line 3, col 20");
    /* Tokens the lexer cannot read: none of them may report the previous token. */
    check_position("{\n\"crc32\":\"0x00000000\",\n@}",
                   BOOT_ENTRIES_REJECT_JSON_PARSE, 3u, 1u, "JSON_PARSE at line 3, col 1");
    check_position("{\"crc32\":\"0x00000000\",\n\"schema_version\":\ntruX}",
                   BOOT_ENTRIES_REJECT_JSON_PARSE, 3u, 1u, "JSON_PARSE at line 3, col 1");
    check_position("{\"crc32\":\"0x00000000\",\n\"x\":\n  \"abc",
                   BOOT_ENTRIES_REJECT_JSON_PARSE, 3u, 3u, "JSON_PARSE at line 3, col 3");
    /* The same mistakes BEFORE the checksum header, found by the header scan. */
    check_position("{\n\"a\":\n@,\n\"crc32\":\"0x00000000\"}",
                   BOOT_ENTRIES_REJECT_JSON_PARSE, 3u, 1u, "JSON_PARSE at line 3, col 1");
    check_position("{\n\"a\":\ntruX,\n\"crc32\":\"0x00000000\"}",
                   BOOT_ENTRIES_REJECT_JSON_PARSE, 3u, 1u, "JSON_PARSE at line 3, col 1");
    /* A malformed header points at its own value. */
    check_position("{\n\"crc32\":\n  \"zz\"}",
                   BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD, 3u, 3u, "BAD_CRC32_FIELD at line 3, col 3");
}

#define POS_ENTRY(id) \
    "{\"id\":\"" id "\",\"title\":\"T\",\"kind\":\"split\",\"flags\":[],\"sort_key\":\"00\"," \
    "\"machine_id\":\"\",\"policy_tags\":[],\"payload\":{\"kernel\":\"k\"}}"

/* Rejects raised after the walk moved on point at the value that caused them. */
static void test_reject_position_after_walk(void)
{
    /* The empty array, not the fields and blank lines after it. */
    check_position("{\"crc32\":\"0x00000000\",\n\"entries\":[],\n\"schema_version\":1,\n\"x\":1\n\n}\n\n",
                   BOOT_ENTRIES_REJECT_NO_ENTRIES, 2u, 11u, "NO_ENTRIES at line 2, col 11");
    /* The second entry's start, not the end of its payload. */
    check_position("{\"crc32\":\"0x00000000\",\"schema_version\":1,\"entries\":[\n" POS_ENTRY("a") ",\n  "
                   POS_ENTRY("a") "\n]}",
                   BOOT_ENTRIES_REJECT_DUPLICATE_ID, 3u, 3u, "DUPLICATE_ID at line 3, col 3");
    /* The entry that lacks the field. */
    check_position("{\"crc32\":\"0x00000000\",\"schema_version\":1,\"entries\":[\n  {\"id\":\"a\",\n\"x\":1\n}\n]}",
                   BOOT_ENTRIES_REJECT_MISSING_FIELD, 2u, 3u, "MISSING_FIELD at line 2, col 3");
    /* A field absent from the whole store has no position to report. */
    check_position("{\"crc32\":\"0x00000000\",\n\"entries\":[]\n}",
                   BOOT_ENTRIES_REJECT_MISSING_FIELD, 0u, 0u, "MISSING_FIELD");
}

/* A checksum failure or an empty input blames the whole file, so neither claims
 * a position, and a short buffer stays terminated. */
static void test_reject_without_position(void)
{
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    char text[BOOT_ENTRIES_REJECT_LOCATOR_LEN];
    unsigned int n = load_fixture("{\n\"crc32\":\"0x00000000\",\n\"schema_version\":1,\"entries\":[]}");
    s_fixture_buf[n - 2u] = (unsigned char)' ';
    TEST_ASSERT_EQ(boot_entries_parse(s_fixture_buf, n, 0, NULL_PTR, r),
                   BOOT_ENTRIES_REJECT_CRC_MISMATCH, "checksum rejected");
    TEST_ASSERT_EQ(r->reject_line, 0u, "checksum: no position");
    boot_entries_reject_locator(r, text, sizeof(text));
    TEST_ASSERT_EQ(rn_streq(text, "CRC_MISMATCH"), 1, "checksum: name only");

    TEST_ASSERT_EQ(boot_entries_parse(s_fixture_buf, 0u, 0, NULL_PTR, r),
                   BOOT_ENTRIES_REJECT_JSON_PARSE, "empty rejected");
    TEST_ASSERT_EQ(r->reject_line, 0u, "empty: no position");
    boot_entries_reject_locator(r, text, 5u);
    TEST_ASSERT_EQ(rn_streq(text, "JSON"), 1, "short buffer truncates and terminates");
}

/* The longest name with the largest position the 16 KiB cap allows must fit, or
 * the notice loses the position it exists to show. */
static void test_reject_locator_fits(void)
{
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    char text[BOOT_ENTRIES_REJECT_LOCATOR_LEN];
    r->reject_line = BOOT_ENTRIES_MAX_TOTAL_BYTES;
    r->reject_column = BOOT_ENTRIES_MAX_TOTAL_BYTES;
    for (int c = 0; c <= (int)BOOT_ENTRIES_REJECT_ESCAPED_KEY; c++) {
        r->reject_code = (boot_entries_reject_code_t)c;
        boot_entries_reject_locator(r, text, sizeof(text));
        unsigned int len = 0;
        while (text[len]) len++;
        TEST_ASSERT(len >= 5u && rn_streq(text + len - 5u, "16384"), "full column survives on every name");
    }
}

/* A stored record is untrusted: the variable lives under a public GUID. */
static void test_store_reject_describe(void)
{
    char t[BOOT_ENTRIES_REJECT_LOCATOR_LEN + 16u];
    const char *cause = (const char *)0;
    boot_store_reject_record_t r = { BOOT_STORE_REJECT_ERR_CODE, (unsigned short)BOOT_STORE_LOAD_OK,
                                     (unsigned short)BOOT_ENTRIES_REJECT_JSON_PARSE, 2, 1 };

    TEST_ASSERT_EQ(boot_store_reject_describe(&r, t, sizeof(t), &cause), 1, "a parser rejection renders");
    TEST_ASSERT(rn_streq(t, "JSON_PARSE at line 2, col 1"), "same token text as the notice");
    TEST_ASSERT(cause == boot_entries_reject_cause((int)BOOT_ENTRIES_REJECT_JSON_PARSE),
                "same cause sentence as the notice");

    r.load_status = (unsigned short)BOOT_STORE_LOAD_UNREADABLE;
    TEST_ASSERT_EQ(boot_store_reject_describe(&r, t, sizeof(t), &cause), 1, "unreadable renders");
    TEST_ASSERT(rn_streq(t, "STORE_UNREADABLE"), "unreadable token");
    r.load_status = BOOT_STORE_REJECT_LOAD_ALLOC_FAILED;
    TEST_ASSERT_EQ(boot_store_reject_describe(&r, t, sizeof(t), &cause), 1, "allocation failure renders");
    TEST_ASSERT(rn_streq(t, "POLICY_ALLOC_EXHAUSTED"), "allocation failure token");

    r.load_status = (unsigned short)BOOT_STORE_LOAD_OK;
    r.err_code = 0;
    TEST_ASSERT_EQ(boot_store_reject_describe(&r, t, sizeof(t), &cause), 0, "wrong code refused");
    r.err_code = BOOT_STORE_REJECT_ERR_CODE;
    r.load_status = (unsigned short)BOOT_STORE_LOAD_ABSENT;
    TEST_ASSERT_EQ(boot_store_reject_describe(&r, t, sizeof(t), &cause), 0, "absent is never recorded");
    r.load_status = (unsigned short)BOOT_STORE_LOAD_OK;
    r.reject_code = (unsigned short)BOOT_ENTRIES_OK;
    TEST_ASSERT_EQ(boot_store_reject_describe(&r, t, sizeof(t), &cause), 0, "an accepted store is never recorded");
    r.reject_code = 0xFFFFu;
    TEST_ASSERT_EQ(boot_store_reject_describe(&r, t, sizeof(t), &cause), 0, "unknown reject code refused");
    r.reject_code = (unsigned short)BOOT_ENTRIES_REJECT_JSON_PARSE;
    r.load_status = (unsigned short)BOOT_STORE_LOAD_UNINSPECTED;
    TEST_ASSERT_EQ(boot_store_reject_describe(&r, t, sizeof(t), &cause), 0, "uninspected is never recorded");
    r.load_status = 0x1234u;
    TEST_ASSERT_EQ(boot_store_reject_describe(&r, t, sizeof(t), &cause), 0, "out-of-range load status refused");
    r.load_status = (unsigned short)BOOT_STORE_LOAD_OK;

    /* Hostile numbers stay inside the buffer. */
    char small[12];
    small[11] = 'x';
    r.reject_code = (unsigned short)BOOT_ENTRIES_REJECT_JSON_PARSE;
    r.line = 0xFFFFFFFFu;
    r.column = 0xFFFFFFFFu;
    TEST_ASSERT_EQ(boot_store_reject_describe(&r, small, 11u, &cause), 1, "huge position renders");
    TEST_ASSERT_EQ(small[11], 'x', "render stays inside the caller's capacity");
    TEST_ASSERT_EQ(small[10], 0, "render stays terminated when cut");
}

/* The kernel renders a stored record into BOOT_STORE_REJECT_TOKEN_LEN, and its
 * positions are untrusted u32s: every reject name must fit the name cap, and the
 * longest name at the largest positions must render without truncation. */
static void test_store_reject_token_bound(void)
{
    unsigned int longest = 0u;
    int longest_code = 0;
    for (int code = 0; code < 256; code++) {
        const char *n = bsr_reject_name(code);
        unsigned int len = 0u;
        while (n[len]) len++;
        TEST_ASSERT(len <= BOOT_STORE_REJECT_NAME_MAX, "every reject name fits the name cap");
        if (len > longest) { longest = len; longest_code = code; }
    }
    boot_store_reject_record_t r = { BOOT_STORE_REJECT_ERR_CODE, (unsigned short)BOOT_STORE_LOAD_OK,
                                     (unsigned short)longest_code, 0xFFFFFFFFu, 0xFFFFFFFFu };
    char t[BOOT_STORE_REJECT_TOKEN_LEN];
    const char *cause = (const char *)0;
    TEST_ASSERT_EQ(boot_store_reject_describe(&r, t, sizeof(t), &cause), 1, "longest name renders");
    unsigned int len = 0u;
    while (t[len]) len++;
    TEST_ASSERT_EQ(len, longest + 9u + 10u + 6u + 10u, "worst-case token renders whole");
    TEST_ASSERT(len < sizeof(t), "worst-case token leaves room for the NUL");
}

/* The recurrence policy of s24: a rejection repeats on every boot until the
 * user fixes the file, so a repeat must not reach NVRAM. */
static void test_store_record_decide(void)
{
    boot_store_reject_record_t a = { 0x1Au, 0, 3, 2, 1 };
    boot_store_reject_record_t same = a;
    boot_store_reject_record_t moved = a;
    boot_store_reject_record_t other = a;
    moved.line = 7;
    other.reject_code = 4;

    /* Only a searched filesystem may clear: a PXE or HTTP boot says nothing
     * about the store that was rejected on disk. */
    TEST_ASSERT_EQ(boot_store_record_intent((int)BOOT_STORE_LOAD_UNINSPECTED,
                                            (int)BOOT_ENTRIES_REJECT_JSON_PARSE),
                   BOOT_STORE_RECORD_LEAVE, "no filesystem neither records nor clears");
    TEST_ASSERT_EQ(boot_store_record_intent((int)BOOT_STORE_LOAD_ABSENT,
                                            (int)BOOT_ENTRIES_REJECT_JSON_PARSE),
                   BOOT_STORE_RECORD_CLEAR, "a removed store clears the record");
    TEST_ASSERT_EQ(boot_store_record_intent((int)BOOT_STORE_LOAD_OK, (int)BOOT_ENTRIES_OK),
                   BOOT_STORE_RECORD_CLEAR, "an accepted store clears the record");
    TEST_ASSERT_EQ(boot_store_record_intent((int)BOOT_STORE_LOAD_OK,
                                            (int)BOOT_ENTRIES_REJECT_CRC_MISMATCH),
                   BOOT_STORE_RECORD_RECORD, "a rejected store is recorded");
    TEST_ASSERT_EQ(boot_store_record_intent((int)BOOT_STORE_LOAD_UNREADABLE, (int)BOOT_ENTRIES_OK),
                   BOOT_STORE_RECORD_RECORD, "an unreadable store is recorded");

    TEST_ASSERT_EQ(boot_store_record_decide(&a, BOOT_STORE_RECORD_ABSENT, &same),
                   BOOT_STORE_RECORD_WRITE, "first rejection is written");
    TEST_ASSERT_EQ(boot_store_record_decide(&a, BOOT_STORE_RECORD_VALID, &same),
                   BOOT_STORE_RECORD_KEEP, "a repeated rejection writes nothing");
    TEST_ASSERT_EQ(boot_store_record_decide(&a, BOOT_STORE_RECORD_VALID, &moved),
                   BOOT_STORE_RECORD_WRITE, "a new position is a new record");
    TEST_ASSERT_EQ(boot_store_record_decide(&a, BOOT_STORE_RECORD_VALID, &other),
                   BOOT_STORE_RECORD_WRITE, "a new reason is a new record");
    TEST_ASSERT_EQ(boot_store_record_decide(&a, BOOT_STORE_RECORD_MALFORMED, &same),
                   BOOT_STORE_RECORD_REPLACE, "bad attributes need delete then create");
    TEST_ASSERT_EQ(boot_store_record_decide(&a, BOOT_STORE_RECORD_UNKNOWN, &same),
                   BOOT_STORE_RECORD_KEEP, "an unreadable variable cannot be compared");

    TEST_ASSERT_EQ(boot_store_record_decide((void *)0, BOOT_STORE_RECORD_VALID, &same),
                   BOOT_STORE_RECORD_DELETE, "a repaired store clears the record");
    TEST_ASSERT_EQ(boot_store_record_decide((void *)0, BOOT_STORE_RECORD_MALFORMED, &same),
                   BOOT_STORE_RECORD_DELETE, "a repaired store clears a malformed record");
    TEST_ASSERT_EQ(boot_store_record_decide((void *)0, BOOT_STORE_RECORD_ABSENT, &same),
                   BOOT_STORE_RECORD_KEEP, "a healthy boot writes nothing");
    TEST_ASSERT_EQ(boot_store_record_decide((void *)0, BOOT_STORE_RECORD_UNKNOWN, &same),
                   BOOT_STORE_RECORD_KEEP, "an unreadable variable is left alone");
}

/* The gate that decides whether a user sees a notice at all.
 *
 * The ABSENT arm is the load-bearing one: the Makefile ESP staging copies
 * only boot.conf, so every dev and smoke image legitimately boots with no
 * store. A notice keyed on "the parse did not return OK" would fire on every
 * one of those boots, which is why the caller synthesizes a JSON_PARSE reject
 * code for the absent case and the gate must still stay silent. */
static void test_store_notice_gate(void)
{
    /* Absent stays silent even carrying the synthesized reject code the
     * policy ladder needs. */
    TEST_ASSERT_EQ(boot_entries_store_notice_warranted(
                       (int)BOOT_STORE_LOAD_ABSENT,
                       (int)BOOT_ENTRIES_REJECT_JSON_PARSE), 0,
                   "absent store is silent despite a synthesized reject code");
    TEST_ASSERT_EQ(boot_entries_store_notice_warranted(
                       (int)BOOT_STORE_LOAD_ABSENT, (int)BOOT_ENTRIES_OK), 0,
                   "absent store is silent");

    TEST_ASSERT_EQ(boot_entries_store_notice_warranted(
                       (int)BOOT_STORE_LOAD_UNINSPECTED,
                       (int)BOOT_ENTRIES_REJECT_JSON_PARSE), 0,
                   "a boot device with no filesystem is silent");

    /* Present but unreadable always notifies -- the parser never ran, so the
     * reject code carries no information and must not gate the notice. */
    TEST_ASSERT_EQ(boot_entries_store_notice_warranted(
                       (int)BOOT_STORE_LOAD_UNREADABLE, (int)BOOT_ENTRIES_OK), 1,
                   "unreadable store notifies regardless of reject code");
    TEST_ASSERT_EQ(boot_entries_store_notice_warranted(
                       (int)BOOT_STORE_LOAD_UNREADABLE,
                       (int)BOOT_ENTRIES_REJECT_CRC_MISMATCH), 1,
                   "unreadable store notifies");

    /* Read successfully: the parser's verdict decides. */
    TEST_ASSERT_EQ(boot_entries_store_notice_warranted(
                       (int)BOOT_STORE_LOAD_OK, (int)BOOT_ENTRIES_OK), 0,
                   "accepted store is silent");
    TEST_ASSERT_EQ(boot_entries_store_notice_warranted(
                       (int)BOOT_STORE_LOAD_OK,
                       (int)BOOT_ENTRIES_REJECT_CRC_MISMATCH), 1,
                   "rejected store notifies");

    /* An out-of-contract status stays quiet rather than crying wolf. */
    TEST_ASSERT_EQ(boot_entries_store_notice_warranted(
                       99, (int)BOOT_ENTRIES_REJECT_CRC_MISMATCH), 0,
                   "unknown load status is silent");
}

/* Fill `buf` with a CRC-correct store of exactly `target` bytes: one valid
 * entry, then JSON whitespace, then the crc32 member as the root's LAST key, so
 * the header locate also runs at the boundary. Returns 0 when the store body
 * alone is larger than `target`. tools/boot-entry-validate/test_validate.py
 * builds the same three sizes and must reach the same verdict at each. */
static unsigned int build_store_of_size(unsigned char *buf, unsigned int target)
{
    static const char HEAD[] =
        "{"
        "\"schema_version\":1,"
        "\"entries\":["
        "{\"id\":\"slot-a\",\"title\":\"Slot A\",\"kind\":\"split\","
        "\"flags\":[\"active\"],\"sort_key\":\"00\","
        "\"machine_id\":\"11111111-2222-3333-4444-555555555555\","
        "\"policy_tags\":[],"
        "\"payload\":{\"kernel\":\"\\\\EFI\\\\ImpossibleOS\\\\kernel.exe\"}}"
        "],";
    static const char TAIL[] = "\"crc32\":\"0x00000000\"}";
    const unsigned int head_len = (unsigned int)sizeof(HEAD) - 1u;
    const unsigned int tail_len = (unsigned int)sizeof(TAIL) - 1u;
    unsigned int i, n = 0;

    if (target < head_len + tail_len)
        return 0;
    for (i = 0; i < head_len; i++)
        buf[n++] = (unsigned char)HEAD[i];
    while (n < target - tail_len)
        buf[n++] = ' ';
    for (i = 0; i < tail_len; i++)
        buf[n++] = (unsigned char)TAIL[i];
    patch_fixture_crc(buf, n);
    return n;
}

/* The cap is inclusive: 16 KiB is the largest store the loader boots. The host
 * validator was asserted to agree by reading only, so the off-by-one direction
 * was unverified on both sides. */
static void test_store_size_cap_boundary(void)
{
    const unsigned int cap = BOOT_ENTRIES_MAX_TOTAL_BYTES;
    TEST_SCRATCH_KBUF(store, cap + 1u);
    TEST_SCRATCH_KBUF(rbuf, sizeof(boot_entries_parse_result_t));
    unsigned char *const buf = (unsigned char *)store;
    boot_entries_parse_result_t *const r = (boot_entries_parse_result_t *)rbuf;
    unsigned int n;
    int rc;

    n = build_store_of_size(buf, cap - 1u);
    TEST_ASSERT_EQ(n, cap - 1u, "fixture one byte under the cap");
    rc = boot_entries_parse(buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK, "a store one byte under the cap is accepted");
    TEST_ASSERT_EQ(r->entry_count, 1u, "and its entry parsed");

    n = build_store_of_size(buf, cap);
    TEST_ASSERT_EQ(n, cap, "fixture exactly at the cap");
    rc = boot_entries_parse(buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_OK, "a store exactly at the cap is accepted");
    TEST_ASSERT_EQ(r->entry_count, 1u, "and its entry parsed");

    n = build_store_of_size(buf, cap + 1u);
    TEST_ASSERT_EQ(n, cap + 1u, "fixture one byte over the cap");
    rc = boot_entries_parse(buf, n, 0, NULL_PTR, r);
    TEST_ASSERT_EQ(rc, BOOT_ENTRIES_REJECT_FILE_TOO_LARGE, "a store one byte over the cap is refused");
    TEST_ASSERT_EQ(r->reject_code, BOOT_ENTRIES_REJECT_FILE_TOO_LARGE, "with the size reject code");

    /* The loader's pre-read check in bootx64.c is this same predicate, so the
     * verdicts above are the loader's too. Pin it directly, including sizes a
     * 64-bit file length can carry and a 32-bit parse length cannot. */
    TEST_ASSERT_EQ(boot_entries_size_within_cap(cap - 1u), 1, "pre-read: one under the cap passes");
    TEST_ASSERT_EQ(boot_entries_size_within_cap(cap), 1, "pre-read: exactly the cap passes");
    TEST_ASSERT_EQ(boot_entries_size_within_cap(cap + 1u), 0, "pre-read: one over the cap is refused");
    TEST_ASSERT_EQ(boot_entries_size_within_cap(0x100000000ull + cap), 0,
                   "pre-read: a size that wraps 32 bits back under the cap is refused");
}

void test_register_boot_entry_parser(void)
{
    test_suite_register_cat("boot-entries: valid minimal",
                            test_parser_valid_minimal, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: bad schema_version rejected",
                            test_parser_bad_schema_version, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: output zeroed before validation",
                            test_parser_zeroes_output_before_validation, TEST_CAT_BOOT);
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
    test_suite_register_cat("boot-entries: empty machine_id wildcard accepted",
                            test_parser_empty_machine_id_accepted, TEST_CAT_BOOT);
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
    test_suite_register_cat("boot-entries: BLS sort by sort_key then id",
                            test_bls_sort_by_sort_key_then_id, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: BLS sort machine_id tiebreak",
                            test_bls_sort_machine_id_tiebreak, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: health_check_subset valid",
                            test_parser_health_subset_valid, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: health_check_subset absent accepted",
                            test_parser_health_subset_absent_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: health_check_subset non-array rejected",
                            test_parser_health_subset_rejects_non_array, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: health_check_subset overflow rejected",
                            test_parser_health_subset_rejects_overflow, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: health_check_subset empty name rejected",
                            test_parser_health_subset_rejects_empty_name, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: health_check_subset oversize name rejected",
                            test_parser_health_subset_rejects_oversize_name, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: health_check_subset backslash rejected",
                            test_parser_health_subset_rejects_backslash_in_name, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: health_check_subset trailing comma rejected",
                            test_parser_health_subset_rejects_trailing_comma, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: repeated entries key rejected",
                            test_parser_repeated_entries_key_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: duplicate id across entries keys rejected",
                            test_parser_duplicate_id_across_entries_keys_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: repeated schema_version rejected",
                            test_parser_repeated_schema_version_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: repeated crc32 rejected",
                            test_parser_repeated_crc32_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: repeated unknown top-level key rejected",
                            test_parser_repeated_unknown_top_level_key_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: repeated entry-object key rejected",
                            test_parser_repeated_entry_object_key_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: escaped top-level key rejected",
                            test_parser_escaped_top_level_key_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: escaped entry key rejected",
                            test_parser_escaped_entry_key_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: many distinct keys accepted",
                            test_parser_many_distinct_keys_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: prefix-sharing keys accepted",
                            test_parser_prefix_sharing_keys_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: repeated entries behind 6-deep extension rejected",
                            test_parser_repeated_entries_behind_nest6, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: repeated entries behind 7-deep extension rejected",
                            test_parser_repeated_entries_behind_nest7, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: repeated entries behind 8-deep extension rejected",
                            test_parser_repeated_entries_behind_nest8, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: deep extension with one entries array accepted",
                            test_parser_deep_extension_single_entries_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: entry extension 8 deep accepted",
                            test_parser_entry_ext_depth8_accepted, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: entry extension 9 deep rejected",
                            test_parser_entry_ext_depth9_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: rescan reports failure not absence",
                            test_parser_rescan_reports_failure_not_absence, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: rescan finds and misses correctly",
                            test_parser_rescan_finds_and_misses_correctly, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: skip_value_depth budget enforced",
                            test_parser_skip_value_depth_budget, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: entries_retain bound enforced",
                            test_parser_entries_retain_bound, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: crc locate CONTROL",
                            test_parser_crc_locate_control, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: crc decoy string value",
                            test_parser_crc_decoy_string_value, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: crc decoy nested member",
                            test_parser_crc_decoy_nested_member, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: crc decoy escaped and array",
                            test_parser_crc_decoy_escaped_and_array, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: crc header after deep value",
                            test_parser_crc_header_after_deep_value, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: crc header over scan budget",
                            test_parser_crc_header_after_over_budget_value, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: crc uppercase prefix rejected",
                            test_parser_crc_uppercase_prefix_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: crc value shapes rejected",
                            test_parser_crc_value_shapes_rejected, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: crc malformed before header",
                            test_parser_crc_malformed_before_header, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: crc broken root is malformed",
                            test_parser_crc_broken_root_is_malformed, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: crc absent root member",
                            test_parser_crc_absent_root_member, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: reject name table complete",
                            test_reject_name_table_complete, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: reject cause table complete",
                            test_reject_cause_table_complete, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: reject position",
                            test_reject_position, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: reject position after walk",
                            test_reject_position_after_walk, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: reject without position",
                            test_reject_without_position, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: reject locator fits",
                            test_reject_locator_fits, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: store notice gate",
                            test_store_notice_gate, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: stored rejection rendering",
                            test_store_reject_describe, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: stored rejection token bound",
                            test_store_reject_token_bound, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: store record recurrence policy",
                            test_store_record_decide, TEST_CAT_BOOT);
    test_suite_register_cat("boot-entries: store size cap boundary",
                            test_store_size_cap_boundary, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
