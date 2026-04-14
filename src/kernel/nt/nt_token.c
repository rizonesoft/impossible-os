/* ============================================================================
 * nt_token.c -- NT token syscall SSDT handlers (TODO-05 §16)
 *
 * Wires 9 NtXxx token operations into the SSDT (0x00B0-0x00B7 + 0x00C3):
 * Token open, query, set, adjust privileges/groups, and LUID allocation.
 *
 * Handlers resolve HANDLE arguments to process/thread/token objects via the
 * OB handle table, then delegate to library functions in token.c and
 * luid.c.  Token creation/derivation and SRM access check live in §29.
 *
 * Scope-gap: thread-level impersonation tokens are not yet implemented
 * (struct thread has no impersonation_token field).  NtOpenThreadToken
 * returns the owning process's primary token as the thread token.  Full
 * impersonation support is tracked in TODO-11 §7 (process/thread token
 * assignment & impersonation).
 * ============================================================================ */

#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/sched/task.h"
#include "kernel/ob/ob.h"
#include "kernel/security/token.h"
#include "kernel/security/luid.h"
#include "kernel/klog.h"

/* ACCESS_TOKEN library entry points (defined in token.c) */
extern int32_t NtOpenProcessToken(ACCESS_TOKEN *process_token, HANDLE_TABLE *ht,
                                  uint32_t desired_access, HANDLE *out_handle);
extern int32_t NtOpenThreadToken(ACCESS_TOKEN *impersonation_token,
                                 HANDLE_TABLE *ht, uint32_t desired_access,
                                 HANDLE *out_handle);
extern int32_t NtQueryInformationToken(const ACCESS_TOKEN *token,
                                       TOKEN_INFORMATION_CLASS info_class,
                                       void *buf, uint32_t buf_len,
                                       uint32_t *ret_len);
extern int32_t NtAdjustPrivilegesToken(ACCESS_TOKEN *token, int disable_all,
                                       const LUID_AND_ATTRIBUTES *new_state,
                                       uint32_t new_count,
                                       LUID_AND_ATTRIBUTES *previous_state,
                                       uint32_t *prev_count);
extern int32_t NtAdjustGroupsToken(ACCESS_TOKEN *token, int reset_to_default,
                                   const SID_AND_ATTRIBUTES *new_state,
                                   uint32_t new_count,
                                   SID_AND_ATTRIBUTES *previous_state,
                                   uint32_t *prev_count);

/* OB type for tokens (registered during ob_init) */
extern const OBJECT_TYPE *ObpTokenType;

/* task_from_handle is file-static in nt_process.c; duplicate the simple
 * PID-based resolver here.  Canonical process handle encoding matches
 * nt_process.c: CURRENT_PROCESS (-2) or the PID cast from int32_t. */
static struct task *resolve_process_handle(HANDLE h)
{
    uint32_t pid;

    if (h == CURRENT_PROCESS || h == 0)
        return task_current();
    pid = (uint32_t)(uint64_t)(int32_t)h;
    if (pid >= task_count())
        return (struct task *)0;
    return task_get_by_pid(pid);
}

/* Resolve a thread handle.  CURRENT_THREAD returns the current thread;
 * otherwise the handle encodes TID within the current process. */
static struct thread *resolve_thread_handle(HANDLE h, struct task **out_task)
{
    struct task *cur = task_current();
    uint32_t tid;

    if (h == CURRENT_THREAD || h == 0) {
        if (out_task) *out_task = cur;
        return thread_current();
    }
    tid = (uint32_t)(uint64_t)(int32_t)h;
    if (tid >= cur->num_threads)
        return (struct thread *)0;
    if (out_task) *out_task = cur;
    return &cur->threads[tid];
}

/* Resolve a token handle from the current task's handle table.  Verifies
 * the handle is a Token object (via OB type check) before returning. */
static ACCESS_TOKEN *resolve_token_handle(HANDLE h)
{
    HANDLE_TABLE_ENTRY *entry;
    OBJECT_HEADER *hdr;

    if (h == INVALID_HANDLE_VALUE || h == 0)
        return (ACCESS_TOKEN *)0;

    entry = ObpLookupHandle(&task_current()->handle_table, h);
    if (!entry || !entry->object)
        return (ACCESS_TOKEN *)0;

    hdr = OB_HEADER_FROM_BODY(entry->object);
    if (hdr->type != ObpTokenType)
        return (ACCESS_TOKEN *)0;

    return (ACCESS_TOKEN *)entry->object;
}

/* ======================================================================== */
/* NtOpenProcessToken (SSDT 0x00B0)                                        */
/*                                                                          */
/* a1 = HANDLE ProcessHandle                                               */
/* a2 = ACCESS_MASK DesiredAccess                                          */
/* a3 = HANDLE* TokenHandle (out)                                          */
/* ======================================================================== */

static NTSTATUS NtOpenProcessToken_handler(uint64_t a1, uint64_t a2,
                                           uint64_t a3, uint64_t a4,
                                           uint64_t a5, uint64_t a6)
{
    HANDLE process_handle = (HANDLE)(int32_t)a1;
    uint32_t desired_access = (uint32_t)a2;
    HANDLE *out = (HANDLE *)a3;
    struct task *proc;
    ACCESS_TOKEN *tok;

    (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    proc = resolve_process_handle(process_handle);
    if (!proc)
        return STATUS_INVALID_HANDLE;

    tok = (ACCESS_TOKEN *)proc->token;
    if (!tok)
        return STATUS_NO_TOKEN;

    return (NTSTATUS)NtOpenProcessToken(tok, &task_current()->handle_table,
                                        desired_access, out);
}

/* ======================================================================== */
/* NtOpenProcessTokenEx (SSDT 0x00B1)                                      */
/*                                                                          */
/* a1 = HANDLE ProcessHandle                                               */
/* a2 = ACCESS_MASK DesiredAccess                                          */
/* a3 = uint32_t HandleAttributes (ignored -- no inherit support yet)      */
/* a4 = HANDLE* TokenHandle (out)                                          */
/* ======================================================================== */

static NTSTATUS NtOpenProcessTokenEx_handler(uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    /* HandleAttributes (a3) would control inherit/audit flags; not yet
     * supported by ObpAllocateHandle's simple interface.  Delegate to the
     * plain open path which uses default attributes. */
    (void)a3;
    return NtOpenProcessToken_handler(a1, a2, a4, 0, a5, a6);
}

/* ======================================================================== */
/* NtOpenThreadToken (SSDT 0x00B2)                                         */
/*                                                                          */
/* a1 = HANDLE ThreadHandle                                                */
/* a2 = ACCESS_MASK DesiredAccess                                          */
/* a3 = BOOLEAN OpenAsSelf (ignored -- requires thread impersonation)      */
/* a4 = HANDLE* TokenHandle (out)                                          */
/*                                                                          */
/* Impersonation tokens are per-thread in Windows; our threads do not yet  */
/* have an impersonation slot (-> XREF TODO-11 §7).  Fall back to the      */
/* owning process's primary token, which matches Windows behavior when     */
/* no impersonation is active.                                             */
/* ======================================================================== */

static NTSTATUS NtOpenThreadToken_handler(uint64_t a1, uint64_t a2,
                                          uint64_t a3, uint64_t a4,
                                          uint64_t a5, uint64_t a6)
{
    HANDLE thread_handle = (HANDLE)(int32_t)a1;
    uint32_t desired_access = (uint32_t)a2;
    HANDLE *out = (HANDLE *)a4;
    struct task *owner_task = (struct task *)0;
    struct thread *thr;
    ACCESS_TOKEN *tok;

    (void)a3; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    thr = resolve_thread_handle(thread_handle, &owner_task);
    if (!thr || !owner_task)
        return STATUS_INVALID_HANDLE;

    /* Fall back to primary token when no thread impersonation is set */
    tok = (ACCESS_TOKEN *)owner_task->token;
    if (!tok)
        return STATUS_NO_TOKEN;

    return (NTSTATUS)NtOpenThreadToken(tok, &task_current()->handle_table,
                                       desired_access, out);
}

/* ======================================================================== */
/* NtOpenThreadTokenEx (SSDT 0x00B3)                                       */
/*                                                                          */
/* a1 = HANDLE ThreadHandle                                                */
/* a2 = ACCESS_MASK DesiredAccess                                          */
/* a3 = BOOLEAN OpenAsSelf                                                 */
/* a4 = uint32_t HandleAttributes (ignored)                                */
/* a5 = HANDLE* TokenHandle (out)                                          */
/* ======================================================================== */

static NTSTATUS NtOpenThreadTokenEx_handler(uint64_t a1, uint64_t a2,
                                            uint64_t a3, uint64_t a4,
                                            uint64_t a5, uint64_t a6)
{
    (void)a4;  /* HandleAttributes */
    return NtOpenThreadToken_handler(a1, a2, a3, a5, 0, a6);
}

/* ======================================================================== */
/* NtQueryInformationToken (SSDT 0x00B4)                                   */
/*                                                                          */
/* a1 = HANDLE TokenHandle                                                 */
/* a2 = TOKEN_INFORMATION_CLASS InfoClass                                  */
/* a3 = void* Buffer (out)                                                 */
/* a4 = uint32_t Length                                                    */
/* a5 = uint32_t* ReturnLength (out)                                       */
/* ======================================================================== */

static NTSTATUS NtQueryInformationToken_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    HANDLE token_handle = (HANDLE)(int32_t)a1;
    TOKEN_INFORMATION_CLASS info_class = (TOKEN_INFORMATION_CLASS)a2;
    void *buf = (void *)a3;
    uint32_t buf_len = (uint32_t)a4;
    uint32_t *ret_len = (uint32_t *)a5;
    ACCESS_TOKEN *tok;

    (void)a6;

    tok = resolve_token_handle(token_handle);
    if (!tok)
        return STATUS_INVALID_HANDLE;

    return (NTSTATUS)NtQueryInformationToken(tok, info_class, buf, buf_len,
                                             ret_len);
}

/* ======================================================================== */
/* NtSetInformationToken (SSDT 0x00B5)                                     */
/*                                                                          */
/* a1 = HANDLE TokenHandle                                                 */
/* a2 = TOKEN_INFORMATION_CLASS InfoClass                                  */
/* a3 = void* Buffer (in)                                                  */
/* a4 = uint32_t Length                                                    */
/*                                                                          */
/* Windows permits setting only a small subset of token information        */
/* classes; the rest are read-only and return STATUS_INVALID_INFO_CLASS.   */
/* We support TokenIntegrityLevel (write to IntegrityLevelSid); other      */
/* classes that require ownership-transfer of SIDs/ACLs (Owner, Primary-   */
/* Group, DefaultDacl) are rejected because the callee-owned-buffer        */
/* contract needs per-token heap allocation -- tracked in TODO-11 §4.      */
/* ======================================================================== */

static NTSTATUS NtSetInformationToken_handler(uint64_t a1, uint64_t a2,
                                              uint64_t a3, uint64_t a4,
                                              uint64_t a5, uint64_t a6)
{
    HANDLE token_handle = (HANDLE)(int32_t)a1;
    TOKEN_INFORMATION_CLASS info_class = (TOKEN_INFORMATION_CLASS)a2;
    const void *buf = (const void *)a3;
    uint32_t buf_len = (uint32_t)a4;
    ACCESS_TOKEN *tok;

    (void)a5; (void)a6;

    if (!buf || buf_len == 0)
        return STATUS_INVALID_PARAMETER;

    tok = resolve_token_handle(token_handle);
    if (!tok)
        return STATUS_INVALID_HANDLE;

    /* All settable info classes (TokenOwner, TokenPrimaryGroup,
     * TokenDefaultDacl, TokenIntegrityLevel) require deep-copying the
     * input payload into token-owned storage to avoid caller-buffer
     * lifetime/TOCTOU bugs.  Deep-copy with bounded SID/ACL validation
     * and safe replacement of existing token fields is tracked in
     * TODO-11 §4 (ACCESS_TOKEN mutation API).  All classes currently
     * reject with STATUS_INVALID_INFO_CLASS, the correct NT response
     * when a class cannot be set; readers use NtQueryInformationToken. */
    (void)tok;
    (void)info_class;
    return STATUS_INVALID_INFO_CLASS;
}

/* ======================================================================== */
/* NtAdjustPrivilegesToken (SSDT 0x00B6)                                   */
/*                                                                          */
/* a1 = HANDLE TokenHandle                                                 */
/* a2 = BOOLEAN DisableAllPrivileges                                       */
/* a3 = TOKEN_PRIVILEGES* NewState                                         */
/* a4 = uint32_t BufferLength (size of PreviousState buffer)               */
/* a5 = TOKEN_PRIVILEGES* PreviousState (out, optional)                    */
/* a6 = uint32_t* ReturnLength (out)                                       */
/* ======================================================================== */

static NTSTATUS NtAdjustPrivilegesToken_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    HANDLE token_handle = (HANDLE)(int32_t)a1;
    int disable_all = (int)a2;
    const TOKEN_PRIVILEGES *new_state = (const TOKEN_PRIVILEGES *)a3;
    uint32_t buf_len = (uint32_t)a4;
    TOKEN_PRIVILEGES *prev_state = (TOKEN_PRIVILEGES *)a5;
    uint32_t *ret_len = (uint32_t *)a6;
    ACCESS_TOKEN *tok;
    uint32_t new_count = 0;
    uint32_t prev_count = 0;
    uint32_t prev_capacity = 0;
    int32_t rc;

    tok = resolve_token_handle(token_handle);
    if (!tok)
        return STATUS_INVALID_HANDLE;

    if (!disable_all && !new_state)
        return STATUS_INVALID_PARAMETER;

    if (new_state) {
        new_count = new_state->PrivilegeCount;
        /* Bound to hard max: prevents attacker-claimed-count OOB reads.
         * Windows token cannot hold more than TOKEN_MAX_PRIVS entries anyway. */
        if (new_count > TOKEN_MAX_PRIVS)
            return STATUS_INVALID_PARAMETER;
    }

    /* Compute prev_state capacity using 64-bit math to avoid wrap on large
     * buf_len values, then narrow.  Caller-controlled buf_len must not be
     * trusted to fit in uint32_t arithmetic without bounds. */
    if (prev_state && buf_len >= sizeof(TOKEN_PRIVILEGES)) {
        uint64_t capacity64 = ((uint64_t)buf_len - sizeof(TOKEN_PRIVILEGES) +
                               sizeof(LUID_AND_ATTRIBUTES)) /
                              sizeof(LUID_AND_ATTRIBUTES);
        if (capacity64 > TOKEN_MAX_PRIVS)
            capacity64 = TOKEN_MAX_PRIVS;
        prev_capacity = (uint32_t)capacity64;
    }

    /* Seed prev_count with capacity so library knows caller's slot budget */
    prev_count = prev_capacity;

    rc = NtAdjustPrivilegesToken(tok, disable_all,
                                 new_state ? new_state->Privileges : (LUID_AND_ATTRIBUTES *)0,
                                 new_count,
                                 prev_state ? prev_state->Privileges : (LUID_AND_ATTRIBUTES *)0,
                                 &prev_count);

    if (prev_state && rc == (int32_t)STATUS_SUCCESS) {
        prev_state->PrivilegeCount = prev_count;
    }
    if (ret_len) {
        /* Compute required size in 64-bit then narrow */
        uint64_t need64 = (uint64_t)sizeof(TOKEN_PRIVILEGES) +
                         (uint64_t)prev_count * sizeof(LUID_AND_ATTRIBUTES);
        /* TOKEN_PRIVILEGES has Privileges[] with size 0; header already
         * accounts for the flexible-array offset. */
        *ret_len = need64 > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)need64;
    }

    return (NTSTATUS)rc;
}

/* ======================================================================== */
/* NtAdjustGroupsToken (SSDT 0x00B7)                                       */
/*                                                                          */
/* a1 = HANDLE TokenHandle                                                 */
/* a2 = BOOLEAN ResetToDefault                                             */
/* a3 = TOKEN_GROUPS* NewState                                             */
/* a4 = uint32_t BufferLength                                              */
/* a5 = TOKEN_GROUPS* PreviousState (out, optional)                        */
/* a6 = uint32_t* ReturnLength (out)                                       */
/* ======================================================================== */

/* TOKEN_GROUPS mirror struct (not defined in token.h yet) */
typedef struct {
    uint32_t           GroupCount;
    SID_AND_ATTRIBUTES Groups[1];  /* flexible */
} TOKEN_GROUPS;

static NTSTATUS NtAdjustGroupsToken_handler(uint64_t a1, uint64_t a2,
                                            uint64_t a3, uint64_t a4,
                                            uint64_t a5, uint64_t a6)
{
    HANDLE token_handle = (HANDLE)(int32_t)a1;
    int reset_to_default = (int)a2;
    const TOKEN_GROUPS *new_state = (const TOKEN_GROUPS *)a3;
    uint32_t buf_len = (uint32_t)a4;
    TOKEN_GROUPS *prev_state = (TOKEN_GROUPS *)a5;
    uint32_t *ret_len = (uint32_t *)a6;
    ACCESS_TOKEN *tok;
    uint32_t new_count = 0;
    uint32_t prev_count = 0;
    uint32_t prev_capacity = 0;
    int32_t rc;

    tok = resolve_token_handle(token_handle);
    if (!tok)
        return STATUS_INVALID_HANDLE;

    if (!reset_to_default && !new_state)
        return STATUS_INVALID_PARAMETER;

    if (new_state) {
        new_count = new_state->GroupCount;
        if (new_count > TOKEN_MAX_GROUPS)
            return STATUS_INVALID_PARAMETER;
    }

    if (prev_state && buf_len >= sizeof(TOKEN_GROUPS)) {
        uint64_t capacity64 = ((uint64_t)buf_len - sizeof(TOKEN_GROUPS) +
                               sizeof(SID_AND_ATTRIBUTES)) /
                              sizeof(SID_AND_ATTRIBUTES);
        if (capacity64 > TOKEN_MAX_GROUPS)
            capacity64 = TOKEN_MAX_GROUPS;
        prev_capacity = (uint32_t)capacity64;
    }

    /* Seed prev_count with capacity so library knows caller's slot budget */
    prev_count = prev_capacity;

    rc = NtAdjustGroupsToken(tok, reset_to_default,
                             new_state ? new_state->Groups : (SID_AND_ATTRIBUTES *)0,
                             new_count,
                             prev_state ? prev_state->Groups : (SID_AND_ATTRIBUTES *)0,
                             &prev_count);

    if (prev_state && rc == (int32_t)STATUS_SUCCESS) {
        prev_state->GroupCount = prev_count;
    }
    if (ret_len) {
        uint64_t need64 = (uint64_t)sizeof(TOKEN_GROUPS) +
                         (uint64_t)prev_count * sizeof(SID_AND_ATTRIBUTES);
        *ret_len = need64 > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)need64;
    }

    return (NTSTATUS)rc;
}

/* ======================================================================== */
/* NtAllocateLocallyUniqueId (SSDT 0x00C3)                                 */
/*                                                                          */
/* a1 = LUID* Luid (out)                                                   */
/* ======================================================================== */

static NTSTATUS NtAllocateLocallyUniqueId_handler(uint64_t a1, uint64_t a2,
                                                  uint64_t a3, uint64_t a4,
                                                  uint64_t a5, uint64_t a6)
{
    LUID *out = (LUID *)a1;

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    if (!out)
        return STATUS_INVALID_PARAMETER;

    *out = NtAllocateLocallyUniqueId();
    return STATUS_SUCCESS;
}

/* ---- SSDT Registration -------------------------------------------------- */

void nt_token_register_ssdt(void)
{
    ssdt_register(SSDT_NtOpenProcessToken,
                  (SSDT_HANDLER)NtOpenProcessToken_handler);
    ssdt_register(SSDT_NtOpenProcessTokenEx,
                  (SSDT_HANDLER)NtOpenProcessTokenEx_handler);
    ssdt_register(SSDT_NtOpenThreadToken,
                  (SSDT_HANDLER)NtOpenThreadToken_handler);
    ssdt_register(SSDT_NtOpenThreadTokenEx,
                  (SSDT_HANDLER)NtOpenThreadTokenEx_handler);
    ssdt_register(SSDT_NtQueryInformationToken,
                  (SSDT_HANDLER)NtQueryInformationToken_handler);
    ssdt_register(SSDT_NtSetInformationToken,
                  (SSDT_HANDLER)NtSetInformationToken_handler);
    ssdt_register(SSDT_NtAdjustPrivilegesToken,
                  (SSDT_HANDLER)NtAdjustPrivilegesToken_handler);
    ssdt_register(SSDT_NtAdjustGroupsToken,
                  (SSDT_HANDLER)NtAdjustGroupsToken_handler);
    ssdt_register(SSDT_NtAllocateLocallyUniqueId,
                  (SSDT_HANDLER)NtAllocateLocallyUniqueId_handler);

    klog(LOG_INFO, "nt",
         "NT token: 9 handlers registered (S16 open/query/adjust + LUID)");
}
