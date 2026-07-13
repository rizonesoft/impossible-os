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
#include "kernel/sched/task.h"
#include "kernel/sched/mutex.h"
#include "kernel/mm/heap.h"    /* kmalloc_fail_next for the OOM-safety test */
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

void test_register_env(void)
{
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
}

#endif /* KERNEL_TESTS */
