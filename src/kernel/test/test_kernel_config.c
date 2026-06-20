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
#include "kernel/tunables.h"
#include "kernel/feature.h"
#include "kernel/policy_lock.h"
#include "kernel/boot_status.h"
#include "kernel/nt/sysconfig_info.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/sched/irql.h"
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

    /* Corrupt/unknown boot_mode must FAIL SAFE, never collapse to normal mode. */
    safe_mode_resolve(99, SAFE_MODE_OFF, 0, &lvl, &rsn);
    TEST_ASSERT_EQ(lvl, SAFE_MODE_MINIMAL, "unknown boot_mode floors to minimal (fail-safe)");
    TEST_ASSERT_EQ(rsn, SAFE_REASON_BOOT_POLICY, "unknown boot_mode reads boot-policy reason");

    /* Corrupt safemode= arg above the highest level clamps to DSREPAIR. */
    safe_mode_resolve(0, 99, 1, &lvl, &rsn);
    TEST_ASSERT_EQ(lvl, SAFE_MODE_DSREPAIR, "out-of-range safemode arg clamps to dsrepair");
    TEST_ASSERT_EQ(rsn, SAFE_REASON_OPERATOR, "operator-set out-of-range arg reads operator reason");
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

/* ---- Runtime Tunable Registry ------------------------------------------- */
/* Test tunables use a distinct owner id and self-clean via
 * kernel_tunable_unregister_owner() so re-runs never collide. */
#define T_OWN_A 0xA0u
#define T_OWN_B 0xA1u
#define T_OWN_C 0xA2u
#define T_OWN_D 0xA3u
#define T_OWN_E 0xA4u
#define T_OWN_F 0xA5u
#define T_OWN_G 0xA6u   /* callback semantics */
#define T_OWN_H 0xA7u   /* audit / dump */
#define T_OWN_I 0xA8u   /* capacity exhaustion */

static void test_tunable_register_get(void)
{
    int64_t v = 0;
    NTSTATUS rc = kernel_tunable_register("test.depth", TUNABLE_UINT, TUNABLE_RUNTIME,
                                          0, 100, 7, (tunable_cb_t)0, (void *)0,
                                          T_OWN_A, TUNABLE_SRC_BUILTIN);
    TEST_ASSERT_EQ(rc, STATUS_SUCCESS, "register a tunable");
    TEST_ASSERT_EQ(kernel_tunable_get("test.depth", &v), STATUS_SUCCESS, "get registered tunable");
    TEST_ASSERT_EQ(v, 7, "get returns the registered default");
    /* duplicate name is rejected */
    rc = kernel_tunable_register("test.depth", TUNABLE_UINT, TUNABLE_RUNTIME,
                                 0, 100, 1, (tunable_cb_t)0, (void *)0,
                                 T_OWN_A, TUNABLE_SRC_BUILTIN);
    TEST_ASSERT_EQ(rc, STATUS_OBJECT_NAME_COLLISION, "duplicate name rejected");
    kernel_tunable_unregister_owner(T_OWN_A);
    TEST_ASSERT_EQ(kernel_tunable_get("test.depth", &v), STATUS_NOT_FOUND,
                   "unregister_owner removed the tunable");
}

static void test_tunable_clamp(void)
{
    int64_t v = 0;
    kernel_tunable_register("test.timeout", TUNABLE_UINT, TUNABLE_RUNTIME,
                            0, 3600, 30, (tunable_cb_t)0, (void *)0,
                            T_OWN_B, TUNABLE_SRC_BUILTIN);
    /* over-max clamps to max, not stored verbatim */
    NTSTATUS rc = kernel_tunable_set("test.timeout", 9999, TUNABLE_SET_PRIVILEGED);
    TEST_ASSERT_EQ(rc, STATUS_SUCCESS, "set over-max succeeds (clamped)");
    kernel_tunable_get("test.timeout", &v);
    TEST_ASSERT_EQ(v, 3600, "over-max value clamped to declared max");
    /* below-min clamps to min */
    kernel_tunable_set("test.timeout", -5, TUNABLE_SET_PRIVILEGED);
    kernel_tunable_get("test.timeout", &v);
    TEST_ASSERT_EQ(v, 0, "below-min value clamped to declared min");
    kernel_tunable_unregister_owner(T_OWN_B);
}

static void test_tunable_access(void)
{
    int64_t v = 0;
    kernel_tunable_register("test.ro", TUNABLE_UINT, TUNABLE_READONLY,
                            0, 10, 5, (tunable_cb_t)0, (void *)0,
                            T_OWN_C, TUNABLE_SRC_BUILTIN);
    TEST_ASSERT_EQ(kernel_tunable_set("test.ro", 3, TUNABLE_SET_PRIVILEGED),
                   STATUS_ACCESS_DENIED, "read-only write denied");
    kernel_tunable_get("test.ro", &v);
    TEST_ASSERT_EQ(v, 5, "read-only denied write leaves value unchanged");

    kernel_tunable_register("test.priv", TUNABLE_UINT, TUNABLE_PRIVILEGED,
                            0, 10, 1, (tunable_cb_t)0, (void *)0,
                            T_OWN_C, TUNABLE_SRC_BUILTIN);
    TEST_ASSERT_EQ(kernel_tunable_set("test.priv", 4, 0),
                   STATUS_ACCESS_DENIED, "privileged write without privilege denied");
    kernel_tunable_get("test.priv", &v);
    TEST_ASSERT_EQ(v, 1, "unprivileged denied write leaves value unchanged");
    TEST_ASSERT_EQ(kernel_tunable_set("test.priv", 4, TUNABLE_SET_PRIVILEGED),
                   STATUS_SUCCESS, "privileged write with privilege allowed");

    /* DEBUG_ONLY: outcome tracks the live debug_enabled config flag. */
    kernel_tunable_register("test.debugonly", TUNABLE_UINT, TUNABLE_DEBUG_ONLY,
                            0, 10, 0, (tunable_cb_t)0, (void *)0,
                            T_OWN_C, TUNABLE_SRC_BUILTIN);
    const kernel_config_t *kc = kernel_config_get();
    NTSTATUS dbg = kernel_tunable_set("test.debugonly", 7, TUNABLE_SET_PRIVILEGED);
    if (kc && kc->debug_enabled) {
        TEST_ASSERT_EQ(dbg, STATUS_SUCCESS, "debug-only write allowed when debug enabled");
    } else {
        TEST_ASSERT_EQ(dbg, STATUS_ACCESS_DENIED, "debug-only write denied without debug");
        kernel_tunable_get("test.debugonly", &v);
        TEST_ASSERT_EQ(v, 0, "debug-only denied write leaves value unchanged");
    }

    TEST_ASSERT_EQ(kernel_tunable_set("test.absent", 1, TUNABLE_SET_PRIVILEGED),
                   STATUS_NOT_FOUND, "set on unknown name returns not-found");
    kernel_tunable_unregister_owner(T_OWN_C);
}

static void test_tunable_module_namespace(void)
{
    int64_t v = 0;
    NTSTATUS rc = kernel_tunable_register("module.testmod.depth", TUNABLE_UINT,
                                          TUNABLE_RUNTIME, 0, 64, 8,
                                          (tunable_cb_t)0, (void *)0,
                                          T_OWN_D, TUNABLE_SRC_MODULE);
    TEST_ASSERT_EQ(rc, STATUS_SUCCESS, "module-namespace tunable registers");
    TEST_ASSERT_EQ(kernel_tunable_get("module.testmod.depth", &v), STATUS_SUCCESS,
                   "module tunable is gettable");
    /* unregister_owner mimics module unload: the tunable disappears. */
    TEST_ASSERT_EQ(kernel_tunable_unregister_owner(T_OWN_D), 1u,
                   "module unload removes exactly its tunable");
    TEST_ASSERT_EQ(kernel_tunable_get("module.testmod.depth", &v), STATUS_NOT_FOUND,
                   "module tunable gone after unload");
}

/* Recursive self-write guard: a change callback that tries to set its own
 * tunable must be refused with STATUS_UNSUCCESSFUL. */
static volatile NTSTATUS s_recursive_rc;
static void cb_recursive(const char *name, int64_t v, void *ctx)
{
    (void)v; (void)ctx;
    s_recursive_rc = kernel_tunable_set(name, 1, TUNABLE_SET_PRIVILEGED);
}

static void test_tunable_recursive_guard(void)
{
    /* The callback only runs inline (synchronously) at PASSIVE_LEVEL; above it
     * the dispatch defers to a workqueue that the scheduler-disabled boot-test
     * context never drains. Force PASSIVE for the duration of this check so the
     * guard is exercised deterministically, then restore the prior level. */
    KIRQL saved = KeGetCurrentIrql();
    if (saved != PASSIVE_LEVEL)
        KeLowerIrql(PASSIVE_LEVEL);

    s_recursive_rc = STATUS_SUCCESS;
    kernel_tunable_register("test.recurse", TUNABLE_UINT, TUNABLE_RUNTIME,
                            0, 10, 0, cb_recursive, (void *)0,
                            T_OWN_E, TUNABLE_SRC_BUILTIN);
    KIRQL at_set = KeGetCurrentIrql();
    NTSTATUS outer = kernel_tunable_set("test.recurse", 5, TUNABLE_SET_PRIVILEGED);
    TEST_ASSERT_EQ(at_set, PASSIVE_LEVEL, "set runs at PASSIVE after lower");
    TEST_ASSERT_EQ(outer, STATUS_SUCCESS, "inline set returns success");
    TEST_ASSERT_EQ(s_recursive_rc, STATUS_UNSUCCESSFUL,
                   "recursive self-write refused inside callback");
    kernel_tunable_unregister_owner(T_OWN_E);

    if (saved != PASSIVE_LEVEL) {
        KIRQL old;
        KeRaiseIrql(saved, &old);
    }
}

/* Callback contract: the inline callback receives the right name/value/ctx at
 * PASSIVE_LEVEL, sees the published current value, and the guard clears after. */
static volatile int      s_cb_count;
static volatile int64_t  s_cb_value;
static volatile void    *s_cb_ctx;
static volatile KIRQL    s_cb_irql;
static volatile int64_t  s_cb_seen_cur;
static void cb_record(const char *name, int64_t v, void *ctx)
{
    s_cb_count++;
    s_cb_value = v;
    s_cb_ctx = ctx;
    s_cb_irql = KeGetCurrentIrql();
    kernel_tunable_get(name, (int64_t *)&s_cb_seen_cur);
}

static void test_tunable_callback_semantics(void)
{
    int marker = 0;
    s_cb_count = 0; s_cb_value = -1; s_cb_ctx = (void *)0;
    s_cb_irql = 0xFF; s_cb_seen_cur = -1;

    KIRQL saved = KeGetCurrentIrql();
    if (saved != PASSIVE_LEVEL) KeLowerIrql(PASSIVE_LEVEL);

    kernel_tunable_register("test.cb", TUNABLE_UINT, TUNABLE_RUNTIME,
                            0, 100, 0, cb_record, &marker, T_OWN_G, TUNABLE_SRC_BUILTIN);
    NTSTATUS r = kernel_tunable_set("test.cb", 6, TUNABLE_SET_PRIVILEGED);
    TEST_ASSERT_EQ(r, STATUS_SUCCESS, "inline callback set succeeds");
    TEST_ASSERT_EQ(s_cb_count, 1, "callback invoked exactly once");
    TEST_ASSERT_EQ(s_cb_value, 6, "callback received the new value");
    TEST_ASSERT_EQ((void *)s_cb_ctx, (void *)&marker, "callback received its ctx");
    TEST_ASSERT_EQ(s_cb_irql, PASSIVE_LEVEL, "callback runs at PASSIVE_LEVEL");
    TEST_ASSERT_EQ(s_cb_seen_cur, 6, "current value published before callback runs");
    /* Guard cleared: a second set must succeed, not be refused as recursive. */
    TEST_ASSERT_EQ(kernel_tunable_set("test.cb", 7, TUNABLE_SET_PRIVILEGED),
                   STATUS_SUCCESS, "in_callback guard cleared after callback returns");
    kernel_tunable_unregister_owner(T_OWN_G);

    if (saved != PASSIVE_LEVEL) { KIRQL o; KeRaiseIrql(saved, &o); }
}

/* Lock-phase sealing: BOOT_ONLY writable in BOOT, sealed at RUNTIME, runtime
 * tunable still writable, and the advance is monotonic (no downgrade). LOCKED
 * is deliberately NOT exercised in-process: it is terminal + monotonic, so
 * advancing to it here would seal the registry for the whole boot session and
 * neuter every runtime tunable -- validated instead by the access-flag paths. */
static void test_tunable_phase_seal(void)
{
    kernel_tunable_register("test.bo", TUNABLE_UINT, TUNABLE_BOOT_ONLY,
                            0, 10, 0, (tunable_cb_t)0, (void *)0,
                            T_OWN_F, TUNABLE_SRC_BUILTIN);
    kernel_tunable_register("test.rt", TUNABLE_UINT, TUNABLE_RUNTIME,
                            0, 10, 0, (tunable_cb_t)0, (void *)0,
                            T_OWN_F, TUNABLE_SRC_BUILTIN);

    if (kernel_tunable_lock_phase_get() == TUNABLE_PHASE_BOOT)
        TEST_ASSERT_EQ(kernel_tunable_set("test.bo", 3, TUNABLE_SET_PRIVILEGED),
                       STATUS_SUCCESS, "boot-only writable during BOOT phase");

    kernel_tunable_lock_phase_advance(TUNABLE_PHASE_RUNTIME);
    TEST_ASSERT_EQ(kernel_tunable_lock_phase_get(), TUNABLE_PHASE_RUNTIME,
                   "phase advanced to RUNTIME");
    TEST_ASSERT_EQ(kernel_tunable_set("test.bo", 4, TUNABLE_SET_PRIVILEGED),
                   STATUS_ACCESS_DENIED, "boot-only sealed at RUNTIME");
    TEST_ASSERT_EQ(kernel_tunable_set("test.rt", 4, TUNABLE_SET_PRIVILEGED),
                   STATUS_SUCCESS, "runtime tunable still writable at RUNTIME");

    /* Monotonic: a downgrade request is ignored. */
    kernel_tunable_lock_phase_advance(TUNABLE_PHASE_BOOT);
    TEST_ASSERT_EQ(kernel_tunable_lock_phase_get(), TUNABLE_PHASE_RUNTIME,
                   "phase advance is monotonic; downgrade refused");
    /* Out-of-range phase requests are rejected (cannot escape the model),
     * including values whose low byte aliases a valid phase (257, 258). */
    kernel_tunable_lock_phase_advance((tunable_phase_t)99);
    kernel_tunable_lock_phase_advance((tunable_phase_t)257);
    kernel_tunable_lock_phase_advance((tunable_phase_t)258);
    TEST_ASSERT_EQ(kernel_tunable_lock_phase_get(), TUNABLE_PHASE_RUNTIME,
                   "out-of-range phase requests (99/257/258) ignored");
    kernel_tunable_unregister_owner(T_OWN_F);
}

static void test_tunable_audit(void)
{
    tunable_snapshot_t rows[64];
    uint32_t before = kernel_tunable_count();
    kernel_tunable_register("test.audit", TUNABLE_ENUM,
                            TUNABLE_RUNTIME | TUNABLE_PRIVILEGED,
                            2, 9, 5, (tunable_cb_t)0, (void *)0,
                            T_OWN_H, TUNABLE_SRC_MODULE);
    TEST_ASSERT_EQ(kernel_tunable_count(), before + 1, "count tracks registration");

    /* dump edge cases */
    TEST_ASSERT_EQ(kernel_tunable_dump((tunable_snapshot_t *)0, 1), 0u,
                   "dump rejects NULL buffer");
    TEST_ASSERT_EQ(kernel_tunable_dump(rows, 0), 0u, "dump rejects zero rows");

    uint32_t n = kernel_tunable_dump(rows, 64);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(rows[i].name, "test.audit") != 0) continue;
        found = 1;
        TEST_ASSERT_EQ(rows[i].cur, 5, "dump row current value");
        TEST_ASSERT_EQ(rows[i].def, 5, "dump row default");
        TEST_ASSERT_EQ(rows[i].min, 2, "dump row min");
        TEST_ASSERT_EQ(rows[i].max, 9, "dump row max");
        TEST_ASSERT_EQ(rows[i].type, TUNABLE_ENUM, "dump row type");
        TEST_ASSERT_EQ(rows[i].owner_subsys, T_OWN_H, "dump row owner");
        TEST_ASSERT_EQ(rows[i].source, TUNABLE_SRC_MODULE, "dump row provenance");
        TEST_ASSERT_EQ(rows[i].flags, TUNABLE_RUNTIME | TUNABLE_PRIVILEGED,
                       "dump row flags");
    }
    TEST_ASSERT_EQ(found, 1, "registered tunable appears in audit dump");

    /* get_u64 helper: returns value, fallback on unknown, fallback on negative. */
    TEST_ASSERT_EQ(kernel_tunable_get_u64("test.audit", 99), 5u,
                   "get_u64 returns current value");
    TEST_ASSERT_EQ(kernel_tunable_get_u64("test.nope", 99), 99u,
                   "get_u64 returns fallback for unknown name");
    kernel_tunable_register("test.neg", TUNABLE_INT, TUNABLE_RUNTIME,
                            -10, 10, -3, (tunable_cb_t)0, (void *)0,
                            T_OWN_H, TUNABLE_SRC_BUILTIN);
    TEST_ASSERT_EQ(kernel_tunable_get_u64("test.neg", 99), 99u,
                   "get_u64 returns fallback for negative value");

    kernel_tunable_unregister_owner(T_OWN_H);
    TEST_ASSERT_EQ(kernel_tunable_count(), before, "count restored after unregister");
}

/* Slot reuse must not leak stale name tail bytes into the audit dump. */
static void test_tunable_reuse_name_hygiene(void)
{
    tunable_snapshot_t rows[64];
    /* Register a long name, then drop it so its slot becomes free. */
    kernel_tunable_register("test.reuse.long.name.xyz", TUNABLE_UINT,
                            TUNABLE_RUNTIME, 0, 1, 0, (tunable_cb_t)0, (void *)0,
                            T_OWN_H, TUNABLE_SRC_BUILTIN);
    kernel_tunable_unregister_owner(T_OWN_H);
    /* A short name reuses the freed slot; its dumped name must be NUL-clean. */
    kernel_tunable_register("test.s", TUNABLE_UINT, TUNABLE_RUNTIME,
                            0, 1, 0, (tunable_cb_t)0, (void *)0,
                            T_OWN_H, TUNABLE_SRC_BUILTIN);
    uint32_t n = kernel_tunable_dump(rows, 64);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(rows[i].name, "test.s") != 0) continue;
        found = 1;
        /* Every byte at/after the terminator must be zero (no stale suffix). */
        int clean = 1;
        for (uint32_t j = 6 /* len("test.s") */; j < sizeof(rows[i].name); j++)
            if (rows[i].name[j] != 0) clean = 0;
        TEST_ASSERT_EQ(clean, 1, "reused slot name tail is zeroed in dump");
    }
    TEST_ASSERT_EQ(found, 1, "reused short-name tunable present in dump");
    kernel_tunable_unregister_owner(T_OWN_H);
}

static void test_tunable_malformed_and_capacity(void)
{
    /* Malformed descriptors are rejected. */
    TEST_ASSERT_EQ(kernel_tunable_register((const char *)0, TUNABLE_UINT,
                   TUNABLE_RUNTIME, 0, 1, 0, (tunable_cb_t)0, (void *)0, T_OWN_I,
                   TUNABLE_SRC_BUILTIN), STATUS_INVALID_PARAMETER, "NULL name rejected");
    TEST_ASSERT_EQ(kernel_tunable_register("", TUNABLE_UINT, TUNABLE_RUNTIME,
                   0, 1, 0, (tunable_cb_t)0, (void *)0, T_OWN_I, TUNABLE_SRC_BUILTIN),
                   STATUS_INVALID_PARAMETER, "empty name rejected");
    TEST_ASSERT_EQ(kernel_tunable_register(
                   "test.this.name.is.way.too.long.for.the.forty.eight.cap",
                   TUNABLE_UINT, TUNABLE_RUNTIME, 0, 1, 0, (tunable_cb_t)0, (void *)0,
                   T_OWN_I, TUNABLE_SRC_BUILTIN), STATUS_INVALID_PARAMETER,
                   "over-length name rejected");
    TEST_ASSERT_EQ(kernel_tunable_register("test.badrange", TUNABLE_UINT,
                   TUNABLE_RUNTIME, 10, 5, 7, (tunable_cb_t)0, (void *)0, T_OWN_I,
                   TUNABLE_SRC_BUILTIN), STATUS_INVALID_PARAMETER, "min>max rejected");
    TEST_ASSERT_EQ(kernel_tunable_register("test.deflow", TUNABLE_UINT,
                   TUNABLE_RUNTIME, 0, 10, -1, (tunable_cb_t)0, (void *)0, T_OWN_I,
                   TUNABLE_SRC_BUILTIN), STATUS_INVALID_PARAMETER, "default below min rejected");
    TEST_ASSERT_EQ(kernel_tunable_register("test.defhigh", TUNABLE_UINT,
                   TUNABLE_RUNTIME, 0, 10, 11, (tunable_cb_t)0, (void *)0, T_OWN_I,
                   TUNABLE_SRC_BUILTIN), STATUS_INVALID_PARAMETER, "default above max rejected");

    /* Capacity exhaustion: register until the table is full, then assert the
     * first non-success is STATUS_INSUFFICIENT_RESOURCES; unregister restores. */
    uint32_t before = kernel_tunable_count();
    char name[24];
    NTSTATUS last = STATUS_SUCCESS;
    int registered = 0;
    for (int i = 0; i < 128 && last == STATUS_SUCCESS; i++) {
        /* name = "test.cap.<i>" */
        int p = 0;
        const char *pfx = "test.cap.";
        while (pfx[p]) { name[p] = pfx[p]; p++; }
        if (i >= 10) name[p++] = (char)('0' + (i / 10));
        name[p++] = (char)('0' + (i % 10));
        name[p] = 0;
        last = kernel_tunable_register(name, TUNABLE_UINT, TUNABLE_RUNTIME,
                                       0, 1, 0, (tunable_cb_t)0, (void *)0,
                                       T_OWN_I, TUNABLE_SRC_BUILTIN);
        if (last == STATUS_SUCCESS) registered++;
    }
    TEST_ASSERT_EQ(last, STATUS_INSUFFICIENT_RESOURCES,
                   "registration fails with insufficient-resources when full");
    TEST_ASSERT(registered > 0, "at least one capacity tunable registered");
    kernel_tunable_unregister_owner(T_OWN_I);
    TEST_ASSERT_EQ(kernel_tunable_count(), before, "count restored after capacity unregister");
}

/* ---- Feature Flag Gates and Experiment Cohorts -------------------------- */

static void test_feature_namespace(void)
{
    /* Names outside the reserved namespaces are rejected. */
    TEST_ASSERT_EQ(kernel_feature_register("badname", 1, 0, 0, 0), -1,
                   "non-namespaced feature name rejected");
    TEST_ASSERT_EQ(kernel_feature_register("kpti", 0, 0, 0, 0), -1,
                   "bare name (no namespace) rejected");
    TEST_ASSERT_EQ(kernel_feature_register("feature.ns_ok", 1, 0, 0, 0), 0,
                   "feature. namespace accepted");
    TEST_ASSERT_EQ(kernel_feature_register("experiment.ns_exp", 0, 0, 0, 0), 0,
                   "experiment. namespace accepted");
    /* Anonymous prefix-only names are rejected (no flag after the namespace). */
    TEST_ASSERT_EQ(kernel_feature_register("feature.", 1, 0, 0, 0), -1,
                   "prefix-only feature. rejected");
    TEST_ASSERT_EQ(kernel_feature_register("experiment.", 0, 0, 0, 0), -1,
                   "prefix-only experiment. rejected");
    /* Duplicate + out-of-range percent rejected. */
    TEST_ASSERT_EQ(kernel_feature_register("feature.ns_ok", 1, 0, 0, 0), -1,
                   "duplicate feature name rejected");
    TEST_ASSERT_EQ(kernel_feature_register("feature.badpct", 1, 0, 101, 0), -1,
                   "rollout_percent > 100 rejected");
    /* >255 must be rejected, not truncated (300 % 256 == 44 would slip a u8). */
    TEST_ASSERT_EQ(kernel_feature_register("feature.bigpct", 1, 0, 300, 0), -1,
                   "rollout_percent > 255 rejected, not truncated");
    TEST_ASSERT_EQ(kernel_feature_register((const char *)0, 1, 0, 0, 0), -1,
                   "NULL feature name rejected");
    /* Length boundary: 47-char name accepted, 48-char (== cap) rejected.
     * Build the names so the lengths are exact (no literal miscount). */
    {
        char nm[64];
        const char *pfx = "feature.";   /* 8 chars */
        uint32_t p = 0;
        for (; pfx[p]; p++) nm[p] = pfx[p];
        uint32_t base = p;
        for (uint32_t j = 0; j < 39; j++) nm[base + j] = 'a';  /* 8+39 = 47 */
        nm[base + 39] = 0;
        TEST_ASSERT_EQ(kernel_feature_register(nm, 1, 0, 0, 0), 0, "47-char name accepted");
        for (uint32_t j = 0; j < 40; j++) nm[base + j] = 'b';  /* 8+40 = 48 */
        nm[base + 40] = 0;
        TEST_ASSERT_EQ(kernel_feature_register(nm, 1, 0, 0, 0), -1,
                       "48-char name (== cap) rejected");
    }
}

static void test_feature_default(void)
{
    kernel_feature_register("feature.def_on", 1, 0, 0, 0);
    kernel_feature_register("feature.def_off", 0, 0, 0, 0);
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.def_on"), 1,
                   "default-on feature resolves enabled");
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.def_off"), 0,
                   "default-off feature resolves disabled");
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.absent"), 0,
                   "unknown feature resolves disabled");
}

static void test_feature_cmdline_override(void)
{
    /* Inject a raw cmdline; restore it after. */
    char saved[BOOT_CONF_CMDLINE_MAX];
    for (uint32_t i = 0; i < BOOT_CONF_CMDLINE_MAX; i++)
        saved[i] = g_boot_info.config.cmdline[i];

    const char *inj = "quiet feature.ovr_off=off feature.ovr_on=on splash";
    uint32_t k = 0;
    for (; inj[k] && k < BOOT_CONF_CMDLINE_MAX - 1; k++)
        g_boot_info.config.cmdline[k] = inj[k];
    g_boot_info.config.cmdline[k] = 0;

    /* Override beats the registered default in both directions. */
    kernel_feature_register("feature.ovr_off", 1, 0, 0, 0);
    kernel_feature_register("feature.ovr_on", 0, 0, 0, 0);
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.ovr_off"), 0,
                   "cmdline =off overrides default-on");
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.ovr_on"), 1,
                   "cmdline =on overrides default-off");

    for (uint32_t i = 0; i < BOOT_CONF_CMDLINE_MAX; i++)
        g_boot_info.config.cmdline[i] = saved[i];
}

static void test_feature_cohort_deterministic(void)
{
    /* Cohort resolution is cached + deterministic: same feature, same answer. */
    kernel_feature_register("experiment.coh", 0, 0, 50, 0);
    int a = kernel_feature_enabled("experiment.coh");
    int b = kernel_feature_enabled("experiment.coh");
    TEST_ASSERT_EQ(a, b, "cohort resolution is stable across calls");
    /* rollout_percent 0 ignores cohort and uses the default. */
    kernel_feature_register("experiment.no_roll", 0, 0, 0, 0);
    TEST_ASSERT_EQ(kernel_feature_enabled("experiment.no_roll"), 0,
                   "rollout 0 uses the registered default");
}

static int feature_find_row(const char *name, feature_snapshot_t *out)
{
    feature_snapshot_t rows[64];
    uint32_t n = kernel_feature_dump(rows, 64);
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(rows[i].name, name) == 0) { *out = rows[i]; return 1; }
    return 0;
}

static void test_feature_audit(void)
{
    feature_snapshot_t row;
    uint32_t before = kernel_feature_count();
    TEST_ASSERT_EQ(kernel_feature_register("experiment.aud", 1, 0, 25, 7), 0,
                   "register audit feature");
    TEST_ASSERT_EQ(kernel_feature_count(), before + 1, "count increments on register");
    TEST_ASSERT_EQ(kernel_feature_register("experiment.aud", 1, 0, 25, 7), -1,
                   "duplicate register fails");
    TEST_ASSERT_EQ(kernel_feature_count(), before + 1,
                   "failed register leaves count unchanged");
    TEST_ASSERT_EQ(kernel_feature_dump((feature_snapshot_t *)0, 1), 0u, "dump NULL -> 0");
    TEST_ASSERT_EQ(kernel_feature_dump(&row, 0), 0u, "dump 0 rows -> 0");
    TEST_ASSERT_EQ(kernel_feature_dump(&row, 1), 1u, "dump respects max_rows cap");
    TEST_ASSERT_EQ(feature_find_row("experiment.aud", &row), 1, "audit feature in dump");
    TEST_ASSERT(row.flags & FEATURE_EXPERIMENT, "experiment.* row carries FEATURE_EXPERIMENT");
    TEST_ASSERT_EQ(row.default_enabled, 1, "dump exposes default");
    TEST_ASSERT_EQ(row.rollout_percent, 25, "dump exposes rollout");
    TEST_ASSERT_EQ(row.owner, 7, "dump exposes owner");
}

static void test_feature_locked(void)
{
    char saved[BOOT_CONF_CMDLINE_MAX];
    for (uint32_t i = 0; i < BOOT_CONF_CMDLINE_MAX; i++)
        saved[i] = g_boot_info.config.cmdline[i];
    const char *inj = "feature.lock_x=on";
    uint32_t k = 0;
    for (; inj[k] && k < BOOT_CONF_CMDLINE_MAX - 1; k++)
        g_boot_info.config.cmdline[k] = inj[k];
    g_boot_info.config.cmdline[k] = 0;

    /* A locked feature ignores the cmdline override; default is authoritative. */
    kernel_feature_register("feature.lock_x", 0, FEATURE_LOCKED, 0, 0);
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.lock_x"), 0,
                   "locked feature ignores cmdline override");
    feature_snapshot_t row;
    if (feature_find_row("feature.lock_x", &row))
        TEST_ASSERT_EQ(row.source, FEATURE_SRC_DEFAULT, "locked feature resolves from default");

    for (uint32_t i = 0; i < BOOT_CONF_CMDLINE_MAX; i++)
        g_boot_info.config.cmdline[i] = saved[i];
}

static void test_feature_cmdline_aliases(void)
{
    char saved[BOOT_CONF_CMDLINE_MAX];
    for (uint32_t i = 0; i < BOOT_CONF_CMDLINE_MAX; i++)
        saved[i] = g_boot_info.config.cmdline[i];
    const char *inj =
        "feature.t1=true feature.t2=1 feature.t3=yes feature.t4=enabled "
        "feature.f1=false feature.f2=0 feature.f3=no feature.f4=disabled "
        "feature.badv=enable";
    uint32_t k = 0;
    for (; inj[k] && k < BOOT_CONF_CMDLINE_MAX - 1; k++)
        g_boot_info.config.cmdline[k] = inj[k];
    g_boot_info.config.cmdline[k] = 0;

    kernel_feature_register("feature.t1", 0, 0, 0, 0);
    kernel_feature_register("feature.t2", 0, 0, 0, 0);
    kernel_feature_register("feature.t3", 0, 0, 0, 0);
    kernel_feature_register("feature.t4", 0, 0, 0, 0);
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.t1"), 1, "alias true -> on");
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.t2"), 1, "alias 1 -> on");
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.t3"), 1, "alias yes -> on");
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.t4"), 1, "alias enabled -> on");
    kernel_feature_register("feature.f1", 1, 0, 0, 0);
    kernel_feature_register("feature.f2", 1, 0, 0, 0);
    kernel_feature_register("feature.f3", 1, 0, 0, 0);
    kernel_feature_register("feature.f4", 1, 0, 0, 0);
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.f1"), 0, "alias false -> off");
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.f2"), 0, "alias 0 -> off");
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.f3"), 0, "alias no -> off");
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.f4"), 0, "alias disabled -> off");
    /* Unrecognized value falls back to the default (not misparsed). */
    kernel_feature_register("feature.badv", 1, 0, 0, 0);
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.badv"), 1,
                   "invalid override value falls back to default");
    feature_snapshot_t row;
    if (feature_find_row("feature.badv", &row))
        TEST_ASSERT_EQ(row.source, FEATURE_SRC_DEFAULT, "invalid value resolves from default");
    /* Override source is recorded as cmdline. */
    if (feature_find_row("feature.t1", &row))
        TEST_ASSERT_EQ(row.source, FEATURE_SRC_CMDLINE, "override source is cmdline");

    for (uint32_t i = 0; i < BOOT_CONF_CMDLINE_MAX; i++)
        g_boot_info.config.cmdline[i] = saved[i];
}

static void test_feature_cmdline_boundary(void)
{
    char saved[BOOT_CONF_CMDLINE_MAX];
    for (uint32_t i = 0; i < BOOT_CONF_CMDLINE_MAX; i++)
        saved[i] = g_boot_info.config.cmdline[i];
    /* None of these tokens is an EXACT "feature.bnd=" match. */
    const char *inj = "xfeature.bnd=off feature.bnd.extra=off feature.bndx=off";
    uint32_t k = 0;
    for (; inj[k] && k < BOOT_CONF_CMDLINE_MAX - 1; k++)
        g_boot_info.config.cmdline[k] = inj[k];
    g_boot_info.config.cmdline[k] = 0;

    kernel_feature_register("feature.bnd", 1, 0, 0, 0);
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.bnd"), 1,
                   "substring/prefix tokens do not false-match the feature");

    /* Tab-delimited overrides parse like the boot-arg parser (space AND tab). */
    const char *inj2 = "foo=bar\tfeature.tabd=off\tquiet";
    k = 0;
    for (; inj2[k] && k < BOOT_CONF_CMDLINE_MAX - 1; k++)
        g_boot_info.config.cmdline[k] = inj2[k];
    g_boot_info.config.cmdline[k] = 0;
    kernel_feature_register("feature.tabd", 1, 0, 0, 0);
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.tabd"), 0,
                   "tab-delimited override is parsed");

    for (uint32_t i = 0; i < BOOT_CONF_CMDLINE_MAX; i++)
        g_boot_info.config.cmdline[i] = saved[i];
}

static void test_feature_secure_boot_guard(void)
{
    uint8_t saved_sb = g_boot_info.secure_boot_enabled;
    uint32_t saved_dt = g_boot_info.degraded_trust_flags;
    char saved_cl[BOOT_CONF_CMDLINE_MAX];
    for (uint32_t i = 0; i < BOOT_CONF_CMDLINE_MAX; i++)
        saved_cl[i] = g_boot_info.config.cmdline[i];

    const char *inj = "feature.sec_a=off feature.sec_b=off";
    uint32_t k = 0;
    for (; inj[k] && k < BOOT_CONF_CMDLINE_MAX - 1; k++)
        g_boot_info.config.cmdline[k] = inj[k];
    g_boot_info.config.cmdline[k] = 0;

    /* Secure Boot ACTIVE: a security feature cannot be disabled (fail closed). */
    g_boot_info.secure_boot_enabled = 1;
    g_boot_info.degraded_trust_flags = 0;
    kernel_feature_register("feature.sec_a", 1, FEATURE_SECURITY, 0, 0);
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.sec_a"), 1,
                   "security feature disable refused under active Secure Boot");
    {
        feature_snapshot_t r;
        if (feature_find_row("feature.sec_a", &r))
            TEST_ASSERT_EQ(r.source, FEATURE_SRC_SB_GUARD,
                           "blocked override records Secure Boot guard source");
    }

    /* Secure Boot KNOWN-OFF (readable + inactive): disable is allowed. */
    g_boot_info.secure_boot_enabled = 0;
    g_boot_info.degraded_trust_flags = 0;
    kernel_feature_register("feature.sec_b", 1, FEATURE_SECURITY, 0, 0);
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.sec_b"), 0,
                   "security feature disable allowed when Secure Boot known-off");

    /* Secure Boot UNREADABLE (unknown): fail closed even with sb_enabled==0. */
    const char *inj2 = "feature.sec_c=off";
    k = 0;
    for (; inj2[k] && k < BOOT_CONF_CMDLINE_MAX - 1; k++)
        g_boot_info.config.cmdline[k] = inj2[k];
    g_boot_info.config.cmdline[k] = 0;
    g_boot_info.secure_boot_enabled = 0;
    g_boot_info.degraded_trust_flags = BOOT_DEGRADED_TRUST_SECURE_BOOT_UNREADABLE;
    kernel_feature_register("feature.sec_c", 1, FEATURE_SECURITY, 0, 0);
    TEST_ASSERT_EQ(kernel_feature_enabled("feature.sec_c"), 1,
                   "security feature disable refused when Secure Boot state unknown");

    g_boot_info.secure_boot_enabled = saved_sb;
    g_boot_info.degraded_trust_flags = saved_dt;
    for (uint32_t i = 0; i < BOOT_CONF_CMDLINE_MAX; i++)
        g_boot_info.config.cmdline[i] = saved_cl[i];
}

/* ---- Native Query Config Syscall (read path) ---------------------------- */
/* SYSTEM_KERNEL_CONFIG_INFORMATION marshaller, exercised directly: at CPL 0 the
 * IfUser probes are no-ops and copy_to_user to a kernel buffer works. The SET
 * path (NtSetSystemInformation config write) is deferred until the security
 * reference monitor provides SeSinglePrivilegeCheck + the per-token lock. */
extern NTSTATUS nt_query_kernel_config_information(void *buffer, uint32_t buf_size,
                                                   uint32_t *return_length);

static void test_cfg_query_syscall(void)
{
    SYSTEM_KERNEL_CONFIG_INFORMATION out;
    uint32_t need = 0;
    const uint32_t want = (uint32_t)sizeof(SYSTEM_KERNEL_CONFIG_INFORMATION);

    /* Undersized buffer -> INFO_LENGTH_MISMATCH, required size reported. */
    NTSTATUS rc = nt_query_kernel_config_information(&out, 4, &need);
    TEST_ASSERT_EQ(rc, STATUS_INFO_LENGTH_MISMATCH, "undersized buffer rejected");
    TEST_ASSERT_EQ(need, want, "required size reported");
    /* NULL buffer with zero size -> INFO_LENGTH_MISMATCH. */
    TEST_ASSERT_EQ(nt_query_kernel_config_information((void *)0, 0, &need),
                   STATUS_INFO_LENGTH_MISMATCH, "NULL/zero buffer rejected");

    /* Adequate buffer -> success, every field mirrors the published snapshot. */
    rc = nt_query_kernel_config_information(&out, want, &need);
    TEST_ASSERT_EQ(rc, STATUS_SUCCESS, "query succeeds with adequate buffer");
    const kernel_config_t *kc = kernel_config_get();
    TEST_ASSERT_NOT_NULL(kc, "snapshot present");
    TEST_ASSERT_EQ(out.Version, kc->version, "queried version mirrors snapshot");
    TEST_ASSERT_EQ(out.Size, kc->size, "queried size mirrors snapshot");
    TEST_ASSERT_EQ(out.BootMode, kc->boot_mode, "queried boot_mode mirrors snapshot");
    TEST_ASSERT_EQ(out.SafeMode, kc->safe_mode, "queried safe_mode mirrors snapshot");
    TEST_ASSERT_EQ(out.SafeModeReason, kc->safe_mode_reason, "queried safe_mode_reason");
    TEST_ASSERT_EQ(out.DebugEnabled, kc->debug_enabled, "queried debug_enabled");
    TEST_ASSERT_EQ(out.TestMode, kc->test_mode, "queried test_mode");
    TEST_ASSERT_EQ(out.LockPhase, (uint8_t)kernel_tunable_lock_phase_get(), "queried lock phase");
    TEST_ASSERT_EQ(out.LockdownLevel, (uint8_t)kernel_lockdown_level_get(), "queried lockdown level mirrors policy");
    TEST_ASSERT_EQ(out.Reserved[0], 0, "reserved[0] zeroed (no stack leak)");
    TEST_ASSERT_EQ(out.BootReason, kc->boot_reason, "queried boot_reason");
    TEST_ASSERT_EQ(out.SelectionReason, kc->selection_reason, "queried selection_reason");
    TEST_ASSERT_EQ(out.TunableCount, kernel_tunable_count(), "queried tunable count");
    TEST_ASSERT_EQ(out.FeatureCount, kernel_feature_count(), "queried feature count");

    /* Optional return_length omitted -> still succeeds. */
    TEST_ASSERT_EQ(nt_query_kernel_config_information(&out, want, (uint32_t *)0),
                   STATUS_SUCCESS, "query succeeds without return_length");

    /* Oversized buffer -> success, only the required bytes are written. */
    struct { SYSTEM_KERNEL_CONFIG_INFORMATION body; uint8_t canary; } wrap;
    wrap.canary = 0xA5;
    need = 0;
    rc = nt_query_kernel_config_information(&wrap, (uint32_t)sizeof(wrap), &need);
    TEST_ASSERT_EQ(rc, STATUS_SUCCESS, "oversized buffer accepted");
    TEST_ASSERT_EQ(need, want, "oversized: required size still the struct size");
    TEST_ASSERT_EQ(wrap.canary, 0xA5, "only the required bytes written (canary intact)");
}

/* Exercise the FULL NtQuerySystemInformation route via ssdt_dispatch, not just
 * the marshaller, so a class value / switch-case / SSDT-registration regression
 * is caught (at CPL 0 the IfUser probes are no-ops; copy_to_user to a kernel
 * buffer works). */
static void test_cfg_query_syscall_route(void)
{
    TEST_ASSERT_EQ((uint32_t)SystemKernelConfigInformation, 0x1000u,
                   "config info class value pinned at 0x1000");
    SYSTEM_KERNEL_CONFIG_INFORMATION out;
    uint32_t need = 0;
    NTSTATUS rc = ssdt_dispatch(SSDT_NtQuerySystemInformation,
                                (uint64_t)SystemKernelConfigInformation,
                                (uint64_t)&out, (uint64_t)sizeof(out),
                                (uint64_t)&need, 0, 0);
    TEST_ASSERT_EQ(rc, STATUS_SUCCESS, "config query routes through NtQuerySystemInformation");
    TEST_ASSERT_EQ(need, (uint32_t)sizeof(out), "routed query reports full length");
    const kernel_config_t *kc = kernel_config_get();
    TEST_ASSERT_NOT_NULL(kc, "snapshot present");
    TEST_ASSERT_EQ(out.Version, kc->version, "routed query mirrors snapshot version");
    TEST_ASSERT_EQ(out.TunableCount, kernel_tunable_count(), "routed query mirrors tunable count");
    /* Undersized buffer through the route -> INFO_LENGTH_MISMATCH. */
    rc = ssdt_dispatch(SSDT_NtQuerySystemInformation,
                       (uint64_t)SystemKernelConfigInformation,
                       (uint64_t)&out, 4, (uint64_t)&need, 0, 0);
    TEST_ASSERT_EQ(rc, STATUS_INFO_LENGTH_MISMATCH, "routed undersized buffer rejected");
}

/* ---- Section 9: policy lock phases + tamper audit ----------------------- *
 * The policy registry and lock phase are global singletons; by test time the
 * boot has already advanced s_phase. Tests therefore register policies that
 * seal at PRE_MEMORY (always sealed regardless of current phase) and NEVER
 * advance the phase forward (that would seal real boot policies). The
 * KernelMode SB/CI downgrade -> KeBugCheckEx path is deliberately NOT exercised
 * (it would halt the kernel); it is validated by the on-hardware serial-log
 * test checkpoint. The UserMode downgrade-of-hard-class path IS exercised to
 * prove an untrusted caller cannot trigger the bugcheck. */
static void test_policy_register_validation(void)
{
    TEST_ASSERT_EQ(kernel_policy_register((const char *)0, 0, 100,
                       POLICY_PHASE_PRE_MEMORY, POLICY_CLASS_RATCHET, 0),
                   STATUS_INVALID_PARAMETER, "NULL name rejected");
    TEST_ASSERT_EQ(kernel_policy_register("bad.name", 0, 100,
                       POLICY_PHASE_PRE_MEMORY, POLICY_CLASS_RATCHET, 0),
                   STATUS_INVALID_PARAMETER, "non-policy namespace rejected");
    TEST_ASSERT_EQ(kernel_policy_register("policy.", 0, 100,
                       POLICY_PHASE_PRE_MEMORY, POLICY_CLASS_RATCHET, 0),
                   STATUS_INVALID_PARAMETER, "prefix-only name rejected");
    TEST_ASSERT_EQ(kernel_policy_register("policy.t9.overdomain", 9, 5,
                       POLICY_PHASE_PRE_MEMORY, POLICY_CLASS_RATCHET, 0),
                   STATUS_INVALID_PARAMETER, "initial value above max_value rejected");
    TEST_ASSERT_EQ(kernel_policy_register("policy.t9.valid", 7, 100,
                       POLICY_PHASE_PRE_MEMORY, POLICY_CLASS_RATCHET, 0),
                   STATUS_SUCCESS, "valid policy registered");
    TEST_ASSERT_EQ(kernel_policy_register("policy.t9.valid", 9, 100,
                       POLICY_PHASE_PRE_MEMORY, POLICY_CLASS_RATCHET, 0),
                   STATUS_OBJECT_NAME_COLLISION, "duplicate rejected");
    uint64_t v = 0;
    TEST_ASSERT_EQ(kernel_policy_get("policy.t9.valid", &v), 0, "registered policy readable");
    TEST_ASSERT_EQ(v, 7u, "registered value preserved (collision did not overwrite)");
    TEST_ASSERT_EQ(kernel_policy_get("policy.t9.missing", &v), -1, "unknown policy returns -1");
}

static void test_policy_ratchet_seal(void)
{
    /* Sealed-from-birth ratchet policy (seals at PRE_MEMORY). */
    TEST_ASSERT_EQ(kernel_policy_register("policy.t9.rat", 1, 2,
                       POLICY_PHASE_PRE_MEMORY, POLICY_CLASS_RATCHET, 0),
                   STATUS_SUCCESS, "ratchet policy registered");
    uint64_t before = kernel_policy_tamper_count();

    /* A value above the domain is rejected before any ratchet logic, so it can
     * never poison the row (the lockdown-truncation bypass class). */
    TEST_ASSERT_EQ(kernel_policy_set("policy.t9.rat", 256, POLICY_CALLER_KERNEL),
                   STATUS_INVALID_PARAMETER, "out-of-domain value rejected, not ratcheted");
    uint64_t dv = 0; kernel_policy_get("policy.t9.rat", &dv);
    TEST_ASSERT_EQ(dv, 1u, "out-of-domain write did not enter the row");

    /* Upward (strictly more restrictive) accepted. */
    TEST_ASSERT_EQ(kernel_policy_set("policy.t9.rat", 2, POLICY_CALLER_KERNEL),
                   STATUS_SUCCESS, "ratchet upgrade allowed");
    uint64_t v = 0; kernel_policy_get("policy.t9.rat", &v);
    TEST_ASSERT_EQ(v, 2u, "ratchet upgrade applied");

    /* Equal value -> allowed no-op, NOT counted as tamper. */
    uint64_t mid = kernel_policy_tamper_count();
    TEST_ASSERT_EQ(kernel_policy_set("policy.t9.rat", 2, POLICY_CALLER_KERNEL),
                   STATUS_SUCCESS, "equal-value write is an allowed no-op");
    TEST_ASSERT_EQ(kernel_policy_tamper_count(), mid, "no-op does not count as tamper");

    /* Downward (downgrade) denied, value unchanged, tamper counted + sticky. */
    TEST_ASSERT_EQ(kernel_policy_set("policy.t9.rat", 1, POLICY_CALLER_KERNEL),
                   STATUS_ACCESS_DENIED, "ratchet downgrade denied");
    kernel_policy_get("policy.t9.rat", &v);
    TEST_ASSERT_EQ(v, 2u, "denied downgrade did not change value");
    TEST_ASSERT(kernel_policy_tamper_count() > before, "downgrade counted as tamper");
    TEST_ASSERT_EQ(kernel_policy_tamper_sticky(), 1, "sticky tamper flag set");
}

static void test_policy_hard_user_downgrade_no_panic(void)
{
    /* A hard-class (Secure Boot) policy: a UserMode downgrade after seal MUST
     * return ACCESS_DENIED and NOT bugcheck -- the no-DoS guarantee. */
    TEST_ASSERT_EQ(kernel_policy_register("policy.t9.sb", 1, 1,
                       POLICY_PHASE_PRE_MEMORY,
                       POLICY_CLASS_SECURE_BOOT | POLICY_CLASS_RATCHET, 0),
                   STATUS_SUCCESS, "hard-class policy registered");
    TEST_ASSERT_EQ(kernel_policy_set("policy.t9.sb", 0, POLICY_CALLER_USER),
                   STATUS_ACCESS_DENIED, "UserMode hard downgrade denied, not paniced");
    uint64_t v = 0; kernel_policy_get("policy.t9.sb", &v);
    TEST_ASSERT_EQ(v, 1u, "UserMode hard downgrade did not change value");
}

static void test_policy_nonratchet_locked(void)
{
    /* Non-ratchet sealed policy refuses ANY differing write (even upward). */
    TEST_ASSERT_EQ(kernel_policy_register("policy.t9.lk", 5, 100,
                       POLICY_PHASE_PRE_MEMORY, 0 /* no ratchet */, 0),
                   STATUS_SUCCESS, "non-ratchet policy registered");
    TEST_ASSERT_EQ(kernel_policy_set("policy.t9.lk", 6, POLICY_CALLER_KERNEL),
                   STATUS_ACCESS_DENIED, "non-ratchet sealed: any change denied");
    TEST_ASSERT_EQ(kernel_policy_set("policy.t9.lk", 5, POLICY_CALLER_KERNEL),
                   STATUS_SUCCESS, "non-ratchet sealed: equal value allowed");
    TEST_ASSERT_EQ(kernel_policy_set("policy.t9.missing", 1, POLICY_CALLER_KERNEL),
                   STATUS_INVALID_PARAMETER, "set on unknown policy rejected");
}

static void test_policy_phase_and_lockdown(void)
{
    /* Forward-only phase advance: advancing to the current level or below is a
     * no-op (we never advance forward in tests to avoid sealing boot policy). */
    policy_lock_phase_t cur = kernel_policy_lock_phase_get();
    kernel_policy_lock_phase_advance(cur);
    TEST_ASSERT_EQ(kernel_policy_lock_phase_get(), cur, "advance to current is a no-op");
    if (cur > POLICY_PHASE_PRE_MEMORY) {
        kernel_policy_lock_phase_advance((policy_lock_phase_t)(cur - 1));
        TEST_ASSERT_EQ(kernel_policy_lock_phase_get(), cur, "backward advance ignored");
    }
    /* Out-of-range phase rejected (does not publish an undefined phase). */
    kernel_policy_lock_phase_advance((policy_lock_phase_t)(POLICY_PHASE_POST_USER_MODE + 1));
    TEST_ASSERT_EQ(kernel_policy_lock_phase_get(), cur, "out-of-range advance ignored");

    /* Core registration is idempotent and publishes the lockless lockdown
     * scalar in sync with the "policy.lockdown" row. */
    TEST_ASSERT_EQ(kernel_policy_register_core(), 0,
                   "core registration succeeds (idempotent, all required rows present)");
    TEST_ASSERT(kernel_policy_count() >= 8u, "core policies registered");
    uint64_t ld = 0;
    TEST_ASSERT_EQ(kernel_policy_get("policy.lockdown", &ld), 0, "lockdown policy present");
    TEST_ASSERT_EQ((uint64_t)kernel_lockdown_level_get(), ld,
                   "lockless lockdown scalar mirrors the policy row");

    /* policy.lockdown cannot be (re)registered outside its enum domain -- the
     * guard fires before the dup check, so an out-of-domain value/max can never
     * poison the narrowing-published scalar. */
    kernel_lockdown_level_t ld_before = kernel_lockdown_level_get();
    TEST_ASSERT_EQ(kernel_policy_register("policy.lockdown", 256, 256,
                       POLICY_PHASE_POST_SECURITY_INIT, POLICY_CLASS_RATCHET, 0),
                   STATUS_INVALID_PARAMETER, "out-of-domain lockdown registration rejected");
    TEST_ASSERT_EQ(kernel_lockdown_level_get(), ld_before,
                   "rejected lockdown registration left the scalar unchanged");

    /* Ratchet the lockdown level upward (more restrictive): both the row and
     * the lockless scalar must reflect the new value. policy.lockdown seals at
     * POST_SECURITY_INIT, which the boot has passed, so this exercises the
     * sealed-ratchet upgrade path on a live core policy. */
    NTSTATUS lr = kernel_policy_set("policy.lockdown",
                                    (uint64_t)KERNEL_LOCKDOWN_CONFIDENTIALITY,
                                    POLICY_CALLER_KERNEL);
    TEST_ASSERT_EQ(lr, STATUS_SUCCESS, "lockdown ratchet to confidentiality allowed");
    TEST_ASSERT_EQ((uint64_t)kernel_lockdown_level_get(),
                   (uint64_t)KERNEL_LOCKDOWN_CONFIDENTIALITY,
                   "lockless scalar updated after ratchet upgrade");
    kernel_policy_get("policy.lockdown", &ld);
    TEST_ASSERT_EQ(ld, (uint64_t)KERNEL_LOCKDOWN_CONFIDENTIALITY,
                   "policy row updated after ratchet upgrade");
}

static void test_policy_audit_ring(void)
{
    /* A fresh denied attempt must be the newest audit record. */
    kernel_policy_register("policy.t9.aud", 3, 100, POLICY_PHASE_PRE_MEMORY,
                           POLICY_CLASS_RATCHET, 0);
    TEST_ASSERT_EQ(kernel_policy_set("policy.t9.aud", 1, POLICY_CALLER_USER),
                   STATUS_ACCESS_DENIED, "downgrade denied (for audit)");
    policy_audit_record_t recs[POLICY_AUDIT_RING + 8];
    uint32_t n = kernel_policy_audit_dump(recs, 4);
    TEST_ASSERT(n >= 1u, "audit ring has records");
    TEST_ASSERT_EQ(recs[0].result, (uint8_t)POLICY_RESULT_DENIED_RATCHET,
                   "newest record is the denied downgrade");
    TEST_ASSERT_EQ(recs[0].attempted, 1u, "newest record carries attempted value");
    TEST_ASSERT_EQ(recs[0].caller, (uint8_t)POLICY_CALLER_USER, "newest record carries caller");
    TEST_ASSERT_EQ(kernel_policy_audit_dump((policy_audit_record_t *)0, 4), 0u,
                   "NULL out yields zero");

    /* Wrap the 64-entry ring: a high-value ratchet policy downgraded with
     * distinct attempted values floods the ring past capacity. Verify the dump
     * caps at POLICY_AUDIT_RING, newest-first ordering survives the wrap, the
     * retained-oldest is correct, and tamper_count advances by the exact count.
     * The attempts stay below the current value so each is a denied downgrade
     * (no KeBugCheckEx: this is a plain ratchet, not a hard class). */
    kernel_policy_register("policy.t9.wrap", 5000, 10000, POLICY_PHASE_PRE_MEMORY,
                           POLICY_CLASS_RATCHET, 0);
    const uint32_t floods = POLICY_AUDIT_RING + 5u;  /* 69 denied writes */
    uint64_t tc0 = kernel_policy_tamper_count();
    for (uint32_t i = 0; i < floods; i++) {
        /* attempted = 1000 + i: strictly increasing but always < 5000, so each
         * is a downgrade relative to the unchanged stored value -> denied. */
        kernel_policy_set("policy.t9.wrap", 1000u + i, POLICY_CALLER_KERNEL);
    }
    TEST_ASSERT_EQ(kernel_policy_tamper_count() - tc0, (uint64_t)floods,
                   "every denied flood write counted as tamper");
    n = kernel_policy_audit_dump(recs, POLICY_AUDIT_RING + 8u);
    TEST_ASSERT_EQ(n, (uint32_t)POLICY_AUDIT_RING, "dump caps at ring capacity after wrap");
    TEST_ASSERT_EQ(recs[0].attempted, 1000u + (floods - 1u),
                   "newest record is the last flood attempt");
    /* recs[POLICY_AUDIT_RING-1] is the retained-oldest: floods-64 entries were
     * overwritten, so the oldest surviving attempt is index (floods-64). */
    TEST_ASSERT_EQ(recs[POLICY_AUDIT_RING - 1u].attempted,
                   1000u + (floods - (uint32_t)POLICY_AUDIT_RING),
                   "retained-oldest record correct after wrap");

    /* Allowed writes must NOT evict tamper evidence: record one blocked write,
     * then flood the ring's worth of allowed equal-value no-ops, and confirm the
     * blocked record is still the newest (allowed writes never enter the ring). */
    kernel_policy_register("policy.t9.eko", 2, 100, POLICY_PHASE_PRE_MEMORY,
                           POLICY_CLASS_RATCHET, 0);
    TEST_ASSERT_EQ(kernel_policy_set("policy.t9.eko", 1, POLICY_CALLER_USER),
                   STATUS_ACCESS_DENIED, "downgrade blocked (the tamper record to protect)");
    for (uint32_t k = 0; k < POLICY_AUDIT_RING + 5u; k++)
        TEST_ASSERT_EQ(kernel_policy_set("policy.t9.eko", 2, POLICY_CALLER_KERNEL),
                       STATUS_SUCCESS, "equal-value allowed no-op");
    n = kernel_policy_audit_dump(recs, 2);
    TEST_ASSERT(n >= 1u, "ring still holds the blocked record");
    TEST_ASSERT_EQ(recs[0].result, (uint8_t)POLICY_RESULT_DENIED_RATCHET,
                   "blocked record survived the allowed-write flood (not evicted)");
    TEST_ASSERT_EQ(recs[0].attempted, 1u, "surviving record is the blocked downgrade");
}

/* Pre-seal free-write path including the equal no-op. A policy sealing at the
 * FINAL phase is unsealed iff the global phase has not yet reached it; the test
 * skips when the boot has already sealed everything. */
static void test_policy_preseal_writes(void)
{
    policy_lock_phase_t cur = kernel_policy_lock_phase_get();
    kernel_policy_register("policy.t9.pre", 4, 100, POLICY_PHASE_POST_USER_MODE,
                           POLICY_CLASS_RATCHET, 0);
    if (cur >= POLICY_PHASE_POST_USER_MODE) {
        TEST_SKIP("global phase already POST_USER_MODE -- pre-seal path unreachable");
        return;
    }
    uint64_t tc = kernel_policy_tamper_count();
    TEST_ASSERT_EQ(kernel_policy_set("policy.t9.pre", 4, POLICY_CALLER_KERNEL),
                   STATUS_SUCCESS, "pre-seal equal write is an allowed no-op");
    uint64_t v = 0; kernel_policy_get("policy.t9.pre", &v);
    TEST_ASSERT_EQ(v, 4u, "pre-seal equal write left value unchanged");
    TEST_ASSERT_EQ(kernel_policy_set("policy.t9.pre", 2, POLICY_CALLER_KERNEL),
                   STATUS_SUCCESS, "pre-seal real write applies freely (even a downgrade)");
    kernel_policy_get("policy.t9.pre", &v);
    TEST_ASSERT_EQ(v, 2u, "pre-seal real write changed value");
    TEST_ASSERT_EQ(kernel_policy_tamper_count(), tc, "pre-seal writes are never tamper");
}

/* Fixed-capacity + bounded-name boundaries. Registered LAST among policy tests
 * because the overflow check permanently fills the global registry. */
static void test_policy_capacity_boundary(void)
{
    /* Public bad-phase path. */
    TEST_ASSERT_EQ(kernel_policy_register("policy.t9.badphase", 0, 1,
                       (policy_lock_phase_t)99, POLICY_CLASS_RATCHET, 0),
                   STATUS_INVALID_PARAMETER, "out-of-range seal phase rejected");

    /* Name boundary: "policy." (7) + 32 chars = 39 (< POLICY_NAME_CAP) valid;
     * + 33 chars = 40 (== cap, no room for NUL) rejected. */
    char ok_name[POLICY_NAME_CAP];   /* 39 chars + NUL */
    char big_name[POLICY_NAME_CAP + 2];
    memcpy(ok_name, "policy.", 7);
    for (uint32_t i = 7; i < POLICY_NAME_CAP - 1u; i++) ok_name[i] = 'a';
    ok_name[POLICY_NAME_CAP - 1u] = 0;
    memcpy(big_name, "policy.", 7);
    for (uint32_t i = 7; i < POLICY_NAME_CAP + 1u; i++) big_name[i] = 'b';
    big_name[POLICY_NAME_CAP + 1u] = 0;
    TEST_ASSERT_EQ(kernel_policy_register(ok_name, 1, 1, POLICY_PHASE_PRE_MEMORY,
                                          POLICY_CLASS_RATCHET, 0),
                   STATUS_SUCCESS, "max-length name accepted");
    TEST_ASSERT_EQ(kernel_policy_register(big_name, 1, 1, POLICY_PHASE_PRE_MEMORY,
                                          POLICY_CLASS_RATCHET, 0),
                   STATUS_INVALID_PARAMETER, "over-cap name rejected");

    /* Unterminated exactly-POLICY_NAME_CAP-byte buffer: name_len must stop at
     * the cap and never read byte [POLICY_NAME_CAP]; registration rejects it. */
    char raw_cap[POLICY_NAME_CAP];   /* "policy." + 33 non-NUL = 40 bytes, no NUL */
    memcpy(raw_cap, "policy.", 7);
    for (uint32_t i = 7; i < POLICY_NAME_CAP; i++) raw_cap[i] = 'c';
    TEST_ASSERT_EQ(kernel_policy_register(raw_cap, 1, 1, POLICY_PHASE_PRE_MEMORY,
                                          POLICY_CLASS_RATCHET, 0),
                   STATUS_INVALID_PARAMETER, "unterminated cap-length name rejected (no overread)");

    /* Fill to capacity, then assert overflow returns INSUFFICIENT_RESOURCES and
     * the count does not move on a failed registration. */
    char nm[24];
    memcpy(nm, "policy.fill.", 12);
    NTSTATUS rc = STATUS_SUCCESS;
    for (uint32_t i = 0; i < POLICY_MAX + 4u && rc == STATUS_SUCCESS; i++) {
        nm[12] = (char)('A' + (i / 26));
        nm[13] = (char)('a' + (i % 26));
        nm[14] = 0;
        rc = kernel_policy_register(nm, 0, 1, POLICY_PHASE_PRE_MEMORY,
                                    POLICY_CLASS_RATCHET, 0);
    }
    TEST_ASSERT_EQ(rc, STATUS_INSUFFICIENT_RESOURCES, "registry overflow rejected");
    uint32_t full = kernel_policy_count();
    TEST_ASSERT_EQ(kernel_policy_register("policy.fill.zz", 0, 1,
                       POLICY_PHASE_PRE_MEMORY, POLICY_CLASS_RATCHET, 0),
                   STATUS_INSUFFICIENT_RESOURCES, "second overflow also rejected");
    TEST_ASSERT_EQ(kernel_policy_count(), full, "count stable on failed registration");
}

/* ---- boot status policy + acceptance ledger (TODO-02 boot-status section) -- */

/* Pure policy resolver: enum mapping, recovery flag, default + clamped accept
 * stage, provenance. The live accept_advance / init paths touch NVRAM + A/B
 * disk + per-entry MarkGood and are boot-path (validated on QEMU/bare metal),
 * not unit-testable in WSL, so only the pure helpers are exercised here. */
static void test_boot_status_policy_resolve(void)
{
    boot_status_policy_t p;

    /* Defaults: show failures, recovery on, accept at UI_READY. */
    boot_status_policy_resolve((uint8_t)BOOT_STATUS_DISPLAY_ALL_FAILURES, 1, 0, &p);
    TEST_ASSERT_EQ(p.failure_display, (uint8_t)BOOT_STATUS_DISPLAY_ALL_FAILURES,
                   "default failure display = show");
    TEST_ASSERT_EQ(p.recovery_enabled, 1u, "recovery enabled");
    TEST_ASSERT_EQ(p.accept_stage, (uint8_t)BOOT_ACCEPT_UI_READY,
                   "default accept stage = UI_READY");
    TEST_ASSERT_EQ(p.provenance, (uint8_t)BOOT_STATUS_PROV_DEFAULT, "default provenance");

    /* IgnoreAllFailures + recovery off + cmdline provenance. */
    boot_status_policy_resolve((uint8_t)BOOT_STATUS_IGNORE_ALL_FAILURES, 0, 1, &p);
    TEST_ASSERT_EQ(p.failure_display, (uint8_t)BOOT_STATUS_IGNORE_ALL_FAILURES,
                   "ignore-all-failures honored");
    TEST_ASSERT_EQ(p.recovery_enabled, 0u, "recovery disabled");
    TEST_ASSERT_EQ(p.provenance, (uint8_t)BOOT_STATUS_PROV_CMDLINE, "cmdline provenance");

    /* Out-of-domain display enum clamps to the safe default (show). */
    boot_status_policy_resolve(200, 1, 0, &p);
    TEST_ASSERT_EQ(p.failure_display, (uint8_t)BOOT_STATUS_DISPLAY_ALL_FAILURES,
                   "out-of-domain display clamps to show");
}

/* Monotonic max-advance + exactly-once accept-crossing detection. */
static void test_boot_status_ledger_step(void)
{
    boot_accept_stage_t nw = BOOT_ACCEPT_PENDING;

    /* Advance forward crosses the accept threshold exactly once. */
    int fired = boot_status_ledger_step(BOOT_ACCEPT_PENDING, BOOT_ACCEPT_UI_READY,
                                        BOOT_ACCEPT_UI_READY, &nw);
    TEST_ASSERT_EQ(nw, BOOT_ACCEPT_UI_READY, "advanced to UI_READY");
    TEST_ASSERT_EQ(fired, 1, "crossing accept stage fires once");

    /* Re-advancing at/above accept does not fire again. */
    fired = boot_status_ledger_step(BOOT_ACCEPT_UI_READY, BOOT_ACCEPT_UI_READY,
                                    BOOT_ACCEPT_UI_READY, &nw);
    TEST_ASSERT_EQ(fired, 0, "no re-fire once already at accept stage");

    /* A lower requested stage is a no-op (monotonic): stays at current. */
    fired = boot_status_ledger_step(BOOT_ACCEPT_CRITICAL_READY, BOOT_ACCEPT_UI_READY,
                                    BOOT_ACCEPT_UI_READY, &nw);
    TEST_ASSERT_EQ(nw, BOOT_ACCEPT_CRITICAL_READY, "monotonic: lower request held");
    TEST_ASSERT_EQ(fired, 0, "already past accept -> no fire");

    /* Below-accept advance does not fire. */
    fired = boot_status_ledger_step(BOOT_ACCEPT_PENDING, BOOT_ACCEPT_UI_READY,
                                    BOOT_ACCEPT_REGISTRY_FLUSHED, &nw);
    TEST_ASSERT_EQ(fired, 0, "advance below accept stage does not fire");
}

/* Durable-record CRC: round-trips clean, detects a tampered field. */
static void test_boot_status_record_crc(void)
{
    boot_status_record_t r;
    r.schema_version = BOOT_STATUS_RECORD_VERSION;
    r.last_stage = (uint8_t)BOOT_ACCEPT_ACCEPTED;
    r.failure_bucket = 0;
    r.recovery_suppressed = 1;
    r.rollback_hint = 0;
    r.crc32 = boot_status_record_crc(&r);

    TEST_ASSERT_EQ(r.crc32, boot_status_record_crc(&r), "crc recompute stable");

    boot_status_record_t t = r;
    t.recovery_suppressed = 0;   /* tamper one field */
    TEST_ASSERT(boot_status_record_crc(&t) != r.crc32,
                "crc changes when a covered field is tampered");
}

void test_register_kernel_config(void)
{
    test_suite_register_cat("CONF: tunable register+get",  test_tunable_register_get,      TEST_CAT_BOOT);
    test_suite_register_cat("CONF: tunable clamp",         test_tunable_clamp,             TEST_CAT_BOOT);
    test_suite_register_cat("CONF: tunable access control", test_tunable_access,           TEST_CAT_BOOT);
    test_suite_register_cat("CONF: tunable module namespace", test_tunable_module_namespace, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: tunable recursive guard", test_tunable_recursive_guard, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: tunable callback semantics", test_tunable_callback_semantics, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: tunable audit + helpers", test_tunable_audit,           TEST_CAT_BOOT);
    test_suite_register_cat("CONF: tunable reuse name hygiene", test_tunable_reuse_name_hygiene, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: tunable malformed+capacity", test_tunable_malformed_and_capacity, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: tunable phase seal",    test_tunable_phase_seal,        TEST_CAT_BOOT);
    test_suite_register_cat("CONF: feature namespace",     test_feature_namespace,         TEST_CAT_BOOT);
    test_suite_register_cat("CONF: feature default",       test_feature_default,           TEST_CAT_BOOT);
    test_suite_register_cat("CONF: feature cmdline override", test_feature_cmdline_override, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: feature cohort stable", test_feature_cohort_deterministic, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: feature audit + dump",  test_feature_audit,             TEST_CAT_BOOT);
    test_suite_register_cat("CONF: feature locked",        test_feature_locked,            TEST_CAT_BOOT);
    test_suite_register_cat("CONF: feature cmdline aliases", test_feature_cmdline_aliases, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: feature cmdline boundary", test_feature_cmdline_boundary, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: feature secure-boot guard", test_feature_secure_boot_guard, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: config query syscall",  test_cfg_query_syscall,         TEST_CAT_BOOT);
    test_suite_register_cat("CONF: config query route",    test_cfg_query_syscall_route,   TEST_CAT_BOOT);
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
    test_suite_register_cat("CONF: policy register validation", test_policy_register_validation, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: policy ratchet seal",      test_policy_ratchet_seal,         TEST_CAT_BOOT);
    test_suite_register_cat("CONF: policy hard user no-panic", test_policy_hard_user_downgrade_no_panic, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: policy non-ratchet locked", test_policy_nonratchet_locked,   TEST_CAT_BOOT);
    test_suite_register_cat("CONF: policy phase + lockdown",  test_policy_phase_and_lockdown,   TEST_CAT_BOOT);
    test_suite_register_cat("CONF: policy audit ring",        test_policy_audit_ring,           TEST_CAT_BOOT);
    test_suite_register_cat("CONF: policy pre-seal writes",   test_policy_preseal_writes,       TEST_CAT_BOOT);
    test_suite_register_cat("CONF: policy capacity boundary", test_policy_capacity_boundary,    TEST_CAT_BOOT);
    test_suite_register_cat("CONF: boot-status policy resolve", test_boot_status_policy_resolve, TEST_CAT_BOOT);
    test_suite_register_cat("CONF: boot-status ledger step",  test_boot_status_ledger_step,     TEST_CAT_BOOT);
    test_suite_register_cat("CONF: boot-status record crc",   test_boot_status_record_crc,      TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
