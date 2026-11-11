/* test_boot_conf_lexer.c -- the loader's boot.conf key=value lexer.
 *
 * The lexer is pure C with no UEFI types, so it is included here and run in
 * the kernel test build, the same way test_boot_entry_parser.c runs the
 * entry store parser. These pin the grammar the loader has always used and
 * the overlong-key and overlong-value results that replaced silent
 * truncation.
 */
#include "kernel/test/test.h"
#include "../../boot/uefi/boot_conf_lexer.c"
#include "kernel/types.h"
#include "libc/string.h"

static char s_key[BOOT_CONF_KEY_MAX];
static char s_val[BOOT_CONF_VAL_MAX];
static char s_buf[1024];

static boot_conf_tok_t lex(const char *buf, unsigned int len, unsigned int *pos)
{
    unsigned int kl, vl;
    return boot_conf_next(buf, len, pos, s_key, &kl, s_val, &vl);
}

static void test_boot_conf_lexer_grammar(void)
{
    static const char conf[] =
        "# comment\n"
        "\n"
        "  debug = 1   # trailing\r\n"
        "sp lit=a b \n"
        "noequals\n"
        "empty=\n"
        "=orphan\n";
    unsigned int pos = 0;

    TEST_ASSERT_EQ(lex(conf, sizeof(conf) - 1, &pos), BOOT_CONF_PAIR, "first pair");
    TEST_ASSERT_EQ(strcmp(s_key, "debug"), 0, "key read");
    TEST_ASSERT_EQ(strcmp(s_val, "1"), 0, "value trimmed at the comment");
    TEST_ASSERT_EQ(lex(conf, sizeof(conf) - 1, &pos), BOOT_CONF_PAIR, "second pair");
    TEST_ASSERT_EQ(strcmp(s_key, "split"), 0, "blanks inside a key are dropped");
    TEST_ASSERT_EQ(strcmp(s_val, "a b"), 0, "inner blanks kept, trailing trimmed");
    TEST_ASSERT_EQ(lex(conf, sizeof(conf) - 1, &pos), BOOT_CONF_PAIR, "line without '=' skipped");
    TEST_ASSERT_EQ(strcmp(s_key, "empty"), 0, "empty value is still a pair");
    TEST_ASSERT_EQ(s_val[0], 0, "empty value");
    TEST_ASSERT_EQ(lex(conf, sizeof(conf) - 1, &pos), BOOT_CONF_PAIR, "orphan value pair");
    TEST_ASSERT_EQ(s_key[0], 0, "empty key");
    TEST_ASSERT_EQ(lex(conf, sizeof(conf) - 1, &pos), BOOT_CONF_EOF, "end of file");
}

static void test_boot_conf_lexer_limits(void)
{
    unsigned int pos, i, n;

    /* A 255-character value fits, trailing blanks past it included. */
    n = 0;
    s_buf[n++] = 'v'; s_buf[n++] = '=';
    for (i = 0; i < BOOT_CONF_VAL_MAX - 1; i++) s_buf[n++] = 'x';
    s_buf[n++] = ' '; s_buf[n++] = ' '; s_buf[n++] = '\n';
    pos = 0;
    TEST_ASSERT_EQ(lex(s_buf, n, &pos), BOOT_CONF_PAIR, "a full value is a pair");
    TEST_ASSERT_EQ(strlen(s_val), BOOT_CONF_VAL_MAX - 1, "the whole value is kept");

    /* One more character is reported, not cut off. */
    n = 0;
    s_buf[n++] = 'v'; s_buf[n++] = '=';
    for (i = 0; i < BOOT_CONF_VAL_MAX; i++) s_buf[n++] = 'x';
    s_buf[n++] = '\n';
    s_buf[n++] = 'a'; s_buf[n++] = '='; s_buf[n++] = '1';
    pos = 0;
    TEST_ASSERT_EQ(lex(s_buf, n, &pos), BOOT_CONF_VALUE_TOO_LONG, "an overlong value is reported");
    TEST_ASSERT_EQ(strcmp(s_key, "v"), 0, "the report names the key");
    TEST_ASSERT_EQ(lex(s_buf, n, &pos), BOOT_CONF_PAIR, "lexing resumes on the next line");
    TEST_ASSERT_EQ(strcmp(s_key, "a"), 0, "the next pair is read");

    /* An overlong key is reported too (it used to drop the line). */
    n = 0;
    for (i = 0; i < BOOT_CONF_KEY_MAX; i++) s_buf[n++] = 'k';
    s_buf[n++] = '='; s_buf[n++] = '1';
    pos = 0;
    TEST_ASSERT_EQ(lex(s_buf, n, &pos), BOOT_CONF_KEY_TOO_LONG, "an overlong key is reported");
    TEST_ASSERT_EQ(lex(s_buf, n, &pos), BOOT_CONF_EOF, "and its line is consumed");

    /* A NUL byte ends the file, as it always has. */
    memcpy(s_buf, "a=1\0b=2\n", 8);
    pos = 0;
    TEST_ASSERT_EQ(lex(s_buf, 8, &pos), BOOT_CONF_PAIR, "pair before the NUL");
    TEST_ASSERT_EQ(lex(s_buf, 8, &pos), BOOT_CONF_EOF, "nothing after the NUL");
}

void test_register_boot_conf_lexer(void)
{
    test_suite_register_cat("boot.conf lexer: grammar",
                            test_boot_conf_lexer_grammar, TEST_CAT_BOOT);
    test_suite_register_cat("boot.conf lexer: overlong key and value reported",
                            test_boot_conf_lexer_limits, TEST_CAT_BOOT);
}
