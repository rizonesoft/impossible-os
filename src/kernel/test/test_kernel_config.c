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
#include "kernel/boot_info.h"
#include "libc/string.h"

extern struct boot_info g_boot_info;

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

static void test_cfg_allow_unknown_never_masks_bad_value(void)
{
    /* boot.allow_unknown downgrades the UNKNOWN_KEY error, but it must NEVER
     * mask a later malformed KNOWN key -- a hard BAD_VALUE/OVERFLOW always
     * survives so the schema's reject-invalid-arg promise still holds. */
    boot_args_status_t st =
        boot_args_parse_cmdline("boot.allow_unknown=1 kernel.foo=1 testsigning=maybe", &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_ERR_BAD_VALUE, "bad value survives allow_unknown downgrade");

    st = boot_args_parse_cmdline(
        "boot.allow_unknown=1 kernel.foo=1 verifier=a,b,c,d,e,f,g,h,i", &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_ERR_OVERFLOW, "CSV overflow survives allow_unknown downgrade");

    /* Without allow_unknown both are fatal; the FIRST failure (the unknown key)
     * is reported, preserving the original unknown-key diagnostic + err_key. */
    st = boot_args_parse_cmdline("kernel.foo=1 testsigning=maybe", &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_ERR_UNKNOWN_KEY, "no allow_unknown: first failure (unknown key) kept");
    TEST_ASSERT(strcmp(s_args.err_key, "kernel.foo") == 0, "err_key names the unknown key");

    /* An EARLY-RETURN overflow (over-long value) after an unknown key must still
     * route through the allow_unknown promotion -- not bypass it via early exit. */
    st = boot_args_parse_cmdline(
        "boot.allow_unknown=1 kernel.foo=1 debug=000000000000000000000000000000000000000000000000",
        &s_args);
    TEST_ASSERT_EQ(st, BOOT_ARGS_ERR_OVERFLOW, "early-overflow promoted past allow_unknown downgrade");
    TEST_ASSERT(strcmp(s_args.err_key, "debug") == 0, "err_key names the overflowing key");
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

/* ---- Section 2: kernel_config_t immutable snapshot ----------------------- */

static void test_cfg_snapshot_published(void)
{
    /* boot_args_init + kernel_config_publish ran in Phase 0, so by test time the
     * snapshot must be published with a valid header. */
    const kernel_config_t *k = kernel_config_get();
    TEST_ASSERT_NOT_NULL(k, "kernel_config_get returns the published snapshot");
    TEST_ASSERT_EQ(k->magic, KERNEL_CONFIG_MAGIC, "snapshot magic is CFG1");
    TEST_ASSERT_EQ(k->version, KERNEL_CONFIG_VERSION, "snapshot version matches header");
    TEST_ASSERT_EQ(k->size, sizeof(kernel_config_t), "snapshot size self-describes the struct");
}

static void test_cfg_snapshot_consistent(void)
{
    /* The flattened fields must agree with the authoritative sources the
     * snapshot was built from -- the snapshot is a faithful read-only view. */
    const kernel_config_t *k = kernel_config_get();
    TEST_ASSERT_NOT_NULL(k, "snapshot present for consistency check");
    TEST_ASSERT_EQ(k->boot_mode, g_boot_info.config.boot_mode,
                   "snapshot boot_mode mirrors boot_config");
    TEST_ASSERT_EQ(k->serial_debug, g_boot_info.config.serial_debug,
                   "snapshot serial_debug mirrors boot_config");
    TEST_ASSERT_EQ(k->boot_reason, g_boot_info.boot_reason,
                   "snapshot boot_reason mirrors validated boot decision");
    TEST_ASSERT_EQ(k->selection_reason, g_boot_info.selection_reason,
                   "snapshot selection_reason mirrors the boot-entry ladder reason");
    /* The boot-arg provenance accessor returns the ladder selection_reason,
     * not boot_reason. */
    TEST_ASSERT_EQ(boot_args_selection_reason(), g_boot_info.selection_reason,
                   "boot_args_selection_reason returns the ladder reason field");
    /* debug_enabled is the resolved boot-arg value (cmdline over boot_config). */
    const boot_args_t *a = boot_args_parsed();
    if (a) {
        const boot_arg_value_t *v = boot_args_get(a, "debug");
        uint8_t want = v ? (uint8_t)v->ival : (uint8_t)g_boot_info.config.debug;
        TEST_ASSERT_EQ(k->debug_enabled, want, "snapshot debug_enabled = resolved boot arg");
    }
}

/* ---- Section 5: Safe Mode policy object ---------------------------------- */

static void test_cfg_safe_mode_floor(void)
{
    uint8_t lvl, rsn;
    /* The H1 fix: boot-policy safe (boot_mode=1) + cmdline safemode=off must
     * NOT downgrade -- the floor holds at MINIMAL, reason boot-policy. */
    safe_mode_resolve(1, SAFE_MODE_OFF, 1, &lvl, &rsn);
    TEST_ASSERT_EQ(lvl, SAFE_MODE_MINIMAL, "boot-policy safe floors over cmdline off");
    TEST_ASSERT_EQ(rsn, SAFE_REASON_BOOT_POLICY, "floored level reads boot-policy reason");

    /* Normal boot, no safemode arg -> off. */
    safe_mode_resolve(0, SAFE_MODE_OFF, 0, &lvl, &rsn);
    TEST_ASSERT_EQ(lvl, SAFE_MODE_OFF, "normal boot resolves off");
    TEST_ASSERT_EQ(rsn, SAFE_REASON_NONE, "off has no reason");

    /* Operator escalates above the floor (boot_mode=1 + safemode=network). */
    safe_mode_resolve(1, SAFE_MODE_NETWORK, 1, &lvl, &rsn);
    TEST_ASSERT_EQ(lvl, SAFE_MODE_NETWORK, "operator escalates above the floor");
    TEST_ASSERT_EQ(rsn, SAFE_REASON_OPERATOR, "operator-chosen level reads operator reason");

    /* Recovery boot floors to minimal with recovery reason. */
    safe_mode_resolve(2, SAFE_MODE_OFF, 0, &lvl, &rsn);
    TEST_ASSERT_EQ(lvl, SAFE_MODE_MINIMAL, "recovery boot floors to minimal");
    TEST_ASSERT_EQ(rsn, SAFE_REASON_RECOVERY, "recovery boot reads recovery reason");

    /* Operator sets safemode on a normal boot. */
    safe_mode_resolve(0, SAFE_MODE_MINIMAL, 1, &lvl, &rsn);
    TEST_ASSERT_EQ(lvl, SAFE_MODE_MINIMAL, "operator safemode on normal boot");
    TEST_ASSERT_EQ(rsn, SAFE_REASON_OPERATOR, "operator reason on normal boot");

    /* Boot-policy safe + safemode=minimal (== floor): the operator did NOT
     * escalate, so boot policy is the cause -- reason must be boot-policy. */
    safe_mode_resolve(1, SAFE_MODE_MINIMAL, 1, &lvl, &rsn);
    TEST_ASSERT_EQ(lvl, SAFE_MODE_MINIMAL, "safe boot + safemode=minimal stays minimal");
    TEST_ASSERT_EQ(rsn, SAFE_REASON_BOOT_POLICY, "equal-to-floor is boot-policy, not operator");
}

static void test_cfg_safe_mode_gating(void)
{
    /* Off level allows everything. */
    TEST_ASSERT_EQ(safe_mode_component_allowed(SAFE_MODE_OFF, SAFE_COMP_NETWORK), 1,
                   "normal boot allows networking");
    TEST_ASSERT_EQ(safe_mode_component_allowed(SAFE_MODE_OFF, SAFE_COMP_GUI), 1,
                   "normal boot allows the GUI");
    /* Minimal gates network + GUI + third-party + CI relax. */
    TEST_ASSERT_EQ(safe_mode_component_allowed(SAFE_MODE_MINIMAL, SAFE_COMP_NETWORK), 0,
                   "minimal safe mode gates networking");
    TEST_ASSERT_EQ(safe_mode_component_allowed(SAFE_MODE_MINIMAL, SAFE_COMP_GUI), 0,
                   "minimal safe mode gates the GUI");
    /* Network level allows networking but still gates GUI + CI relax. */
    TEST_ASSERT_EQ(safe_mode_component_allowed(SAFE_MODE_NETWORK, SAFE_COMP_NETWORK), 1,
                   "network safe mode allows networking");
    TEST_ASSERT_EQ(safe_mode_component_allowed(SAFE_MODE_NETWORK, SAFE_COMP_CI_RELAX), 0,
                   "CI relaxation never auto-allowed in safe mode");
    TEST_ASSERT_EQ(safe_mode_component_allowed(SAFE_MODE_NETWORK, SAFE_COMP_GUI), 0,
                   "network safe mode still gates the full GUI");
}

static void test_cfg_safe_mode_accessors(void)
{
    /* Accessors must agree with the published snapshot. */
    const kernel_config_t *k = kernel_config_get();
    TEST_ASSERT_NOT_NULL(k, "snapshot present for safe-mode accessors");
    TEST_ASSERT_EQ(kernel_safe_mode(), k->safe_mode, "kernel_safe_mode mirrors snapshot");
    TEST_ASSERT_EQ(kernel_safe_mode_reason(), k->safe_mode_reason,
                   "kernel_safe_mode_reason mirrors snapshot");
}

void test_register_kernel_config(void)
{
    test_suite_register_cat("CONF: safe-mode floor resolver", test_cfg_safe_mode_floor,     TEST_CAT_BOOT);
    test_suite_register_cat("CONF: safe-mode gating",         test_cfg_safe_mode_gating,     TEST_CAT_BOOT);
    test_suite_register_cat("CONF: safe-mode accessors",      test_cfg_safe_mode_accessors,  TEST_CAT_BOOT);
    test_suite_register_cat("CONF: snapshot published",      test_cfg_snapshot_published,      TEST_CAT_BOOT);
    test_suite_register_cat("CONF: snapshot consistent",     test_cfg_snapshot_consistent,     TEST_CAT_BOOT);
    test_suite_register_cat("CONF: boot arg basic parse",     test_cfg_basic_parse,             TEST_CAT_BOOT);
    test_suite_register_cat("CONF: unknown kernel key fatal", test_cfg_unknown_kernel_key_fatal, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: allow_unknown downgrade",  test_cfg_allow_unknown_downgrades, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: allow_unknown no-mask", test_cfg_allow_unknown_never_masks_bad_value, TEST_CAT_BOOT);
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
