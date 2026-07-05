/* ============================================================================
 * privileges.c -- Privilege LUID table and debug helpers
 *
 * Privilege constants and PRIVILEGE_SET utilities for the security subsystem.
 * ============================================================================ */

#include "kernel/security/privileges.h"
#include "kernel/security/token.h"   /* ACCESS_TOKEN, Privileges[] */
#include "kernel/sched/task.h"       /* task_current / thread_current, ->token */

extern int snprintf(char *buf, size_t size, const char *fmt, ...);

/* --- Named LUID constants ------------------------------------------------ */

const LUID SeCreateTokenPrivilege          = SE_CREATE_TOKEN_PRIVILEGE;
const LUID SeAssignPrimaryTokenPrivilege   = SE_ASSIGNPRIMARYTOKEN_PRIVILEGE;
const LUID SeLockMemoryPrivilege           = SE_LOCK_MEMORY_PRIVILEGE;
const LUID SeIncreaseQuotaPrivilege        = SE_INCREASE_QUOTA_PRIVILEGE;
const LUID SeTcbPrivilege                  = SE_TCB_PRIVILEGE;
const LUID SeSecurityPrivilege             = SE_SECURITY_PRIVILEGE;
const LUID SeTakeOwnershipPrivilege        = SE_TAKE_OWNERSHIP_PRIVILEGE;
const LUID SeLoadDriverPrivilege           = SE_LOAD_DRIVER_PRIVILEGE;
const LUID SeSystemProfilePrivilege        = SE_SYSTEM_PROFILE_PRIVILEGE;
const LUID SeSystemtimePrivilege           = SE_SYSTEMTIME_PRIVILEGE;
const LUID SeProfileSingleProcessPrivilege = SE_PROF_SINGLE_PROCESS_PRIVILEGE;
const LUID SeIncreaseBasePriorityPrivilege = SE_INC_BASE_PRIORITY_PRIVILEGE;
const LUID SeCreatePagefilePrivilege       = SE_CREATE_PAGEFILE_PRIVILEGE;
const LUID SeBackupPrivilege               = SE_BACKUP_PRIVILEGE;
const LUID SeRestorePrivilege              = SE_RESTORE_PRIVILEGE;
const LUID SeShutdownPrivilege             = SE_SHUTDOWN_PRIVILEGE;
const LUID SeDebugPrivilege                = SE_DEBUG_PRIVILEGE;
const LUID SeAuditPrivilege                = SE_AUDIT_PRIVILEGE;
const LUID SeChangeNotifyPrivilege         = SE_CHANGE_NOTIFY_PRIVILEGE;
const LUID SeUndockPrivilege               = SE_UNDOCK_PRIVILEGE;
const LUID SeManageVolumePrivilege         = SE_MANAGE_VOLUME_PRIVILEGE;
const LUID SeImpersonatePrivilege          = SE_IMPERSONATE_PRIVILEGE;
const LUID SeCreateGlobalPrivilege         = SE_CREATE_GLOBAL_PRIVILEGE;
const LUID SeCreateSymbolicLinkPrivilege   = SE_CREATE_SYMBOLIC_LINK_PRIVILEGE;

/* --- Name lookup table --------------------------------------------------- */

static const struct {
    uint32_t    low_part;
    const char *name;
} g_priv_names[] = {
    {  2, "SeCreateTokenPrivilege"          },
    {  3, "SeAssignPrimaryTokenPrivilege"   },
    {  4, "SeLockMemoryPrivilege"           },
    {  5, "SeIncreaseQuotaPrivilege"        },
    {  7, "SeTcbPrivilege"                  },
    {  8, "SeSecurityPrivilege"             },
    {  9, "SeTakeOwnershipPrivilege"        },
    { 10, "SeLoadDriverPrivilege"           },
    { 11, "SeSystemProfilePrivilege"        },
    { 12, "SeSystemtimePrivilege"           },
    { 13, "SeProfileSingleProcessPrivilege" },
    { 14, "SeIncreaseBasePriorityPrivilege" },
    { 15, "SeCreatePagefilePrivilege"       },
    { 17, "SeBackupPrivilege"               },
    { 18, "SeRestorePrivilege"              },
    { 19, "SeShutdownPrivilege"             },
    { 20, "SeDebugPrivilege"                },
    { 21, "SeAuditPrivilege"                },
    { 23, "SeChangeNotifyPrivilege"         },
    { 25, "SeUndockPrivilege"               },
    { 28, "SeManageVolumePrivilege"         },
    { 29, "SeImpersonatePrivilege"          },
    { 30, "SeCreateGlobalPrivilege"         },
    { 35, "SeCreateSymbolicLinkPrivilege"   },
    {  0, (const char *)0                   },
};

const char *RtlPrivilegeLuidToName(const LUID *luid)
{
    uint32_t i;

    if (!luid || luid->HighPart != 0)
        return (const char *)0;

    for (i = 0; g_priv_names[i].name; i++) {
        if (g_priv_names[i].low_part == luid->LowPart)
            return g_priv_names[i].name;
    }

    return (const char *)0;
}

/* --- Debug formatter ----------------------------------------------------- */

int RtlPrivilegeSetToString(const PRIVILEGE_SET *ps, char *buf, uint32_t len)
{
    uint32_t pos = 0;
    uint32_t i;
    int written;

    if (!ps || !buf || len < 4)
        return -1;

    /* Clamp to buffer: if caller passes a very small len, the snprintf
     * truncation path must not write past the buffer. Early return for
     * buffers too small to hold even a truncated privilege name. */
    if (len < 8) {
        buf[0] = '\0';
        return 0;
    }

    buf[0] = '\0';

    for (i = 0; i < ps->PrivilegeCount && pos < len - 1; i++) {
        const char *name = RtlPrivilegeLuidToName(&ps->Privilege[i].Luid);
        uint32_t remaining = len - pos;
        char flag;

        if (ps->Privilege[i].Attributes & SE_PRIVILEGE_REMOVED)
            flag = 'R';
        else if (ps->Privilege[i].Attributes & SE_PRIVILEGE_ENABLED)
            flag = 'E';
        else
            flag = 'D';

        if (name) {
            written = snprintf(buf + pos, remaining, "%s%s(%c)",
                               (i > 0) ? " " : "", name, flag);
        } else {
            written = snprintf(buf + pos, remaining, "%sPriv%u(%c)",
                               (i > 0) ? " " : "",
                               ps->Privilege[i].Luid.LowPart, flag);
        }

        if (written < 0 || (uint32_t)written >= remaining)
            break;
        pos += (uint32_t)written;
    }

    return (int)pos;
}

/* --- Privilege checks (SePrivilegeCheck) --------------------------------- */

/* Effective token: the current thread's impersonation token if it has one,
 * else the current task's primary token. Interim resolution shared with mic.c
 * (SeQuerySubjectContextToken / SECURITY_SUBJECT_CONTEXT are owned by the
 * SeAccessCheck section). No Ob reference is taken and no per-token lock is
 * held -- both are the accepted single-cursor-safe SMP gaps tracked against the
 * per-token-lock work; a torn read cannot occur on the flat scheduler today. */
static ACCESS_TOKEN *sep_effective_token(void)
{
    struct thread *thr = thread_current();
    struct task   *cur = task_current();
    ACCESS_TOKEN  *tok = thr ? (ACCESS_TOKEN *)thr->impersonation_token
                             : (ACCESS_TOKEN *)0;

    if (!tok)
        tok = cur ? (ACCESS_TOKEN *)cur->token : (ACCESS_TOKEN *)0;
    return tok;
}

/* True iff `tok` holds `luid` with SE_PRIVILEGE_ENABLED. Fails closed on a
 * PrivilegeCount above the fixed Privileges[TOKEN_MAX_PRIVS] backing array: a
 * corrupt/over-large count is a malformed token, and scanning past the array
 * would be an out-of-bounds read on the auth-critical path. */
static int sep_token_holds(const ACCESS_TOKEN *tok, const LUID *luid)
{
    uint32_t i;

    if (!tok || !luid || tok->PrivilegeCount > TOKEN_MAX_PRIVS)
        return 0;
    for (i = 0; i < tok->PrivilegeCount; i++) {
        if (RtlEqualLuid(&tok->Privileges[i].Luid, luid) &&
            (tok->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED))
            return 1;
    }
    return 0;
}

int SePrivilegeCheck(PRIVILEGE_SET *ps, uint32_t access_mode)
{
    ACCESS_TOKEN *tok;
    uint32_t i, held = 0;
    int all_necessary;

    if (access_mode == SE_KERNEL_MODE)
        return 1;                       /* kernel code is trusted */
    if (!ps)
        return 0;                       /* fail closed */

    /* Resolve the effective token FIRST and deny on absence: a UserMode subject
     * with no token fails closed for EVERY check, including a degenerate empty
     * ALL_NECESSARY set (otherwise an empty/malformed set would authorize a
     * token-less subject -- a fail-open edge). */
    tok = sep_effective_token();
    if (!tok)
        return 0;                       /* no effective token -> deny */

    /* Fail closed on an implausibly large privilege count: a set requesting more
     * than the maximum a subject can hold is malformed, and iterating (and
     * writing SE_PRIVILEGE_USED_FOR_ACCESS into) ps->Privilege[] past its
     * backing storage would be an out-of-bounds read/write. Any user-sourced
     * PRIVILEGE_SET must also be byte-length-validated by its caller before
     * reaching this primitive. */
    if (ps->PrivilegeCount > TOKEN_MAX_PRIVS)
        return 0;

    all_necessary = (ps->Control & PRIVILEGE_SET_ALL_NECESSARY) != 0;

    /* With a token present, an empty ALL_NECESSARY set is vacuously satisfied;
     * an empty ANY set holds nothing (matches Windows: empty passes only for
     * ALL_NECESSARY). */
    if (ps->PrivilegeCount == 0)
        return all_necessary ? 1 : 0;

    for (i = 0; i < ps->PrivilegeCount; i++) {
        if (sep_token_holds(tok, &ps->Privilege[i].Luid)) {
            /* Windows marks the privileges actually used for the access. */
            ps->Privilege[i].Attributes |= SE_PRIVILEGE_USED_FOR_ACCESS;
            held++;
        }
    }

    return all_necessary ? (held == ps->PrivilegeCount) : (held > 0);
}

int SeSinglePrivilegeCheck(const LUID *privilege, uint32_t access_mode)
{
    if (access_mode == SE_KERNEL_MODE)
        return 1;
    if (!privilege)
        return 0;
    return sep_token_holds(sep_effective_token(), privilege);
}
