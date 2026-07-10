/* ============================================================================
 * test_proc_ext.c -- Process model extensions unit tests (TODO-21)
 *
 * Section 1 (Working Directory): pure tests for vfs_resolve_path path
 * canonicalization + task cwd get/set storage. These exercise behavior without
 * touching live boot infrastructure (no scheduler, no VFS mounts, no _init).
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/fs/vfs.h"
#include "kernel/sched/task.h"
#include "kernel/sched/spinlock.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/nt_file.h"
#include "kernel/types.h"

/* Local ASCII string compare (no dependency on live libc in the test TU). */
static int pe_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* --- vfs_resolve_path: canonicalization --- */

static void test_resolve_absolute_passthrough(void)
{
    char out[VFS_MAX_PATH];
    int r = vfs_resolve_path("C:\\", "D:\\foo\\bar", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0, "absolute input resolves");
    TEST_ASSERT(pe_streq(out, "D:\\foo\\bar"), "absolute input keeps its own drive");
}

static void test_resolve_relative_join(void)
{
    char out[VFS_MAX_PATH];
    int r = vfs_resolve_path("C:\\Users", "docs", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0, "relative join resolves");
    TEST_ASSERT(pe_streq(out, "C:\\Users\\docs"), "relative joins onto cwd");
}

static void test_resolve_relative_from_root(void)
{
    char out[VFS_MAX_PATH];
    int r = vfs_resolve_path("C:\\", "System\\Logs", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0, "relative from root resolves");
    TEST_ASSERT(pe_streq(out, "C:\\System\\Logs"), "no doubled root separator");
}

static void test_resolve_dot_collapse(void)
{
    char out[VFS_MAX_PATH];
    int r = vfs_resolve_path("C:\\a", ".\\b", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0, "'.' component resolves");
    TEST_ASSERT(pe_streq(out, "C:\\a\\b"), "'.' is dropped");
}

static void test_resolve_dotdot_pop(void)
{
    char out[VFS_MAX_PATH];
    int r = vfs_resolve_path("C:\\a\\b", "..", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0, "'..' component resolves");
    TEST_ASSERT(pe_streq(out, "C:\\a"), "'..' pops one component");
}

static void test_resolve_dotdot_at_root_stays_root(void)
{
    char out[VFS_MAX_PATH];
    int r = vfs_resolve_path("C:\\", "..", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0, "'..' at root resolves");
    TEST_ASSERT(pe_streq(out, "C:\\"), "'..' cannot escape the drive root");
}

static void test_resolve_dotdot_cannot_escape(void)
{
    char out[VFS_MAX_PATH];
    int r = vfs_resolve_path("C:\\", "..\\..\\x", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0, "over-popping resolves");
    TEST_ASSERT(pe_streq(out, "C:\\x"), "traversal above root is clamped");
}

static void test_resolve_forward_slash_normalized(void)
{
    char out[VFS_MAX_PATH];
    int r = vfs_resolve_path("C:\\", "a/b/c", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0, "forward slashes resolve");
    TEST_ASSERT(pe_streq(out, "C:\\a\\b\\c"), "'/' normalized to '\\'");
}

static void test_resolve_root_relative(void)
{
    char out[VFS_MAX_PATH];
    int r = vfs_resolve_path("D:\\cur\\dir", "\\foo", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0, "root-relative resolves");
    TEST_ASSERT(pe_streq(out, "D:\\foo"), "root-relative takes drive from cwd, resets to root");
}

static void test_resolve_multi_separator_collapse(void)
{
    char out[VFS_MAX_PATH];
    int r = vfs_resolve_path("C:\\", "a\\\\b", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0, "doubled separator resolves");
    TEST_ASSERT(pe_streq(out, "C:\\a\\b"), "empty components collapse");
}

static void test_resolve_overflow_rejected(void)
{
    char out[8];
    int r = vfs_resolve_path("C:\\", "averylongdirectoryname", out, sizeof(out));
    TEST_ASSERT_EQ(r, -1, "overflow is rejected, not truncated");
}

static void test_resolve_invalid_inputs(void)
{
    char out[VFS_MAX_PATH];
    /* Relative input with a non-absolute cwd is invalid. */
    TEST_ASSERT_EQ(vfs_resolve_path("bogus", "x", out, sizeof(out)), -1,
                   "relative resolve needs an absolute cwd");
    /* NULL args are rejected. */
    TEST_ASSERT_EQ(vfs_resolve_path("C:\\", (const char *)0, out, sizeof(out)), -1,
                   "NULL input rejected");
}

/* --- task cwd get/set storage (fixture task, no live scheduler) --- */

static struct task s_cwd_fixture;

static void cwd_fixture_reset(void)
{
    spinlock_t init = SPINLOCK_INIT;
    s_cwd_fixture.cwd[0] = '\0';
    s_cwd_fixture.cwd_lock = init;
}

static void test_task_cwd_set_get_roundtrip(void)
{
    char out[TASK_CWD_MAX];
    cwd_fixture_reset();
    TEST_ASSERT_EQ(task_set_cwd(&s_cwd_fixture, "C:\\Impossible\\System32"), 0,
                   "set_cwd accepts an in-range path");
    task_get_cwd(&s_cwd_fixture, out, sizeof(out));
    TEST_ASSERT(pe_streq(out, "C:\\Impossible\\System32"),
                "get_cwd returns exactly what set_cwd stored");
}

static void test_task_cwd_overflow_leaves_unchanged(void)
{
    char big[TASK_CWD_MAX + 8];
    char out[TASK_CWD_MAX];
    uint32_t i;
    cwd_fixture_reset();
    task_set_cwd(&s_cwd_fixture, "C:\\seed");
    for (i = 0; i < sizeof(big) - 1; i++)
        big[i] = 'a';
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_EQ(task_set_cwd(&s_cwd_fixture, big), -1,
                   "oversized path is rejected");
    task_get_cwd(&s_cwd_fixture, out, sizeof(out));
    TEST_ASSERT(pe_streq(out, "C:\\seed"),
                "rejected set leaves the prior cwd intact");
}

static void test_task_cwd_get_null_task(void)
{
    char out[TASK_CWD_MAX];
    out[0] = 'x';
    task_get_cwd((struct task *)0, out, sizeof(out));
    TEST_ASSERT_EQ((int)out[0], 0, "NULL task yields an empty string");
}

/* Regression: a zero-Length ObjectName must be treated as an empty name --
 * oa_extract_path must NOT scan Buffer past the declared length (adversarial
 * round 2). Non-NUL garbage in Buffer + Length 0 -> OBJECT_NAME_NOT_FOUND. */
static void test_ntcreatefile_zero_length_name(void)
{
    HANDLE h = (HANDLE)0;
    IO_STATUS_BLOCK iosb;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    uint16_t junk[4];
    NTSTATUS s;

    junk[0] = 0x4141; junk[1] = 0x4242; junk[2] = 0x4343; junk[3] = 0x4444;
    name.Length = 0;
    name.MaximumLength = (uint16_t)sizeof(junk);
    name._pad = 0;
    name.Buffer = junk;

    oa.Length = sizeof(OBJECT_ATTRIBUTES);
    oa.RootDirectory = (HANDLE)0;
    oa._pad1 = 0;
    oa.ObjectName = &name;
    oa.Attributes = 0;
    oa._pad2 = 0;
    oa.SecurityDescriptor = (void *)0;
    oa.SecurityQualityOfService = (void *)0;

    iosb.Status = 0;
    iosb._pad = 0;
    iosb.Information = 0;

    s = ssdt_dispatch(SSDT_NtCreateFile, (uint64_t)&h, GENERIC_READ,
                      (uint64_t)&oa, (uint64_t)&iosb, FILE_OPEN, 0);
    TEST_ASSERT_EQ(s, STATUS_OBJECT_NAME_NOT_FOUND,
                   "zero-Length name -> NOT_FOUND (Buffer never scanned)");
}

void test_register_proc_ext(void)
{
    test_suite_register_cat("ProcExt: resolve absolute passthrough",
                            test_resolve_absolute_passthrough, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: resolve relative join",
                            test_resolve_relative_join, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: resolve relative from root",
                            test_resolve_relative_from_root, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: resolve '.' collapse",
                            test_resolve_dot_collapse, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: resolve '..' pop",
                            test_resolve_dotdot_pop, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: resolve '..' at root stays root",
                            test_resolve_dotdot_at_root_stays_root, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: resolve '..' cannot escape drive",
                            test_resolve_dotdot_cannot_escape, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: resolve '/' normalized to '\\'",
                            test_resolve_forward_slash_normalized, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: resolve root-relative",
                            test_resolve_root_relative, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: resolve multi-separator collapse",
                            test_resolve_multi_separator_collapse, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: resolve overflow rejected",
                            test_resolve_overflow_rejected, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: resolve invalid inputs",
                            test_resolve_invalid_inputs, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: cwd set/get roundtrip",
                            test_task_cwd_set_get_roundtrip, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: cwd overflow leaves unchanged",
                            test_task_cwd_overflow_leaves_unchanged, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: cwd get NULL task",
                            test_task_cwd_get_null_task, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: NtCreateFile zero-Length name",
                            test_ntcreatefile_zero_length_name, TEST_CAT_SCHED);
}

#endif /* KERNEL_TESTS */
