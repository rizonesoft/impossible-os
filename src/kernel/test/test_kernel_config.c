/* ============================================================================
 * test_kernel_config.c -- Boot argument schema and parser tests (Section 1)
 *
 * Exercises the PURE parser (boot_args_parse_cmdline) and the descriptor table.
 * No live boot infrastructure: the parser takes a string and fills a
 * caller-owned boot_args_t, so every assertion runs against an in-memory
 * fixture. boot_args_init() (the Phase-0 wrapper that halts) is NOT called here.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/config.h"
#include "libc/string.h"

/* boot_args_t is several KB; keep one file-static fixture rather than a stack
 * temporary per test. Each test re-parses into it (parse memsets it first). */
static boot_args_t s_args;

static void test_cfg_basic_parse(void)
{
    boot_args_status_t st = boot_args_parse_cmdline("debug=1 safemode=network", &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_OK, "debug=1 safemode=network parses OK");

    const boot_arg_value_t *dbg = boot_args_get(&s_args, "debug");
    TEST_ASSERT_NOT_NULL(dbg, "debug value present");
    TEST_ASSERT_EQ(dbg->ival, 1, "debug == 1");
    TEST_ASSERT_EQ(dbg->source, BOOT_ARG_SRC_CMDLINE, "debug source is CMDLINE");

    const boot_arg_value_t *sm = boot_args_get(&s_args, "safemode");
    TEST_ASSERT_NOT_NULL(sm, "safemode value present");
    TEST_ASSERT_EQ(sm->ival, 2, "safemode == network (enum index 2)");
}

static void test_cfg_unknown_kernel_key_fatal(void)
{
    boot_args_status_t st = boot_args_parse_cmdline("kernel.unknown=1", &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_ERR_UNKNOWN_KEY, "unknown kernel.* key is fatal");
    TEST_ASSERT_EQ(s_args.unknown_count, 1, "one unknown key recorded");
    TEST_ASSERT(strcmp(s_args.err_key, "kernel.unknown") == 0, "err_key names the bad key");
}

static void test_cfg_allow_unknown_downgrades(void)
{
    boot_args_status_t st =
        boot_args_parse_cmdline("kernel.unknown=1 boot.allow_unknown=1", &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_OK, "boot.allow_unknown downgrades unknown to OK");
    TEST_ASSERT_EQ(s_args.allow_unknown, 1, "allow_unknown flag set");
    TEST_ASSERT_EQ(s_args.unknown_count, 1, "unknown key still recorded for audit");
}

static void test_cfg_non_kernel_unknown_ignored(void)
{
    /* An unknown key OUTSIDE the kernel.* namespace is silently ignored, not
     * fatal -- the bootloader/firmware may pass keys the kernel does not own. */
    boot_args_status_t st = boot_args_parse_cmdline("randomkey=5 debug=1", &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_OK, "non-kernel unknown key is ignored");
    TEST_ASSERT_NOT_NULL(boot_args_get(&s_args, "debug"), "valid key still parsed");
}

static void test_cfg_alias_normalization(void)
{
    boot_args_parse_cmdline("/debug nogui", &s_args);
    const boot_arg_value_t *dbg = boot_args_get(&s_args, "debug");
    TEST_ASSERT_NOT_NULL(dbg, "/debug alias parsed");
    TEST_ASSERT_EQ(dbg->ival, 1, "/debug -> debug=1");
    const boot_arg_value_t *ng = boot_args_get(&s_args, "nogui");
    TEST_ASSERT_NOT_NULL(ng, "bare nogui parsed");
    TEST_ASSERT_EQ(ng->ival, 1, "bare BOOL -> 1");
}

static void test_cfg_bool_word_forms(void)
{
    boot_args_parse_cmdline("testsigning=on recoveryenabled=no", &s_args);
    const boot_arg_value_t *ts = boot_args_get(&s_args, "testsigning");
    TEST_ASSERT_NOT_NULL(ts, "testsigning=on parsed");
    TEST_ASSERT_EQ(ts->ival, 1, "on -> 1");
    const boot_arg_value_t *re = boot_args_get(&s_args, "recoveryenabled");
    TEST_ASSERT_NOT_NULL(re, "recoveryenabled=no parsed");
    TEST_ASSERT_EQ(re->ival, 0, "no -> 0");
}

static void test_cfg_csv_value(void)
{
    boot_args_parse_cmdline("verifier=pool,irql,deadlock", &s_args);
    const boot_arg_value_t *v = boot_args_get(&s_args, "verifier");
    TEST_ASSERT_NOT_NULL(v, "verifier CSV parsed");
    TEST_ASSERT_EQ(v->csv_count, 3, "three CSV elements");
    TEST_ASSERT(strcmp(v->sval, "pool,irql,deadlock") == 0, "CSV joined value preserved");
}

static void test_cfg_bad_value_rejected(void)
{
    boot_args_status_t st = boot_args_parse_cmdline("debug=maybe", &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_ERR_BAD_VALUE, "non-bool debug value rejected");

    st = boot_args_parse_cmdline("safemode=bogus", &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_ERR_BAD_VALUE, "unknown enum value rejected");
}

static void test_cfg_descriptor_lookup(void)
{
    TEST_ASSERT_NOT_NULL(boot_arg_find("debug"), "debug descriptor exists");
    TEST_ASSERT_NULL(boot_arg_find("does_not_exist"), "missing key returns NULL");
    uint32_t n = 0;
    const boot_arg_desc_t *tbl = boot_arg_table(&n);
    TEST_ASSERT_NOT_NULL(tbl, "table accessor returns the table");
    TEST_ASSERT(n > 0, "schema table is non-empty");
}

static void test_cfg_empty_cmdline(void)
{
    boot_args_status_t st = boot_args_parse_cmdline("", &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_OK, "empty cmdline parses OK");
    TEST_ASSERT_EQ(s_args.count, 0, "no values for empty cmdline");
    st = boot_args_parse_cmdline((void *)0, &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_OK, "NULL cmdline parses OK");
}

static void test_cfg_unterminated_cmdline(void)
{
    /* A non-NUL-terminated buffer at the handoff-field size must be REJECTED,
     * not over-read past its end -- the Phase-0 untrusted trust boundary.
     * The bounded parser scans for a NUL within max_len and fails if absent. */
    static char buf[300];
    memset(buf, 'a', sizeof(buf));     /* no NUL anywhere */
    boot_args_status_t st = boot_args_parse_cmdline_n(buf, 256, &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_ERR_OVERFLOW, "unterminated cmdline rejected, not over-read");

    /* A token longer than the value cap is also hard-failed (never truncated). */
    st = boot_args_parse_cmdline(
        "debug=00000000000000000000000000000000000000000000000000000000", &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_ERR_OVERFLOW, "over-long value token rejected");
}

void test_register_kernel_config(void)
{
    test_suite_register_cat("CONF: boot arg basic parse",     test_cfg_basic_parse,             TEST_CAT_BOOT);
    test_suite_register_cat("CONF: unknown kernel key fatal", test_cfg_unknown_kernel_key_fatal, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: allow_unknown downgrade",  test_cfg_allow_unknown_downgrades, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: non-kernel unknown ignored", test_cfg_non_kernel_unknown_ignored, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: alias normalization",      test_cfg_alias_normalization,     TEST_CAT_BOOT);
    test_suite_register_cat("CONF: bool word forms",          test_cfg_bool_word_forms,         TEST_CAT_BOOT);
    test_suite_register_cat("CONF: CSV value",                test_cfg_csv_value,               TEST_CAT_BOOT);
    test_suite_register_cat("CONF: bad value rejected",       test_cfg_bad_value_rejected,      TEST_CAT_BOOT);
    test_suite_register_cat("CONF: descriptor lookup",        test_cfg_descriptor_lookup,       TEST_CAT_BOOT);
    test_suite_register_cat("CONF: empty cmdline",            test_cfg_empty_cmdline,           TEST_CAT_BOOT);
    test_suite_register_cat("CONF: unterminated cmdline",     test_cfg_unterminated_cmdline,    TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
