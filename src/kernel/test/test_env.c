/* ============================================================================
 * test_env.c -- Environment variable kernel API unit tests (TODO-22)
 *
 * Exercises the per-process environ storage + kernel API (env_get_copy,
 * env_set, env_unset, env_copy, env_free, env_lock/env_peek_locked) against an
 * in-memory struct task fixture. No live boot infrastructure: only the heap/PMM
 * allocators and the mutex primitive, which are ordinary runtime dependencies.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/env.h"
#include "kernel/env_searchpath.h" /* SearchPathW/A, SetSearchPathMode (s14) */
#include "kernel/fs/vfs.h"         /* vfs_create/vfs_stat/vfs_unlink for probes */
#include "kernel/nt/nt_rtlenv.h" /* RtlExpandEnvironmentStrings_U (UTF-16 path) */
#include "kernel/nt/ssdt.h"      /* ssdt_dispatch (s5 syscall route) */
#include "kernel/nt/service_numbers.h" /* SSDT_Nt{Query,Set}EnvironmentVariable */
#include "kernel/ob/peb.h"       /* UNICODE_STRING */
#include "kernel/nt/ntstatus.h"  /* STATUS_* */
#include "kernel/sched/task.h"
#include "kernel/sched/mutex.h"
#include "kernel/security/token.h" /* ACCESS_TOKEN for s16 elevation tests */
#include "kernel/security/sid.h"   /* SeILLow/Medium/High/System integrity SIDs */
#include "kernel/mm/heap.h"    /* kmalloc_fail_next for the OOM-safety test */
#include "kernel/boot_init.h" /* kernel_subsystem_ready() for the overlay test */
#include "registry.h"         /* HKCU PATH-append test injects a user value */
#include "kernel/types.h"

/* Local ASCII string compare (no live libc dependency in the test TU). */
static int env_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static uint32_t env_test_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* One fixture reused across tests. env_fixture_reset frees the prior test's
 * allocations (no leaks) then re-initializes the mutex + NULL fields, mirroring
 * the boot-time all-slots init in task_init. */
static struct task s_env_fixture;

static void env_fixture_reset(void)
{
    env_free(&s_env_fixture);        /* frees + NULLs any prior environ/argv */
    s_env_fixture.environ = NULL;
    s_env_fixture.environ_count = 0;
    s_env_fixture.argv = NULL;
    s_env_fixture.argc = 0;
    s_env_fixture.search_path_mode = 0;  /* s14: fresh SearchPath ordering per test */
    s_env_fixture.cwd[0] = '\0';         /* s14: no stale cwd into a SearchPath probe */
    s_env_fixture.token = NULL;          /* s16: default non-elevated context per test */
    mutex_init(&s_env_fixture.environ_lock, "test-env");
}

/* --- set / get roundtrip --- */

static void test_env_set_get_roundtrip(void)
{
    char out[64];
    int r;
    env_fixture_reset();
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "GREETING", "hello"), ENV_OK,
                   "env_set stores a new variable");
    r = env_get_copy(&s_env_fixture, "GREETING", out, sizeof(out));
    TEST_ASSERT_EQ(r, 5, "env_get_copy returns the value length");
    TEST_ASSERT(env_streq(out, "hello"), "env_get_copy returns the stored value");
    env_free(&s_env_fixture);   /* heap-neutral: free before per-test leak check */
}

/* --- replace in place --- */

static void test_env_set_replace(void)
{
    char out[64];
    env_fixture_reset();
    env_set(&s_env_fixture, "K", "first");
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "K", "second"), ENV_OK,
                   "env_set replaces an existing variable");
    TEST_ASSERT_EQ(s_env_fixture.environ_count, 1u,
                   "replace does not grow the array");
    env_get_copy(&s_env_fixture, "K", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "second"), "replaced value is returned");
    env_free(&s_env_fixture);
}

/* --- unset --- */

static void test_env_unset(void)
{
    char out[64];
    env_fixture_reset();
    env_set(&s_env_fixture, "GONE", "x");
    TEST_ASSERT_EQ(env_unset(&s_env_fixture, "GONE"), ENV_OK, "env_unset removes it");
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "GONE", out, sizeof(out)),
                   ENV_ERR_NOTFOUND, "get after unset reports not-found");
    TEST_ASSERT_EQ(env_unset(&s_env_fixture, "GONE"), ENV_ERR_NOTFOUND,
                   "unset of an absent var reports not-found");
    env_free(&s_env_fixture);   /* releases the grown pointer array */
}

/* --- absent variable --- */

static void test_env_get_absent(void)
{
    char out[16];
    env_fixture_reset();
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "NOEXIST", out, sizeof(out)),
                   ENV_ERR_NOTFOUND, "absent variable reports not-found");
    TEST_ASSERT(out[0] == '\0', "out is NUL-terminated on not-found");
}

/* --- case-insensitive names (Windows semantics) --- */

static void test_env_case_insensitive(void)
{
    char out[64];
    env_fixture_reset();
    env_set(&s_env_fixture, "PATH", "C:\\Bin");
    env_get_copy(&s_env_fixture, "path", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "C:\\Bin"), "lowercase lookup matches uppercase name");
    /* A ci-equal name replaces the existing entry, not append a duplicate. */
    env_set(&s_env_fixture, "Path", "C:\\Sys");
    TEST_ASSERT_EQ(s_env_fixture.environ_count, 1u,
                   "ci-equal name replaces rather than duplicates");
    env_free(&s_env_fixture);
}

/* --- name / value validation --- */

static void test_env_invalid_name(void)
{
    env_fixture_reset();
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "BAD=NAME", "v"), ENV_ERR_INVAL,
                   "name containing '=' is rejected");
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "", "v"), ENV_ERR_INVAL,
                   "empty name is rejected");
    TEST_ASSERT_EQ(env_set(&s_env_fixture, (const char *)0, "v"), ENV_ERR_INVAL,
                   "NULL name is rejected");
    TEST_ASSERT_EQ(s_env_fixture.environ_count, 0u,
                   "no invalid variable was stored");
    /* Lookups of a syntactically invalid name are rejected as INVAL, not
     * silently treated as an ordinary miss (consistent with env_set). */
    {
        char out[8];
        TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "A=B", out, sizeof(out)),
                       ENV_ERR_INVAL, "get of a '='-name returns INVAL, not NOTFOUND");
        TEST_ASSERT_EQ(env_unset(&s_env_fixture, "A=B"), ENV_ERR_INVAL,
                       "unset of a '='-name returns INVAL");
    }
}

static void test_env_value_too_long(void)
{
    static char big[ENV_VALUE_MAX + 8];
    uint32_t i;
    env_fixture_reset();
    for (i = 0; i < sizeof(big) - 1; i++)
        big[i] = 'v';
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "HUGE", big), ENV_ERR_TOOLONG,
                   "value exceeding ENV_VALUE_MAX is rejected");
}

/* Name-length boundary: exactly ENV_NAME_MAX is storable; one over is TOOLONG
 * (distinct from ENV_ERR_INVAL, which is reserved for NULL/empty/'='). */
static void test_env_name_length_boundary(void)
{
    static char name_max[ENV_NAME_MAX + 1];   /* 256 chars + NUL */
    static char name_over[ENV_NAME_MAX + 2];  /* 257 chars + NUL */
    uint32_t i;
    env_fixture_reset();
    for (i = 0; i < ENV_NAME_MAX; i++)
        name_max[i] = 'N';
    name_max[ENV_NAME_MAX] = '\0';
    for (i = 0; i < ENV_NAME_MAX + 1; i++)
        name_over[i] = 'N';
    name_over[ENV_NAME_MAX + 1] = '\0';
    TEST_ASSERT_EQ(env_set(&s_env_fixture, name_max, "v"), ENV_OK,
                   "a 256-byte name is accepted");
    TEST_ASSERT_EQ(env_set(&s_env_fixture, name_over, "v"), ENV_ERR_TOOLONG,
                   "a 257-byte name returns TOOLONG (not INVAL)");
    /* Lookups classify names identically to env_set: overlength -> TOOLONG. */
    {
        char out[8];
        TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, name_over, out, sizeof(out)),
                       ENV_ERR_TOOLONG, "get of an overlength name returns TOOLONG");
    }
    env_free(&s_env_fixture);
}

/* --- truncating get returns full required length --- */

static void test_env_get_truncation(void)
{
    char out[3];
    int r;
    env_fixture_reset();
    env_set(&s_env_fixture, "K", "HELLO");   /* 5 bytes */
    r = env_get_copy(&s_env_fixture, "K", out, sizeof(out));
    TEST_ASSERT_EQ(r, 5, "truncated get still returns the FULL required length");
    TEST_ASSERT(env_streq(out, "HE"), "out holds the truncated prefix + NUL");
    env_free(&s_env_fixture);
}

/* --- deep copy is independent of the source --- */

static void test_env_copy_independent(void)
{
    static struct task dst;
    char out[64];
    env_fixture_reset();
    dst.environ = NULL;
    dst.environ_count = 0;
    dst.argv = NULL;
    dst.argc = 0;
    mutex_init(&dst.environ_lock, "test-env-dst");

    env_set(&s_env_fixture, "A", "1");
    env_set(&s_env_fixture, "B", "2");
    TEST_ASSERT_EQ(env_copy(&dst, &s_env_fixture), ENV_OK, "env_copy succeeds");
    TEST_ASSERT_EQ(dst.environ_count, 2u, "child inherits both variables");

    /* Mutating the source must not change the copied child. */
    env_set(&s_env_fixture, "A", "999");
    env_get_copy(&dst, "A", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "1"), "child copy is independent of source mutation");

    env_free(&dst);
    TEST_ASSERT_NULL(dst.environ, "env_free NULLs the child environ");
    env_free(&s_env_fixture);   /* free the source too (heap-neutral) */
}

/* --- env_copy of an empty source yields an empty child --- */

static void test_env_copy_empty(void)
{
    static struct task dst;
    env_fixture_reset();
    dst.environ = (char **)0xdead;   /* env_copy must overwrite, not read this */
    dst.environ_count = 99;
    dst.argv = NULL;
    dst.argc = 0;
    mutex_init(&dst.environ_lock, "test-env-dst2");
    TEST_ASSERT_EQ(env_copy(&dst, &s_env_fixture), ENV_OK, "empty copy succeeds");
    TEST_ASSERT_NULL(dst.environ, "empty source yields NULL child environ");
    TEST_ASSERT_EQ(dst.environ_count, 0u, "empty child count is zero");
}

/* --- borrowed-read fast path under the lock --- */

static void test_env_peek_locked(void)
{
    const char *v;
    env_fixture_reset();
    env_set(&s_env_fixture, "PEEK", "value");
    env_lock(&s_env_fixture);
    v = env_peek_locked(&s_env_fixture, "peek");
    TEST_ASSERT_NOT_NULL(v, "env_peek_locked finds a ci-matching name");
    TEST_ASSERT(env_streq(v, "value"), "env_peek_locked returns the value portion");
    TEST_ASSERT_NULL(env_peek_locked(&s_env_fixture, "absent"),
                     "env_peek_locked returns NULL for an absent name");
    env_unlock(&s_env_fixture);
    env_free(&s_env_fixture);
}

/* --- free leaves no dangling state --- */

static void test_env_free_clears(void)
{
    env_fixture_reset();
    env_set(&s_env_fixture, "X", "1");
    env_set(&s_env_fixture, "Y", "2");
    env_free(&s_env_fixture);
    TEST_ASSERT_NULL(s_env_fixture.environ, "env_free NULLs environ");
    TEST_ASSERT_EQ(s_env_fixture.environ_count, 0u, "env_free zeroes the count");
    /* Free of an already-empty task is a no-op, not a fault. */
    env_free(&s_env_fixture);
    TEST_ASSERT_NULL(s_env_fixture.environ, "double env_free stays clean");
}

/* --- page-backed value crossing the 4 KiB kmalloc/PMM boundary --- */

static void test_env_large_value_pmm_path(void)
{
    static char big[ENV_STR_KMALLOC_MAX + 512];   /* entry > 4 KiB -> PMM path */
    static char out[ENV_STR_KMALLOC_MAX + 512];
    uint32_t i;
    int r;
    env_fixture_reset();
    for (i = 0; i < sizeof(big) - 1; i++)
        big[i] = (char)('A' + (i % 26));
    big[sizeof(big) - 1] = '\0';

    TEST_ASSERT_EQ(env_set(&s_env_fixture, "BIG", big), ENV_OK,
                   "a >4 KiB value stores via the page-backed allocator");
    r = env_get_copy(&s_env_fixture, "BIG", out, sizeof(out));
    TEST_ASSERT_EQ((uint32_t)r, env_test_strlen(big),
                   "large value round-trips at full length");
    TEST_ASSERT(env_streq(out, big), "large value round-trips byte-for-byte");
    /* env_unset must free the PMM-backed string via the matching path. */
    TEST_ASSERT_EQ(env_unset(&s_env_fixture, "BIG"), ENV_OK,
                   "large value unsets (frees page-backed string)");
    env_free(&s_env_fixture);   /* releases the grown pointer array */
}

/* --- OOM during a replace leaves the prior value intact (allocate-before-free) --- */

static void test_env_set_oom_preserves_old(void)
{
    char out[64];
    env_fixture_reset();
    env_set(&s_env_fixture, "K", "original");
    /* Arm the next kmalloc to fail: env_set builds the new "K=new" entry FIRST,
     * so the allocation fails before any array/old-value mutation. */
    kmalloc_fail_next();
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "K", "new-value"), ENV_ERR_NOMEM,
                   "env_set reports NOMEM when the entry allocation fails");
    kmalloc_fail_countdown_clear();   /* defensive: disarm any residue */
    env_get_copy(&s_env_fixture, "K", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "original"),
                "OOM during replace leaves the prior value intact");
    env_free(&s_env_fixture);
}

/* --- system default environment: env_init_defaults -------------------------
 * env_init_defaults() reads the (read-only) Registry and env_sets onto the
 * fixture task -- no live boot infrastructure is touched. env_init_kernel_task()
 * mutates the real PID 0, so it is validated via the boot serial line rather
 * than a unit test. */

static int env_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int env_ends_with(const char *s, const char *suffix)
{
    uint32_t sl = env_test_strlen(s), pl = env_test_strlen(suffix);
    if (pl > sl)
        return 0;
    return env_streq(s + (sl - pl), suffix);
}

static void test_env_defaults_synth_base(void)
{
    char out[600];
    env_fixture_reset();
    TEST_ASSERT_EQ(env_init_defaults(&s_env_fixture), ENV_OK,
                   "env_init_defaults populates the fixture");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "SYSTEMROOT", out, sizeof(out)) > 0 &&
                env_streq(out, "C:\\Impossible"), "SYSTEMROOT default lands");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "SYSTEMDRIVE", out, sizeof(out)) > 0 &&
                env_streq(out, "C:"), "SYSTEMDRIVE default lands");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "TEMP", out, sizeof(out)) > 0 &&
                env_streq(out, "C:\\Temp"), "TEMP default lands");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "TMP", out, sizeof(out)) > 0 &&
                env_streq(out, "C:\\Temp"), "TMP mirrors TEMP");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "USERNAME", out, sizeof(out)) > 0 &&
                env_streq(out, "Default"), "USERNAME default account");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "COMPUTERNAME", out, sizeof(out)) > 0 &&
                env_streq(out, "IMPOSSIBLE-PC"), "COMPUTERNAME default/registry");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "PATH", out, sizeof(out)) > 0 &&
                env_starts_with(out, "C:\\Impossible\\Bin"), "PATH base present");
    env_free(&s_env_fixture);
}

static void test_env_defaults_derived(void)
{
    char out[600];
    env_fixture_reset();
    env_init_defaults(&s_env_fixture);
    TEST_ASSERT(env_get_copy(&s_env_fixture, "USERPROFILE", out, sizeof(out)) > 0 &&
                env_streq(out, "C:\\Users\\Default"),
                "USERPROFILE derived from USERNAME (no trailing separator)");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "APPDATA", out, sizeof(out)) > 0 &&
                env_streq(out, "C:\\Users\\Default\\AppData\\Roaming"),
                "APPDATA derived from USERNAME");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "LOCALAPPDATA", out, sizeof(out)) > 0 &&
                env_streq(out, "C:\\Users\\Default\\AppData\\Local"),
                "LOCALAPPDATA derived from USERNAME");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "OS", out, sizeof(out)) > 0 &&
                env_streq(out, "Impossible_OS"), "OS default");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "PROCESSOR_ARCHITECTURE", out,
                             sizeof(out)) > 0 && env_streq(out, "AMD64"),
                "PROCESSOR_ARCHITECTURE default");
    /* PATHEXT default: only .EXE (exec.c runs PE/ELF/EIF; no .CMD/.BAT
     * interpreter -- section 11). */
    TEST_ASSERT(env_get_copy(&s_env_fixture, "PATHEXT", out, sizeof(out)) > 0 &&
                env_streq(out, ".EXE"), "PATHEXT default is .EXE only");
    {
        int r = env_get_copy(&s_env_fixture, "NUMBER_OF_PROCESSORS", out,
                             sizeof(out));
        TEST_ASSERT(r > 0 && out[0] >= '1' && out[0] <= '9',
                    "NUMBER_OF_PROCESSORS is a positive decimal");
    }
    env_free(&s_env_fixture);
}

static void test_env_defaults_registry_overlay(void)
{
    char out[600];
    if (!kernel_subsystem_ready(SUBSYS_REGISTRY)) {
        TEST_SKIP("registry not ready -- system-env overlay exercised post-Phase-2");
        return;
    }
    env_fixture_reset();
    env_init_defaults(&s_env_fixture);
    /* ComSpec lives ONLY in HKLM Session Manager\Environment (it is not
     * synthesised), so its presence proves the system-registry overlay ran. */
    TEST_ASSERT(env_get_copy(&s_env_fixture, "ComSpec", out, sizeof(out)) > 0 &&
                env_streq(out, "C:\\cmd.exe"),
                "ComSpec overlaid from Session Manager Environment key");
    env_free(&s_env_fixture);
}

static void test_env_defaults_user_path_append(void)
{
    char out[700];
    HKEY hk;
    uint32_t disp = 0;
    if (!kernel_subsystem_ready(SUBSYS_REGISTRY)) {
        TEST_SKIP("registry not ready -- HKCU PATH-append exercised post-Phase-2");
        return;
    }
    /* Inject a user PATH into HKCU\Environment; env_init_defaults must APPEND it
     * to the base PATH with ';' rather than replacing. Value is deleted after
     * the read so the live user environment is not polluted. */
    if (RegCreateKeyEx(HKEY_CURRENT_USER, "Environment", 0, (const char *)0, 0,
                       KEY_ALL_ACCESS, (void *)0, &hk, &disp) != ERROR_SUCCESS) {
        TEST_SKIP("cannot open HKCU\\Environment");
        return;
    }
    RegSetString(hk, "PATH", "C:\\Users\\Default\\bin");
    env_fixture_reset();
    env_init_defaults(&s_env_fixture);
    env_get_copy(&s_env_fixture, "PATH", out, sizeof(out));
    RegDeleteValue(hk, "PATH");
    RegCloseKey(hk);
    TEST_ASSERT(env_starts_with(out, "C:\\Impossible\\Bin"),
                "PATH retains the base after user append");
    TEST_ASSERT(env_ends_with(out, ";C:\\Users\\Default\\bin"),
                "user HKCU PATH is appended with ';'");
    env_free(&s_env_fixture);
}

static void test_env_defaults_user_override(void)
{
    char out[64];
    HKEY hk;
    uint32_t disp = 0;
    if (!kernel_subsystem_ready(SUBSYS_REGISTRY)) {
        TEST_SKIP("registry not ready -- HKCU override exercised post-Phase-2");
        return;
    }
    /* A user TEMP in HKCU\Environment must OVERRIDE the synth default C:\Temp. */
    if (RegCreateKeyEx(HKEY_CURRENT_USER, "Environment", 0, (const char *)0, 0,
                       KEY_ALL_ACCESS, (void *)0, &hk, &disp) != ERROR_SUCCESS) {
        TEST_SKIP("cannot open HKCU\\Environment");
        return;
    }
    RegSetString(hk, "TEMP", "D:\\UserTemp");
    env_fixture_reset();
    env_init_defaults(&s_env_fixture);
    env_get_copy(&s_env_fixture, "TEMP", out, sizeof(out));
    RegDeleteValue(hk, "TEMP");
    RegCloseKey(hk);
    TEST_ASSERT(env_streq(out, "D:\\UserTemp"),
                "HKCU\\Environment TEMP overrides the synth default");
    env_free(&s_env_fixture);
}

static void test_env_defaults_skips_non_string(void)
{
    char out[64];
    HKEY hk;
    uint32_t disp = 0;
    if (!kernel_subsystem_ready(SUBSYS_REGISTRY)) {
        TEST_SKIP("registry not ready -- non-string skip exercised post-Phase-2");
        return;
    }
    /* A REG_DWORD value under the system Environment key must be IGNORED by the
     * overlay (only REG_SZ/REG_EXPAND_SZ are applied). */
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                       "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                       0, (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hk, &disp) != ERROR_SUCCESS) {
        TEST_SKIP("cannot open Session Manager\\Environment");
        return;
    }
    RegSetDword(hk, "EnvTestDword", 42);
    env_fixture_reset();
    env_init_defaults(&s_env_fixture);
    {
        int r = env_get_copy(&s_env_fixture, "EnvTestDword", out, sizeof(out));
        RegDeleteValue(hk, "EnvTestDword");
        RegCloseKey(hk);
        TEST_ASSERT_EQ(r, ENV_ERR_NOTFOUND,
                       "REG_DWORD value is not applied to the environment");
    }
    env_free(&s_env_fixture);
}

static void test_env_defaults_unterminated_value(void)
{
    char out[64];
    HKEY hk;
    uint32_t disp = 0;
    if (!kernel_subsystem_ready(SUBSYS_REGISTRY)) {
        TEST_SKIP("registry not ready -- unterminated-value read exercised post-Phase-2");
        return;
    }
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                       "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                       0, (const char *)0, 0, KEY_ALL_ACCESS, (void *)0,
                       &hk, &disp) != ERROR_SUCCESS) {
        TEST_SKIP("cannot open Session Manager\\Environment");
        return;
    }
    /* Store "AB" as REG_SZ WITHOUT a trailing NUL (cbData = 2). The key already
     * holds longer values (ComSpec), so the read buffer's tail past index 2 is
     * uninitialized -- a capacity-terminator would leak that tail. The read must
     * stop at the returned length (2) and yield exactly "AB". */
    RegSetValueEx(hk, "EnvUnterm", 0, REG_SZ, (const uint8_t *)"AB", 2);
    env_fixture_reset();
    env_init_defaults(&s_env_fixture);
    {
        int r = env_get_copy(&s_env_fixture, "EnvUnterm", out, sizeof(out));
        RegDeleteValue(hk, "EnvUnterm");
        RegCloseKey(hk);
        TEST_ASSERT_EQ(r, 2, "unterminated REG_SZ read stops at the returned length");
        TEST_ASSERT(env_streq(out, "AB"),
                    "no uninitialized buffer tail leaks into the value");
    }
    env_free(&s_env_fixture);
}

/* ============================ %VAR% expansion (section 3) ================== */

/* Fill `buf` with the UTF-16 (Latin-1-direct) transcription of ASCII `s` and
 * return the WCHAR count (excluding NUL). */
static uint32_t env_test_wfill(const char *s, uint16_t *buf)
{
    uint32_t n = 0;
    while (s[n]) {
        buf[n] = (uint16_t)(uint8_t)s[n];
        n++;
    }
    return n;
}

/* True if the WCHAR run `w`[0..wlen) equals ASCII `ascii` exactly. */
static int env_test_weq_ascii(const uint16_t *w, uint32_t wlen, const char *ascii)
{
    uint32_t i;
    for (i = 0; i < wlen; i++) {
        if (ascii[i] == '\0' || w[i] != (uint16_t)(uint8_t)ascii[i])
            return 0;
    }
    return ascii[wlen] == '\0';
}

/* env_expand: %VAR% substituted; verbatim prefix/suffix preserved. */
static void test_env_expand_basic(void)
{
    char out[64];
    int r;
    env_fixture_reset();
    env_set(&s_env_fixture, "PLACE", "world");
    r = env_expand(&s_env_fixture, "hello %PLACE%!", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "hello world!"), "env_expand substitutes %VAR%");
    TEST_ASSERT_EQ(r, (int)env_test_strlen("hello world!"),
                   "env_expand returns bytes written excluding NUL");
    env_free(&s_env_fixture);
}

/* env_expand: %% is NOT a cmd-style escape -- Win32/ntdll ExpandEnvironmentStrings
 * treats it as an empty (unresolved) variable and preserves both percent signs. */
static void test_env_expand_double_percent(void)
{
    char out[32];
    env_fixture_reset();
    env_expand(&s_env_fixture, "100%% done", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "100%% done"),
                "%% preserved verbatim (empty var, not a cmd escape)");
    env_free(&s_env_fixture);
}

/* env_expand: an unknown %NAME% is copied through verbatim. */
static void test_env_expand_unknown_literal(void)
{
    char out[32];
    env_fixture_reset();
    env_expand(&s_env_fixture, "a %NOPE% b", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "a %NOPE% b"), "unknown %NAME% stays literal");
    env_free(&s_env_fixture);
}

/* env_expand: an unmatched trailing % is copied verbatim (no closing %). */
static void test_env_expand_unmatched_percent(void)
{
    char out[32];
    env_fixture_reset();
    env_expand(&s_env_fixture, "tail %OPEN here", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "tail %OPEN here"),
                "unmatched % copied verbatim to end");
    env_free(&s_env_fixture);
}

/* env_expand: single-pass -- an expanded value containing %OTHER% is NOT
 * re-expanded (Unit Tests section 3 checkpoint). */
static void test_env_expand_single_pass(void)
{
    char out[32];
    env_fixture_reset();
    env_set(&s_env_fixture, "A", "%B%");
    env_set(&s_env_fixture, "B", "x");
    env_expand(&s_env_fixture, "%A%", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "%B%"),
                "single-pass: %A% -> literal %B%, never recursively -> x");
    env_free(&s_env_fixture);
}

/* env_expand: overflow truncates + NUL-terminates and returns the max_len
 * sentinel. */
static void test_env_expand_truncation(void)
{
    char out[5];
    int r;
    env_fixture_reset();
    env_set(&s_env_fixture, "V", "123456789");
    r = env_expand(&s_env_fixture, "%V%", out, sizeof(out));
    TEST_ASSERT_EQ(r, (int)sizeof(out), "truncation returns max_len sentinel");
    TEST_ASSERT(env_streq(out, "1234"), "truncated output is NUL-terminated at max_len-1");
    env_free(&s_env_fixture);
}

/* env_build_block_utf16: produces a NAME=VALUE\0 ... \0 block from environ. */
static void test_env_build_block_utf16(void)
{
    uint16_t *blk = NULL;
    uint32_t w = 0;
    int rc;
    env_fixture_reset();
    env_set(&s_env_fixture, "K", "V");
    rc = env_build_block_utf16(&s_env_fixture, &blk, &w, RTL_ENV_BLOCK_MAX_WCHARS);
    TEST_ASSERT_EQ(rc, ENV_OK, "env_build_block_utf16 succeeds");
    /* "K=V\0\0" -> 5 wchars total (entry 'K','=','V',NUL + terminating NUL). */
    TEST_ASSERT_EQ((int)w, 5, "block wchar count includes entry NUL + terminator");
    TEST_ASSERT(blk && env_test_weq_ascii(blk, 3, "K=V"),
                "block holds the KEY=VALUE entry");
    TEST_ASSERT(blk && blk[3] == 0 && blk[4] == 0, "double-NUL terminates the block");
    env_free_block_utf16(blk, w);
    env_free(&s_env_fixture);
}

/* RtlExpandEnvironmentStrings_U: explicit block, %VAR% expansion, lengths. */
static void test_rtl_expand_basic(void)
{
    /* Block "PATH=C:\X\0\0". */
    uint16_t block[] = { 'P','A','T','H','=','C',':','\\','X', 0, 0 };
    uint16_t srcbuf[16], dstbuf[64];
    UNICODE_STRING src, dst;
    uint32_t rl = 0;
    NTSTATUS st;

    src.Length = (uint16_t)(env_test_wfill("%PATH%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(dstbuf);
    dst.Buffer = dstbuf;

    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, &rl);
    TEST_ASSERT_EQ((int)st, (int)STATUS_SUCCESS, "explicit-block expansion succeeds");
    TEST_ASSERT_EQ((int)dst.Length, (int)(env_test_strlen("C:\\X") * 2u),
                   "Destination->Length is result bytes excluding NUL");
    TEST_ASSERT_EQ((int)rl, (int)((env_test_strlen("C:\\X") + 1u) * 2u),
                   "ReturnedLength is required bytes including NUL");
    TEST_ASSERT(env_test_weq_ascii(dstbuf, env_test_strlen("C:\\X"), "C:\\X"),
                "expanded value matches %PATH%");
    TEST_ASSERT_EQ((int)dstbuf[env_test_strlen("C:\\X")], 0,
                   "Destination is NUL-terminated");
}

/* RtlExpandEnvironmentStrings_U: undersized Destination -> STATUS_BUFFER_TOO_SMALL,
 * ReturnedLength set to the requirement, no partial output, Length unchanged. */
static void test_rtl_expand_buffer_too_small(void)
{
    uint16_t block[] = { 'V','=','a','b','c','d','e', 0, 0 };   /* V=abcde */
    uint16_t srcbuf[16], dstbuf[4];
    UNICODE_STRING src, dst;
    uint32_t rl = 0;
    NTSTATUS st;

    src.Length = (uint16_t)(env_test_wfill("%V%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(dstbuf);   /* 4 bytes = 2 wchars: too small */
    dst.Buffer = dstbuf;

    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, &rl);
    TEST_ASSERT_EQ((int)st, (int)STATUS_BUFFER_TOO_SMALL, "small buffer rejected");
    TEST_ASSERT_EQ((int)rl, (int)((env_test_strlen("abcde") + 1u) * 2u),
                   "ReturnedLength reports required bytes including NUL");
    TEST_ASSERT_EQ((int)dst.Length, 0, "Length unchanged on BUFFER_TOO_SMALL");
}

/* RtlExpandEnvironmentStrings_U: %% preserved verbatim + case-insensitive ASCII name match. */
static void test_rtl_expand_escape_and_case(void)
{
    uint16_t block[] = { 'P','a','t','h','=','Q', 0, 0 };   /* Path=Q */
    uint16_t srcbuf[16], dstbuf[32];
    UNICODE_STRING src, dst;
    NTSTATUS st;

    /* "50%% %PATH%" -> "50%% Q": %% is preserved (empty var, Win32 semantics),
     * and Path/PATH matches case-insensitively. */
    src.Length = (uint16_t)(env_test_wfill("50%% %PATH%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(dstbuf);
    dst.Buffer = dstbuf;

    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_SUCCESS, "%% + case expansion succeeds");
    TEST_ASSERT(env_test_weq_ascii(dstbuf, env_test_strlen("50%% Q"), "50%% Q"),
                "%% preserved verbatim + case-insensitive name match");
}

/* RtlExpandEnvironmentStrings_U: an unterminated block (no double-NUL within the
 * cap is impractical to build, but a block whose only content lacks a terminator
 * within a tiny synthetic cap is rejected) and NULL args are rejected. */
static void test_rtl_expand_invalid_args(void)
{
    uint16_t block[] = { 'A','=','b', 0, 0 };
    uint16_t dstbuf[8];
    UNICODE_STRING dst;
    NTSTATUS st;
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(dstbuf);
    dst.Buffer = dstbuf;
    st = RtlExpandEnvironmentStrings_U(block, (UNICODE_STRING *)0, &dst,
                                       (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_INVALID_PARAMETER,
                   "NULL Source rejected with STATUS_INVALID_PARAMETER");
}

/* RtlExpandEnvironmentStrings_U: empty Source expands to an empty string. */
static void test_rtl_expand_empty_source(void)
{
    uint16_t block[] = { 'A','=','b', 0, 0 };
    uint16_t dstbuf[8];
    UNICODE_STRING src, dst;
    uint32_t rl = 0;
    NTSTATUS st;
    src.Length = 0;
    src.MaximumLength = 0;
    src.Buffer = (uint16_t *)0;              /* empty string: NULL Buffer is valid */
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(dstbuf);
    dst.Buffer = dstbuf;
    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, &rl);
    TEST_ASSERT_EQ((int)st, (int)STATUS_SUCCESS, "empty Source expands successfully");
    TEST_ASSERT_EQ((int)dst.Length, 0, "empty Source yields empty result");
    TEST_ASSERT_EQ((int)rl, 2, "ReturnedLength is the lone NUL (2 bytes)");
    TEST_ASSERT_EQ((int)dstbuf[0], 0, "result NUL-terminated");
}

/* RtlExpandEnvironmentStrings_U: a later entry in a multi-entry block resolves. */
static void test_rtl_expand_multi_entry(void)
{
    /* "AAA=1\0BBB=22\0\0" */
    uint16_t block[] = { 'A','A','A','=','1', 0, 'B','B','B','=','2','2', 0, 0 };
    uint16_t srcbuf[16], dstbuf[32];
    UNICODE_STRING src, dst;
    NTSTATUS st;
    src.Length = (uint16_t)(env_test_wfill("%BBB%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(dstbuf);
    dst.Buffer = dstbuf;
    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_SUCCESS, "multi-entry block expands");
    TEST_ASSERT(env_test_weq_ascii(dstbuf, 2, "22"),
                "second block entry resolves correctly");
}

/* env_build_block_utf16: an environ larger than max_wchars returns NOSPACE. */
static void test_env_build_block_over_cap(void)
{
    uint16_t *blk = (uint16_t *)0;
    uint32_t w = 0;
    int rc;
    env_fixture_reset();
    env_set(&s_env_fixture, "K", "value");        /* "K=value\0" + term = 9 wchars */
    rc = env_build_block_utf16(&s_env_fixture, &blk, &w, 4u);   /* cap below need */
    TEST_ASSERT_EQ(rc, ENV_ERR_NOSPACE, "over-cap block build returns NOSPACE");
    TEST_ASSERT(blk == (uint16_t *)0, "no block allocated on over-cap");
    env_free(&s_env_fixture);
}

/* RtlExpandEnvironmentStrings_U: a fitting result with a NULL Destination buffer
 * is a caller error -> STATUS_INVALID_PARAMETER, never a NULL write. */
static void test_rtl_expand_null_dest_buffer(void)
{
    uint16_t block[] = { 'A','=','b', 0, 0 };
    uint16_t srcbuf[8];
    UNICODE_STRING src, dst;
    NTSTATUS st;
    src.Length = 0;                              /* empty source: result fits trivially */
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = 16;                       /* room for the result... */
    dst.Buffer = (uint16_t *)0;                  /* ...but NULL buffer */
    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_INVALID_PARAMETER,
                   "fitting result + NULL Destination buffer rejected, not written");
}

/* RtlExpandEnvironmentStrings_U: a Destination that aliases Source is rejected
 * (the two-pass count/write would tear on an in-place overwrite). */
static void test_rtl_expand_overlap_rejected(void)
{
    uint16_t block[] = { 'A','=','b', 0, 0 };
    uint16_t shared[32];
    UNICODE_STRING src, dst;
    NTSTATUS st;
    (void)env_test_wfill("%A%", shared);
    src.Length = (uint16_t)(env_test_strlen("%A%") * 2u);
    src.MaximumLength = (uint16_t)sizeof(shared);
    src.Buffer = shared;
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(shared);
    dst.Buffer = shared;                         /* aliases Source */
    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_INVALID_PARAMETER,
                   "Destination aliasing Source rejected");
}

/* env_build_block_utf16: a multibyte UTF-8 value converts to a single BMP WCHAR
 * (U+00E9), not two Latin-1 code units. */
static void test_env_build_block_utf8(void)
{
    uint16_t *blk = (uint16_t *)0;
    uint32_t w = 0;
    int rc;
    env_fixture_reset();
    env_set(&s_env_fixture, "K", "\xC3\xA9");     /* value "e-acute" (U+00E9) in UTF-8 */
    rc = env_build_block_utf16(&s_env_fixture, &blk, &w, RTL_ENV_BLOCK_MAX_WCHARS);
    TEST_ASSERT_EQ(rc, ENV_OK, "UTF-8 block build succeeds");
    /* "K=<U+00E9>\0\0" -> 5 wchars: 'K','=',0x00E9, entry NUL, block NUL. */
    TEST_ASSERT_EQ((int)w, 5, "multibyte value is one WCHAR, not two");
    TEST_ASSERT(blk && blk[0] == (uint16_t)'K' && blk[1] == (uint16_t)'=',
                "key converts unchanged");
    TEST_ASSERT(blk && blk[2] == 0x00E9u, "UTF-8 C3 A9 -> single WCHAR U+00E9");
    TEST_ASSERT(blk && blk[3] == 0 && blk[4] == 0, "double-NUL terminated");
    env_free_block_utf16(blk, w);
    env_free(&s_env_fixture);
}

/* env_build_block_utf16: an empty environment yields a double-NUL block. */
static void test_env_build_block_empty(void)
{
    uint16_t *blk = (uint16_t *)0;
    uint32_t w = 0;
    int rc;
    env_fixture_reset();                          /* no variables set */
    rc = env_build_block_utf16(&s_env_fixture, &blk, &w, RTL_ENV_BLOCK_MAX_WCHARS);
    TEST_ASSERT_EQ(rc, ENV_OK, "empty-environment block build succeeds");
    TEST_ASSERT_EQ((int)w, 2, "empty block is two wchars (double-NUL)");
    TEST_ASSERT(blk && blk[0] == 0 && blk[1] == 0,
                "empty block is a proper double-NUL terminator");
    env_free_block_utf16(blk, w);
    env_free(&s_env_fixture);
}

/* env_expand: input aliasing output is rejected (empty result, no corruption). */
static void test_env_expand_overlap_rejected(void)
{
    char buf[32];
    int r;
    env_fixture_reset();
    env_set(&s_env_fixture, "V", "x");
    /* buf holds the template AND is the output -> exact alias. */
    buf[0] = '%'; buf[1] = 'V'; buf[2] = '%'; buf[3] = '\0';
    r = env_expand(&s_env_fixture, buf, buf, sizeof(buf));
    TEST_ASSERT_EQ(r, 0, "aliased input/output rejected with empty result");
    env_free(&s_env_fixture);
}

/* RtlExpandEnvironmentStrings_U: a Destination at a non-empty block's terminating
 * NUL overlaps the block extent and is rejected. */
static void test_rtl_expand_overlap_block_terminator(void)
{
    uint16_t block[] = { 'A','=','b', 0, 0 };     /* terminator index is 4 */
    uint16_t srcbuf[8];
    UNICODE_STRING src, dst;
    NTSTATUS st;
    src.Length = (uint16_t)(env_test_wfill("%A%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = 2;                          /* 1 wchar */
    dst.Buffer = &block[4];                         /* the terminating NUL */
    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_INVALID_PARAMETER,
                   "Destination at the block terminator rejected");
}

/* RtlExpandEnvironmentStrings_U: a Destination aliasing an empty block ("\0\0")
 * is rejected (the extent covers the terminator even when block_wchars == 0). */
static void test_rtl_expand_overlap_empty_block(void)
{
    uint16_t block[8];
    uint16_t srcbuf[8];
    UNICODE_STRING src, dst;
    NTSTATUS st;
    block[0] = 0; block[1] = 0;                     /* empty double-NUL block */
    src.Length = (uint16_t)(env_test_wfill("%X%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = 8;
    dst.Buffer = &block[0];                         /* aliases the empty block */
    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_INVALID_PARAMETER,
                   "Destination aliasing an empty block rejected");
}

/* RtlExpandEnvironmentStrings_U: a name longer than ENV_NAME_MAX is left literal
 * (same limit as env_expand), even when an over-limit entry exists in the block. */
static void test_rtl_expand_name_over_limit(void)
{
    /* Block "<257 A's>=x\0\0" and Source "%<257 A's>%". */
    static uint16_t block[ENV_NAME_MAX + 8];
    static uint16_t srcbuf[ENV_NAME_MAX + 8];
    static uint16_t dstbuf[ENV_NAME_MAX + 16];
    UNICODE_STRING src, dst;
    NTSTATUS st;
    uint32_t n = ENV_NAME_MAX + 1u;              /* 257: one over the limit */
    uint32_t w = 0, k;
    for (k = 0; k < n; k++) block[w++] = (uint16_t)'A';
    block[w++] = (uint16_t)'=';
    block[w++] = (uint16_t)'x';
    block[w++] = 0; block[w++] = 0;              /* entry NUL + block terminator */
    w = 0;
    srcbuf[w++] = (uint16_t)'%';
    for (k = 0; k < n; k++) srcbuf[w++] = (uint16_t)'A';
    srcbuf[w++] = (uint16_t)'%';
    src.Length = (uint16_t)(w * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(dstbuf);
    dst.Buffer = dstbuf;
    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_SUCCESS, "over-limit-name expansion succeeds");
    /* Result is the literal "%<257 A's>%" (n+2 wchars), NOT "x". */
    TEST_ASSERT_EQ((int)dst.Length, (int)((n + 2u) * 2u),
                   "over-ENV_NAME_MAX name left literal, not expanded");
    TEST_ASSERT_EQ((int)dstbuf[0], (int)(uint16_t)'%', "leading % preserved");
}

/* RtlExpandEnvironmentStrings_U: a Destination->Buffer pointing into its own
 * descriptor is rejected (would corrupt the write pointer mid-flight). */
static void test_rtl_expand_self_referential_dest(void)
{
    uint16_t block[] = { 'V','=','1', 0, 0 };
    uint16_t srcbuf[8];
    UNICODE_STRING src, dst;
    NTSTATUS st;
    src.Length = (uint16_t)(env_test_wfill("%V%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = 64;
    dst.Buffer = (uint16_t *)&dst;                 /* aliases the descriptor */
    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_INVALID_PARAMETER,
                   "self-referential Destination buffer rejected");
}

/* RtlExpandEnvironmentStrings_U: a ReturnedLength aliasing the output range is
 * rejected (the *ReturnedLength store would corrupt the output). */
static void test_rtl_expand_returnedlength_alias(void)
{
    uint16_t block[] = { 'V','=','1', 0, 0 };
    uint16_t srcbuf[8], dstbuf[16];
    UNICODE_STRING src, dst;
    NTSTATUS st;
    src.Length = (uint16_t)(env_test_wfill("%V%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(dstbuf);
    dst.Buffer = dstbuf;
    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, (uint32_t *)&dstbuf[0]);
    TEST_ASSERT_EQ((int)st, (int)STATUS_INVALID_PARAMETER,
                   "ReturnedLength aliasing the output range rejected");
}

/* RtlExpandEnvironmentStrings_U: a ReturnedLength aliasing Source data is
 * rejected (the between-passes store would mutate the input). */
static void test_rtl_expand_returnedlength_aliases_source(void)
{
    uint16_t block[] = { 'V','=','1', 0, 0 };
    uint16_t srcbuf[8], dstbuf[16];
    UNICODE_STRING src, dst;
    NTSTATUS st;
    src.Length = (uint16_t)(env_test_wfill("%V%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(dstbuf);
    dst.Buffer = dstbuf;
    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, (uint32_t *)&srcbuf[0]);
    TEST_ASSERT_EQ((int)st, (int)STATUS_INVALID_PARAMETER,
                   "ReturnedLength aliasing Source data rejected");
}

/* RtlExpandEnvironmentStrings_U: Destination at block[1] of an empty "\0\0"
 * block is rejected (the extent covers both terminators). */
static void test_rtl_expand_empty_block_second_nul(void)
{
    uint16_t block[8];
    uint16_t srcbuf[8];
    UNICODE_STRING src, dst;
    NTSTATUS st;
    block[0] = 0; block[1] = 0;
    src.Length = (uint16_t)(env_test_wfill("%X%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = 8;
    dst.Buffer = &block[1];                        /* the second terminator */
    st = RtlExpandEnvironmentStrings_U(block, &src, &dst, (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_INVALID_PARAMETER,
                   "Destination at an empty block's second NUL rejected");
}

/* env_expand: a rejected overlap leaves the SOURCE bytes untouched (exact and
 * both partial-overlap directions). */
static void test_env_expand_overlap_preserves_source(void)
{
    char buf[32];
    int r;
    env_fixture_reset();
    env_set(&s_env_fixture, "V", "x");

    /* Exact alias: input == output. */
    buf[0] = '%'; buf[1] = 'V'; buf[2] = '%'; buf[3] = '\0';
    r = env_expand(&s_env_fixture, buf, buf, sizeof(buf));
    TEST_ASSERT_EQ(r, 0, "exact alias rejected");
    TEST_ASSERT(env_streq(buf, "%V%"), "exact alias leaves source intact");

    /* Forward partial: output starts inside input. */
    buf[0] = '%'; buf[1] = 'V'; buf[2] = '%'; buf[3] = '\0';
    r = env_expand(&s_env_fixture, buf, buf + 1, sizeof(buf) - 1);
    TEST_ASSERT_EQ(r, 0, "forward partial overlap rejected");
    TEST_ASSERT(env_streq(buf, "%V%"), "forward partial leaves source intact");

    /* Backward partial: input starts inside output. */
    buf[0] = 'A'; buf[1] = '%'; buf[2] = 'V'; buf[3] = '%'; buf[4] = '\0';
    r = env_expand(&s_env_fixture, buf + 1, buf, sizeof(buf));
    TEST_ASSERT_EQ(r, 0, "backward partial overlap rejected");
    TEST_ASSERT(env_streq(buf + 1, "%V%"), "backward partial leaves source intact");

    env_free(&s_env_fixture);
}

/* --- argv array + exec argument handoff --- */

/* task_set_argv deep-copies each string; the array is independent of the
 * caller's and NUL-terminated; env_free reclaims it. */
static void test_argv_set_and_free(void)
{
    const char *src[] = { "echo", "hello", "world" };
    int rc;

    env_fixture_reset();
    rc = task_set_argv(&s_env_fixture, 3, src);
    TEST_ASSERT_EQ(rc, ENV_OK, "task_set_argv returns ENV_OK");
    TEST_ASSERT_EQ(s_env_fixture.argc, 3, "argc == 3");
    TEST_ASSERT(s_env_fixture.argv != NULL, "argv array allocated");
    TEST_ASSERT(env_streq(s_env_fixture.argv[0], "echo"), "argv[0] == echo");
    TEST_ASSERT(env_streq(s_env_fixture.argv[1], "hello"), "argv[1] == hello");
    TEST_ASSERT(env_streq(s_env_fixture.argv[2], "world"), "argv[2] == world");
    TEST_ASSERT(s_env_fixture.argv[3] == NULL, "argv[3] == NULL terminator");
    /* Deep copy: mutating the source does not change the stored copy. */
    TEST_ASSERT(s_env_fixture.argv[0] != src[0], "argv[0] is a distinct copy");

    env_free(&s_env_fixture);
    TEST_ASSERT(s_env_fixture.argv == NULL, "env_free NULLs argv");
    TEST_ASSERT_EQ(s_env_fixture.argc, 0, "env_free zeroes argc");
}

/* A second task_set_argv fully replaces the first (no leak, correct contents);
 * an empty argv clears back to the no-argv state. */
static void test_argv_set_replace_and_clear(void)
{
    const char *a[] = { "first", "arg" };
    const char *b[] = { "second" };

    env_fixture_reset();
    TEST_ASSERT_EQ(task_set_argv(&s_env_fixture, 2, a), ENV_OK, "set argv a");
    TEST_ASSERT_EQ(task_set_argv(&s_env_fixture, 1, b), ENV_OK, "set argv b");
    TEST_ASSERT_EQ(s_env_fixture.argc, 1, "replaced argc == 1");
    TEST_ASSERT(env_streq(s_env_fixture.argv[0], "second"), "argv[0] == second");

    TEST_ASSERT_EQ(task_set_argv(&s_env_fixture, 0, NULL), ENV_OK, "clear argv");
    TEST_ASSERT(s_env_fixture.argv == NULL, "cleared argv is NULL");
    TEST_ASSERT_EQ(s_env_fixture.argc, 0, "cleared argc == 0");
    env_free(&s_env_fixture);
}

/* argv_to_cmdline encodes Windows quoting: plain args join with spaces; args
 * with spaces or empty args are quoted; interior quotes are backslash-escaped. */
static void test_argv_to_cmdline_quoting(void)
{
    char out[128];
    const char *plain[] = { "echo", "hello", "world" };
    const char *spaced[] = { "a b" };
    const char *empty[] = { "" };
    const char *quoted[] = { "a\"b" };
    const char *rt[] = { "a b", "c\"d", "" };
    const char *bs[] = { "c:\\path" };

    argv_to_cmdline(3, plain, out, sizeof(out));
    TEST_ASSERT(env_streq(out, "echo hello world"), "plain args join with spaces");

    argv_to_cmdline(1, spaced, out, sizeof(out));
    TEST_ASSERT(env_streq(out, "\"a b\""), "arg with space is quoted");

    argv_to_cmdline(1, empty, out, sizeof(out));
    TEST_ASSERT(env_streq(out, "\"\""), "empty arg encodes as \"\"");

    argv_to_cmdline(1, quoted, out, sizeof(out));
    TEST_ASSERT(env_streq(out, "\"a\\\"b\""), "interior quote is backslash-escaped");

    argv_to_cmdline(3, rt, out, sizeof(out));
    TEST_ASSERT(env_streq(out, "\"a b\" \"c\\\"d\" \"\""), "mixed round-trip encode");

    argv_to_cmdline(1, bs, out, sizeof(out));
    TEST_ASSERT(env_streq(out, "c:\\path"), "backslash w/o quote stays verbatim");
}

/* argv_frame_bytes returns the exact stack footprint, and the SYS_EXEC frame-fit
 * predicate rejects an argv whose frame would overflow the 16 KiB user stack. */
static void test_argv_frame_bytes_and_cap(void)
{
    const char *two[] = { "ab", "cde" };
    static char big[20000];
    const char *huge[1];
    uint32_t i;

    /* Exact builder match: "ab"->1 qw, "cde"->1 qw (str_qwords=2, even, no pad);
     * argc=2 is even -> +1 parity qword; ptr array (2+1)=3 qw; argc slot 1 qw.
     * total = (2 + 1 + 3 + 1) * 8 = 56 bytes. */
    TEST_ASSERT_EQ(argv_frame_bytes(2, two), 56u, "argv_frame_bytes exact size");
    TEST_ASSERT_EQ(argv_frame_bytes(0, NULL), 0u, "empty argv frame is 0 bytes");

    /* A single ~16 KB argument overflows the 16 KiB user stack budget. */
    for (i = 0; i < sizeof(big) - 1; i++) big[i] = 'x';
    big[sizeof(big) - 1] = '\0';
    huge[0] = big;
    TEST_ASSERT((uint64_t)argv_frame_bytes(1, huge) + ARGV_FRAME_RESERVE >
                    USER_STACK_SIZE,
                "oversized argv frame exceeds user stack (rejected)");
    TEST_ASSERT((uint64_t)argv_frame_bytes(2, two) + ARGV_FRAME_RESERVE <=
                    USER_STACK_SIZE,
                "small argv frame fits user stack (accepted)");

    /* Boundary: four 3999-byte args -- each qword-rounds to 500 qw (str=2000),
     * argc=4 even -> +1 pad, ptr array 5 qw, argc 1 qw = 2007 qw = 16056 B. With
     * the exact 280-byte reserve the actual 16336-byte frame fits the 16384-byte
     * stack and MUST be accepted (a conservative reserve wrongly rejected it). */
    {
        static char b3999[4000];
        const char *four[4];
        uint32_t k;
        for (k = 0; k < 3999u; k++) b3999[k] = 'y';
        b3999[3999] = '\0';
        four[0] = four[1] = four[2] = four[3] = b3999;
        TEST_ASSERT_EQ(argv_frame_bytes(4, four), 16056u,
                       "4x3999 argv frame is exactly 16056 bytes");
        TEST_ASSERT((uint64_t)argv_frame_bytes(4, four) + ARGV_FRAME_RESERVE <=
                        USER_STACK_SIZE,
                    "boundary argv frame (16336 B) fits the 16 KiB stack");
    }
}

/* env_adopt_block replaces environ from a "KEY=VALUE" block, skipping malformed
 * entries (no '=', empty key). */
static void test_env_adopt_block(void)
{
    const char *block[] = {
        "FOO=bar", "BAZ=qux", "malformed", "=noname", "EMPTY="
    };
    char buf[64];
    int rc;

    env_fixture_reset();
    rc = env_adopt_block(&s_env_fixture, block, 5);
    TEST_ASSERT_EQ(rc, ENV_OK, "env_adopt_block returns ENV_OK");
    /* FOO, BAZ, EMPTY accepted; "malformed" (no '=') and "=noname" (empty key)
     * skipped. */
    TEST_ASSERT_EQ(s_env_fixture.environ_count, 3u, "3 well-formed entries kept");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "FOO", buf, sizeof(buf)) >= 0 &&
                env_streq(buf, "bar"), "FOO=bar adopted");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "BAZ", buf, sizeof(buf)) >= 0 &&
                env_streq(buf, "qux"), "BAZ=qux adopted");
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "malformed", buf, sizeof(buf)),
                   ENV_ERR_NOTFOUND, "malformed entry skipped");

    /* Empty block clears environ. */
    rc = env_adopt_block(&s_env_fixture, NULL, 0);
    TEST_ASSERT_EQ(rc, ENV_OK, "empty block clears");
    TEST_ASSERT_EQ(s_env_fixture.environ_count, 0u, "environ cleared");
    env_free(&s_env_fixture);
}

/* ==== s5: NtQueryEnvironmentVariable / NtSetEnvironmentVariable ============
 * Exercised through ssdt_dispatch (kernel-mode previous mode: no user probing),
 * which ALSO proves nt_env_register_ssdt() ran during boot. All cases operate
 * on the running thread's own task_current() environ with uniquely-named keys,
 * cleaned up via env_unset so no state leaks between suites. */

/* Build a UNICODE_STRING over `buf` from an ASCII literal (UTF-16 = zero-extend
 * each byte; test names/values are ASCII). */
static void env_mk_us(UNICODE_STRING *us, uint16_t *buf, uint32_t cap_wchars,
                      const char *ascii)
{
    uint32_t n = 0;
    while (n < cap_wchars && ascii[n]) {
        buf[n] = (uint16_t)(uint8_t)ascii[n];
        n++;
    }
    us->Length = (uint16_t)(n * 2u);
    us->MaximumLength = (uint16_t)(cap_wchars * 2u);
    us->_pad = 0u;
    us->Buffer = buf;
}

static NTSTATUS env_nt_set(UNICODE_STRING *name, UNICODE_STRING *val)
{
    return ssdt_dispatch(SSDT_NtSetEnvironmentVariable,
                         (uint64_t)(uintptr_t)name, (uint64_t)(uintptr_t)val,
                         0, 0, 0, 0);
}

static NTSTATUS env_nt_query(UNICODE_STRING *name, UNICODE_STRING *out,
                             uint32_t *vlen)
{
    return ssdt_dispatch(SSDT_NtQueryEnvironmentVariable,
                         (uint64_t)(uintptr_t)name, (uint64_t)(uintptr_t)out,
                         (uint64_t)(uintptr_t)vlen, 0, 0, 0);
}

/* Prewarm the LIVE current task's environ[] to its high-water-mark BEFORE the
 * NtSet/NtQuery syscall suites run, so none of them each trip the one-time array
 * grow inside their own leak-measured window. env_set krealloc-grows
 * task->environ to count+2 and env_unset never shrinks it (the array is freed
 * only at task teardown by env_free -> kfree), so a single add+remove leaves a
 * reachable one-pointer-slot retention (16 bytes at allocator granularity).
 * env_set/env_unset are used directly (no UTF-16 temporaries) so the exemption
 * covers ONLY the array grow -- keeping full heap-leak protection on the syscall
 * suites. Seeding one more system default (e.g. PATHEXT, section 11) shifts which
 * suite would otherwise first trip the grow, which is exactly what this absorbs.
 *
 * TEST_EXPECT_LEAK is a deliberate LOWER bound (test_runner.c: exact heap
 * accounting is fragile because kmalloc/krealloc block-header + MIN_BLOCK_SIZE +
 * split-remainder overhead perturbs the observed delta), so it stays green even
 * if the one-slot grow accounts for 32/48/64 bytes on a different heap layout. It
 * cannot mask a UNIQUE env_set/env_unset leak: those two paths are exercised
 * WITHOUT any exemption by the ~20 other env set/get/replace/unset suites above,
 * which would fail first. This suite only blesses the reachable array retention. */
static void test_ntenv_live_env_hwm(void)
{
    struct task *t = task_current();
    TEST_EXPECT_LEAK(16, "env: live-task environ[] high-water-mark grow (reachable)");
    TEST_ASSERT_EQ((uint32_t)env_set(t, "NTENV_HWM", "1"), (uint32_t)ENV_OK,
                   "seed throwaway var to grow environ[] to high-water-mark");
    TEST_ASSERT_EQ((uint32_t)env_unset(t, "NTENV_HWM"), (uint32_t)ENV_OK,
                   "remove throwaway var (array capacity retained, not shrunk)");
}

static void test_ntenv_set_query_roundtrip(void)
{
    struct task *t = task_current();
    uint16_t nbuf[16], vbuf[16], obuf[32];
    UNICODE_STRING name, val, out;
    uint32_t vlen = 0;

    /* The live-task environ[] high-water-mark grow is absorbed by the
     * test_ntenv_live_env_hwm prewarm registered just before this suite, so
     * this suite keeps full leak protection on the NtSet/NtQuery heap paths
     * (NtQuery allocates a bounce buffer). */
    env_mk_us(&name, nbuf, 16, "NTENV_A");
    env_mk_us(&val, vbuf, 16, "hello");
    TEST_ASSERT_EQ((uint32_t)env_nt_set(&name, &val), (uint32_t)STATUS_SUCCESS,
                   "NtSetEnvironmentVariable stores a value");

    out.Length = 0; out.MaximumLength = (uint16_t)sizeof(obuf);
    out._pad = 0; out.Buffer = obuf;
    obuf[5] = 0x1234;
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, &out, &vlen),
                   (uint32_t)STATUS_SUCCESS, "NtQueryEnvironmentVariable succeeds");
    TEST_ASSERT_EQ((uint32_t)out.Length, 10u,
                   "returned Length is 5 wchars = 10 bytes (excl NUL)");
    TEST_ASSERT_EQ(vlen, 10u, "ValueLength reports required bytes excl NUL");
    TEST_ASSERT_EQ((uint32_t)obuf[0], (uint32_t)'h', "buffer[0] == 'h'");
    TEST_ASSERT_EQ((uint32_t)obuf[4], (uint32_t)'o', "buffer[4] == 'o'");
    TEST_ASSERT_EQ((uint32_t)obuf[5], 0u, "buffer NUL-terminated (room available)");

    env_unset(t, "NTENV_A");
}

static void test_ntenv_query_not_found(void)
{
    uint16_t nbuf[32], obuf[8];
    UNICODE_STRING name, out;

    env_mk_us(&name, nbuf, 32, "NTENV_DEFINITELY_ABSENT");
    out.Length = 0; out.MaximumLength = (uint16_t)sizeof(obuf);
    out._pad = 0; out.Buffer = obuf;
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, &out, (uint32_t *)0),
                   (uint32_t)STATUS_VARIABLE_NOT_FOUND,
                   "absent variable -> STATUS_VARIABLE_NOT_FOUND");
}

static void test_ntenv_delete(void)
{
    uint16_t nbuf[16], vbuf[16], obuf[16];
    UNICODE_STRING name, val, out;

    env_mk_us(&name, nbuf, 16, "NTENV_C");
    env_mk_us(&val, vbuf, 16, "x");
    TEST_ASSERT_EQ((uint32_t)env_nt_set(&name, &val), (uint32_t)STATUS_SUCCESS,
                   "seed NTENV_C");
    /* Value == NULL -> delete. */
    TEST_ASSERT_EQ((uint32_t)env_nt_set(&name, (UNICODE_STRING *)0),
                   (uint32_t)STATUS_SUCCESS,
                   "NtSetEnvironmentVariable(NULL) deletes");
    out.Length = 0; out.MaximumLength = (uint16_t)sizeof(obuf);
    out._pad = 0; out.Buffer = obuf;
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, &out, (uint32_t *)0),
                   (uint32_t)STATUS_VARIABLE_NOT_FOUND, "deleted var is absent");
    /* Deleting an absent var reports VARIABLE_NOT_FOUND. */
    TEST_ASSERT_EQ((uint32_t)env_nt_set(&name, (UNICODE_STRING *)0),
                   (uint32_t)STATUS_VARIABLE_NOT_FOUND,
                   "delete of absent var -> VARIABLE_NOT_FOUND");
}

static void test_ntenv_buffer_too_small(void)
{
    struct task *t = task_current();
    uint16_t nbuf[16], vbuf[16], obuf[2];
    UNICODE_STRING name, val, out;
    uint32_t vlen = 0;

    env_mk_us(&name, nbuf, 16, "NTENV_B");
    env_mk_us(&val, vbuf, 16, "abcdef");        /* 6 wchars = 12 bytes */
    TEST_ASSERT_EQ((uint32_t)env_nt_set(&name, &val), (uint32_t)STATUS_SUCCESS,
                   "seed NTENV_B");
    out.Length = 0; out.MaximumLength = (uint16_t)sizeof(obuf);  /* 4 bytes */
    out._pad = 0; out.Buffer = obuf;
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, &out, &vlen),
                   (uint32_t)STATUS_BUFFER_TOO_SMALL, "too-small buffer rejected");
    TEST_ASSERT_EQ(vlen, 12u, "required size reported (12 bytes) on overflow");
    env_unset(t, "NTENV_B");
}

static void test_ntenv_exact_fit(void)
{
    struct task *t = task_current();
    uint16_t nbuf[16], vbuf[16], obuf[3];       /* 3 wchars = 6 bytes exactly */
    UNICODE_STRING name, val, out;
    uint32_t vlen = 0;

    env_mk_us(&name, nbuf, 16, "NTENV_F");
    env_mk_us(&val, vbuf, 16, "abc");
    TEST_ASSERT_EQ((uint32_t)env_nt_set(&name, &val), (uint32_t)STATUS_SUCCESS,
                   "seed NTENV_F");
    out.Length = 0; out.MaximumLength = 6u;     /* exactly the value, no NUL room */
    out._pad = 0; out.Buffer = obuf;
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, &out, &vlen),
                   (uint32_t)STATUS_SUCCESS, "exact-fit buffer succeeds");
    TEST_ASSERT_EQ((uint32_t)out.Length, 6u, "Length = 6 bytes (excl NUL)");
    TEST_ASSERT_EQ((uint32_t)obuf[2], (uint32_t)'c', "third wchar written");
    env_unset(t, "NTENV_F");
}

static void test_ntenv_embedded_nul_rejected(void)
{
    struct task *t = task_current();
    uint16_t nbuf[16], vbuf[16], obuf[16];
    UNICODE_STRING name, val, out;

    out.Length = 0; out.MaximumLength = (uint16_t)sizeof(obuf);
    out._pad = 0; out.Buffer = obuf;

    /* Name "PA\0TH": embedded NUL must be rejected, not truncated to "PA". */
    nbuf[0] = 'P'; nbuf[1] = 'A'; nbuf[2] = 0; nbuf[3] = 'T'; nbuf[4] = 'H';
    name.Length = 10; name.MaximumLength = 32; name._pad = 0; name.Buffer = nbuf;
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, &out, (uint32_t *)0),
                   (uint32_t)STATUS_INVALID_PARAMETER,
                   "embedded NUL in Name rejected");

    /* Empty name. */
    name.Length = 0; name.MaximumLength = 32; name._pad = 0; name.Buffer = nbuf;
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, &out, (uint32_t *)0),
                   (uint32_t)STATUS_INVALID_PARAMETER, "empty Name rejected");

    /* Value "a\0b" on set: embedded NUL rejected (would store only "a"). */
    env_mk_us(&name, nbuf, 16, "NTENV_D");
    vbuf[0] = 'a'; vbuf[1] = 0; vbuf[2] = 'b';
    val.Length = 6; val.MaximumLength = 32; val._pad = 0; val.Buffer = vbuf;
    TEST_ASSERT_EQ((uint32_t)env_nt_set(&name, &val),
                   (uint32_t)STATUS_INVALID_PARAMETER,
                   "embedded NUL in Value rejected");
    /* Ensure the rejected set did not create the variable. */
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, &out, (uint32_t *)0),
                   (uint32_t)STATUS_VARIABLE_NOT_FOUND,
                   "rejected set created nothing");
    env_unset(t, "NTENV_D");
}

static void test_ntenv_overlap_and_null(void)
{
    struct task *t = task_current();
    uint16_t nbuf[16], vbuf[16], obuf[8];
    uint8_t blob[sizeof(UNICODE_STRING)];
    UNICODE_STRING name, val, *aliased;

    env_mk_us(&name, nbuf, 16, "NTENV_E");
    env_mk_us(&val, vbuf, 16, "zz");
    TEST_ASSERT_EQ((uint32_t)env_nt_set(&name, &val), (uint32_t)STATUS_SUCCESS,
                   "seed NTENV_E");

    /* Value.Buffer aliasing the Value descriptor -> overlap rejected. */
    aliased = (UNICODE_STRING *)blob;
    aliased->Length = 0; aliased->MaximumLength = (uint16_t)sizeof(blob);
    aliased->_pad = 0; aliased->Buffer = (uint16_t *)blob;
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, aliased, (uint32_t *)0),
                   (uint32_t)STATUS_INVALID_PARAMETER,
                   "Value.Buffer overlapping its descriptor rejected");

    /* NULL Name on query and on set. */
    {
        UNICODE_STRING out;
        out.Length = 0; out.MaximumLength = (uint16_t)sizeof(obuf);
        out._pad = 0; out.Buffer = obuf;
        TEST_ASSERT_EQ((uint32_t)env_nt_query((UNICODE_STRING *)0, &out,
                                              (uint32_t *)0),
                       (uint32_t)STATUS_INVALID_PARAMETER, "NULL Name query rejected");
    }
    TEST_ASSERT_EQ((uint32_t)env_nt_set((UNICODE_STRING *)0, &val),
                   (uint32_t)STATUS_INVALID_PARAMETER, "NULL Name set rejected");
    env_unset(t, "NTENV_E");
}

/* s5 regression: a stored EMPTY value still owes a WCHAR
 * terminator when the caller buffer has room, so the need_bytes NULL guard is
 * bypassed for it. A KernelMode/Zw query (ssdt_dispatch runs as KernelMode, the
 * raw-memcpy path) with Buffer == NULL and MaximumLength >= 2 must be rejected,
 * not written through NULL. MaximumLength 0/1 (no terminator room) stays a clean
 * SUCCESS with Length 0. */
static void test_ntenv_empty_value_null_buffer(void)
{
    struct task *t = task_current();
    uint16_t nbuf[16], vbuf[4], obuf[4];
    UNICODE_STRING name, val, out;
    uint32_t vlen = 0xFFFFu;

    env_mk_us(&name, nbuf, 16, "NTENV_EMPTY");
    env_mk_us(&val, vbuf, 4, "");            /* empty value, non-NULL Buffer */
    TEST_ASSERT_EQ((uint32_t)env_nt_set(&name, &val), (uint32_t)STATUS_SUCCESS,
                   "empty value stores successfully");

    /* NULL Buffer + room for a terminator: reject, do not fault (the fix). */
    out.Length = 0xAAu; out.MaximumLength = 2u; out._pad = 0; out.Buffer = (uint16_t *)0;
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, &out, &vlen),
                   (uint32_t)STATUS_INVALID_PARAMETER,
                   "empty value + NULL Buffer + room-for-NUL rejected");
    TEST_ASSERT_EQ(vlen, 0u, "required length reported as 0 bytes");

    /* NULL Buffer, no terminator room (MaximumLength 0 then 1): clean SUCCESS. */
    out.Length = 0xAAu; out.MaximumLength = 0u; out._pad = 0; out.Buffer = (uint16_t *)0;
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, &out, (uint32_t *)0),
                   (uint32_t)STATUS_SUCCESS, "empty value + NULL Buffer + no room OK");
    TEST_ASSERT_EQ((uint32_t)out.Length, 0u, "Length reported 0 (empty value)");
    out.Length = 0xAAu; out.MaximumLength = 1u; out._pad = 0; out.Buffer = (uint16_t *)0;
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, &out, (uint32_t *)0),
                   (uint32_t)STATUS_SUCCESS, "empty value + NULL Buffer + 1 byte OK");

    /* Valid buffer with room: writes just the terminator, Length 0. */
    out.Length = 0xAAu; out.MaximumLength = (uint16_t)sizeof(obuf); out._pad = 0;
    out.Buffer = obuf; obuf[0] = 0x1234;
    TEST_ASSERT_EQ((uint32_t)env_nt_query(&name, &out, &vlen),
                   (uint32_t)STATUS_SUCCESS, "empty value + real buffer succeeds");
    TEST_ASSERT_EQ((uint32_t)out.Length, 0u, "Length 0 for empty value");
    TEST_ASSERT_EQ((uint32_t)obuf[0], 0u, "terminator written to buffer");

    env_unset(t, "NTENV_EMPTY");
}

/* ========================================================================
 * s10: sorted block, caller-buffer build/parse, size caps
 * ======================================================================== */

/* Sorted storage: inserting out of order yields an alphabetically sorted block. */
static void test_env_sorted_block_ansi(void)
{
    char buf[128];
    uint32_t outlen = 0;
    int rc;
    env_fixture_reset();
    env_set(&s_env_fixture, "ZZZ", "1");
    env_set(&s_env_fixture, "AAA", "2");
    env_set(&s_env_fixture, "MMM", "3");
    rc = env_build_block(&s_env_fixture, buf, sizeof(buf), 0, &outlen);
    TEST_ASSERT_EQ(rc, ENV_OK, "env_build_block ANSI succeeds");
    /* "AAA=2\0MMM=3\0ZZZ=1\0\0" = 6 + 6 + 6 + 1 = 19 bytes */
    TEST_ASSERT_EQ((int)outlen, 19, "block length = sum of entries + terminator");
    TEST_ASSERT(env_streq(buf, "AAA=2"), "first entry is AAA (sorted, not insertion order)");
    TEST_ASSERT(env_streq(buf + 6, "MMM=3"), "second entry is MMM");
    TEST_ASSERT(env_streq(buf + 12, "ZZZ=1"), "third entry is ZZZ");
    TEST_ASSERT(buf[18] == '\0', "block is double-NUL terminated");
    env_free(&s_env_fixture);
}

/* Case-insensitive sort: mixed-case names order by upcased key. */
static void test_env_sorted_block_case_insensitive(void)
{
    char buf[64];
    uint32_t outlen = 0;
    env_fixture_reset();
    env_set(&s_env_fixture, "beta", "1");
    env_set(&s_env_fixture, "Alpha", "2");
    env_build_block(&s_env_fixture, buf, sizeof(buf), 0, &outlen);
    TEST_ASSERT(env_streq(buf, "Alpha=2"), "Alpha sorts before beta case-insensitively");
    TEST_ASSERT(env_streq(buf + 8, "beta=1"), "beta second");
    env_free(&s_env_fixture);
}

/* Too-small buffer: no partial write, required length reported; NULL sizes. */
static void test_env_build_block_too_small(void)
{
    char buf[4];
    uint32_t outlen = 0;
    int rc;
    env_fixture_reset();
    env_set(&s_env_fixture, "KEY", "VALUE");   /* "KEY=VALUE\0\0" = 11 bytes */
    rc = env_build_block(&s_env_fixture, buf, sizeof(buf), 0, &outlen);
    TEST_ASSERT_EQ(rc, ENV_ERR_NOSPACE, "too-small buffer returns NOSPACE");
    TEST_ASSERT_EQ((int)outlen, 11, "required byte length reported");
    outlen = 0;
    rc = env_build_block(&s_env_fixture, (void *)0, 0, 0, &outlen);
    TEST_ASSERT_EQ(rc, ENV_ERR_NOSPACE, "NULL sizing call returns NOSPACE");
    TEST_ASSERT_EQ((int)outlen, 11, "NULL sizing call reports required length");
    env_free(&s_env_fixture);
}

/* Unicode block: caller buffer, UTF-16 entries + double-NUL. */
static void test_env_build_block_unicode(void)
{
    uint16_t buf[32];
    uint32_t outlen = 0;
    int rc;
    env_fixture_reset();
    env_set(&s_env_fixture, "K", "V");
    rc = env_build_block(&s_env_fixture, buf, sizeof(buf), 1, &outlen);
    TEST_ASSERT_EQ(rc, ENV_OK, "env_build_block UNICODE succeeds");
    TEST_ASSERT_EQ((int)outlen, 10, "unicode block is 5 wchars = 10 bytes");
    TEST_ASSERT(env_test_weq_ascii(buf, 3, "K=V"), "unicode block holds K=V");
    TEST_ASSERT(buf[3] == 0 && buf[4] == 0, "double-NUL terminates unicode block");
    env_free(&s_env_fixture);
}

/* Empty environment: ANSI "\0\0" (2 bytes), UNICODE 0x0000 0x0000 (4 bytes). */
static void test_env_build_block_empty_caller(void)
{
    char abuf[4];
    uint16_t wbuf[4];
    uint32_t outlen = 0;
    int rc;
    env_fixture_reset();
    rc = env_build_block(&s_env_fixture, abuf, sizeof(abuf), 0, &outlen);
    TEST_ASSERT_EQ(rc, ENV_OK, "empty ANSI block builds");
    TEST_ASSERT_EQ((int)outlen, 2, "empty ANSI block is 2 bytes");
    TEST_ASSERT(abuf[0] == '\0' && abuf[1] == '\0', "empty ANSI block is double-NUL");
    outlen = 0;
    rc = env_build_block(&s_env_fixture, wbuf, sizeof(wbuf), 1, &outlen);
    TEST_ASSERT_EQ(rc, ENV_OK, "empty unicode block builds");
    TEST_ASSERT_EQ((int)outlen, 4, "empty unicode block is 4 bytes");
    TEST_ASSERT(wbuf[0] == 0 && wbuf[1] == 0, "empty unicode block is 0x0000 0x0000");
}

/* env_parse_block ANSI: replaces environ, entries sorted + retrievable. */
static void test_env_parse_block_ansi(void)
{
    const char block[] = "AAA=1\0BBB=2\0";   /* +implicit NUL -> double-NUL terminated */
    char out[64];
    int rc;
    env_fixture_reset();
    env_set(&s_env_fixture, "OLD", "x");      /* must be replaced away by parse */
    rc = env_parse_block(&s_env_fixture, block, sizeof(block), 0);
    TEST_ASSERT_EQ(rc, ENV_OK, "env_parse_block ANSI succeeds");
    TEST_ASSERT_EQ((int)s_env_fixture.environ_count, 2, "parse replaced environ (2 entries)");
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "AAA", out, sizeof(out)), 1, "AAA present");
    TEST_ASSERT(env_streq(out, "1"), "AAA=1");
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "BBB", out, sizeof(out)), 1, "BBB present");
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "OLD", out, sizeof(out)),
                   ENV_ERR_NOTFOUND, "OLD replaced away by block");
    env_free(&s_env_fixture);
}

/* env_parse_block: duplicate name -> last occurrence wins, one entry kept. */
static void test_env_parse_block_dedup(void)
{
    const char block[] = "DUP=first\0DUP=second\0";
    char out[64];
    int rc;
    env_fixture_reset();
    rc = env_parse_block(&s_env_fixture, block, sizeof(block), 0);
    TEST_ASSERT_EQ(rc, ENV_OK, "parse with duplicate name succeeds");
    TEST_ASSERT_EQ((int)s_env_fixture.environ_count, 1, "duplicate collapsed to one entry");
    env_get_copy(&s_env_fixture, "DUP", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "second"), "last occurrence wins");
    env_free(&s_env_fixture);
}

/* env_parse_block: malformed entries (empty name / leading '=') are skipped. */
static void test_env_parse_block_skips_malformed(void)
{
    const char block[] = "=BAD\0GOOD=ok\0";
    char out[64];
    int rc;
    env_fixture_reset();
    rc = env_parse_block(&s_env_fixture, block, sizeof(block), 0);
    TEST_ASSERT_EQ(rc, ENV_OK, "parse skips a malformed entry");
    TEST_ASSERT_EQ((int)s_env_fixture.environ_count, 1, "only the well-formed entry kept");
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "GOOD", out, sizeof(out)), 2, "GOOD present");
    env_free(&s_env_fixture);
}

/* env_parse_block UNICODE: round-trips a value through build+parse. */
static void test_env_parse_block_unicode(void)
{
    uint16_t block[32];
    uint32_t bytes = 0;
    char out[64];
    int rc;
    env_fixture_reset();
    env_set(&s_env_fixture, "UK", "uv");
    rc = env_build_block(&s_env_fixture, block, sizeof(block), 1, &bytes);
    TEST_ASSERT_EQ(rc, ENV_OK, "build unicode block for round-trip");
    env_free(&s_env_fixture);
    env_fixture_reset();
    rc = env_parse_block(&s_env_fixture, block, bytes, 1);
    TEST_ASSERT_EQ(rc, ENV_OK, "parse unicode block succeeds");
    TEST_ASSERT_EQ((int)s_env_fixture.environ_count, 1, "one entry parsed");
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "UK", out, sizeof(out)), 2, "UK present");
    TEST_ASSERT(env_streq(out, "uv"), "UK=uv round-trips through the unicode block");
    env_free(&s_env_fixture);
}

/* 1 MiB block-size DoS cap: repeated large sets eventually return NOSPACE. */
static char s_cap_val[8192];
static void test_env_block_size_cap(void)
{
    char name[16];
    uint32_t i;
    int hit_cap = 0;
    env_fixture_reset();
    for (i = 0; i < sizeof(s_cap_val) - 1u; i++)
        s_cap_val[i] = 'x';
    s_cap_val[sizeof(s_cap_val) - 1u] = '\0';   /* ~8 KiB value */
    for (i = 0; i < ENV_MAX_ENTRIES; i++) {
        int rc;
        name[0] = 'V';
        name[1] = (char)('A' + (int)((i / 26u) % 26u));
        name[2] = (char)('A' + (int)(i % 26u));
        name[3] = '\0';
        rc = env_set(&s_env_fixture, name, s_cap_val);
        if (rc == ENV_ERR_NOSPACE) {
            hit_cap = 1;
            break;
        }
        TEST_ASSERT_EQ(rc, ENV_OK, "env_set succeeds until the block cap");
    }
    TEST_ASSERT(hit_cap, "1 MiB block cap eventually rejects a set with NOSPACE");
    env_free(&s_env_fixture);
}

/* Parser must not install a value over ENV_VALUE_MAX (would let NtQuery read
 * past its ENV_VALUE_MAX-sized buffer): the over-cap entry is skipped. */
static char s_parse_block[ENV_VALUE_MAX + 16];
static void test_env_parse_block_over_value_skipped(void)
{
    uint32_t i, p = 0;
    int rc;
    env_fixture_reset();
    s_parse_block[p++] = 'A';
    s_parse_block[p++] = '=';
    for (i = 0; i < ENV_VALUE_MAX + 2u; i++)   /* value = ENV_VALUE_MAX+2 bytes (over cap) */
        s_parse_block[p++] = 'x';
    s_parse_block[p++] = '\0';                 /* entry terminator */
    s_parse_block[p++] = '\0';                 /* block terminator */
    rc = env_parse_block(&s_env_fixture, s_parse_block, p, 0);
    TEST_ASSERT_EQ(rc, ENV_OK, "parse succeeds (over-value entry skipped, not an error)");
    TEST_ASSERT_EQ((int)s_env_fixture.environ_count, 0,
                   "over-ENV_VALUE_MAX value not stored");
    env_free(&s_env_fixture);
}

/* Oversize block (raw size cannot fit ENV_BLOCK_MAX) is rejected BEFORE any
 * scan/alloc, leaving the prior environment intact. */
static void test_env_parse_block_oversize_rejected(void)
{
    char dummy[4] = { 'A', '=', 'x', 0 };
    char kv[8];
    int rc;
    env_fixture_reset();
    env_set(&s_env_fixture, "KEEP", "me");
    rc = env_parse_block(&s_env_fixture, dummy, ENV_BLOCK_MAX + 1u, 0);
    TEST_ASSERT_EQ(rc, ENV_ERR_NOSPACE, "oversize block rejected up front");
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "KEEP", kv, sizeof(kv)), 2,
                   "prior environ untouched by the rejected block");
    env_free(&s_env_fixture);
}

/* env_adopt_block: reverse-order input with a duplicate name -> sorted output,
 * duplicate collapsed to the LAST occurrence (stable merge sort + linear dedup). */
static void test_env_adopt_block_sorts_dedups(void)
{
    const char *entries[] = { "ZED=1", "MID=2", "AAA=3", "MID=4" };
    env_fixture_reset();
    TEST_ASSERT_EQ(env_adopt_block(&s_env_fixture, entries, 4), ENV_OK,
                   "adopt reverse-order + duplicate block");
    TEST_ASSERT_EQ((int)s_env_fixture.environ_count, 3, "duplicate MID collapsed to one");
    TEST_ASSERT(env_streq(s_env_fixture.environ[0], "AAA=3"), "AAA sorts first");
    TEST_ASSERT(env_streq(s_env_fixture.environ[1], "MID=4"), "MID second, last occurrence wins");
    TEST_ASSERT(env_streq(s_env_fixture.environ[2], "ZED=1"), "ZED sorts last");
    env_free(&s_env_fixture);
}

/* Cached block-byte total stays accurate: fill to the block cap, unset one
 * entry, and a new same-size set must then fit (env_unset decremented the
 * cached total; a drifted counter would wrongly reject). */
static char s_bt_val[8192];
static void test_env_bytes_unset_reclaims(void)
{
    char name[8];
    uint32_t i;
    int hit = 0;
    env_fixture_reset();
    for (i = 0; i < sizeof(s_bt_val) - 1u; i++)
        s_bt_val[i] = 'z';
    s_bt_val[sizeof(s_bt_val) - 1u] = '\0';
    for (i = 0; i < ENV_MAX_ENTRIES; i++) {
        int rc;
        name[0] = 'B';
        name[1] = (char)('A' + (int)((i / 26u) % 26u));
        name[2] = (char)('A' + (int)(i % 26u));
        name[3] = '\0';
        rc = env_set(&s_env_fixture, name, s_bt_val);
        if (rc == ENV_ERR_NOSPACE) {
            hit = 1;
            break;
        }
        TEST_ASSERT_EQ(rc, ENV_OK, "env_set succeeds until the block cap");
    }
    TEST_ASSERT(hit, "environment filled to the 1 MiB block cap");
    TEST_ASSERT_EQ(env_unset(&s_env_fixture, "BAA"), ENV_OK, "unset an existing entry");
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "NEW", s_bt_val), ENV_OK,
                   "unset reclaimed block budget for a new same-size set");
    env_free(&s_env_fixture);
}

/* Adopting an EMPTY block must reset the cached byte total, not retain the old
 * quota charge -- otherwise the next env_set spuriously returns NOSPACE. */
static void test_env_adopt_empty_resets_quota(void)
{
    char name[8];
    uint32_t i;
    int hit = 0;
    env_fixture_reset();
    for (i = 0; i < sizeof(s_bt_val) - 1u; i++)
        s_bt_val[i] = 'q';
    s_bt_val[sizeof(s_bt_val) - 1u] = '\0';
    for (i = 0; i < ENV_MAX_ENTRIES; i++) {
        int rc;
        name[0] = 'Q';
        name[1] = (char)('A' + (int)((i / 26u) % 26u));
        name[2] = (char)('A' + (int)(i % 26u));
        name[3] = '\0';
        rc = env_set(&s_env_fixture, name, s_bt_val);
        if (rc == ENV_ERR_NOSPACE) {
            hit = 1;
            break;
        }
    }
    TEST_ASSERT(hit, "environment filled to the block cap");
    TEST_ASSERT_EQ(env_adopt_block(&s_env_fixture, (const char *const *)0, 0u), ENV_OK,
                   "adopt empty block clears environ");
    TEST_ASSERT_EQ((int)s_env_fixture.environ_count, 0, "environ empty after clear");
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "AFTER", "x"), ENV_OK,
                   "set succeeds after empty adoption reset the cached quota");
    env_free(&s_env_fixture);
}

/* ========================================================================
 * s12: hidden drive-letter current-directory variables (=C:, =D:)
 * ======================================================================== */

/* Round-trip: env_set_drive_cwd stores "=X:" and env_get_drive_cwd / env_get_copy
 * read it back. The hidden entry is a normal store entry with a leading-'=' name. */
static void test_env_drive_cwd_roundtrip(void)
{
    char out[64];
    int r;
    env_fixture_reset();
    TEST_ASSERT_EQ(env_set_drive_cwd(&s_env_fixture, 'C', "C:\\Users"), ENV_OK,
                   "env_set_drive_cwd stores =C:");
    r = env_get_drive_cwd(&s_env_fixture, 'C', out, sizeof(out));
    TEST_ASSERT_EQ(r, 8, "get_drive_cwd returns value length 8");
    TEST_ASSERT(env_streq(out, "C:\\Users"), "=C: value round-trips");
    /* The stored name is literally "=C:" -- readable via env_get_copy too. */
    r = env_get_copy(&s_env_fixture, "=C:", out, sizeof(out));
    TEST_ASSERT_EQ(r, 8, "env_get_copy on =C: returns the same value");
    TEST_ASSERT(env_streq(out, "C:\\Users"), "env_get_copy value matches");
    TEST_ASSERT_EQ(s_env_fixture.environ_count, 1u, "one hidden entry stored");
    env_free(&s_env_fixture);
}

/* An unset drive resolves to its root "X:\" and still succeeds. */
static void test_env_drive_cwd_default_root(void)
{
    char out[64];
    int r;
    env_fixture_reset();
    r = env_get_drive_cwd(&s_env_fixture, 'D', out, sizeof(out));
    TEST_ASSERT_EQ(r, 3, "unset drive returns root length 3");
    TEST_ASSERT(env_streq(out, "D:\\"), "unset drive returns X:\\ root");
    env_free(&s_env_fixture);
}

/* Case-fold: lowercase drive stores/reads the same "=X:" entry; a non-letter
 * drive is rejected. */
static void test_env_drive_cwd_case_and_invalid(void)
{
    char out[64];
    env_fixture_reset();
    TEST_ASSERT_EQ(env_set_drive_cwd(&s_env_fixture, 'c', "C:\\Tmp"), ENV_OK,
                   "lowercase drive accepted (uppercased to =C:)");
    TEST_ASSERT_EQ(env_get_drive_cwd(&s_env_fixture, 'C', out, sizeof(out)), 6,
                   "uppercase get reads the lowercase-set entry (one drive)");
    TEST_ASSERT_EQ(s_env_fixture.environ_count, 1u, "'c' and 'C' are one entry");
    TEST_ASSERT_EQ(env_set_drive_cwd(&s_env_fixture, '1', "X"), ENV_ERR_INVAL,
                   "non-letter drive rejected");
    TEST_ASSERT_EQ(env_get_drive_cwd(&s_env_fixture, '1', out, sizeof(out)),
                   ENV_ERR_INVAL, "non-letter get rejected");
    env_free(&s_env_fixture);
}

/* env_name_classify: only the exact "=X:" shape is a legal '='-name; any other
 * leading-'=' or embedded-'=' name is still rejected by env_set. */
static void test_env_drive_cwd_name_validation(void)
{
    env_fixture_reset();
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "=C:", "C:\\W"), ENV_OK,
                   "=C: is a legal hidden name");
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "=CD:", "v"), ENV_ERR_INVAL,
                   "=CD: (two-letter) rejected");
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "=C", "v"), ENV_ERR_INVAL,
                   "=C (no colon) rejected");
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "=", "v"), ENV_ERR_INVAL,
                   "bare = rejected");
    TEST_ASSERT_EQ(env_set(&s_env_fixture, "A=B", "v"), ENV_ERR_INVAL,
                   "embedded = still rejected");
    TEST_ASSERT_EQ(s_env_fixture.environ_count, 1u, "only the legal =C: stored");
    env_free(&s_env_fixture);
}

/* Hidden "=X:" vars sort BEFORE ordinary names in a built block: '=' (0x3D) is
 * below any letter, so they appear at the front (the CreateProcess contract). */
static void test_env_drive_cwd_sorts_first(void)
{
    char buf[64];
    uint32_t outlen = 0;
    int rc;
    env_fixture_reset();
    env_set(&s_env_fixture, "AAA", "2");
    env_set_drive_cwd(&s_env_fixture, 'C', "C:\\Users");
    rc = env_build_block(&s_env_fixture, buf, sizeof(buf), 0, &outlen);
    TEST_ASSERT_EQ(rc, ENV_OK, "block builds");
    /* "=C:=C:\\Users\0" (13) + "AAA=2\0" (6) + "\0" = 20 bytes */
    TEST_ASSERT_EQ((int)outlen, 20, "block length includes both entries + terminator");
    TEST_ASSERT(env_streq(buf, "=C:=C:\\Users"), "hidden =C: entry sorts FIRST");
    TEST_ASSERT(env_streq(buf + 13, "AAA=2"), "ordinary AAA follows the hidden var");
    env_free(&s_env_fixture);
}

/* A custom CreateProcess block carrying a "=X:" entry is preserved through
 * env_parse_block (the Windows contract that a caller-supplied block may include
 * the hidden drive vars), and it still sorts to the front on rebuild. */
static void test_env_drive_cwd_parse_block_preserved(void)
{
    static const char blk[] = "AAA=1\0=C:=C:\\W\0";   /* unsorted, hidden last */
    char buf[64];
    uint32_t outlen = 0;
    env_fixture_reset();
    TEST_ASSERT_EQ(env_parse_block(&s_env_fixture, blk, sizeof(blk) - 1u, 0),
                   ENV_OK, "parse block with a hidden =C: entry");
    TEST_ASSERT_EQ(s_env_fixture.environ_count, 2u, "both entries retained");
    env_build_block(&s_env_fixture, buf, sizeof(buf), 0, &outlen);
    TEST_ASSERT(env_streq(buf, "=C:=C:\\W"), "hidden =C: sorted to front after parse");
    env_free(&s_env_fixture);
}

/* Public adapter contracts: NULL guards, the out_size<4 boundary, the exact
 * 4-byte root case, oversized-value truncation (full length returned, NUL kept),
 * and env_set NULL-arg propagation. */
static void test_env_drive_cwd_adapter_boundaries(void)
{
    char out[8];
    char big[600];
    uint32_t i;
    env_fixture_reset();
    /* NULL / bad-arg guards. */
    TEST_ASSERT_EQ(env_get_drive_cwd(&s_env_fixture, 'C', (char *)0, 8u),
                   ENV_ERR_INVAL, "NULL out rejected");
    TEST_ASSERT_EQ(env_get_drive_cwd(&s_env_fixture, 'C', out, 3u),
                   ENV_ERR_INVAL, "out_size < 4 rejected");
    TEST_ASSERT_EQ(env_set_drive_cwd(&s_env_fixture, 'C', (const char *)0),
                   ENV_ERR_INVAL, "NULL path rejected");
    TEST_ASSERT_EQ(env_set_drive_cwd((struct task *)0, 'C', "x"),
                   ENV_ERR_INVAL, "NULL task rejected");
    /* Exact 4-byte buffer holds the "X:\" root of an unset drive. */
    TEST_ASSERT_EQ(env_get_drive_cwd(&s_env_fixture, 'E', out, 4u), 3,
                   "unset drive fits exactly in a 4-byte buffer");
    TEST_ASSERT(env_streq(out, "E:\\"), "4-byte root is E:\\");
    /* Oversized stored value: the getter truncates into a small buffer, keeps the
     * NUL, and returns the FULL length so a caller can detect truncation. */
    big[0] = 'C'; big[1] = ':'; big[2] = '\\';
    for (i = 3; i < sizeof(big) - 1u; i++)
        big[i] = 'a';
    big[sizeof(big) - 1u] = '\0';
    TEST_ASSERT_EQ(env_set_drive_cwd(&s_env_fixture, 'C', big), ENV_OK,
                   "oversized =C: stored");
    TEST_ASSERT_EQ(env_get_drive_cwd(&s_env_fixture, 'C', out, sizeof(out)),
                   (int)(sizeof(big) - 1u), "truncated get returns full length");
    TEST_ASSERT(out[sizeof(out) - 1u] == '\0', "truncated out stays NUL-terminated");
    /* A present value naming a DIFFERENT drive (or non-absolute) is rejected by
     * the getter itself -- the resolution base must be on the requested drive. */
    env_free(&s_env_fixture);
    env_fixture_reset();
    env_set(&s_env_fixture, "=D:", "C:\\Trap");        /* foreign drive in value */
    TEST_ASSERT_EQ(env_get_drive_cwd(&s_env_fixture, 'D', out, sizeof(out)),
                   ENV_ERR_INVAL, "foreign-drive =D: value rejected by getter");
    env_free(&s_env_fixture);
}

/* Drive-relative resolution matrix (the hidden-var CONSUMER), exercised
 * hermetically through task_resolve_path_for against the env fixture. Covers
 * current-drive (uses cwd), other-drive (uses =X:), unset (root), absolute,
 * bare drive, dot-dot, and an oversized-=X: truncation fallback. */
static void test_env_drive_cwd_resolve_matrix(void)
{
    char out[TASK_CWD_MAX];              /* a remembered dir can approach TASK_CWD_MAX */
    char big[600];
    uint32_t i;
    env_fixture_reset();
    task_set_cwd(&s_env_fixture, "C:\\Cur\\Dir");     /* current drive = C: */
    env_set_drive_cwd(&s_env_fixture, 'D', "D:\\Saved");

    /* Current drive, relative -> from the live cwd (not any =C: var). */
    TEST_ASSERT_EQ(task_resolve_path_for(&s_env_fixture, "C:sub", out, sizeof(out)),
                   0, "C:sub resolves");
    TEST_ASSERT(env_streq(out, "C:\\Cur\\Dir\\sub"), "C:sub -> cwd\\sub");
    /* Bare current drive -> the cwd itself. */
    task_resolve_path_for(&s_env_fixture, "C:", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "C:\\Cur\\Dir"), "bare C: -> cwd");
    /* Other drive, relative -> from that drive's remembered "=D:". */
    task_resolve_path_for(&s_env_fixture, "D:sub", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "D:\\Saved\\sub"), "D:sub -> =D:\\sub");
    /* Bare other drive -> the remembered directory. */
    task_resolve_path_for(&s_env_fixture, "D:", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "D:\\Saved"), "bare D: -> =D:");
    /* Dot-dot is applied against the remembered base. */
    task_resolve_path_for(&s_env_fixture, "D:..", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "D:\\"), "D:.. pops =D: to root");
    /* Unset drive -> the drive root. */
    task_resolve_path_for(&s_env_fixture, "E:sub", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "E:\\sub"), "unset E:sub -> E:\\sub");
    /* Absolute drive path ignores the remembered directory. */
    task_resolve_path_for(&s_env_fixture, "D:\\abs", out, sizeof(out));
    TEST_ASSERT(env_streq(out, "D:\\abs"), "D:\\abs stays absolute");
    /* A LARGE but representable remembered dir (short components, ~400 bytes,
     * well under sizeof(base)/VFS_MAX_NAME) still resolves -- the truncation guard
     * must not false-trip below the buffer bound. */
    big[0] = 'G'; big[1] = ':';
    for (i = 2; i < 400u; ) { big[i++] = '\\'; big[i++] = 'a'; }   /* "G:\a\a...\a" */
    big[400] = '\0';
    env_set_drive_cwd(&s_env_fixture, 'G', big);
    TEST_ASSERT_EQ(task_resolve_path_for(&s_env_fixture, "G:", out, sizeof(out)),
                   0, "large (400-byte) remembered =G: resolves");
    TEST_ASSERT(env_streq(out, big), "large =G: value resolves verbatim");
    /* Oversized "=F:" (past TASK_CWD_MAX) is PRESENT but unrepresentable: the
     * resolver FAILS CLOSED (returns -1) rather than silently retargeting to the
     * drive root (adversarial re-review -- root-fallback would mis-target). */
    big[0] = 'F'; big[1] = ':'; big[2] = '\\';
    for (i = 3; i < sizeof(big) - 1u; i++)
        big[i] = 'a';
    big[sizeof(big) - 1u] = '\0';                     /* strlen 599 > TASK_CWD_MAX */
    env_set_drive_cwd(&s_env_fixture, 'F', big);
    TEST_ASSERT_EQ(task_resolve_path_for(&s_env_fixture, "F:x", out, sizeof(out)),
                   -1, "oversized =F: fails closed (no root retarget)");
    /* A crafted "=H:" naming ANOTHER drive must NOT retarget H:relative to that
     * drive -- fail closed rather than resolve "H:x" against "C:\Trap". */
    env_set(&s_env_fixture, "=H:", "C:\\Trap");
    TEST_ASSERT_EQ(task_resolve_path_for(&s_env_fixture, "H:x", out, sizeof(out)),
                   -1, "foreign-drive =H: fails closed (no cross-drive retarget)");
    /* A non-absolute "=I:" value ("I:rel", no separator) is also rejected. */
    env_set(&s_env_fixture, "=I:", "I:rel");
    TEST_ASSERT_EQ(task_resolve_path_for(&s_env_fixture, "I:x", out, sizeof(out)),
                   -1, "non-absolute =I: fails closed");
    /* A drive-relative path whose TAIL is itself drive-qualified ("D:C:\Victim")
     * must NOT cross to the tail's drive -- fail closed (both other-drive and
     * current-drive forms). */
    TEST_ASSERT_EQ(task_resolve_path_for(&s_env_fixture, "D:C:\\Victim", out, sizeof(out)),
                   -1, "nested drive qualifier (other drive) fails closed");
    TEST_ASSERT_EQ(task_resolve_path_for(&s_env_fixture, "C:D:\\x", out, sizeof(out)),
                   -1, "nested drive qualifier (current drive) fails closed");

    s_env_fixture.cwd[0] = '\0';                      /* clear cwd for later tests */
    env_free(&s_env_fixture);
}

/* ============================================================================
 * s13: CreateEnvironmentBlock / DestroyEnvironmentBlock / ExpandForUser
 * ========================================================================== */

/* env_create_block (NULL token, inherit): self-describing sorted UTF-16 block
 * that env_destroy_block frees with the pointer alone (no leak). */
static void test_env_create_block_roundtrip(void)
{
    void *block = NULL;
    const uint16_t *body;
    int rc;
    env_fixture_reset();
    env_set(&s_env_fixture, "ZED", "1");
    env_set(&s_env_fixture, "ALPHA", "2");
    rc = env_create_block(&s_env_fixture, (const void *)0, 1, &block);
    TEST_ASSERT_EQ(rc, ENV_OK, "env_create_block succeeds for NULL token + inherit");
    TEST_ASSERT(block != (void *)0, "block pointer returned");
    body = (const uint16_t *)block;
    /* environ is sorted case-insensitively: ALPHA=2 precedes ZED=1. */
    TEST_ASSERT(env_test_weq_ascii(body, 7, "ALPHA=2"),
                "block starts with the alphabetically-first entry");
    env_destroy_block(block);   /* pointer-only free; leak check below catches a miss */
    env_free(&s_env_fixture);
}

/* env_create_block: empty environ still yields a valid "\0\0" block. */
static void test_env_create_block_empty(void)
{
    void *block = NULL;
    const uint16_t *body;
    int rc;
    env_fixture_reset();
    rc = env_create_block(&s_env_fixture, (const void *)0, 1, &block);
    TEST_ASSERT_EQ(rc, ENV_OK, "env_create_block succeeds on empty environ");
    body = (const uint16_t *)block;
    TEST_ASSERT(body && body[0] == 0 && body[1] == 0,
                "empty environment block is the double-NUL form");
    env_destroy_block(block);
    env_free(&s_env_fixture);
}

/* env_create_block: deferred Win32 branches are refused, never fabricated. */
static void test_env_create_block_deferred_branches(void)
{
    void *block = (void *)0x1;   /* sentinel: must be NULLed on every failure path */
    int rc;
    env_fixture_reset();
    env_set(&s_env_fixture, "K", "V");

    /* Non-NULL token -> per-user block needs SID->hive + LoadUserProfile. */
    rc = env_create_block(&s_env_fixture, (const void *)0x1000, 1, &block);
    TEST_ASSERT_EQ(rc, ENV_ERR_UNSUPPORTED, "non-NULL token refused (deferred)");
    TEST_ASSERT(block == (void *)0, "out_block cleared on refusal");

    /* inherit == 0 -> fresh Registry-only block needs SMP-safe snapshot. */
    block = (void *)0x1;
    rc = env_create_block(&s_env_fixture, (const void *)0, 0, &block);
    TEST_ASSERT_EQ(rc, ENV_ERR_UNSUPPORTED, "no-inherit refused (deferred)");
    TEST_ASSERT(block == (void *)0, "out_block cleared on refusal");

    /* NULL out pointer / NULL caller are argument errors. */
    TEST_ASSERT_EQ(env_create_block(&s_env_fixture, (const void *)0, 1, (void **)0),
                   ENV_ERR_INVAL, "NULL out_block rejected");
    block = (void *)0x1;
    TEST_ASSERT_EQ(env_create_block((struct task *)0, (const void *)0, 1, &block),
                   ENV_ERR_INVAL, "NULL caller rejected");
    TEST_ASSERT(block == (void *)0, "out_block cleared on NULL caller");
    env_free(&s_env_fixture);
}

/* env_destroy_block: NULL is a no-op; an in-bounds buffer with a non-matching
 * header is ignored (magic gate); a valid single create/destroy leaves the heap
 * intact. Does NOT test double-destroy: that reads freed memory (UAF) and is a
 * caller-contract violation, not a supported input (the poison is only a best-
 * effort net before allocator reuse). The per-test leak check catches a bad free. */
static void test_env_destroy_block_defensive(void)
{
    void *block = NULL;
    /* 8-wchar buffer: [0..3] stand in for a {magic,wchars} header, [4..7] a body.
     * Passing &buf[4] makes the header read hit buf[0..3] (IN BOUNDS); magic 0 !=
     * ENV_BLK_MAGIC, so it is ignored -- no predecessor fault, no free. */
    uint16_t buf[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    env_destroy_block((void *)0);        /* NULL -> no-op */
    env_destroy_block((void *)&buf[4]);  /* wrong-magic header -> ignored */

    env_fixture_reset();
    env_set(&s_env_fixture, "K", "V");
    TEST_ASSERT_EQ(env_create_block(&s_env_fixture, (const void *)0, 1, &block),
                   ENV_OK, "valid block created");
    env_destroy_block(block);   /* single free of a live pointer (the contract) */
    /* Heap intact after the NULL / wrong-header calls + a valid create/destroy:
     * a fresh block still allocates and frees. */
    block = NULL;
    TEST_ASSERT_EQ(env_create_block(&s_env_fixture, (const void *)0, 1, &block),
                   ENV_OK, "heap intact after defensive calls + a valid destroy");
    env_destroy_block(block);
    env_free(&s_env_fixture);
}

/* ExpandEnvironmentStringsForUser: NULL token expands against the caller env. */
static void test_env_expand_for_user_null_token(void)
{
    uint16_t srcbuf[16], dstbuf[64];
    UNICODE_STRING src, dst;
    uint32_t rl = 0;
    NTSTATUS st;
    env_fixture_reset();
    env_set(&s_env_fixture, "GREET", "Hi");

    src.Length = (uint16_t)(env_test_wfill("%GREET%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(dstbuf);
    dst.Buffer = dstbuf;

    st = ExpandEnvironmentStringsForUser(&s_env_fixture, (const void *)0,
                                         &src, &dst, &rl);
    TEST_ASSERT_EQ((int)st, (int)STATUS_SUCCESS, "NULL-token expansion succeeds");
    TEST_ASSERT(env_test_weq_ascii(dstbuf, 2, "Hi"),
                "expands %GREET% against caller environment");
    TEST_ASSERT_EQ((int)dst.Length, (int)(2u * 2u),
                   "Destination->Length is result bytes excluding NUL");
    env_free(&s_env_fixture);
}

/* ExpandEnvironmentStringsForUser: per-user token + NULL caller are refused. */
static void test_env_expand_for_user_refusals(void)
{
    uint16_t srcbuf[16], dstbuf[16];
    UNICODE_STRING src, dst;
    NTSTATUS st;
    env_fixture_reset();
    src.Length = (uint16_t)(env_test_wfill("%X%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(dstbuf);
    dst.Buffer = dstbuf;

    st = ExpandEnvironmentStringsForUser(&s_env_fixture, (const void *)0x1000,
                                         &src, &dst, (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_NOT_SUPPORTED,
                   "per-user token refused (deferred)");
    st = ExpandEnvironmentStringsForUser((struct task *)0, (const void *)0,
                                         &src, &dst, (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_INVALID_PARAMETER, "NULL caller rejected");
    env_free(&s_env_fixture);
}

/* ExpandEnvironmentStringsForUser: an environ that exceeds RTL_ENV_BLOCK_MAX_WCHARS
 * (64 KiWCHAR) fails to build the block and maps to STATUS_INVALID_PARAMETER (NOT a
 * retryable STATUS_BUFFER_TOO_SMALL) -- the over-cap C1 mapping, consistent with the
 * RtlExpandEnvironmentStrings_U NULL-Environment path. Three ~32 KiB ASCII values
 * (1 byte -> 1 WCHAR) push the block past the cap. */
static void test_env_expand_for_user_over_cap(void)
{
    static char bigval[32001];   /* > ENV_STR_KMALLOC_MAX; 3 of these exceed 64 KiWCHAR */
    uint16_t srcbuf[16], dstbuf[64];
    UNICODE_STRING src, dst;
    uint32_t i;
    NTSTATUS st;

    env_fixture_reset();
    for (i = 0; i < sizeof(bigval) - 1u; i++)
        bigval[i] = 'x';
    bigval[sizeof(bigval) - 1u] = '\0';
    env_set(&s_env_fixture, "V1", bigval);
    env_set(&s_env_fixture, "V2", bigval);
    env_set(&s_env_fixture, "V3", bigval);   /* block now > RTL_ENV_BLOCK_MAX_WCHARS */

    src.Length = (uint16_t)(env_test_wfill("%V1%", srcbuf) * 2u);
    src.MaximumLength = (uint16_t)sizeof(srcbuf);
    src.Buffer = srcbuf;
    dst.Length = 0;
    dst.MaximumLength = (uint16_t)sizeof(dstbuf);
    dst.Buffer = dstbuf;

    st = ExpandEnvironmentStringsForUser(&s_env_fixture, (const void *)0,
                                         &src, &dst, (uint32_t *)0);
    TEST_ASSERT_EQ((int)st, (int)STATUS_INVALID_PARAMETER,
                   "over-cap environ -> STATUS_INVALID_PARAMETER (non-retryable)");
    env_free(&s_env_fixture);
}

/* ---- s14: SearchPathW / SearchPathA / SetSearchPathMode -------------------
 * These probe the live VFS. When the harness build has no writable C: drive the
 * setup helpers fail and the case TEST_SKIPs rather than false-failing. */

/* Ensure a directory exists (tolerates pre-existing). 0 = present afterward. */
static int sp_test_mkdir(const char *path)
{
    struct vfs_stat st;
    if (vfs_stat(path, &st) == 0)
        return (st.type == VFS_DIRECTORY) ? 0 : -1;
    return vfs_create(path, VFS_DIRECTORY);
}

/* Ensure an empty regular file exists at path (parent must exist). 0 = present. */
static int sp_test_touch(const char *path)
{
    struct vfs_stat st;
    if (vfs_stat(path, &st) == 0)
        return (st.type == VFS_FILE) ? 0 : -1;
    return vfs_create(path, VFS_FILE);
}

static void test_env_searchpath_finds_system32(void)
{
    char out[SP_PATH_MAX];
    uint32_t err = 0, rc;

    env_fixture_reset();
    if (sp_test_mkdir("C:\\Impossible") != 0 ||
        sp_test_mkdir("C:\\Impossible\\System32") != 0 ||
        sp_test_touch("C:\\Impossible\\System32\\sp_sys.exe") != 0) {
        env_free(&s_env_fixture);
        TEST_SKIP("VFS unavailable for SearchPath probe");
        return;
    }
    rc = env_search_path(&s_env_fixture, (const char *)0, "sp_sys.exe",
                         (const char *)0, out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT(rc > 0, "SearchPath finds a System32 binary");
    TEST_ASSERT(env_streq(out, "C:\\Impossible\\System32\\sp_sys.exe"),
                "returns the System32 path");
    TEST_ASSERT_EQ(err, (uint32_t)ERROR_SUCCESS, "no error on success");
    vfs_unlink("C:\\Impossible\\System32\\sp_sys.exe");
    env_free(&s_env_fixture);
}

static void test_env_searchpath_missing(void)
{
    char out[SP_PATH_MAX];
    uint32_t err = 0, rc;

    env_fixture_reset();
    rc = env_search_path(&s_env_fixture, (const char *)0,
                         "sp_no_such_file_zzz.exe", (const char *)0,
                         out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT_EQ(rc, 0u, "missing file returns 0");
    TEST_ASSERT_EQ(err, (uint32_t)ERROR_FILE_NOT_FOUND,
                   "missing file sets ERROR_FILE_NOT_FOUND");
    env_free(&s_env_fixture);
}

static void test_env_searchpath_explicit_ext(void)
{
    char out[SP_PATH_MAX];
    uint32_t err = 0, rc;

    env_fixture_reset();
    if (sp_test_mkdir("C:\\sp_expl") != 0 ||
        sp_test_touch("C:\\sp_expl\\tool.exe") != 0) {
        env_free(&s_env_fixture);
        TEST_SKIP("VFS unavailable for SearchPath probe");
        return;
    }
    /* "tool" (no extension) + ".exe" -> tool.exe, found only in the explicit dir. */
    rc = env_search_path(&s_env_fixture, "C:\\sp_expl", "tool", ".exe",
                         out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT(rc > 0 && env_streq(out, "C:\\sp_expl\\tool.exe"),
                "explicit lpPath + appended .exe finds tool.exe");
    vfs_unlink("C:\\sp_expl\\tool.exe");
    env_free(&s_env_fixture);
}

static void test_env_searchpath_qualified(void)
{
    char out[SP_PATH_MAX];
    uint32_t err = 0, rc;

    env_fixture_reset();
    if (sp_test_mkdir("C:\\Impossible") != 0 ||
        sp_test_touch("C:\\Impossible\\sp_q.exe") != 0) {
        env_free(&s_env_fixture);
        TEST_SKIP("VFS unavailable for SearchPath probe");
        return;
    }
    /* A qualified name is probed directly, no directory iteration. */
    rc = env_search_path(&s_env_fixture, (const char *)0,
                         "C:\\Impossible\\sp_q.exe", (const char *)0,
                         out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT(rc > 0 && env_streq(out, "C:\\Impossible\\sp_q.exe"),
                "qualified path probed directly");
    rc = env_search_path(&s_env_fixture, (const char *)0,
                         "C:\\Impossible\\sp_q_absent.exe", (const char *)0,
                         out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT_EQ(rc, 0u, "qualified missing path -> 0");
    TEST_ASSERT_EQ(err, (uint32_t)ERROR_FILE_NOT_FOUND,
                   "qualified missing -> FILE_NOT_FOUND");
    vfs_unlink("C:\\Impossible\\sp_q.exe");
    env_free(&s_env_fixture);
}

static void test_env_searchpath_relative_subpath(void)
{
    char out[SP_PATH_MAX];
    uint32_t err = 0, rc;

    env_fixture_reset();
    /* An ORDINARY relative subpath is searched BENEATH each directory, NOT probed
     * directly against the CWD -- so it cannot bypass an explicit lpPath or the
     * trusted default-search legs. The file exists only under System32\plug, and
     * the fixture CWD is empty, so finding it proves the beneath-each-dir search. */
    if (sp_test_mkdir("C:\\Impossible") != 0 ||
        sp_test_mkdir("C:\\Impossible\\System32") != 0 ||
        sp_test_mkdir("C:\\Impossible\\System32\\plug") != 0 ||
        sp_test_touch("C:\\Impossible\\System32\\plug\\tool.exe") != 0) {
        env_free(&s_env_fixture);
        TEST_SKIP("VFS unavailable for SearchPath probe");
        return;
    }
    rc = env_search_path(&s_env_fixture, (const char *)0, "plug\\tool.exe",
                         (const char *)0, out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT(rc > 0 &&
                env_streq(out, "C:\\Impossible\\System32\\plug\\tool.exe"),
                "relative subpath searched beneath the System32 leg (not against CWD)");
    /* Explicit lpPath: the subpath is honored beneath the given directory. */
    err = 0;
    rc = env_search_path(&s_env_fixture, "C:\\Impossible\\System32", "plug\\tool.exe",
                         (const char *)0, out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT(rc > 0 &&
                env_streq(out, "C:\\Impossible\\System32\\plug\\tool.exe"),
                "relative subpath honored beneath explicit lpPath");
    /* An interior ".." in an unqualified name is REFUSED -- it would canonicalize
     * out of the trusted/explicit directory. Both separator forms, default + explicit. */
    err = 0;
    rc = env_search_path(&s_env_fixture, (const char *)0, "x\\..\\..\\..\\Temp\\evil.exe",
                         (const char *)0, out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT_EQ(rc, 0u, "interior .. (default search) refused");
    TEST_ASSERT_EQ(err, (uint32_t)ERROR_INVALID_PARAMETER,
                   "interior .. -> ERROR_INVALID_PARAMETER (no escape from trusted leg)");
    err = 0;
    rc = env_search_path(&s_env_fixture, "C:\\Impossible\\System32", "a/../../evil.exe",
                         (const char *)0, out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT_EQ(rc, 0u, "interior .. under explicit lpPath refused (forward-slash)");
    TEST_ASSERT_EQ(err, (uint32_t)ERROR_INVALID_PARAMETER,
                   "interior .. under explicit lpPath -> INVALID_PARAMETER");
    vfs_unlink("C:\\Impossible\\System32\\plug\\tool.exe");
    env_free(&s_env_fixture);
}

static void test_env_searchpath_mode_precedence(void)
{
    char out[SP_PATH_MAX];
    uint32_t err = 0, rc;

    env_fixture_reset();
    if (sp_test_mkdir("C:\\Impossible") != 0 ||
        sp_test_mkdir("C:\\Impossible\\System32") != 0 ||
        sp_test_mkdir("C:\\sp_cwd") != 0 ||
        sp_test_touch("C:\\Impossible\\System32\\dup.exe") != 0 ||
        sp_test_touch("C:\\sp_cwd\\dup.exe") != 0) {
        env_free(&s_env_fixture);
        TEST_SKIP("VFS unavailable for SearchPath probe");
        return;
    }
    task_set_cwd(&s_env_fixture, "C:\\sp_cwd");

    /* Safe/unset ordering: the system directory precedes the current directory. */
    rc = env_search_path(&s_env_fixture, (const char *)0, "dup.exe",
                         (const char *)0, out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT(rc > 0 && env_streq(out, "C:\\Impossible\\System32\\dup.exe"),
                "safe mode: System32 precedes CWD");

    /* Unsafe ordering: the current directory wins. */
    TEST_ASSERT_EQ(SetSearchPathMode(&s_env_fixture,
                       BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE), 1,
                   "SetSearchPathMode(DISABLE) succeeds");
    rc = env_search_path(&s_env_fixture, (const char *)0, "dup.exe",
                         (const char *)0, out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT(rc > 0 && env_streq(out, "C:\\sp_cwd\\dup.exe"),
                "unsafe mode: CWD precedes System32");

    vfs_unlink("C:\\Impossible\\System32\\dup.exe");
    vfs_unlink("C:\\sp_cwd\\dup.exe");
    env_free(&s_env_fixture);
}

static void test_env_setsearchpathmode(void)
{
    env_fixture_reset();
    TEST_ASSERT_EQ(SetSearchPathMode(&s_env_fixture,
                       BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE |
                       BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE),
                   0, "enable+disable -> invalid");
    TEST_ASSERT_EQ(SetSearchPathMode(&s_env_fixture, BASE_SEARCH_PATH_PERMANENT),
                   0, "PERMANENT without ENABLE -> invalid");
    TEST_ASSERT_EQ(SetSearchPathMode(&s_env_fixture, 0x4u),
                   0, "unknown bit -> invalid");
    TEST_ASSERT_EQ(SetSearchPathMode(&s_env_fixture, 0u),
                   0, "no mode bits -> invalid");
    TEST_ASSERT_EQ(SetSearchPathMode(&s_env_fixture,
                       BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE),
                   1, "enable -> ok");
    TEST_ASSERT_EQ(SetSearchPathMode(&s_env_fixture,
                       BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE |
                       BASE_SEARCH_PATH_PERMANENT),
                   1, "permanent enable -> ok");
    TEST_ASSERT_EQ(SetSearchPathMode(&s_env_fixture,
                       BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE),
                   0, "change after PERMANENT -> denied");
    env_free(&s_env_fixture);
}

static void test_env_need_current_dir(void)
{
    env_fixture_reset();
    TEST_ASSERT_EQ(env_need_current_dir_for_exe(&s_env_fixture, "dir\\app.exe"),
                   1, "backslash name -> current dir needed");
    TEST_ASSERT_EQ(env_need_current_dir_for_exe(&s_env_fixture, "app.exe"),
                   1, "bare name, var absent -> current dir needed");
    env_set(&s_env_fixture, "NoDefaultCurrentDirectoryInExePath", "");
    TEST_ASSERT_EQ(env_need_current_dir_for_exe(&s_env_fixture, "app.exe"),
                   0, "var present (empty) -> current dir excluded");
    TEST_ASSERT_EQ(env_need_current_dir_for_exe(&s_env_fixture, "dir\\app.exe"),
                   1, "backslash overrides the exclusion var");
    env_free(&s_env_fixture);
}

static void test_env_searchpath_cwd_not_gated_by_exe_var(void)
{
    char out[SP_PATH_MAX];
    uint32_t err = 0, rc;

    env_fixture_reset();
    if (sp_test_mkdir("C:\\sp_cwd2") != 0 ||
        sp_test_touch("C:\\sp_cwd2\\cwdonly.exe") != 0) {
        env_free(&s_env_fixture);
        TEST_SKIP("VFS unavailable for SearchPath probe");
        return;
    }
    task_set_cwd(&s_env_fixture, "C:\\sp_cwd2");
    /* NoDefaultCurrentDirectoryInExePath governs NeedCurrentDirectoryForExePath,
     * NOT SearchPath ordering: SearchPath still consults the CWD with the var set
     * (Win32 keeps the two behaviors distinct; the mode only reorders the CWD). */
    env_set(&s_env_fixture, "NoDefaultCurrentDirectoryInExePath", "");
    rc = env_search_path(&s_env_fixture, (const char *)0, "cwdonly.exe",
                         (const char *)0, out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT(rc > 0 && env_streq(out, "C:\\sp_cwd2\\cwdonly.exe"),
                "SearchPath includes CWD regardless of NoDefaultCurrentDirectoryInExePath");
    /* ...while the standalone helper still reports FALSE for the same variable. */
    TEST_ASSERT_EQ(env_need_current_dir_for_exe(&s_env_fixture, "cwdonly.exe"), 0,
                   "NeedCurrentDirectoryForExePath stays a separate policy (FALSE)");
    vfs_unlink("C:\\sp_cwd2\\cwdonly.exe");
    env_free(&s_env_fixture);
}

static void test_env_searchpath_a_sizing(void)
{
    char buf[64];
    const char *expect = "C:\\Impossible\\sp_a.exe";
    uint32_t elen = 0, rc, i;

    while (expect[elen])
        elen++;                                 /* ASCII: ACP bytes == char count */
    env_fixture_reset();
    if (sp_test_mkdir("C:\\Impossible") != 0 ||
        sp_test_touch("C:\\Impossible\\sp_a.exe") != 0) {
        env_free(&s_env_fixture);
        TEST_SKIP("VFS unavailable for SearchPath probe");
        return;
    }

    /* EXACT fit (capacity == elen + 1): returns length excl NUL, buffer filled. */
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = '#';
    rc = SearchPathA(&s_env_fixture, (const char *)0, "C:\\Impossible\\sp_a.exe",
                     (const char *)0, elen + 1u, buf, (char **)0);
    TEST_ASSERT_EQ(rc, elen, "SearchPathA exact fit returns length excl NUL");
    TEST_ASSERT(env_streq(buf, expect), "SearchPathA exact fit fills the buffer");

    /* ONE SHORT (capacity == elen, one below the elen+1 needed): returns required
     * size INCL NUL and leaves EVERY byte of the buffer untouched. */
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = '#';
    rc = SearchPathA(&s_env_fixture, (const char *)0, "C:\\Impossible\\sp_a.exe",
                     (const char *)0, elen, buf, (char **)0);
    TEST_ASSERT_EQ(rc, elen + 1u, "SearchPathA one-short returns required incl NUL");
    for (i = 0; i < elen; i++)
        if (buf[i] != '#')
            break;
    TEST_ASSERT_EQ(i, elen, "SearchPathA one-short leaves the whole buffer untouched");

    vfs_unlink("C:\\Impossible\\sp_a.exe");
    env_free(&s_env_fixture);
}

static void test_env_searchpath_w_filepart(void)
{
    static const uint16_t wname[] = {
        'C', ':', '\\', 'I', 'm', 'p', 'o', 's', 's', 'i', 'b', 'l', 'e',
        '\\', 's', 'p', '_', 'w', '.', 'e', 'x', 'e', 0
    };
    uint16_t out16[64];
    uint16_t *filepart = (uint16_t *)0;
    uint32_t need, rc, i;

    env_fixture_reset();
    if (sp_test_mkdir("C:\\Impossible") != 0 ||
        sp_test_touch("C:\\Impossible\\sp_w.exe") != 0) {
        env_free(&s_env_fixture);
        TEST_SKIP("VFS unavailable for SearchPath probe");
        return;
    }

    /* Sizing pass (NULL buffer) -> required UTF-16 units INCL NUL. */
    need = SearchPathW(&s_env_fixture, (const uint16_t *)0, wname,
                       (const uint16_t *)0, 0, (uint16_t *)0, (uint16_t **)0);
    TEST_ASSERT(need > 1 && need <= sizeof(out16) / sizeof(out16[0]),
                "SearchPathW sizing returns required units incl NUL");

    /* ONE SHORT (capacity == need - 1): returns required incl NUL, buffer untouched. */
    for (i = 0; i < sizeof(out16) / sizeof(out16[0]); i++)
        out16[i] = 0xAAAA;
    rc = SearchPathW(&s_env_fixture, (const uint16_t *)0, wname,
                     (const uint16_t *)0, need - 1u, out16, (uint16_t **)0);
    TEST_ASSERT_EQ(rc, need, "SearchPathW one-short returns required incl NUL");
    for (i = 0; i < need - 1u; i++)
        if (out16[i] != 0xAAAA)
            break;
    TEST_ASSERT_EQ(i, need - 1u, "SearchPathW one-short leaves the buffer untouched");

    /* EXACT fit (capacity == need): returns length excl NUL and sets lpFilePart. */
    rc = SearchPathW(&s_env_fixture, (const uint16_t *)0, wname,
                     (const uint16_t *)0, need, out16, &filepart);
    TEST_ASSERT_EQ(rc, need - 1u, "SearchPathW exact fit returns length excl NUL");
    TEST_ASSERT(filepart != (uint16_t *)0 &&
                filepart[0] == (uint16_t)'s' && filepart[1] == (uint16_t)'p',
                "lpFilePart points at the file-name component");
    vfs_unlink("C:\\Impossible\\sp_w.exe");
    env_free(&s_env_fixture);
}

static void test_env_searchpath_w_ignored_bad_ext(void)
{
    static const uint16_t wname[] = {
        'C', ':', '\\', 'I', 'm', 'p', 'o', 's', 's', 'i', 'b', 'l', 'e', '\\',
        'h', 'a', 's', 'e', 'x', 't', '.', 'd', 'l', 'l', 0
    };
    static const uint16_t wbadext[] = { 0xD800, 0 };   /* unpaired high surrogate */
    uint16_t out16[64];
    uint32_t rc;

    env_fixture_reset();
    if (sp_test_mkdir("C:\\Impossible") != 0 ||
        sp_test_touch("C:\\Impossible\\hasext.dll") != 0) {
        env_free(&s_env_fixture);
        TEST_SKIP("VFS unavailable for SearchPath probe");
        return;
    }
    /* lpFileName already has an extension, so lpExtension is IGNORED. A malformed
     * (unconvertible) extension must NOT be converted or fail the W wrapper --
     * the file is found regardless. */
    rc = SearchPathW(&s_env_fixture, (const uint16_t *)0, wname, wbadext,
                     sizeof(out16) / sizeof(out16[0]), out16, (uint16_t **)0);
    TEST_ASSERT(rc > 0, "malformed ignored extension does not fail SearchPathW");
    vfs_unlink("C:\\Impossible\\hasext.dll");

    /* A LEADING-dot filename also counts as having an extension: the malformed
     * lpExtension must be ignored (not converted) by the W wrapper. */
    {
        static const uint16_t wdot[] = {
            'C', ':', '\\', 'I', 'm', 'p', 'o', 's', 's', 'i', 'b', 'l', 'e',
            '\\', '.', 'p', 'r', 'o', 'f', 'i', 'l', 'e', 0
        };
        if (sp_test_touch("C:\\Impossible\\.profile") == 0) {
            rc = SearchPathW(&s_env_fixture, (const uint16_t *)0, wdot, wbadext,
                             sizeof(out16) / sizeof(out16[0]), out16,
                             (uint16_t **)0);
            TEST_ASSERT(rc > 0,
                        "leading-dot filename ignores malformed extension in SearchPathW");
            vfs_unlink("C:\\Impossible\\.profile");
        }
    }
    env_free(&s_env_fixture);
}

static void test_env_searchpath_ext_containment(void)
{
    char out[SP_PATH_MAX];
    uint32_t err = 0, rc;

    env_fixture_reset();
    /* A separator-bearing extension is rejected before any probe, so a bare
     * filename cannot append traversal to escape the trusted search dirs. */
    rc = env_search_path(&s_env_fixture, (const char *)0, "probe",
                         ".\\..\\..\\Temp\\hit.exe", out, sizeof(out),
                         (uint32_t *)0, &err);
    TEST_ASSERT_EQ(rc, 0u, "separator-bearing extension rejected");
    TEST_ASSERT_EQ(err, (uint32_t)ERROR_INVALID_PARAMETER,
                   "bad extension -> ERROR_INVALID_PARAMETER");
    /* A drive-colon in the extension is likewise rejected. */
    rc = env_search_path(&s_env_fixture, (const char *)0, "probe", ".e:x",
                         out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT_EQ(rc, 0u, "drive-colon extension rejected");
    TEST_ASSERT_EQ(err, (uint32_t)ERROR_INVALID_PARAMETER,
                   "drive-colon extension -> ERROR_INVALID_PARAMETER");
    /* An extension WITHOUT the required leading period (used because the bare
     * filename has none) is rejected per the documented contract. */
    err = 0;
    rc = env_search_path(&s_env_fixture, (const char *)0, "probe", "exe",
                         out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT_EQ(err, (uint32_t)ERROR_INVALID_PARAMETER,
                   "extension without leading period -> ERROR_INVALID_PARAMETER");
    /* When lpFileName already has an extension, lpExtension is IGNORED -- even a
     * malformed one does not trigger rejection (contract: ignored in that case);
     * the search simply proceeds and misses. */
    err = 0;
    rc = env_search_path(&s_env_fixture, (const char *)0, "haveext.dll", ".\\bad",
                         out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT_EQ(rc, 0u, "filename-with-extension + bad ext -> not rejected, just misses");
    TEST_ASSERT_EQ(err, (uint32_t)ERROR_FILE_NOT_FOUND,
                   "malformed extension ignored when filename already has one");
    /* A LEADING-dot filename (".profile") counts as HAVING an extension per Win32,
     * so lpExtension is ignored -- a malformed one is not rejected/converted. */
    err = 0;
    rc = env_search_path(&s_env_fixture, (const char *)0, ".profile", ".\\bad",
                         out, sizeof(out), (uint32_t *)0, &err);
    TEST_ASSERT_EQ(rc, 0u, "leading-dot filename + bad ext -> not rejected, just misses");
    TEST_ASSERT_EQ(err, (uint32_t)ERROR_FILE_NOT_FOUND,
                   "leading-dot filename treated as having an extension (ext ignored)");
    env_free(&s_env_fixture);
}

static void test_env_searchpath_oom(void)
{
    char out[SP_PATH_MAX];
    uint32_t err = 0, rc, i;

    env_fixture_reset();
    for (i = 0; i < sizeof(out); i++)
        out[i] = '#';
    /* Force the work-buffer allocation to fail: SearchPath must report
     * ERROR_OUTOFMEMORY and leave the output buffer untouched (no fail-open). */
    kmalloc_fail_next();
    rc = env_search_path(&s_env_fixture, (const char *)0, "anything.exe",
                         (const char *)0, out, sizeof(out), (uint32_t *)0, &err);
    kmalloc_fail_countdown_clear();
    TEST_ASSERT_EQ(rc, 0u, "allocation failure returns 0");
    TEST_ASSERT_EQ(err, (uint32_t)ERROR_OUTOFMEMORY,
                   "allocation failure -> ERROR_OUTOFMEMORY");
    TEST_ASSERT(out[0] == '#', "allocation failure leaves output untouched");
    env_free(&s_env_fixture);
}

static void test_env_searchpath_path_oom(void)
{
    char out[SP_PATH_MAX];
    uint32_t err, rc, i, cd;
    int saw_oom = 0;

    env_fixture_reset();
    /* Target exists ONLY in the CWD. In safe mode the CWD leg runs LAST (after
     * PATH), so a fail-open PATH-buffer allocation failure would incorrectly
     * return the CWD hit. The C: drive is a real filesystem whose probe
     * `vfs_stat`s allocate a non-deterministic number of times, so we cannot
     * target the PATH buffer by a fixed kmalloc index; instead we SWEEP the
     * failure point and assert the fail-closed invariant. env_search_path only
     * returns ERROR_OUTOFMEMORY from the PATH-leg -1 (the work buffer is
     * kmalloc #1, never hit at cd>=2), so any OUTOFMEMORY here proves the
     * PATH-leg failed closed rather than falling through to CWD. */
    if (sp_test_mkdir("C:\\sp_oom_cwd") != 0 ||
        sp_test_touch("C:\\sp_oom_cwd\\oomtgt.exe") != 0) {
        env_free(&s_env_fixture);
        TEST_SKIP("VFS unavailable for SearchPath probe");
        return;
    }
    task_set_cwd(&s_env_fixture, "C:\\sp_oom_cwd");
    env_set(&s_env_fixture, "PATH", "C:\\sp_oom_path");   /* real dir, misses */

    for (cd = 2; cd <= 20; cd++) {
        for (i = 0; i < sizeof(out); i++)
            out[i] = '#';
        kmalloc_fail_countdown_set(cd);
        err = 0;
        rc = env_search_path(&s_env_fixture, (const char *)0, "oomtgt.exe",
                             (const char *)0, out, sizeof(out), (uint32_t *)0, &err);
        kmalloc_fail_countdown_clear();
        if (rc == 0) {
            /* Either the PATH buffer failed (-> OUTOFMEMORY, fail closed) or a
             * probe/CWD `vfs_stat` alloc failed (that leg misses -> FILE_NOT_FOUND).
             * Never a wrong success, and the output is untouched either way. */
            TEST_ASSERT(err == (uint32_t)ERROR_OUTOFMEMORY ||
                        err == (uint32_t)ERROR_FILE_NOT_FOUND,
                        "PATH-leg alloc failure fails closed (OUTOFMEMORY/NOT_FOUND)");
            TEST_ASSERT(out[0] == '#', "alloc failure leaves output untouched");
            if (err == (uint32_t)ERROR_OUTOFMEMORY)
                saw_oom = 1;
        } else {
            /* A non-critical (probe) alloc failed; the search still resolves the
             * only real copy of the file, in CWD -- never a stale/garbage path. */
            TEST_ASSERT(env_streq(out, "C:\\sp_oom_cwd\\oomtgt.exe"),
                        "probe-alloc failure still returns the correct CWD hit");
        }
    }
    /* A fail-OPEN regression (PATH -1 treated as not-found) would fall through
     * to CWD for every countdown, so no sweep point would ever yield OUTOFMEMORY. */
    TEST_ASSERT(saw_oom, "PATH-leg OOM reachable and fails closed (no CWD fallthrough)");

    vfs_unlink("C:\\sp_oom_cwd\\oomtgt.exe");
    env_free(&s_env_fixture);
}

/* ---- s15 CommandLineToArgvW command-line decode --------------------------- */

static uint32_t cmdl_w_len(const uint16_t *w)
{
    uint32_t n = 0;
    while (w[n])
        n++;
    return n;
}

/* Compare a UTF-16 argument to an ASCII expectation, unit by unit. */
static int cmdl_w_eq_ascii(const uint16_t *w, const char *a)
{
    uint32_t i = 0;
    while (a[i]) {
        if (w[i] != (uint16_t)(unsigned char)a[i])
            return 0;
        i++;
    }
    return w[i] == 0;
}

static void test_env_cmdline_basic_quotes(void)
{
    int argc = -1;
    char **argv = cmdline_to_argv((struct task *)0, "foo \"bar baz\" qux", &argc);
    TEST_ASSERT(argv != (char **)0, "cmdline_to_argv returns a block");
    TEST_ASSERT_EQ(argc, 3, "three arguments parsed");
    TEST_ASSERT(env_streq(argv[0], "foo"), "argv[0] == foo");
    TEST_ASSERT(env_streq(argv[1], "bar baz"), "quoted argv[1] keeps its space");
    TEST_ASSERT(env_streq(argv[2], "qux"), "argv[2] == qux");
    TEST_ASSERT(argv[3] == (char *)0, "vector is NULL-terminated");
    cmdline_free_argv(argv);
}

static void test_env_cmdline_backslash_quote(void)
{
    /* argv[1] source bytes: a \ \ \ " b -- 3 backslashes + quote -> 1 backslash
     * + a literal quote (the 2n+1 rule), yielding a \ " b. */
    int argc = -1;
    char **argv = cmdline_to_argv((struct task *)0, "prog a\\\\\\\"b", &argc);
    TEST_ASSERT(argv != (char **)0, "cmdline_to_argv returns a block");
    TEST_ASSERT_EQ(argc, 2, "two arguments parsed");
    TEST_ASSERT(env_streq(argv[0], "prog"), "argv[0] == prog");
    TEST_ASSERT(env_streq(argv[1], "a\\\"b"), "2n+1 backslash rule yields a backslash-quote-b");
    cmdline_free_argv(argv);
}

static void test_env_cmdline_roundtrip(void)
{
    /* Encode with s4's argv_to_cmdline, decode with s15, expect the same args. */
    static const char *const in[] = { "a b", "c\"d", "" };
    char cmd[128];
    int argc = -1;
    char **argv;
    uint32_t n = argv_to_cmdline(3, in, cmd, sizeof(cmd));
    TEST_ASSERT(n > 0 && n < sizeof(cmd), "encode fits the buffer");
    argv = cmdline_to_argv((struct task *)0, cmd, &argc);
    TEST_ASSERT(argv != (char **)0, "decode returns a block");
    TEST_ASSERT_EQ(argc, 3, "round-trip preserves the argument count");
    TEST_ASSERT(env_streq(argv[0], "a b"), "round-trip argv[0]");
    TEST_ASSERT(env_streq(argv[1], "c\"d"), "round-trip argv[1] (embedded quote)");
    TEST_ASSERT(env_streq(argv[2], ""), "round-trip argv[2] (empty argument)");
    cmdline_free_argv(argv);
}

static void test_env_cmdline_empty_module_path(void)
{
    int argc = -1;
    int n = -1;
    char **argv;
    uint16_t **wargv;
    static const uint16_t empty_w[1] = { 0 };

    env_fixture_reset();
    s_env_fixture.name = "C:\\Test\\prog.exe";

    argv = cmdline_to_argv(&s_env_fixture, "", &argc);
    TEST_ASSERT(argv != (char **)0, "empty cmdline still returns a block");
    TEST_ASSERT_EQ(argc, 1, "empty cmdline -> one argument");
    TEST_ASSERT(env_streq(argv[0], "C:\\Test\\prog.exe"),
                "argv[0] is the module path, not empty");
    cmdline_free_argv(argv);

    wargv = CommandLineToArgvW(&s_env_fixture, empty_w, &n);
    TEST_ASSERT(wargv != (uint16_t **)0, "wide empty cmdline returns a block");
    TEST_ASSERT_EQ(n, 1, "wide empty cmdline -> one argument");
    TEST_ASSERT(cmdl_w_eq_ascii(wargv[0], "C:\\Test\\prog.exe"),
                "wide argv[0] is the widened module path");
    cmdline_free_argv(wargv);

    env_free(&s_env_fixture);
}

static void test_env_cmdline_null(void)
{
    int argc = 5;
    int n = 5;
    TEST_ASSERT(cmdline_to_argv((struct task *)0, (const char *)0, &argc) == (char **)0,
                "NULL cmdline -> NULL");
    TEST_ASSERT_EQ(argc, 0, "NULL cmdline zeroes the count");
    TEST_ASSERT(CommandLineToArgvW((struct task *)0, (const uint16_t *)0, &n)
                == (uint16_t **)0, "NULL wide cmdline -> NULL");
    TEST_ASSERT_EQ(n, 0, "NULL wide cmdline zeroes the count");
    /* Windows requires pNumArgs: a valid command line with a NULL count returns
     * NULL (not an allocated, un-sizable vector). */
    {
        static const uint16_t cmd[] = { 'a', ' ', 'b', 0 };
        TEST_ASSERT(CommandLineToArgvW((struct task *)0, cmd, (int *)0)
                    == (uint16_t **)0, "NULL pNumArgs with a valid cmdline -> NULL");
    }
}

static void test_env_cmdline_wide_verbatim(void)
{
    /* A lone high surrogate must survive verbatim: the W path parses UTF-16 code
     * units directly, so it is never rewritten to U+FFFD by a transcode. */
    static const uint16_t cmd[] = { 'a', 0xD800u, 'b', 0 };
    int n = -1;
    uint16_t **wargv = CommandLineToArgvW((struct task *)0, cmd, &n);
    TEST_ASSERT(wargv != (uint16_t **)0, "wide parse returns a block");
    TEST_ASSERT_EQ(n, 1, "single argument");
    TEST_ASSERT_EQ(cmdl_w_len(wargv[0]), 3u, "argv[0] keeps all three code units");
    TEST_ASSERT(wargv[0][0] == 'a' && wargv[0][1] == 0xD800u && wargv[0][2] == 'b',
                "lone surrogate preserved verbatim (no transcode mutation)");
    cmdline_free_argv(wargv);
}

static void test_env_cmdline_wide_quotes(void)
{
    static const uint16_t cmd[] = { '"','x','"',' ','"','y',' ','z','"', 0 };
    int n = -1;
    uint16_t **wargv = CommandLineToArgvW((struct task *)0, cmd, &n);
    TEST_ASSERT(wargv != (uint16_t **)0, "wide quoted parse returns a block");
    TEST_ASSERT_EQ(n, 2, "two arguments");
    TEST_ASSERT(cmdl_w_eq_ascii(wargv[0], "x"), "wide argv[0] == x");
    TEST_ASSERT(cmdl_w_eq_ascii(wargv[1], "y z"), "wide quoted argv[1] keeps its space");
    cmdline_free_argv(wargv);
}

static void test_env_cmdline_quote_runs(void)
{
    /* Consecutive-quote runs per the documented algorithm: a bare "" is an empty
     * quoted argument; a "" while already inside quotes is one literal '"'. */
    int argc = -1;
    char **argv;

    argv = cmdline_to_argv((struct task *)0, "prog \"\"", &argc);
    TEST_ASSERT(argv != (char **)0, "empty quoted arg decodes");
    TEST_ASSERT_EQ(argc, 2, "prog + empty quoted arg");
    TEST_ASSERT(env_streq(argv[1], ""), "\"\" is an empty argument");
    cmdline_free_argv(argv);

    argv = cmdline_to_argv((struct task *)0, "prog \"\"\"\"", &argc);
    TEST_ASSERT(argv != (char **)0, "four-quote run decodes");
    TEST_ASSERT_EQ(argc, 2, "four quotes -> one argument");
    TEST_ASSERT(env_streq(argv[1], "\""), "open + \"\" literal + close -> one quote");
    cmdline_free_argv(argv);

    argv = cmdline_to_argv((struct task *)0, "prog \"a\"\"b\"", &argc);
    TEST_ASSERT(argv != (char **)0, "in-quotes literal decodes");
    TEST_ASSERT_EQ(argc, 2, "one argument");
    TEST_ASSERT(env_streq(argv[1], "a\"b"), "\"\" inside quotes is a literal quote");
    cmdline_free_argv(argv);

    /* Mid-argument quote run: `exe "two"" next` -> exe, two", next (the ""
     * closes the quoted region, so the following space delimits -- Windows
     * modulo-3 conformance, not the "stay in quotes" merge). */
    argv = cmdline_to_argv((struct task *)0, "exe \"two\"\" next", &argc);
    TEST_ASSERT(argv != (char **)0, "mid-argument quote run decodes");
    TEST_ASSERT_EQ(argc, 3, "\"\" closes quotes -> three arguments, not two");
    TEST_ASSERT(env_streq(argv[0], "exe"), "argv[0] == exe");
    TEST_ASSERT(env_streq(argv[1], "two\""), "argv[1] == two + literal quote");
    TEST_ASSERT(env_streq(argv[2], "next"), "argv[2] == next (not merged)");
    cmdline_free_argv(argv);

    /* argv[0] special parse: an argv[0] not opening with '"' runs to the first
     * whitespace with embedded quotes taken LITERALLY (Windows program-name
     * rule), so it is NOT a general inverse of argv_to_cmdline -- documents the
     * accepted asymmetry (the program-name encoder follow-up owns closing it). */
    argv = cmdline_to_argv((struct task *)0, "exe\"x\" foo", &argc);
    TEST_ASSERT(argv != (char **)0, "embedded-quote argv[0] decodes");
    TEST_ASSERT_EQ(argc, 2, "argv[0] runs to whitespace, then foo");
    TEST_ASSERT(env_streq(argv[0], "exe\"x\""), "argv[0] keeps embedded quotes literal");
    TEST_ASSERT(env_streq(argv[1], "foo"), "argv[1] == foo");
    cmdline_free_argv(argv);
}

static void test_env_cmdline_large_alloc_crossover(void)
{
    /* A decoded block over ENV_STR_KMALLOC_MAX (4 KiB) rides the PMM allocator;
     * decode + free must stay heap/PMM-neutral. One ~5000-char argument. */
    static char big[5001];
    uint32_t i;
    int argc = -1;
    char **argv;
    for (i = 0; i < 5000u; i++)
        big[i] = 'x';
    big[5000] = '\0';
    argv = cmdline_to_argv((struct task *)0, big, &argc);
    TEST_ASSERT(argv != (char **)0, "large cmdline decodes");
    TEST_ASSERT_EQ(argc, 1, "one big argument");
    TEST_ASSERT_EQ(env_test_strlen(argv[0]), 5000u, "argument length preserved");
    cmdline_free_argv(argv);
}

/* ==== s16: environment security & sanitization (AT_SECURE parallel) ========
 * env_name_is_privilege_sensitive / env_is_secure_context / the read+build
 * gates / env_sanitize_for_elevation. Tokens are built on the stack: only the
 * IntegrityLevelSid is read (via SeGetTokenIntegrityLevel), so a zero-init body
 * plus one SID pointer is sufficient. */

/* Case-insensitive substring scan over a raw environment block (bytes may hold
 * embedded NULs between entries; needle is NUL-terminated). */
static int env_test_block_has(const char *blk, uint32_t len, const char *needle)
{
    uint32_t nlen = env_test_strlen(needle);
    uint32_t i;
    if (nlen == 0u || len < nlen)
        return 0;
    for (i = 0; i + nlen <= len; i++) {
        uint32_t j = 0;
        while (j < nlen && blk[i + j] == needle[j])
            j++;
        if (j == nlen)
            return 1;
    }
    return 0;
}

static void test_env_privilege_sensitive_names(void)
{
    /* Exact matches, case-insensitive. */
    TEST_ASSERT(env_name_is_privilege_sensitive("LD_PRELOAD", 10u),
                "LD_PRELOAD is blocklisted");
    TEST_ASSERT(env_name_is_privilege_sensitive("ld_preload", 10u),
                "lowercase ld_preload is blocklisted (store is case-insensitive)");
    TEST_ASSERT(env_name_is_privilege_sensitive("LD_LIBRARY_PATH", 15u),
                "LD_LIBRARY_PATH is blocklisted");
    /* Prefix match for the debug family, case-insensitive. */
    TEST_ASSERT(env_name_is_privilege_sensitive("_IMPOSSIBLE_DEBUG_", 18u),
                "_IMPOSSIBLE_DEBUG_ (exact prefix) is blocklisted");
    TEST_ASSERT(env_name_is_privilege_sensitive("_IMPOSSIBLE_DEBUG_TRACE", 23u),
                "_IMPOSSIBLE_DEBUG_TRACE is blocklisted (prefix)");
    TEST_ASSERT(env_name_is_privilege_sensitive("_impossible_debug_trace", 23u),
                "lowercase debug prefix is blocklisted");
    /* Negatives: near-misses must NOT be blocklisted. */
    TEST_ASSERT(!env_name_is_privilege_sensitive("PATH", 4u),
                "PATH is not blocklisted");
    TEST_ASSERT(!env_name_is_privilege_sensitive("LD_PRELOADX", 11u),
                "LD_PRELOADX (superstring) is not blocklisted");
    TEST_ASSERT(!env_name_is_privilege_sensitive("_IMPOSSIBLE_DEBUG", 17u),
                "_IMPOSSIBLE_DEBUG (one short of prefix) is not blocklisted");
    TEST_ASSERT(!env_name_is_privilege_sensitive((const char *)0, 0u),
                "NULL name is not blocklisted");
}

static void test_env_secure_context(void)
{
    ACCESS_TOKEN tok = {0};

    env_fixture_reset();
    /* No token assigned -> fail-closed secure. */
    TEST_ASSERT(env_is_secure_context(&s_env_fixture),
                "NULL token is a secure context (fail closed)");

    s_env_fixture.token = &tok;
    tok.IntegrityLevelSid = (SID *)SeILLow;
    TEST_ASSERT(!env_is_secure_context(&s_env_fixture),
                "Low integrity is not secure");
    tok.IntegrityLevelSid = (SID *)SeILMedium;
    TEST_ASSERT(!env_is_secure_context(&s_env_fixture),
                "Medium integrity is not secure");
    tok.IntegrityLevelSid = (SID *)SeILHigh;
    TEST_ASSERT(env_is_secure_context(&s_env_fixture),
                "High integrity is a secure context");
    tok.IntegrityLevelSid = (SID *)SeILSystem;
    TEST_ASSERT(env_is_secure_context(&s_env_fixture),
                "System integrity is a secure context");

    /* Malformed token (NULL IL SID) must FAIL CLOSED -> secure, so a corrupt
     * token cannot masquerade as benign Medium and read a blocklisted var. */
    tok.IntegrityLevelSid = (SID *)0;
    TEST_ASSERT(env_is_secure_context(&s_env_fixture),
                "malformed token (NULL IL SID) fails closed to secure");
    env_set(&s_env_fixture, "LD_PRELOAD", "x.so");
    {
        char buf[32];
        TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "LD_PRELOAD", buf, sizeof(buf)),
                       ENV_ERR_NOTFOUND,
                       "malformed-token task cannot read a blocklisted var");
    }

    s_env_fixture.token = NULL;   /* do not leak the stack token past this test */
    env_free(&s_env_fixture);     /* free the LD_PRELOAD entry before leak check */
}

static void test_env_secure_read_gate(void)
{
    ACCESS_TOKEN tok = {0};
    char buf[64];
    const char *peek;

    env_fixture_reset();
    /* env_set is NOT gated; the values exist in the store regardless. */
    env_set(&s_env_fixture, "LD_PRELOAD", "evil.so");
    env_set(&s_env_fixture, "PATH", "C:\\Bin");
    s_env_fixture.token = &tok;

    /* Medium (non-secure): the Linux compat layer legitimately reads LD_PRELOAD. */
    tok.IntegrityLevelSid = (SID *)SeILMedium;
    TEST_ASSERT(env_get_copy(&s_env_fixture, "LD_PRELOAD", buf, sizeof(buf)) > 0
                && env_streq(buf, "evil.so"),
                "Medium process reads LD_PRELOAD");
    env_lock(&s_env_fixture);
    peek = env_peek_locked(&s_env_fixture, "LD_PRELOAD");
    TEST_ASSERT(peek != NULL && env_streq(peek, "evil.so"),
                "Medium env_peek_locked returns LD_PRELOAD");
    env_unlock(&s_env_fixture);

    /* High (secure): blocklisted name reads as absent via both getters. */
    tok.IntegrityLevelSid = (SID *)SeILHigh;
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "LD_PRELOAD", buf, sizeof(buf)),
                   ENV_ERR_NOTFOUND,
                   "High process reads LD_PRELOAD as absent");
    env_lock(&s_env_fixture);
    peek = env_peek_locked(&s_env_fixture, "LD_PRELOAD");
    TEST_ASSERT(peek == NULL, "High env_peek_locked hides LD_PRELOAD");
    env_unlock(&s_env_fixture);
    /* A non-blocklisted name stays visible to the elevated process. */
    TEST_ASSERT(env_get_copy(&s_env_fixture, "PATH", buf, sizeof(buf)) > 0
                && env_streq(buf, "C:\\Bin"),
                "High process still reads PATH");

    s_env_fixture.token = NULL;
    env_free(&s_env_fixture);   /* heap-neutral: free before per-test leak check */
}

static void test_env_sanitize_for_elevation(void)
{
    ACCESS_TOKEN tok = {0};
    char buf[64];
    int removed;

    env_fixture_reset();
    env_set(&s_env_fixture, "LD_PRELOAD", "a.so");
    env_set(&s_env_fixture, "LD_LIBRARY_PATH", "/lib");
    env_set(&s_env_fixture, "_IMPOSSIBLE_DEBUG_A", "1");
    env_set(&s_env_fixture, "_IMPOSSIBLE_DEBUG_B", "1");
    env_set(&s_env_fixture, "PATH", "C:\\Bin");
    env_set(&s_env_fixture, "GREETING", "hi");

    removed = env_sanitize_for_elevation(&s_env_fixture);
    TEST_ASSERT_EQ(removed, 4, "sanitize removes all four blocklisted vars");

    /* Verify PHYSICAL removal with a Medium token (read gate would otherwise mask
     * a still-present var). */
    s_env_fixture.token = &tok;
    tok.IntegrityLevelSid = (SID *)SeILMedium;
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "LD_PRELOAD", buf, sizeof(buf)),
                   ENV_ERR_NOTFOUND, "LD_PRELOAD physically stripped");
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "_IMPOSSIBLE_DEBUG_A", buf, sizeof(buf)),
                   ENV_ERR_NOTFOUND, "adjacent debug var A stripped");
    TEST_ASSERT_EQ(env_get_copy(&s_env_fixture, "_IMPOSSIBLE_DEBUG_B", buf, sizeof(buf)),
                   ENV_ERR_NOTFOUND, "adjacent debug var B stripped");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "PATH", buf, sizeof(buf)) > 0
                && env_streq(buf, "C:\\Bin"), "PATH survives sanitize");
    TEST_ASSERT(env_get_copy(&s_env_fixture, "GREETING", buf, sizeof(buf)) > 0
                && env_streq(buf, "hi"), "GREETING survives sanitize");
    s_env_fixture.token = NULL;

    /* Idempotent: a clean environment removes nothing. */
    removed = env_sanitize_for_elevation(&s_env_fixture);
    TEST_ASSERT_EQ(removed, 0, "sanitize is idempotent on a clean environment");

    env_free(&s_env_fixture);   /* heap-neutral: free before per-test leak check */
}

static void test_env_secure_block_excludes(void)
{
    ACCESS_TOKEN tok = {0};
    static char blk[4096];
    uint32_t len = 0;

    env_fixture_reset();
    env_set(&s_env_fixture, "LD_PRELOAD", "x.so");
    env_set(&s_env_fixture, "PATH", "C:\\Bin");
    s_env_fixture.token = &tok;

    /* Medium (non-secure): serialized block still carries LD_PRELOAD. */
    tok.IntegrityLevelSid = (SID *)SeILMedium;
    TEST_ASSERT_EQ(env_build_block(&s_env_fixture, blk, sizeof(blk), 0, &len),
                   ENV_OK, "ANSI block builds (Medium)");
    TEST_ASSERT(env_test_block_has(blk, len, "LD_PRELOAD"),
                "Medium block includes LD_PRELOAD");

    /* High (secure): block omits LD_PRELOAD but keeps PATH. */
    tok.IntegrityLevelSid = (SID *)SeILHigh;
    TEST_ASSERT_EQ(env_build_block(&s_env_fixture, blk, sizeof(blk), 0, &len),
                   ENV_OK, "ANSI block builds (High)");
    TEST_ASSERT(!env_test_block_has(blk, len, "LD_PRELOAD"),
                "High block excludes LD_PRELOAD");
    TEST_ASSERT(env_test_block_has(blk, len, "PATH"),
                "High block still includes PATH");

    s_env_fixture.token = NULL;
    env_free(&s_env_fixture);   /* heap-neutral: free before per-test leak check */
}

static void test_env_copy_secure_source_excludes(void)
{
    static struct task dst;   /* static: struct task is large; keep it off the stack */
    ACCESS_TOKEN tok = {0};
    char buf[64];

    env_fixture_reset();
    env_set(&s_env_fixture, "LD_PRELOAD", "x.so");
    env_set(&s_env_fixture, "PATH", "C:\\Bin");

    /* Secure SOURCE (High): env_copy must NOT export the blocklisted name. */
    dst.environ = NULL; dst.environ_count = 0; dst.environ_bytes = 0; dst.token = NULL;
    mutex_init(&dst.environ_lock, "test-env-dst");
    s_env_fixture.token = &tok;
    tok.IntegrityLevelSid = (SID *)SeILHigh;
    TEST_ASSERT_EQ(env_copy(&dst, &s_env_fixture), ENV_OK, "env_copy from secure source");
    s_env_fixture.token = NULL;
    /* Read the child under a Medium token to prove PHYSICAL exclusion (a NULL/High
     * dst token would merely mask the name via the read gate). */
    dst.token = &tok;
    tok.IntegrityLevelSid = (SID *)SeILMedium;
    TEST_ASSERT_EQ(env_get_copy(&dst, "LD_PRELOAD", buf, sizeof(buf)), ENV_ERR_NOTFOUND,
                   "child did not inherit LD_PRELOAD from a secure parent");
    TEST_ASSERT(env_get_copy(&dst, "PATH", buf, sizeof(buf)) > 0 && env_streq(buf, "C:\\Bin"),
                "child inherited PATH from a secure parent");
    dst.token = NULL;
    env_free(&dst);

    /* Non-secure SOURCE (Medium): full inheritance incl LD_PRELOAD (compat layer). */
    dst.environ = NULL; dst.environ_count = 0; dst.environ_bytes = 0; dst.token = NULL;
    mutex_init(&dst.environ_lock, "test-env-dst2");
    s_env_fixture.token = &tok;
    tok.IntegrityLevelSid = (SID *)SeILMedium;
    TEST_ASSERT_EQ(env_copy(&dst, &s_env_fixture), ENV_OK, "env_copy from Medium source");
    s_env_fixture.token = NULL;
    dst.token = &tok;   /* still Medium */
    TEST_ASSERT(env_get_copy(&dst, "LD_PRELOAD", buf, sizeof(buf)) > 0 && env_streq(buf, "x.so"),
                "child inherited LD_PRELOAD from a Medium parent");
    dst.token = NULL;
    env_free(&dst);

    env_free(&s_env_fixture);   /* heap-neutral: free before per-test leak check */
}

void test_register_env(void)
{
    test_suite_register_cat("Env: drive-cwd =X: round-trip",
                            test_env_drive_cwd_roundtrip, TEST_CAT_ABI);
    test_suite_register_cat("Env: drive-cwd unset -> X:\\ root",
                            test_env_drive_cwd_default_root, TEST_CAT_ABI);
    test_suite_register_cat("Env: drive-cwd case-fold + invalid drive",
                            test_env_drive_cwd_case_and_invalid, TEST_CAT_ABI);
    test_suite_register_cat("Env: =X: name validation (only =X: legal)",
                            test_env_drive_cwd_name_validation, TEST_CAT_ABI);
    test_suite_register_cat("Env: hidden =X: sorts first in block",
                            test_env_drive_cwd_sorts_first, TEST_CAT_ABI);
    test_suite_register_cat("Env: =X: preserved through parse_block",
                            test_env_drive_cwd_parse_block_preserved, TEST_CAT_ABI);
    test_suite_register_cat("Env: drive-cwd adapter boundaries",
                            test_env_drive_cwd_adapter_boundaries, TEST_CAT_ABI);
    test_suite_register_cat("Env: drive-relative resolution matrix",
                            test_env_drive_cwd_resolve_matrix, TEST_CAT_ABI);
    test_suite_register_cat("Env: argv set + free",
                            test_argv_set_and_free, TEST_CAT_ABI);
    test_suite_register_cat("Env: argv replace + clear",
                            test_argv_set_replace_and_clear, TEST_CAT_ABI);
    test_suite_register_cat("Env: argv_to_cmdline quoting",
                            test_argv_to_cmdline_quoting, TEST_CAT_ABI);
    test_suite_register_cat("Env: argv frame bytes + stack cap",
                            test_argv_frame_bytes_and_cap, TEST_CAT_ABI);
    test_suite_register_cat("Env: adopt envp block",
                            test_env_adopt_block, TEST_CAT_ABI);
    test_suite_register_cat("Env: set/get roundtrip",
                            test_env_set_get_roundtrip, TEST_CAT_ABI);
    test_suite_register_cat("Env: replace in place",
                            test_env_set_replace, TEST_CAT_ABI);
    test_suite_register_cat("Env: unset",
                            test_env_unset, TEST_CAT_ABI);
    test_suite_register_cat("Env: absent variable",
                            test_env_get_absent, TEST_CAT_ABI);
    test_suite_register_cat("Env: case-insensitive names",
                            test_env_case_insensitive, TEST_CAT_ABI);
    test_suite_register_cat("Env: invalid name rejected",
                            test_env_invalid_name, TEST_CAT_ABI);
    test_suite_register_cat("Env: value too long rejected",
                            test_env_value_too_long, TEST_CAT_ABI);
    test_suite_register_cat("Env: name length boundary (256/257)",
                            test_env_name_length_boundary, TEST_CAT_ABI);
    test_suite_register_cat("Env: truncating get returns full length",
                            test_env_get_truncation, TEST_CAT_ABI);
    test_suite_register_cat("Env: deep copy is independent",
                            test_env_copy_independent, TEST_CAT_ABI);
    test_suite_register_cat("Env: copy of empty source",
                            test_env_copy_empty, TEST_CAT_ABI);
    test_suite_register_cat("Env: borrowed peek under lock",
                            test_env_peek_locked, TEST_CAT_ABI);
    test_suite_register_cat("Env: free clears state",
                            test_env_free_clears, TEST_CAT_ABI);
    test_suite_register_cat("Env: large value PMM path",
                            test_env_large_value_pmm_path, TEST_CAT_ABI);
    test_suite_register_cat("Env: OOM replace preserves old value",
                            test_env_set_oom_preserves_old, TEST_CAT_ABI);
    test_suite_register_cat("Env: init_defaults synth base",
                            test_env_defaults_synth_base, TEST_CAT_ABI);
    test_suite_register_cat("Env: init_defaults derived vars",
                            test_env_defaults_derived, TEST_CAT_ABI);
    test_suite_register_cat("Env: init_defaults registry overlay",
                            test_env_defaults_registry_overlay, TEST_CAT_ABI);
    test_suite_register_cat("Env: init_defaults user PATH append",
                            test_env_defaults_user_path_append, TEST_CAT_ABI);
    test_suite_register_cat("Env: init_defaults user override precedence",
                            test_env_defaults_user_override, TEST_CAT_ABI);
    test_suite_register_cat("Env: init_defaults skips non-string values",
                            test_env_defaults_skips_non_string, TEST_CAT_ABI);
    test_suite_register_cat("Env: init_defaults unterminated REG_SZ",
                            test_env_defaults_unterminated_value, TEST_CAT_ABI);
    test_suite_register_cat("Env: expand %VAR% substitution",
                            test_env_expand_basic, TEST_CAT_ABI);
    test_suite_register_cat("Env: expand %% preserved (Win32, not cmd escape)",
                            test_env_expand_double_percent, TEST_CAT_ABI);
    test_suite_register_cat("Env: expand unknown var literal",
                            test_env_expand_unknown_literal, TEST_CAT_ABI);
    test_suite_register_cat("Env: expand unmatched percent",
                            test_env_expand_unmatched_percent, TEST_CAT_ABI);
    test_suite_register_cat("Env: expand single-pass (no recursion)",
                            test_env_expand_single_pass, TEST_CAT_ABI);
    test_suite_register_cat("Env: expand truncation sentinel",
                            test_env_expand_truncation, TEST_CAT_ABI);
    test_suite_register_cat("Env: build UTF-16 block",
                            test_env_build_block_utf16, TEST_CAT_ABI);
    test_suite_register_cat("Env: RtlExpandEnvironmentStrings_U basic",
                            test_rtl_expand_basic, TEST_CAT_ABI);
    test_suite_register_cat("Env: RtlExpandEnvironmentStrings_U buffer too small",
                            test_rtl_expand_buffer_too_small, TEST_CAT_ABI);
    test_suite_register_cat("Env: RtlExpandEnvironmentStrings_U escape+case",
                            test_rtl_expand_escape_and_case, TEST_CAT_ABI);
    test_suite_register_cat("Env: RtlExpandEnvironmentStrings_U invalid args",
                            test_rtl_expand_invalid_args, TEST_CAT_ABI);
    test_suite_register_cat("Env: RtlExpandEnvironmentStrings_U empty source",
                            test_rtl_expand_empty_source, TEST_CAT_ABI);
    test_suite_register_cat("Env: RtlExpandEnvironmentStrings_U multi-entry",
                            test_rtl_expand_multi_entry, TEST_CAT_ABI);
    test_suite_register_cat("Env: build UTF-16 block over cap",
                            test_env_build_block_over_cap, TEST_CAT_ABI);
    test_suite_register_cat("Env: RtlExpandEnvironmentStrings_U NULL dest buffer",
                            test_rtl_expand_null_dest_buffer, TEST_CAT_ABI);
    test_suite_register_cat("Env: RtlExpandEnvironmentStrings_U overlap rejected",
                            test_rtl_expand_overlap_rejected, TEST_CAT_ABI);
    test_suite_register_cat("Env: build UTF-16 empty block double-NUL",
                            test_env_build_block_empty, TEST_CAT_ABI);
    test_suite_register_cat("Env: build UTF-16 block UTF-8 conversion",
                            test_env_build_block_utf8, TEST_CAT_ABI);
    test_suite_register_cat("Env: expand input/output overlap rejected",
                            test_env_expand_overlap_rejected, TEST_CAT_ABI);
    test_suite_register_cat("Env: Rtl expand block-terminator overlap",
                            test_rtl_expand_overlap_block_terminator, TEST_CAT_ABI);
    test_suite_register_cat("Env: Rtl expand empty-block overlap",
                            test_rtl_expand_overlap_empty_block, TEST_CAT_ABI);
    test_suite_register_cat("Env: Rtl expand name over ENV_NAME_MAX literal",
                            test_rtl_expand_name_over_limit, TEST_CAT_ABI);
    test_suite_register_cat("Env: Rtl expand self-referential dest",
                            test_rtl_expand_self_referential_dest, TEST_CAT_ABI);
    test_suite_register_cat("Env: Rtl expand ReturnedLength alias",
                            test_rtl_expand_returnedlength_alias, TEST_CAT_ABI);
    test_suite_register_cat("Env: Rtl expand ReturnedLength aliases source",
                            test_rtl_expand_returnedlength_aliases_source, TEST_CAT_ABI);
    test_suite_register_cat("Env: Rtl expand empty-block second NUL",
                            test_rtl_expand_empty_block_second_nul, TEST_CAT_ABI);
    test_suite_register_cat("Env: expand overlap preserves source",
                            test_env_expand_overlap_preserves_source, TEST_CAT_ABI);
    /* s5: Nt/Zw environment-variable syscalls (via ssdt_dispatch). The prewarm
     * suite runs first to absorb the live-task environ[] high-water-mark grow. */
    test_suite_register_cat("Env: live-task environ[] high-water-mark prewarm",
                            test_ntenv_live_env_hwm, TEST_CAT_ABI);
    test_suite_register_cat("Env: NtSet/NtQuery roundtrip",
                            test_ntenv_set_query_roundtrip, TEST_CAT_ABI);
    test_suite_register_cat("Env: NtQuery variable not found",
                            test_ntenv_query_not_found, TEST_CAT_ABI);
    test_suite_register_cat("Env: NtSet(NULL) deletes",
                            test_ntenv_delete, TEST_CAT_ABI);
    test_suite_register_cat("Env: NtQuery buffer too small",
                            test_ntenv_buffer_too_small, TEST_CAT_ABI);
    test_suite_register_cat("Env: NtQuery exact-fit buffer",
                            test_ntenv_exact_fit, TEST_CAT_ABI);
    test_suite_register_cat("Env: Nt embedded-NUL / empty name rejected",
                            test_ntenv_embedded_nul_rejected, TEST_CAT_ABI);
    test_suite_register_cat("Env: Nt overlap + NULL param rejected",
                            test_ntenv_overlap_and_null, TEST_CAT_ABI);
    test_suite_register_cat("Env: Nt empty value + NULL buffer rejected",
                            test_ntenv_empty_value_null_buffer, TEST_CAT_ABI);
    /* s10: sorted block, caller-buffer build/parse, size caps. */
    test_suite_register_cat("Env: sorted ANSI block (insertion order ignored)",
                            test_env_sorted_block_ansi, TEST_CAT_ABI);
    test_suite_register_cat("Env: sorted block case-insensitive",
                            test_env_sorted_block_case_insensitive, TEST_CAT_ABI);
    test_suite_register_cat("Env: build block too small reports length",
                            test_env_build_block_too_small, TEST_CAT_ABI);
    test_suite_register_cat("Env: build UNICODE caller-buffer block",
                            test_env_build_block_unicode, TEST_CAT_ABI);
    test_suite_register_cat("Env: build empty block double-NUL",
                            test_env_build_block_empty_caller, TEST_CAT_ABI);
    test_suite_register_cat("Env: parse ANSI block replaces environ",
                            test_env_parse_block_ansi, TEST_CAT_ABI);
    test_suite_register_cat("Env: parse block dedup last-wins",
                            test_env_parse_block_dedup, TEST_CAT_ABI);
    test_suite_register_cat("Env: parse block skips malformed",
                            test_env_parse_block_skips_malformed, TEST_CAT_ABI);
    test_suite_register_cat("Env: parse UNICODE block round-trip",
                            test_env_parse_block_unicode, TEST_CAT_ABI);
    test_suite_register_cat("Env: 1 MiB block-size cap",
                            test_env_block_size_cap, TEST_CAT_ABI);
    test_suite_register_cat("Env: parse skips over-value entry",
                            test_env_parse_block_over_value_skipped, TEST_CAT_ABI);
    test_suite_register_cat("Env: parse rejects oversize block up front",
                            test_env_parse_block_oversize_rejected, TEST_CAT_ABI);
    test_suite_register_cat("Env: adopt sorts + dedups (last-wins)",
                            test_env_adopt_block_sorts_dedups, TEST_CAT_ABI);
    test_suite_register_cat("Env: unset reclaims cached block budget",
                            test_env_bytes_unset_reclaims, TEST_CAT_ABI);
    test_suite_register_cat("Env: empty adoption resets cached quota",
                            test_env_adopt_empty_resets_quota, TEST_CAT_ABI);
    /* s13: CreateEnvironmentBlock / DestroyEnvironmentBlock / ExpandForUser. */
    test_suite_register_cat("Env: create/destroy block round-trip",
                            test_env_create_block_roundtrip, TEST_CAT_ABI);
    test_suite_register_cat("Env: create block empty double-NUL",
                            test_env_create_block_empty, TEST_CAT_ABI);
    test_suite_register_cat("Env: create block deferred branches refused",
                            test_env_create_block_deferred_branches, TEST_CAT_ABI);
    test_suite_register_cat("Env: destroy block NULL/foreign no-op",
                            test_env_destroy_block_defensive, TEST_CAT_ABI);
    test_suite_register_cat("Env: ExpandForUser NULL token expands",
                            test_env_expand_for_user_null_token, TEST_CAT_ABI);
    test_suite_register_cat("Env: ExpandForUser per-user/NULL-caller refused",
                            test_env_expand_for_user_refusals, TEST_CAT_ABI);
    test_suite_register_cat("Env: ExpandForUser over-cap -> INVALID_PARAMETER",
                            test_env_expand_for_user_over_cap, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPath finds System32 binary",
                            test_env_searchpath_finds_system32, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPath missing -> FILE_NOT_FOUND",
                            test_env_searchpath_missing, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPath explicit path + extension append",
                            test_env_searchpath_explicit_ext, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPath qualified name bypasses iteration",
                            test_env_searchpath_qualified, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPath relative subpath searched beneath legs",
                            test_env_searchpath_relative_subpath, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPath mode reorders CWD precedence",
                            test_env_searchpath_mode_precedence, TEST_CAT_ABI);
    test_suite_register_cat("Env: SetSearchPathMode validation + PERMANENT lock",
                            test_env_setsearchpathmode, TEST_CAT_ABI);
    test_suite_register_cat("Env: NeedCurrentDirectoryForExePath rules",
                            test_env_need_current_dir, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPath CWD not gated by exe-path var",
                            test_env_searchpath_cwd_not_gated_by_exe_var, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPathA buffer sizing (untouched on small)",
                            test_env_searchpath_a_sizing, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPathW lpFilePart component pointer",
                            test_env_searchpath_w_filepart, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPathW ignores malformed unused extension",
                            test_env_searchpath_w_ignored_bad_ext, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPath rejects traversal via extension",
                            test_env_searchpath_ext_containment, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPath OOM -> OUTOFMEMORY, untouched",
                            test_env_searchpath_oom, TEST_CAT_ABI);
    test_suite_register_cat("Env: SearchPath PATH-leg OOM fails closed (no CWD)",
                            test_env_searchpath_path_oom, TEST_CAT_ABI);
    test_suite_register_cat("Env: CommandLineToArgv quoted args + NUL-term",
                            test_env_cmdline_basic_quotes, TEST_CAT_ABI);
    test_suite_register_cat("Env: CommandLineToArgv 2n+1 backslash-quote rule",
                            test_env_cmdline_backslash_quote, TEST_CAT_ABI);
    test_suite_register_cat("Env: CommandLineToArgv round-trips argv_to_cmdline",
                            test_env_cmdline_roundtrip, TEST_CAT_ABI);
    test_suite_register_cat("Env: CommandLineToArgv empty -> module path",
                            test_env_cmdline_empty_module_path, TEST_CAT_ABI);
    test_suite_register_cat("Env: CommandLineToArgv NULL -> NULL",
                            test_env_cmdline_null, TEST_CAT_ABI);
    test_suite_register_cat("Env: CommandLineToArgvW preserves lone surrogate",
                            test_env_cmdline_wide_verbatim, TEST_CAT_ABI);
    test_suite_register_cat("Env: CommandLineToArgvW quoted args (wide path)",
                            test_env_cmdline_wide_quotes, TEST_CAT_ABI);
    test_suite_register_cat("Env: CommandLineToArgv consecutive-quote runs",
                            test_env_cmdline_quote_runs, TEST_CAT_ABI);
    test_suite_register_cat("Env: CommandLineToArgv PMM allocator crossover",
                            test_env_cmdline_large_alloc_crossover, TEST_CAT_ABI);
    /* s16: environment security & sanitization */
    test_suite_register_cat("Env: s16 privilege-sensitive name blocklist",
                            test_env_privilege_sensitive_names, TEST_CAT_ABI);
    test_suite_register_cat("Env: s16 secure-context integrity threshold",
                            test_env_secure_context, TEST_CAT_ABI);
    test_suite_register_cat("Env: s16 AT_SECURE read gate hides blocklisted",
                            test_env_secure_read_gate, TEST_CAT_ABI);
    test_suite_register_cat("Env: s16 sanitize strips blocklisted from environ",
                            test_env_sanitize_for_elevation, TEST_CAT_ABI);
    test_suite_register_cat("Env: s16 secure block omits blocklisted names",
                            test_env_secure_block_excludes, TEST_CAT_ABI);
    test_suite_register_cat("Env: s16 env_copy secure source excludes blocklisted",
                            test_env_copy_secure_source_excludes, TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
