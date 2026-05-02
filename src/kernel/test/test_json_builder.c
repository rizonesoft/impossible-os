/* SPDX-License-Identifier: MIT */
/* Unit tests for kernel JSON builder (include/kernel/util/json_builder.h).
 *
 * Coverage:
 *   - Empty input produces empty buffer.
 *   - Plain string emits with surrounding quotes.
 *   - Hex64 emits "0x" + 16 hex digits (lowercase).
 *   - U32 decimal handles 0 / max / mid.
 *   - Escape covers \\ \" \n \r \t and \u00XX for control bytes.
 *   - Tiny cap latches truncated=1; pos never exceeds cap-1.
 *   - Truncated builder leaves NUL-terminator slot reserved (cap-1 max). */

#include "kernel/types.h"
#include "kernel/test/test.h"
#include "kernel/util/json_builder.h"

static int strn_eq_local(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0) return 1;
    }
    return 1;
}

static void test_jb_empty_init(void)
{
    char buf[32];
    struct json_builder jb;
    jb_init(&jb, buf, sizeof(buf));
    TEST_ASSERT_EQ(jb_pos(&jb), (size_t)0, "fresh builder has pos=0");
    TEST_ASSERT(!jb_truncated(&jb), "fresh builder is not truncated");
    TEST_ASSERT(jb_buf(&jb) == buf, "buf pointer round-trips");
}

static void test_jb_putc_puts(void)
{
    char buf[16];
    struct json_builder jb;
    jb_init(&jb, buf, sizeof(buf));
    jb_putc(&jb, 'a');
    jb_puts(&jb, "bc");
    TEST_ASSERT_EQ(jb_pos(&jb), (size_t)3, "abc has 3 bytes");
    TEST_ASSERT(strn_eq_local(buf, "abc", 3), "abc round-trips");
    TEST_ASSERT(!jb_truncated(&jb), "small write not truncated");
}

static void test_jb_str_quotes(void)
{
    char buf[16];
    struct json_builder jb;
    jb_init(&jb, buf, sizeof(buf));
    jb_str(&jb, "ok");
    TEST_ASSERT_EQ(jb_pos(&jb), (size_t)4, "\"ok\" has 4 bytes");
    TEST_ASSERT(strn_eq_local(buf, "\"ok\"", 4), "string quoted correctly");
}

static void test_jb_str_escapes(void)
{
    char buf[64];
    struct json_builder jb;
    jb_init(&jb, buf, sizeof(buf));
    jb_str(&jb, "a\\b\"c\nd\re\tf");
    /* Expected: "a\\b\"c\nd\re\tf" inside JSON quotes
     * = '"' + a\\b\"c\nd\re\tf + '"' = 16 chars */
    static const char expected[] = "\"a\\\\b\\\"c\\nd\\re\\tf\"";
    size_t expected_len = sizeof(expected) - 1;
    TEST_ASSERT_EQ(jb_pos(&jb), expected_len, "escape length matches");
    TEST_ASSERT(strn_eq_local(buf, expected, expected_len),
                "escapes match RFC 8259");
}

static void test_jb_str_control_byte(void)
{
    char buf[32];
    struct json_builder jb;
    jb_init(&jb, buf, sizeof(buf));
    /* 0x01 is a control byte; should emit as . */
    char input[3] = { 'x', 0x01, 0 };
    jb_str(&jb, input);
    /* Expected: "x" -- 9 chars */
    static const char expected[] = "\"x\\u0001\"";
    size_t expected_len = sizeof(expected) - 1;
    TEST_ASSERT_EQ(jb_pos(&jb), expected_len, "control byte length");
    TEST_ASSERT(strn_eq_local(buf, expected, expected_len),
                "control byte emits as \\u00XX");
}

static void test_jb_hex64(void)
{
    char buf[32];
    struct json_builder jb;
    jb_init(&jb, buf, sizeof(buf));
    jb_hex64(&jb, 0xDEADBEEFu);
    /* Expected: "0x00000000deadbeef" -- 20 chars */
    static const char expected[] = "\"0x00000000deadbeef\"";
    size_t expected_len = sizeof(expected) - 1;
    TEST_ASSERT_EQ(jb_pos(&jb), expected_len, "hex64 length");
    TEST_ASSERT(strn_eq_local(buf, expected, expected_len),
                "hex64 emits 16 lowercase digits with 0x prefix");
}

static void test_jb_u32_dec_zero(void)
{
    char buf[16];
    struct json_builder jb;
    jb_init(&jb, buf, sizeof(buf));
    jb_u32_dec(&jb, 0);
    TEST_ASSERT_EQ(jb_pos(&jb), (size_t)1, "0 emits one digit");
    TEST_ASSERT(buf[0] == '0', "u32_dec(0) == \"0\"");
}

static void test_jb_u32_dec_max(void)
{
    char buf[16];
    struct json_builder jb;
    jb_init(&jb, buf, sizeof(buf));
    jb_u32_dec(&jb, 0xFFFFFFFFu);
    /* 4294967295 is 10 digits */
    TEST_ASSERT_EQ(jb_pos(&jb), (size_t)10, "u32 max has 10 digits");
    TEST_ASSERT(strn_eq_local(buf, "4294967295", 10),
                "u32_dec(0xFFFFFFFF) decimal correct");
}

static void test_jb_truncation_latches(void)
{
    /* Cap=4 means max 3 bytes written (cap-1 reserved for terminator). */
    char buf[4] = {0};
    struct json_builder jb;
    jb_init(&jb, buf, sizeof(buf));
    jb_puts(&jb, "abcdef");
    TEST_ASSERT(jb_truncated(&jb), "truncation flag latched");
    TEST_ASSERT(jb_pos(&jb) <= sizeof(buf) - 1,
                "pos never exceeds cap-1 on truncation");
    TEST_ASSERT_EQ(jb_pos(&jb), (size_t)3, "wrote exactly cap-1 bytes");
}

static void test_jb_truncation_zero_cap(void)
{
    /* cap=0 -- jb_putc sets truncated immediately; pos stays 0. */
    char buf[1] = {0};
    struct json_builder jb;
    jb_init(&jb, buf, 0);
    jb_putc(&jb, 'x');
    TEST_ASSERT(jb_truncated(&jb), "zero-cap truncates on first byte");
    TEST_ASSERT_EQ(jb_pos(&jb), (size_t)0, "zero-cap pos stays 0");
}

void test_register_json_builder(void)
{
    test_suite_register_cat("json_builder: empty init",
        test_jb_empty_init, TEST_CAT_BOOT);
    test_suite_register_cat("json_builder: putc + puts",
        test_jb_putc_puts, TEST_CAT_BOOT);
    test_suite_register_cat("json_builder: str quotes",
        test_jb_str_quotes, TEST_CAT_BOOT);
    test_suite_register_cat("json_builder: str escapes",
        test_jb_str_escapes, TEST_CAT_BOOT);
    test_suite_register_cat("json_builder: control byte \\u00XX",
        test_jb_str_control_byte, TEST_CAT_BOOT);
    test_suite_register_cat("json_builder: hex64 16 digits",
        test_jb_hex64, TEST_CAT_BOOT);
    test_suite_register_cat("json_builder: u32_dec(0)",
        test_jb_u32_dec_zero, TEST_CAT_BOOT);
    test_suite_register_cat("json_builder: u32_dec(MAX)",
        test_jb_u32_dec_max, TEST_CAT_BOOT);
    test_suite_register_cat("json_builder: truncation latches at cap-1",
        test_jb_truncation_latches, TEST_CAT_BOOT);
    test_suite_register_cat("json_builder: zero cap truncates on first byte",
        test_jb_truncation_zero_cap, TEST_CAT_BOOT);
}
