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
#include "kernel/nt/mitigation_policy.h"
#include "kernel/nt/pledge.h"
#include "kernel/sched/syscall.h"
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

/* Boundary: a component == VFS_MAX_NAME-1 (255) is the longest walk_path can
 * look up; a 256-byte component would be silently truncated (alias), so
 * vfs_resolve_path must reject it (review adversarial A2). */
static void test_resolve_component_255_ok(void)
{
    char comp[256];
    char out[VFS_MAX_PATH];
    uint32_t i;
    for (i = 0; i < 255; i++) comp[i] = 'a';
    comp[255] = '\0';
    /* 255-char component (== VFS_MAX_NAME-1) is the max walk_path accepts. */
    TEST_ASSERT_EQ(vfs_resolve_path("C:\\", comp, out, sizeof(out)), 0,
                   "255-byte component resolves");
}

static void test_resolve_component_256_rejected(void)
{
    char comp[257];
    char out[VFS_MAX_PATH];
    uint32_t i;
    for (i = 0; i < 256; i++) comp[i] = 'a';
    comp[256] = '\0';
    /* 256-char component would be silently truncated by walk_path -> reject. */
    TEST_ASSERT_EQ(vfs_resolve_path("C:\\", comp, out, sizeof(out)), -1,
                   "256-byte component rejected (no truncation alias)");
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

/* --- Section 9: per-process resource limits (rlimits) ---
 * Pure tests over the locked accessor API on a stack-local fixture, plus two
 * read-only oracle queries against the live PID 0 / current task to prove the
 * boot-time default + create-path inherit ran. No live boot infra touched. */

static struct task s_rlimit_fixture;
static struct task s_rlimit_child;

/* Seed every resource to a known baseline {cur=1000, max=2000} under a fresh lock. */
static void rlimit_fixture_reset(void)
{
    spinlock_t init = SPINLOCK_INIT;
    int i;
    s_rlimit_fixture.rlimit_lock = init;
    for (i = 0; i < RLIM_NLIMITS; i++) {
        s_rlimit_fixture.rlimits[i].rlim_cur = 1000;
        s_rlimit_fixture.rlimits[i].rlim_max = 2000;
    }
}

static void test_rlimit_get_set_roundtrip(void)
{
    rlimit_t rl;
    rlimit_t nl = { 500, 1500 };
    rlimit_fixture_reset();
    TEST_ASSERT_EQ(task_rlimit_set(&s_rlimit_fixture, RLIMIT_NOFILE, &nl, 0), RLIMIT_OK,
                   "moving soft/hard within the cap is unprivileged");
    TEST_ASSERT_EQ(task_rlimit_get(&s_rlimit_fixture, RLIMIT_NOFILE, &rl), RLIMIT_OK,
                   "get succeeds");
    TEST_ASSERT_EQ((int)rl.rlim_cur, 500, "soft limit committed");
    TEST_ASSERT_EQ((int)rl.rlim_max, 1500, "hard limit committed");
}

static void test_rlimit_cur_gt_max_rejected(void)
{
    rlimit_t nl = { 3000, 2000 };  /* rlim_cur > rlim_max */
    rlimit_fixture_reset();
    TEST_ASSERT_EQ(task_rlimit_set(&s_rlimit_fixture, RLIMIT_AS, &nl, 1), RLIMIT_ERR_INVAL,
                   "rlim_cur > rlim_max is invalid even for a privileged caller");
}

static void test_rlimit_raise_hard_needs_priv(void)
{
    rlimit_t rl;
    rlimit_t nl = { 1000, 5000 };  /* raise hard limit 2000 -> 5000 */
    rlimit_fixture_reset();
    TEST_ASSERT_EQ(task_rlimit_set(&s_rlimit_fixture, RLIMIT_AS, &nl, 0), RLIMIT_ERR_PERM,
                   "unprivileged raise of the hard limit is denied");
    task_rlimit_get(&s_rlimit_fixture, RLIMIT_AS, &rl);
    TEST_ASSERT_EQ((int)rl.rlim_max, 2000, "denied set leaves the hard limit intact");
    TEST_ASSERT_EQ(task_rlimit_set(&s_rlimit_fixture, RLIMIT_AS, &nl, 1), RLIMIT_OK,
                   "privileged raise of the hard limit is allowed");
    task_rlimit_get(&s_rlimit_fixture, RLIMIT_AS, &rl);
    TEST_ASSERT_EQ((int)rl.rlim_max, 5000, "privileged raise committed");
}

static void test_rlimit_lower_hard_unprivileged(void)
{
    rlimit_t rl;
    rlimit_t nl = { 500, 1000 };  /* lower hard limit 2000 -> 1000 */
    rlimit_fixture_reset();
    TEST_ASSERT_EQ(task_rlimit_set(&s_rlimit_fixture, RLIMIT_AS, &nl, 0), RLIMIT_OK,
                   "unprivileged lowering of the hard limit is allowed (irreversible)");
    task_rlimit_get(&s_rlimit_fixture, RLIMIT_AS, &rl);
    TEST_ASSERT_EQ((int)rl.rlim_max, 1000, "hard limit lowered");
}

static void test_rlimit_bad_resource_rejected(void)
{
    rlimit_t rl;
    rlimit_t nl = { 1, 1 };
    rlimit_fixture_reset();
    TEST_ASSERT_EQ(task_rlimit_set(&s_rlimit_fixture, RLIM_NLIMITS, &nl, 1), RLIMIT_ERR_INVAL,
                   "out-of-range resource index rejected on set");
    rl.rlim_cur = 7;
    rl.rlim_max = 7;
    TEST_ASSERT_EQ(task_rlimit_get(&s_rlimit_fixture, -1, &rl), RLIMIT_ERR_INVAL,
                   "negative resource index rejected on get");
    TEST_ASSERT_EQ((int)rl.rlim_cur, 0, "get failure zeroes the output");
}

static void test_rlimit_pid0_defaults(void)
{
    rlimit_t rl;
    struct task *sys = task_get_by_pid(0);
    TEST_ASSERT(sys != (struct task *)0, "PID 0 task exists");
    TEST_ASSERT_EQ(task_rlimit_get(sys, RLIMIT_STACK, &rl), RLIMIT_OK, "get PID0 stack limit");
    TEST_ASSERT_EQ((int)rl.rlim_cur, (int)RLIMIT_DEFAULT_STACK_CUR,
                   "PID 0 carries the default 8 MiB soft stack limit");
    task_rlimit_get(sys, RLIMIT_NOFILE, &rl);
    TEST_ASSERT_EQ((int)rl.rlim_max, (int)RLIMIT_DEFAULT_NOFILE_MAX,
                   "PID 0 carries the default NOFILE hard limit");
}

static void test_rlimit_inherit_copies_full_array(void)
{
    spinlock_t init = SPINLOCK_INIT;
    rlimit_t rl;
    int i;
    /* Seed the parent with distinctive per-resource values (incl. a lowered hard
     * limit), then inherit into a poisoned fresh child: a missed entry shows up
     * as the poison value, and the lowered hard limit must carry over intact. */
    rlimit_fixture_reset();  /* parent baseline: every resource {1000, 2000} */
    s_rlimit_fixture.rlimits[RLIMIT_AS].rlim_cur = 111;
    s_rlimit_fixture.rlimits[RLIMIT_AS].rlim_max = 222;   /* deliberately lowered hard limit */
    s_rlimit_fixture.rlimits[RLIMIT_NOFILE].rlim_cur = 64;
    s_rlimit_fixture.rlimits[RLIMIT_STACK].rlim_max = 333;

    s_rlimit_child.rlimit_lock = init;
    for (i = 0; i < RLIM_NLIMITS; i++) {
        s_rlimit_child.rlimits[i].rlim_cur = 0xDEAD;
        s_rlimit_child.rlimits[i].rlim_max = 0xBEEF;
    }

    task_rlimit_inherit(&s_rlimit_child, &s_rlimit_fixture);

    task_rlimit_get(&s_rlimit_child, RLIMIT_AS, &rl);
    TEST_ASSERT_EQ((int)rl.rlim_cur, 111, "child inherits parent RLIMIT_AS soft");
    TEST_ASSERT_EQ((int)rl.rlim_max, 222, "child inherits parent's lowered RLIMIT_AS hard limit");
    task_rlimit_get(&s_rlimit_child, RLIMIT_NOFILE, &rl);
    TEST_ASSERT_EQ((int)rl.rlim_cur, 64, "child inherits parent RLIMIT_NOFILE soft");
    task_rlimit_get(&s_rlimit_child, RLIMIT_STACK, &rl);
    TEST_ASSERT_EQ((int)rl.rlim_max, 333, "child inherits parent RLIMIT_STACK hard");
    task_rlimit_get(&s_rlimit_child, RLIMIT_CPU, &rl);
    TEST_ASSERT_EQ((int)rl.rlim_cur, 1000, "child inherits an unmodified baseline entry");
    TEST_ASSERT_EQ((int)rl.rlim_max, 2000, "no child entry retains its poison value");
}

/* MEMLOCK ships a FINITE hard ceiling, so an unprivileged raise above it is denied.
 * The PID 0 check stays READ-ONLY (mutating live PID 0 would poison every later
 * task via inheritance); the denial semantics are exercised on a stack fixture. */
static void test_rlimit_memlock_hard_ceiling_default(void)
{
    rlimit_t rl;
    rlimit_t nl;
    struct task *sys = task_get_by_pid(0);
    TEST_ASSERT(sys != (struct task *)0, "PID 0 task exists");
    task_rlimit_get(sys, RLIMIT_MEMLOCK, &rl);
    TEST_ASSERT_EQ((int)rl.rlim_max, (int)RLIMIT_DEFAULT_MEMLOCK_MAX,
                   "MEMLOCK default hard limit is finite (not RLIM_INFINITY)");
    /* Denial on a stack fixture (hard limit 2000): an unprivileged raise to 2001
     * is rejected. No live-task mutation, no rlim_max+1 overflow risk. */
    rlimit_fixture_reset();
    nl.rlim_cur = 2001;
    nl.rlim_max = 2001;
    TEST_ASSERT_EQ(task_rlimit_set(&s_rlimit_fixture, RLIMIT_MEMLOCK, &nl, 0), RLIMIT_ERR_PERM,
                   "unprivileged raise above a finite MEMLOCK hard ceiling is denied");
}

/* ---- Per-process mitigation policy (section 11) -------------------------
 * Pure tests over the task_mitigation_* accessors on a stack-local fixture:
 * monotonic set, no-clear, and the child-policy decision helper. The ring-3
 * NtSet/QueryInformationProcess(ProcessMitigationPolicy) handlers are deferred
 * (both return STATUS_NOT_SUPPORTED) pending fault-safe usercopy, so there is
 * no ring-3 ABI surface to exercise here; MIT_NO_CHILD_PROCESS enforcement is
 * serial-validated. */

static struct task s_mit_fixture;

static void mit_fixture_reset(void)
{
    s_mit_fixture.mitigation_flags = 0;
}

static void test_mit_fresh_is_zero(void)
{
    mit_fixture_reset();
    TEST_ASSERT_EQ((int)task_mitigation_get(&s_mit_fixture), 0,
                   "a fresh task carries no mitigation policy");
}

static void test_mit_apply_sets_bit(void)
{
    mit_fixture_reset();
    task_mitigation_apply(&s_mit_fixture, MIT_NO_CHILD_PROCESS);
    TEST_ASSERT((task_mitigation_get(&s_mit_fixture) & MIT_NO_CHILD_PROCESS) != 0,
                "task_mitigation_apply sets MIT_NO_CHILD_PROCESS");
}

static void test_mit_apply_is_monotonic(void)
{
    mit_fixture_reset();
    task_mitigation_apply(&s_mit_fixture, MIT_NO_CHILD_PROCESS);
    /* OR-ing an unrelated bit must not clear the first; re-applying is idempotent. */
    task_mitigation_apply(&s_mit_fixture, (1ull << 5));
    task_mitigation_apply(&s_mit_fixture, MIT_NO_CHILD_PROCESS);
    TEST_ASSERT((task_mitigation_get(&s_mit_fixture) & MIT_NO_CHILD_PROCESS) != 0,
                "monotonic OR preserves an already-set bit across later applies");
    TEST_ASSERT((task_mitigation_get(&s_mit_fixture) & (1ull << 5)) != 0,
                "monotonic OR accumulates additional bits");
}

static void test_mit_child_set_enables(void)
{
    mit_fixture_reset();
    TEST_ASSERT_EQ(task_mitigation_child_set(&s_mit_fixture,
                       PROC_MIT_CHILD_NO_CHILD_CREATION), 0,
                   "NoChildProcessCreation request succeeds");
    TEST_ASSERT((task_mitigation_get(&s_mit_fixture) & MIT_NO_CHILD_PROCESS) != 0,
                "child-policy request sets MIT_NO_CHILD_PROCESS");
}

static void test_mit_child_set_noop_when_unset(void)
{
    mit_fixture_reset();
    /* A request that omits the bit while it is not set is a benign no-op. */
    TEST_ASSERT_EQ(task_mitigation_child_set(&s_mit_fixture, 0), 0,
                   "omitting the bit while unset is a benign no-op");
    TEST_ASSERT_EQ((int)(task_mitigation_get(&s_mit_fixture) & MIT_NO_CHILD_PROCESS), 0,
                   "no-op request leaves the bit clear");
}

static void test_mit_child_set_clear_denied(void)
{
    mit_fixture_reset();
    task_mitigation_child_set(&s_mit_fixture, PROC_MIT_CHILD_NO_CHILD_CREATION);
    /* Omitting the bit once it is set is a clear attempt: refused, bit intact. */
    TEST_ASSERT_EQ(task_mitigation_child_set(&s_mit_fixture, 0), -1,
                   "clearing an already-set NO_CHILD is refused");
    TEST_ASSERT((task_mitigation_get(&s_mit_fixture) & MIT_NO_CHILD_PROCESS) != 0,
                "a refused clear leaves MIT_NO_CHILD_PROCESS set");
}

/* ============================================================================
 * Section 12: pledge / unveil pure decision core
 * ============================================================================ */

static void test_pledge_parse_valid(void)
{
    uint64_t m = 0;
    TEST_ASSERT_EQ(pledge_parse("stdio rpath", &m), 0, "parse of known tokens ok");
    TEST_ASSERT((m & PLEDGE_STDIO) != 0, "stdio bit set");
    TEST_ASSERT((m & PLEDGE_RPATH) != 0, "rpath bit set");
    TEST_ASSERT((m & PLEDGE_WPATH) == 0, "wpath bit not set");
    TEST_ASSERT((m & PLEDGE_PLEDGED) == 0, "parse yields no sentinel (raw mask)");
}

static void test_pledge_parse_unknown_rejected(void)
{
    uint64_t m = 0;
    TEST_ASSERT_EQ(pledge_parse("stdio bogus", &m), -1, "unknown token rejects the call");
}

static void test_pledge_parse_empty(void)
{
    uint64_t m = 0xFF;
    TEST_ASSERT_EQ(pledge_parse("   ", &m), 0, "whitespace-only parses ok");
    TEST_ASSERT_EQ(m, 0ull, "empty promise yields mask 0");
}

static void test_pledge_tighten_first(void)
{
    uint64_t m = pledge_tighten(0, PLEDGE_STDIO | PLEDGE_RPATH);
    TEST_ASSERT((m & PLEDGE_PLEDGED) != 0, "first pledge sets the pledged sentinel");
    TEST_ASSERT((m & PLEDGE_STDIO) != 0 && (m & PLEDGE_RPATH) != 0,
                "first pledge adopts the requested categories");
}

static void test_pledge_tighten_intersect_narrows(void)
{
    uint64_t first = pledge_tighten(0, PLEDGE_STDIO | PLEDGE_RPATH | PLEDGE_WPATH);
    uint64_t second = pledge_tighten(first, PLEDGE_STDIO);
    TEST_ASSERT((second & PLEDGE_STDIO) != 0, "kept the intersected category");
    TEST_ASSERT((second & PLEDGE_RPATH) == 0, "dropped rpath on tighten");
    TEST_ASSERT((second & PLEDGE_WPATH) == 0, "dropped wpath on tighten");
}

static void test_pledge_tighten_cannot_widen(void)
{
    uint64_t first = pledge_tighten(0, PLEDGE_STDIO);
    /* Re-pledging a category not currently held must NOT add it (tighten-only). */
    uint64_t widened = pledge_tighten(first, PLEDGE_STDIO | PLEDGE_INET);
    TEST_ASSERT((widened & PLEDGE_INET) == 0, "tighten cannot re-add a dropped category");
    TEST_ASSERT((widened & PLEDGE_STDIO) != 0, "still holds the surviving category");
}

static void test_pledge_classify_core_and_deny(void)
{
    TEST_ASSERT_EQ(pledge_syscall_category(SSDT_NtClose), PLEDGE_REQ_CORE,
                   "NtClose is survival-core (always allowed)");
    TEST_ASSERT((pledge_syscall_category(SSDT_NtCreateProcess) & PLEDGE_PROC) != 0,
                "NtCreateProcess requires proc");
    TEST_ASSERT((pledge_syscall_category(SSDT_NtSetInformationThread) & PLEDGE_PROC) != 0,
                "NtSetInformationThread requires proc (priority escalation gate)");
    /* An unclassified/unused index defaults to deny (fail-closed allowlist). */
    TEST_ASSERT((pledge_syscall_category(0x03FF) & PLEDGE_REQ_DENY) != 0,
                "unclassified syscall defaults to deny");
}

static void test_pledge_file_categories(void)
{
    TEST_ASSERT_EQ(pledge_file_categories(VFS_O_READ), PLEDGE_RPATH, "read -> rpath");
    TEST_ASSERT_EQ(pledge_file_categories(VFS_O_WRITE), PLEDGE_WPATH, "write -> wpath");
    TEST_ASSERT_EQ(pledge_file_categories(VFS_O_CREATE), PLEDGE_CPATH, "create -> cpath");
    TEST_ASSERT_EQ(pledge_file_categories(VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE),
                   PLEDGE_RPATH | PLEDGE_WPATH | PLEDGE_CPATH, "rw+create -> all three");
}

static void test_pledge_is_allowed(void)
{
    uint64_t mask = PLEDGE_PLEDGED | PLEDGE_STDIO | PLEDGE_RPATH;
    TEST_ASSERT_EQ(pledge_is_allowed(mask, PLEDGE_REQ_CORE), 1, "core always allowed");
    TEST_ASSERT_EQ(pledge_is_allowed(mask, PLEDGE_REQ_DENY), 0, "deny sentinel blocks");
    TEST_ASSERT_EQ(pledge_is_allowed(mask, PLEDGE_RPATH), 1, "held category allowed");
    TEST_ASSERT_EQ(pledge_is_allowed(mask, PLEDGE_WPATH), 0, "absent category denied");
    TEST_ASSERT_EQ(pledge_is_allowed(mask, PLEDGE_RPATH | PLEDGE_WPATH), 0,
                   "AND semantics: one absent category denies");
}

static void test_unveil_perms_for_vfs(void)
{
    TEST_ASSERT_EQ(unveil_perms_for_vfs(VFS_O_READ), UNVEIL_R, "read -> r");
    TEST_ASSERT_EQ(unveil_perms_for_vfs(VFS_O_WRITE), UNVEIL_W, "write -> w");
    TEST_ASSERT_EQ(unveil_perms_for_vfs(VFS_O_CREATE), UNVEIL_C, "create -> c");
}

/* Build a small stack-resident folded unveil list for the pure matcher. Paths
 * must already be folded (uppercase, backslash-separated) as unveil_add stores. */
static void test_unveil_covers_exact_and_boundary(void)
{
    unveil_entry_t e = { (unveil_entry_t *)0, UNVEIL_R, "C:\\ALLOWED" };
    TEST_ASSERT_EQ(unveil_list_covers(&e, "C:\\ALLOWED", UNVEIL_R), 1, "exact match allowed");
    TEST_ASSERT_EQ(unveil_list_covers(&e, "C:\\ALLOWED\\FILE.TXT", UNVEIL_R), 1,
                   "child at a component boundary allowed");
    /* Sibling sharing a textual prefix must NOT be treated as a descendant. */
    TEST_ASSERT_EQ(unveil_list_covers(&e, "C:\\ALLOWEDEVIL", UNVEIL_R), 0,
                   "sibling prefix is not a descendant");
}

static void test_unveil_covers_deny_and_perms(void)
{
    unveil_entry_t e = { (unveil_entry_t *)0, UNVEIL_R, "C:\\ALLOWED" };
    TEST_ASSERT_EQ(unveil_list_covers(&e, "C:\\OTHER", UNVEIL_R), 0,
                   "unmatched path denied");
    TEST_ASSERT_EQ(unveil_list_covers(&e, "C:\\ALLOWED\\F", UNVEIL_W), 0,
                   "insufficient perms denied");
    TEST_ASSERT_EQ(unveil_list_covers((unveil_entry_t *)0, "C:\\ANY", UNVEIL_R), 1,
                   "NULL list (never unveiled) allows all");
}

static void test_unveil_longest_match_wins(void)
{
    /* A more specific entry (deeper path) overrides a broader one's perms. */
    unveil_entry_t deep = { (unveil_entry_t *)0, UNVEIL_R | UNVEIL_W, "C:\\A\\B" };
    unveil_entry_t root = { &deep, UNVEIL_R, "C:\\A" };
    TEST_ASSERT_EQ(unveil_list_covers(&root, "C:\\A\\B\\F", UNVEIL_W), 1,
                   "deeper entry's write perm applies under it");
    TEST_ASSERT_EQ(unveil_list_covers(&root, "C:\\A\\C", UNVEIL_W), 0,
                   "broader entry (r only) denies write outside the deep subtree");
}

/* Legacy INT 0x80 ABI pledge gate (fixture task, no live dispatch). */
static struct task s_pledge_fixture;

static void test_pledge_legacy_gate(void)
{
    s_pledge_fixture.pledge_mask = PLEDGE_PLEDGED | PLEDGE_STDIO;  /* no proc */
    TEST_ASSERT_EQ(pledge_check_legacy(&s_pledge_fixture, SYS_FORK),
                   STATUS_PLEDGE_VIOLATION, "legacy SYS_FORK without proc is a violation");
    TEST_ASSERT_EQ(pledge_check_legacy(&s_pledge_fixture, SYS_EXIT),
                   STATUS_SUCCESS, "legacy SYS_EXIT is survival-core");
    /* IPC / system-control ops are NOT core: fail-closed under an empty-ish pledge. */
    TEST_ASSERT_EQ(pledge_check_legacy(&s_pledge_fixture, SYS_SHMEM_CREATE),
                   STATUS_PLEDGE_VIOLATION, "legacy SYS_SHMEM_CREATE fails closed (not core)");
    TEST_ASSERT_EQ(pledge_check_legacy(&s_pledge_fixture, SYS_REBOOT),
                   STATUS_PLEDGE_VIOLATION, "legacy SYS_REBOOT fails closed (not core)");
    TEST_ASSERT_EQ(pledge_check_legacy(&s_pledge_fixture, SYS_WRITE),
                   STATUS_SUCCESS, "legacy SYS_WRITE covered by stdio");
    s_pledge_fixture.pledge_mask = PLEDGE_PLEDGED | PLEDGE_PROC;
    TEST_ASSERT_EQ(pledge_check_legacy(&s_pledge_fixture, SYS_FORK),
                   STATUS_SUCCESS, "legacy SYS_FORK with proc is allowed");
    s_pledge_fixture.pledge_mask = 0;   /* unpledged */
    TEST_ASSERT_EQ(pledge_check_legacy(&s_pledge_fixture, SYS_FORK),
                   STATUS_SUCCESS, "unpledged task: legacy path unrestricted");
}

/* Empty locked unveil set must deny all (fixture task, no boot infra). */
static void test_unveil_lock_empty_denies(void)
{
    struct task fix;
    spinlock_t init = SPINLOCK_INIT;
    fix.unveil_lock = init;
    fix.unveil_list = (struct unveil_entry *)0;
    fix.unveil_locked = 0;
    fix.unveil_active = 0;
    TEST_ASSERT_EQ(unveil_check(&fix, "C:\\Any", UNVEIL_R), STATUS_SUCCESS,
                   "never-unveiled task allows all paths");
    unveil_lock(&fix);   /* lock with no entries -> active + empty */
    TEST_ASSERT_EQ(unveil_check(&fix, "C:\\Any", UNVEIL_R), STATUS_ACCESS_DENIED,
                   "active empty (lock-before-add) unveil set denies all");
}

/* Re-unveiling a path is tighten-only: a dropped bit cannot be restored. */
static void test_unveil_perms_tighten_only(void)
{
    struct task fix;
    spinlock_t init = SPINLOCK_INIT;
    fix.unveil_lock = init;
    fix.unveil_list = (struct unveil_entry *)0;
    fix.unveil_locked = 0;
    fix.unveil_active = 0;
    fix.unveil_gen = 0;
    fix.pledge_mask = 0;
    unveil_add(&fix, "C:\\P", UNVEIL_R | UNVEIL_W | UNVEIL_C);
    TEST_ASSERT_EQ(unveil_check(&fix, "C:\\P", UNVEIL_W), STATUS_SUCCESS,
                   "initial rwc grants write");
    unveil_add(&fix, "C:\\P", UNVEIL_R);   /* narrow to r */
    TEST_ASSERT_EQ(unveil_check(&fix, "C:\\P", UNVEIL_W), STATUS_ACCESS_DENIED,
                   "narrowing to r drops write");
    unveil_add(&fix, "C:\\P", UNVEIL_R | UNVEIL_W | UNVEIL_C);   /* attempt widen */
    TEST_ASSERT_EQ(unveil_check(&fix, "C:\\P", UNVEIL_W), STATUS_ACCESS_DENIED,
                   "re-unveil cannot restore a dropped bit (tighten-only)");
    pledge_unveil_teardown(&fix);   /* free the list */
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
    test_suite_register_cat("ProcExt: resolve 255-byte component ok",
                            test_resolve_component_255_ok, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: resolve 256-byte component rejected",
                            test_resolve_component_256_rejected, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: NtCreateFile zero-Length name",
                            test_ntcreatefile_zero_length_name, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: rlimit get/set roundtrip",
                            test_rlimit_get_set_roundtrip, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: rlimit cur>max rejected",
                            test_rlimit_cur_gt_max_rejected, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: rlimit raise hard needs privilege",
                            test_rlimit_raise_hard_needs_priv, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: rlimit lower hard unprivileged",
                            test_rlimit_lower_hard_unprivileged, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: rlimit bad resource rejected",
                            test_rlimit_bad_resource_rejected, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: rlimit PID0 defaults",
                            test_rlimit_pid0_defaults, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: rlimit inherit copies full array",
                            test_rlimit_inherit_copies_full_array, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: rlimit MEMLOCK hard ceiling default",
                            test_rlimit_memlock_hard_ceiling_default, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: mitigation fresh is zero",
                            test_mit_fresh_is_zero, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: mitigation apply sets bit",
                            test_mit_apply_sets_bit, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: mitigation apply is monotonic",
                            test_mit_apply_is_monotonic, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: mitigation child-set enables",
                            test_mit_child_set_enables, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: mitigation child-set no-op when unset",
                            test_mit_child_set_noop_when_unset, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: mitigation child-set clear denied",
                            test_mit_child_set_clear_denied, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: pledge parse valid tokens",
                            test_pledge_parse_valid, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: pledge parse unknown rejected",
                            test_pledge_parse_unknown_rejected, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: pledge parse empty",
                            test_pledge_parse_empty, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: pledge tighten first adopts",
                            test_pledge_tighten_first, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: pledge tighten narrows",
                            test_pledge_tighten_intersect_narrows, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: pledge tighten cannot widen",
                            test_pledge_tighten_cannot_widen, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: pledge classify core/proc/deny",
                            test_pledge_classify_core_and_deny, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: pledge file categories",
                            test_pledge_file_categories, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: pledge is-allowed decision",
                            test_pledge_is_allowed, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: unveil perms for vfs flags",
                            test_unveil_perms_for_vfs, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: unveil exact + component boundary",
                            test_unveil_covers_exact_and_boundary, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: unveil deny + insufficient perms",
                            test_unveil_covers_deny_and_perms, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: unveil longest match wins",
                            test_unveil_longest_match_wins, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: pledge legacy INT 0x80 gate",
                            test_pledge_legacy_gate, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: unveil empty-locked denies all",
                            test_unveil_lock_empty_denies, TEST_CAT_SCHED);
    test_suite_register_cat("ProcExt: unveil perms tighten-only",
                            test_unveil_perms_tighten_only, TEST_CAT_SCHED);
}

#endif /* KERNEL_TESTS */
