/* ============================================================================
 * pledge.c -- OpenBSD-style process self-restriction (pledge + unveil)
 *
 * See include/kernel/nt/pledge.h for the design contract. Enforcement is split:
 *   - Coarse per-syscall category check runs in ssdt_dispatch (pledge_check_
 *     syscall); a violation is terminated by the dispatcher at a point with no
 *     in-flight audit session.
 *   - Fine, argument-dependent checks (rpath/wpath/cpath for file opens and
 *     NtSetInformationFile mutations) run inside the handlers via
 *     pledge_check_file / pledge_check_setinfo and only RETURN the violation
 *     status; the dispatcher terminates after the audit POST completes.
 *   - unveil restricts the reachable filesystem subtree; a denial returns
 *     STATUS_ACCESS_DENIED and does NOT terminate.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/nt/pledge.h"
#include "kernel/nt/ssdt.h"            /* SSDT_TABLE_MAIN, ssdt_register */
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/nt_types.h"        /* HANDLE etc. (nt_file.h depends on it) */
#include "kernel/nt/nt_file.h"         /* File*Information class constants */
#include "kernel/nt/nt_unicode.h"      /* UNICODE_STRING, decode/narrow helpers */
#include "kernel/nt/zw.h"              /* ssdt_previous_mode, SSDT_USER_MODE */
#include "kernel/sched/task.h"         /* struct task, task_current, task_exit */
#include "kernel/sched/syscall.h"      /* SYS_* legacy INT 0x80 ABI numbers */
#include "kernel/mm/heap.h"            /* kmalloc / kfree */
#include "kernel/klog.h"
#include "kernel/fs/vfs.h"             /* vfs_path_fold, VFS_O_* */

/* ---- Small freestanding string helpers (kernel has no libc) -------------- */

static uint32_t p_strlen(const char *s, uint32_t max)
{
    uint32_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

/* Bounded, NUL-terminated copy of `src` into `dst` (capacity `cap`). */
static void p_strlcpy(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0;
    if (cap == 0)
        return;
    for (; i < cap - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int p_streq(const char *a, const char *b)
{
    uint32_t i = 0;
    for (; a[i] && b[i]; i++)
        if (a[i] != b[i])
            return 0;
    return a[i] == b[i];   /* both terminated at the same point */
}

/* Compare `len` bytes of `a` against NUL-terminated token `b` exactly. */
static int p_tok_eq(const char *a, uint32_t len, const char *b)
{
    uint32_t i = 0;
    for (; i < len; i++) {
        if (b[i] == '\0' || a[i] != b[i])
            return 0;
    }
    return b[len] == '\0';
}

/* ---- Pure decision core -------------------------------------------------- */

static uint64_t pledge_token_bit(const char *tok, uint32_t len)
{
    if (p_tok_eq(tok, len, "stdio")) return PLEDGE_STDIO;
    if (p_tok_eq(tok, len, "rpath")) return PLEDGE_RPATH;
    if (p_tok_eq(tok, len, "wpath")) return PLEDGE_WPATH;
    if (p_tok_eq(tok, len, "cpath")) return PLEDGE_CPATH;
    if (p_tok_eq(tok, len, "inet"))  return PLEDGE_INET;
    if (p_tok_eq(tok, len, "proc"))  return PLEDGE_PROC;
    if (p_tok_eq(tok, len, "exec"))  return PLEDGE_EXEC;
    if (p_tok_eq(tok, len, "dns"))   return PLEDGE_DNS;
    if (p_tok_eq(tok, len, "tty"))   return PLEDGE_TTY;
    return 0;   /* unknown token */
}

int pledge_parse(const char *promises, uint64_t *out_mask)
{
    uint64_t mask = 0;
    const char *p = promises;

    if (!promises || !out_mask)
        return -1;

    while (*p) {
        const char *start;
        uint32_t len;
        uint64_t bit;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        len = (uint32_t)(p - start);
        bit = pledge_token_bit(start, len);
        if (bit == 0)
            return -1;   /* unknown promise -> reject the whole call */
        mask |= bit;
    }

    *out_mask = mask;
    return 0;
}

uint64_t pledge_syscall_category(uint32_t service_number)
{
    switch (service_number) {
    /* Survival core: always permitted regardless of the pledge set. */
    case SSDT_NtClose:
    case SSDT_NtWaitForSingleObject:
    case SSDT_NtWaitForMultipleObjects:
    case SSDT_NtQueryInformationProcess:
    case SSDT_NtGetContextThread:
    case SSDT_NtQueryInformationThread:
    case SSDT_NtYieldExecution:
    case SSDT_NtTestAlert:
    case SSDT_NtDelayExecution:
    case SSDT_NtAllocateVirtualMemory:
    case SSDT_NtFreeVirtualMemory:
    case SSDT_NtProtectVirtualMemory:
    case SSDT_NtQueryVirtualMemory:
    case SSDT_NtFlushVirtualMemory:
    case SSDT_NtRaiseHardError:
    case SSDT_NtQuerySystemTime:
    case SSDT_NtQueryPerformanceCounter:
    case SSDT_NtRaiseException:
    case SSDT_NtContinue:
    case SSDT_NtGetRandom:
    case SSDT_NtTerminateProcess:
    case SSDT_NtTerminateThread:
    case SSDT_NtSetCurrentDirectory:
    case SSDT_NtQueryCurrentDirectory:
    case SSDT_NtPledge:
    case SSDT_NtUnveil:
    /* Environment variables are process-local state (own task->environ),
     * comparable to the CWD syscalls above -- survival-core, not a promise. */
    case SSDT_NtQueryEnvironmentVariable:
    case SSDT_NtSetEnvironmentVariable:
    /* File OPEN/CREATE/DELETE + SetInformationFile reach the handler; their
     * rpath/wpath/cpath split needs the ACCESS_MASK / info class, enforced by
     * pledge_check_file / pledge_check_setinfo inside the handler. */
    case SSDT_NtCreateFile:
    case SSDT_NtOpenFile:
    case SSDT_NtDeleteFile:
    case SSDT_NtSetInformationFile:
        return PLEDGE_REQ_CORE;

    /* stdio: read/write/close/flush on already-open handles. */
    case SSDT_NtReadFile:
    case SSDT_NtWriteFile:
    case SSDT_NtQueryInformationFile:
    case SSDT_NtFlushBuffersFile:
    case SSDT_NtReadFileScatter:
    case SSDT_NtWriteFileGather:
        return PLEDGE_STDIO;

    /* rpath: metadata read by path. */
    case SSDT_NtQueryDirectoryFile:
    case SSDT_NtQueryAttributesFile:
    case SSDT_NtQueryFullAttributesFile:
        return PLEDGE_RPATH;

    /* proc: process / thread creation, lookup, and management. NtSetInformation
     * Thread can raise a thread to real-time priority (CPU monopolization), so
     * it needs `proc` -- it is NOT a survival-core operation. */
    case SSDT_NtCreateProcess:
    case SSDT_NtCreateProcessEx:
    case SSDT_NtOpenProcess:
    case SSDT_NtCreateThread:
    case SSDT_NtCreateThreadEx:
    case SSDT_NtSetInformationThread:
        return PLEDGE_PROC;

    /* exec: launch a new image. */
    case SSDT_NtCreateUserProcess:
        return PLEDGE_EXEC;

    default:
        /* Fail-closed allowlist: an unclassified or newly registered syscall is
         * DENIED under an active pledge (over-restricts, never under-restricts).
         * inet/dns/tty categories are defined for the promise vocabulary but map
         * to no SSDT syscall yet (those subsystems are not wired), so a process
         * that needs them still reaches this deny until they are classified. */
        return PLEDGE_REQ_DENY;
    }
}

uint64_t pledge_file_categories(uint32_t vfs_flags)
{
    uint64_t need = 0;
    if (vfs_flags & VFS_O_READ)
        need |= PLEDGE_RPATH;
    if (vfs_flags & (VFS_O_WRITE | VFS_O_TRUNC | VFS_O_APPEND))
        need |= PLEDGE_WPATH;
    if (vfs_flags & (VFS_O_CREATE | VFS_O_DELETE_ON_CLOSE))
        need |= PLEDGE_CPATH;
    return need;
}

/* Category bits an NtSetInformationFile class requires (0 = benign/core). */
static uint64_t pledge_setinfo_categories(uint32_t info_class)
{
    switch (info_class) {
    case FileRenameInformation:
    case FileDispositionInformation:
        return PLEDGE_CPATH;
    case FileBasicInformation:
    case FileAllocationInformation:
    case FileEndOfFileInformation:
        return PLEDGE_WPATH;
    default:
        return 0;   /* position, mode, etc. -- no path-mutation category */
    }
}

int pledge_is_allowed(uint64_t mask, uint64_t required)
{
    if (required == PLEDGE_REQ_CORE)
        return 1;
    if (required & PLEDGE_REQ_DENY)
        return 0;
    /* Every required category bit must be present (AND semantics). */
    return (required & ~mask) == 0;
}

uint64_t pledge_tighten(uint64_t current, uint64_t requested)
{
    uint64_t req = (requested & PLEDGE_CATEGORY_MASK) | PLEDGE_PLEDGED;
    if ((current & PLEDGE_PLEDGED) == 0)
        return req;                          /* first pledge: adopt as-is */
    return (current & req) | PLEDGE_PLEDGED; /* subsequent: intersect only */
}

uint8_t unveil_perms_for_vfs(uint32_t vfs_flags)
{
    uint8_t w = 0;
    if (vfs_flags & VFS_O_READ)
        w |= UNVEIL_R;
    if (vfs_flags & (VFS_O_WRITE | VFS_O_TRUNC | VFS_O_APPEND))
        w |= UNVEIL_W;
    if (vfs_flags & (VFS_O_CREATE | VFS_O_DELETE_ON_CLOSE))
        w |= UNVEIL_C;
    return w;
}

/* Does `path` fall under `prefix` (length `plen`) at a component boundary? */
static int p_prefix_covers(const char *prefix, uint32_t plen, const char *path)
{
    uint32_t i;
    char after;
    for (i = 0; i < plen; i++) {
        if (path[i] != prefix[i])   /* also fails if path ended early (NUL) */
            return 0;
    }
    after = path[plen];
    if (after == '\0')
        return 1;                        /* exact match */
    if (plen > 0 && prefix[plen - 1] == '\\')
        return 1;                        /* prefix already ends at a separator */
    if (after == '\\')
        return 1;                        /* child at a component boundary */
    return 0;                            /* e.g. C:\ALLOWEDEVIL vs C:\ALLOWED */
}

int unveil_list_covers(const unveil_entry_t *list, const char *folded_path,
                       uint8_t want)
{
    const unveil_entry_t *best = (const unveil_entry_t *)0;
    uint32_t best_len = 0;
    const unveil_entry_t *e;

    if (!list)
        return 1;   /* task never unveiled -> full filesystem visible */

    /* Longest (most specific) covering entry wins, mirroring OpenBSD. */
    for (e = list; e; e = e->next) {
        uint32_t elen = p_strlen(e->path, VFS_MAX_PATH);
        if (!p_prefix_covers(e->path, elen, folded_path))
            continue;
        if (best == (const unveil_entry_t *)0 || elen >= best_len) {
            best_len = elen;
            best = e;
        }
    }

    if (!best)
        return 0;   /* no unveiled subtree contains the path */
    return (best->perms & want) == want;
}

/* ---- Live (task-aware) API ---------------------------------------------- */

int pledge_user_mode(void)
{
    return ssdt_previous_mode() == SSDT_USER_MODE;
}

NTSTATUS pledge_apply(struct task *t, uint64_t requested)
{
    uint64_t cur, next;

    if (!t)
        return STATUS_INVALID_PARAMETER;

    do {
        cur = __atomic_load_n(&t->pledge_mask, __ATOMIC_ACQUIRE);
        next = pledge_tighten(cur, requested);
    } while (!__atomic_compare_exchange_n(&t->pledge_mask, &cur, next,
                                          0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return STATUS_SUCCESS;
}

NTSTATUS pledge_check_syscall(struct task *t, uint32_t table_id,
                              uint32_t index, uint32_t service_number)
{
    uint64_t mask, required;

    (void)index;   /* service_number already encodes the index */

    if (!t)
        return STATUS_SUCCESS;
    mask = __atomic_load_n(&t->pledge_mask, __ATOMIC_ACQUIRE);
    if ((mask & PLEDGE_PLEDGED) == 0)
        return STATUS_SUCCESS;   /* not pledged */

    /* Shadow (win32k/GUI) table has no pledge category -> fail closed. */
    if (table_id != SSDT_TABLE_MAIN)
        return STATUS_PLEDGE_VIOLATION;

    required = pledge_syscall_category(service_number);
    return pledge_is_allowed(mask, required) ? STATUS_SUCCESS
                                             : STATUS_PLEDGE_VIOLATION;
}

/* Classify a legacy INT 0x80 SYS_* number into a required pledge category.
 * Mirrors pledge_syscall_category for the second (Linux-style) ring-3 ABI so
 * the same restriction holds on both entry paths. File SYS_* (openfile/
 * readfile/readdir) return CORE and are fine-checked inside their handlers. */
static uint64_t pledge_legacy_category(uint64_t sys_nr)
{
    switch (sys_nr) {
    /* Genuine survival operations only, matching the SSDT CORE set: exit, waits,
     * own-memory management, self queries, logging, the ABI handshake, close.
     * IPC (pipe/shmem), object-manager (opendirobj/querydirobj), fault injection,
     * process enumeration, and system control (reboot/shutdown) are NOT core --
     * they fall through to fail-closed DENY so an empty pledge cannot use them,
     * matching the SSDT classifier's treatment of their equivalents. */
    case SYS_EXIT:  case SYS_YIELD:  case SYS_WAITPID: case SYS_GETPID:
    case SYS_UPTIME: case SYS_LOG:   case SYS_SIGNAL:  case SYS_ABI_HANDSHAKE:
    case SYS_CLOSEHANDLE: case SYS_MMAP: case SYS_MUNMAP: case SYS_UNMAPVIEW:
    /* Reporting your own result is survival, on the same footing as
     * SYS_LOG: it writes three counters on the caller's OWN TCB and
     * reaches nothing else. It has to be CORE because UTEST_END sits in
     * the footer of EVERY test binary -- leaving it to the fail-closed
     * default would make pledge_terminate kill any binary that pledged,
     * at the exact moment it tried to report what it had proved. Unlike
     * SYS_FAULT_INJECT (deliberately DENY) it grants no reach outside
     * the caller. */
    case SYS_TEST_REPORT:
    /* File SYS_* reach the handler for the fine rpath/wpath + unveil check. */
    case SYS_OPENFILE: case SYS_READFILE: case SYS_READDIR:
        return PLEDGE_REQ_CORE;

    case SYS_WRITE: case SYS_READ: case SYS_READHANDLE: case SYS_WRITEHANDLE:
        return PLEDGE_STDIO;

    case SYS_FORK: case SYS_KILL:
        return PLEDGE_PROC;

    case SYS_EXEC:
        return PLEDGE_EXEC;

    case SYS_PING: case SYS_NETINFO:
        return PLEDGE_INET;

    default:
        return PLEDGE_REQ_DENY;   /* fail-closed */
    }
}

NTSTATUS pledge_check_legacy(struct task *t, uint64_t sys_nr)
{
    uint64_t mask, required;

    if (!t)
        return STATUS_SUCCESS;
    mask = __atomic_load_n(&t->pledge_mask, __ATOMIC_ACQUIRE);
    if ((mask & PLEDGE_PLEDGED) == 0)
        return STATUS_SUCCESS;

    required = pledge_legacy_category(sys_nr);
    return pledge_is_allowed(mask, required) ? STATUS_SUCCESS
                                             : STATUS_PLEDGE_VIOLATION;
}

NTSTATUS pledge_check_file(struct task *t, uint32_t vfs_flags)
{
    uint64_t mask, need;

    if (!t)
        return STATUS_SUCCESS;
    mask = __atomic_load_n(&t->pledge_mask, __ATOMIC_ACQUIRE);
    if ((mask & PLEDGE_PLEDGED) == 0)
        return STATUS_SUCCESS;

    need = pledge_file_categories(vfs_flags);
    if ((need & ~mask) != 0) {
        struct thread *th = thread_current();
        if (th)
            th->pledge_pending = 1;   /* provenance on THIS thread (see ssdt tail) */
        return STATUS_PLEDGE_VIOLATION;
    }
    return STATUS_SUCCESS;
}

NTSTATUS pledge_check_setinfo(struct task *t, uint32_t info_class)
{
    uint64_t mask, need;

    if (!t)
        return STATUS_SUCCESS;
    mask = __atomic_load_n(&t->pledge_mask, __ATOMIC_ACQUIRE);
    if ((mask & PLEDGE_PLEDGED) == 0)
        return STATUS_SUCCESS;

    need = pledge_setinfo_categories(info_class);
    if ((need & ~mask) != 0) {
        struct thread *th = thread_current();
        if (th)
            th->pledge_pending = 1;   /* provenance on THIS thread (see ssdt tail) */
        return STATUS_PLEDGE_VIOLATION;
    }
    return STATUS_SUCCESS;
}

void pledge_terminate(uint32_t service_number)
{
    struct task *t = task_current();

    klog(LOG_WARN, "pledge",
         "pledge violation -- syscall 0x%X not in pledge set (pid %u), terminating",
         (uint64_t)service_number, (uint64_t)(t ? t->pid : 0));

    task_exit((int32_t)STATUS_PLEDGE_VIOLATION);
    /* task_exit never returns (yields forever); loop to honor noreturn. */
    for (;;) { }
}

/* ---- Unveil live API ---------------------------------------------------- */

NTSTATUS unveil_add(struct task *t, const char *canon_path, uint8_t perms)
{
    char folded[VFS_MAX_PATH];
    unveil_entry_t *e, *x;
    uint64_t irq;

    if (!t || !canon_path)
        return STATUS_INVALID_PARAMETER;

    vfs_path_fold(canon_path, folded, sizeof(folded));

    /* Allocate the clone BEFORE taking the leaf lock (no alloc under spinlock);
     * free it on the lose paths (locked, or a duplicate replace). */
    e = (unveil_entry_t *)kmalloc(sizeof(*e));
    if (!e)
        return STATUS_INSUFFICIENT_RESOURCES;
    p_strlcpy(e->path, folded, VFS_MAX_PATH);
    e->perms = perms & UNVEIL_PERM_MASK;
    e->next = (unveil_entry_t *)0;

    spin_lock_irqsave(&t->unveil_lock, &irq);
    if (t->unveil_locked) {
        spin_unlock_irqrestore(&t->unveil_lock, irq);
        kfree(e);
        return STATUS_ACCESS_DENIED;
    }
    {
        uint32_t count = 0;
        for (x = t->unveil_list; x; x = x->next)
            count++;
        if (count >= UNVEIL_MAX_ENTRIES) {
            /* Bound the IRQ-off unveil_check scan; a distinct new path past the
             * cap is refused (existing paths can still be re-unveiled to tighten). */
            uint8_t is_dup = 0;
            for (x = t->unveil_list; x; x = x->next)
                if (p_streq(x->path, folded)) { is_dup = 1; break; }
            if (!is_dup) {
                spin_unlock_irqrestore(&t->unveil_lock, irq);
                kfree(e);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
    }
    for (x = t->unveil_list; x; x = x->next) {
        if (p_streq(x->path, folded)) {
            /* Tighten-only: re-unveiling an existing path can only narrow its
             * permissions (intersect), never restore a dropped w/x/c bit --
             * unveil is irreversible per the section contract. Atomic store: a
             * lock-free inherit clone may read this byte on another CPU. */
            uint8_t narrowed = (uint8_t)(x->perms & perms & UNVEIL_PERM_MASK);
            __atomic_store_n(&x->perms, narrowed, __ATOMIC_RELEASE);
            t->unveil_gen++;                       /* mutation: invalidate clones */
            __atomic_store_n(&t->unveil_active, 1, __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&t->unveil_lock, irq);
            kfree(e);
            return STATUS_SUCCESS;
        }
    }
    /* Publish with release so a lock-free inherit-clone reader on another CPU
     * sees a fully initialized entry. */
    e->next = t->unveil_list;
    __atomic_store_n(&t->unveil_list, e, __ATOMIC_RELEASE);
    t->unveil_gen++;
    __atomic_store_n(&t->unveil_active, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&t->unveil_lock, irq);
    return STATUS_SUCCESS;
}

void unveil_lock(struct task *t)
{
    uint64_t irq;
    if (!t)
        return;
    spin_lock_irqsave(&t->unveil_lock, &irq);
    t->unveil_locked = 1;
    /* Locking activates the restriction even with an empty list (deny-all). */
    __atomic_store_n(&t->unveil_active, 1, __ATOMIC_RELEASE);
    /* Bump the generation: locking is a policy change, so an in-flight
     * inheritance clone that snapshotted the pre-lock state must retry (else a
     * fork racing NtUnveil(NULL,NULL) could inherit the stale unlocked policy). */
    t->unveil_gen++;
    spin_unlock_irqrestore(&t->unveil_lock, irq);
}

NTSTATUS unveil_check(struct task *t, const char *resolved_path, uint8_t want)
{
    char folded[VFS_MAX_PATH];
    uint64_t irq;
    int ok;

    if (!t || !resolved_path)
        return STATUS_SUCCESS;

    /* Never unveiled -> full filesystem visible. An ACTIVE but empty set (locked
     * before any add) denies all, so gate on unveil_active, not the list. */
    if (!__atomic_load_n(&t->unveil_active, __ATOMIC_ACQUIRE))
        return STATUS_SUCCESS;

    vfs_path_fold(resolved_path, folded, sizeof(folded));

    spin_lock_irqsave(&t->unveil_lock, &irq);
    ok = t->unveil_list ? unveil_list_covers(t->unveil_list, folded, want) : 0;
    spin_unlock_irqrestore(&t->unveil_lock, irq);

    return ok ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
}

/* ---- Lifecycle ---------------------------------------------------------- */

int pledge_unveil_inherit(struct task *child, struct task *parent)
{
    unveil_entry_t *src;

    if (!child)
        return 0;

    /* Always start the child from a clean slot before deciding inheritance.
     * (pledge_pending is per-thread and reset at thread-slot init in task.c.) */
    child->pledge_mask = 0;
    child->unveil_list = (unveil_entry_t *)0;
    child->unveil_locked = 0;
    child->unveil_active = 0;

    if (!parent)
        return 0;

    child->pledge_mask = __atomic_load_n(&parent->pledge_mask, __ATOMIC_ACQUIRE);

    /* Clone the parent's unveil list into a CONSISTENT snapshot. The head, the
     * locked flag, and the mutation generation are read together under the
     * parent lock (linearizable: a fork racing the parent's first add+lock can
     * never see head==NULL yet locked==1). The clone walk then runs outside the
     * lock (no alloc under a leaf lock); afterwards the generation is re-read --
     * if unveil_add replaced any entry's perms mid-walk, the clone may mix pre-
     * and post-mutation state, so we discard it and retry. Bounded retries fail
     * the fork CLOSED rather than publish an inconsistent child policy. */
    {
        int attempt;
        for (attempt = 0; attempt < 16; attempt++) {
            uint64_t irq;
            uint32_t gen0, gen1;

            spin_lock_irqsave(&parent->unveil_lock, &irq);
            src = parent->unveil_list;
            child->unveil_locked = parent->unveil_locked;
            child->unveil_active = parent->unveil_active;
            gen0 = parent->unveil_gen;
            spin_unlock_irqrestore(&parent->unveil_lock, irq);

            for (; src; src = src->next) {
                unveil_entry_t *c = (unveil_entry_t *)kmalloc(sizeof(*c));
                if (!c) {
                    pledge_unveil_teardown(child);   /* fail child creation closed */
                    return -1;
                }
                p_strlcpy(c->path, src->path, VFS_MAX_PATH);
                /* Acquire-load the mutable perms byte (unveil_add stores it with
                 * release); the generation retry then rejects a logically mixed
                 * snapshot. Paths/links are immutable, so a plain copy is fine. */
                c->perms = __atomic_load_n(&src->perms, __ATOMIC_ACQUIRE);
                c->next = child->unveil_list;
                child->unveil_list = c;
            }

            spin_lock_irqsave(&parent->unveil_lock, &irq);
            gen1 = parent->unveil_gen;
            spin_unlock_irqrestore(&parent->unveil_lock, irq);
            if (gen1 == gen0)
                return 0;   /* consistent snapshot */

            /* Mutation raced the clone: discard and retry. */
            pledge_unveil_teardown(child);
            child->pledge_mask = __atomic_load_n(&parent->pledge_mask,
                                                 __ATOMIC_ACQUIRE);
        }
        pledge_unveil_teardown(child);
        return -1;   /* pathological mutation storm: fail closed */
    }
}

void pledge_unveil_teardown(struct task *t)
{
    unveil_entry_t *e;

    if (!t)
        return;

    e = t->unveil_list;
    t->unveil_list = (unveil_entry_t *)0;
    t->unveil_locked = 0;
    t->unveil_active = 0;
    t->pledge_mask = 0;
    while (e) {
        unveil_entry_t *n = e->next;
        kfree(e);
        e = n;
    }
}

/* ---- Syscall handlers --------------------------------------------------- */

/* Parse a "rwxc" permission string into UNVEIL_* bits. -1 on unknown char. */
static int unveil_parse_perms(const char *s, uint8_t *out)
{
    uint8_t p = 0;
    uint32_t i;
    for (i = 0; s[i]; i++) {
        switch (s[i]) {
        case 'r': case 'R': p |= UNVEIL_R; break;
        case 'w': case 'W': p |= UNVEIL_W; break;
        case 'x': case 'X': p |= UNVEIL_X; break;
        case 'c': case 'C': p |= UNVEIL_C; break;
        default: return -1;
        }
    }
    *out = p;
    return 0;
}

/* NtPledge(UNICODE_STRING *Promises) -- SSDT 0x03DB. Tighten-only. */
static NTSTATUS NtPledge_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    const UNICODE_STRING *promises = (const UNICODE_STRING *)a1;
    uint32_t prev = ssdt_previous_mode();
    uint16_t wbuf[128];
    char abuf[128];
    uint32_t wchars = 0;
    uint64_t reqmask = 0;
    NTSTATUS st;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!promises)
        return STATUS_INVALID_PARAMETER;

    st = nt_decode_unicode_string(promises, wbuf, 128, &wchars, prev);
    if (st != STATUS_SUCCESS)
        return st;
    st = nt_unicode_to_ascii(wbuf, wchars, abuf, sizeof(abuf), (uint32_t *)0);
    if (st != STATUS_SUCCESS)
        return st;
    if (pledge_parse(abuf, &reqmask) != 0)
        return STATUS_INVALID_PARAMETER;

    return pledge_apply(task_current(), reqmask);
}

/* NtUnveil(UNICODE_STRING *Path, UNICODE_STRING *Permissions) -- SSDT 0x03DC.
 * NtUnveil(NULL, NULL) locks the set. Irreversible once locked. */
static NTSTATUS NtUnveil_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    const UNICODE_STRING *upath = (const UNICODE_STRING *)a1;
    const UNICODE_STRING *uperm = (const UNICODE_STRING *)a2;
    uint32_t prev = ssdt_previous_mode();
    uint16_t wpath[VFS_MAX_PATH];
    uint16_t wperm[16];
    char rawpath[VFS_MAX_PATH];
    char resolved[VFS_MAX_PATH];
    char permbuf[16];
    uint32_t wc = 0, wpc = 0;
    uint8_t perms = 0;
    NTSTATUS st;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!upath && !uperm) {
        unveil_lock(task_current());   /* NtUnveil(NULL, NULL) -> lock set */
        return STATUS_SUCCESS;
    }
    if (!upath || !uperm)
        return STATUS_INVALID_PARAMETER;

    st = nt_decode_unicode_string(upath, wpath, VFS_MAX_PATH, &wc, prev);
    if (st != STATUS_SUCCESS)
        return st;
    st = nt_unicode_to_ascii(wpath, wc, rawpath, sizeof(rawpath), (uint32_t *)0);
    if (st != STATUS_SUCCESS)
        return st;
    st = nt_decode_unicode_string(uperm, wperm, 16, &wpc, prev);
    if (st != STATUS_SUCCESS)
        return st;
    st = nt_unicode_to_ascii(wperm, wpc, permbuf, sizeof(permbuf), (uint32_t *)0);
    if (st != STATUS_SUCCESS)
        return st;
    if (unveil_parse_perms(permbuf, &perms) != 0)
        return STATUS_INVALID_PARAMETER;

    /* Canonicalize against the caller's cwd (absolute, "." / ".." collapsed). */
    if (task_resolve_path(rawpath, resolved, sizeof(resolved)) != 0)
        return STATUS_OBJECT_PATH_INVALID;

    return unveil_add(task_current(), resolved, perms);
}

void pledge_register_ssdt(void)
{
    ssdt_register(SSDT_NtPledge, (SSDT_HANDLER)NtPledge_handler);
    ssdt_register(SSDT_NtUnveil, (SSDT_HANDLER)NtUnveil_handler);
    klog(LOG_INFO, "pledge", "pledge/unveil: 2 NtXxx handlers registered");
}
