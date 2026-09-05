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
    s_fixture_buf_size = n;
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
}

#endif /* KERNEL_TESTS */
