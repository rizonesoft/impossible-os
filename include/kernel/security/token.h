/* ============================================================================
 * token.h -- ACCESS_TOKEN kernel object
 *
 * The token represents the security context of a process or thread:
 * who the user is (SID), what groups they belong to, what privileges
 * they hold, and what integrity level they run at.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/security/sid.h"
#include "kernel/security/luid.h"
#include "kernel/security/privileges.h"
#include "kernel/security/acl.h"
#include "kernel/ob/handle_table.h"

/* Quota blocks are opaque (kernel/quota/quota.h); a pointer needs no layout. */
struct quota_block;

/* --- Limits -------------------------------------------------------------- */

#define TOKEN_MAX_GROUPS  32
#define TOKEN_MAX_PRIVS   36

/* --- Token type ---------------------------------------------------------- */

typedef enum {
    TokenPrimary       = 1,   /* process token */
    TokenImpersonation = 2,   /* thread impersonation token */
} TOKEN_TYPE;

/* --- Impersonation level ------------------------------------------------- */

typedef enum {
    SecurityAnonymous      = 0,
    SecurityIdentification = 1,
    SecurityImpersonation  = 2,
    SecurityDelegation     = 3,
} SECURITY_IMPERSONATION_LEVEL;

/* --- Elevation type ------------------------------------------------------ */

typedef enum {
    TokenElevationTypeDefault = 1,   /* not split (non-admin user) */
    TokenElevationTypeFull    = 2,   /* full admin token */
    TokenElevationTypeLimited = 3,   /* filtered/limited token */
} TOKEN_ELEVATION_TYPE;

/* --- Mandatory policy flags ---------------------------------------------- */

#define TOKEN_MANDATORY_POLICY_OFF             0x0
#define TOKEN_MANDATORY_POLICY_NO_WRITE_UP     0x1
#define TOKEN_MANDATORY_POLICY_NEW_PROCESS_MIN 0x2

/* --- Token flags --------------------------------------------------------- */

#define TOKEN_IS_RESTRICTED  0x0001

/* --- ACCESS_TOKEN -------------------------------------------------------- */

typedef struct access_token {
    SID                          *UserSid;
    uint32_t                      GroupCount;
    SID_AND_ATTRIBUTES            Groups[TOKEN_MAX_GROUPS];
    uint32_t                      PrivilegeCount;
    LUID_AND_ATTRIBUTES           Privileges[TOKEN_MAX_PRIVS];
    SID                          *PrimaryGroup;
    ACL                          *DefaultDacl;
    TOKEN_TYPE                    TokenType;
    SECURITY_IMPERSONATION_LEVEL  ImpersonationLevel;
    LUID                          TokenId;
    LUID                          AuthenticationId;
    LUID                          ModifiedId;
    uint32_t                      SessionId;
    SID                          *IntegrityLevelSid;
    uint32_t                      IntegrityPolicy;
    uint32_t                      IsElevated;
    LUID                          LinkedTokenId;
    TOKEN_ELEVATION_TYPE          ElevationType;
    SID                          *RestrictedSids;
    uint32_t                      RestrictedSidCount;
    uint32_t                      Flags;
    /* The canonical per-owner-SID quota block (kernel/quota/quota.h). Every
     * token for the same UserSid points at the SAME block, including every
     * duplicate: a per-user budget that a second token lineage could bypass
     * would not be a budget at all. The token holds one reference, released
     * when the token object is deleted. NULL only if the block could not be
     * acquired (allocation failure or a malformed SID). */
    struct quota_block           *QuotaBlock;
} ACCESS_TOKEN;

/* --- Token Ob type ------------------------------------------------------- */

void ob_token_type_init(void);

/* --- Token creation ------------------------------------------------------ */

/*
 * SeCreateSystemToken -- create the initial SYSTEM token (Phase 0).
 * UserSid = SeLocalSystemSid, all privileges enabled, IL = System.
 * Returns an Ob-allocated ACCESS_TOKEN body pointer, or NULL.
 */
ACCESS_TOKEN *SeCreateSystemToken(void);

/*
 * SeCreateUserToken -- create a primary token for interactive logon.
 * user_sid: the user's SID.
 * admin:    if non-zero, creates a High IL admin token; otherwise Medium IL.
 * Returns an Ob-allocated ACCESS_TOKEN body pointer, or NULL.
 */
ACCESS_TOKEN *SeCreateUserToken(const SID *user_sid, int admin);

/* --- Primary token assignment + thread impersonation --------------------- */

struct task;   /* forward decl -- primary-token API takes a task pointer */

/*
 * PsReferencePrimaryToken -- pin a task's primary token and return it. Takes an
 * Ob reference so the token cannot be freed while the caller holds the pointer;
 * the caller MUST balance it with PsDereferencePrimaryToken. Returns NULL when
 * the task is NULL or has no token assigned yet.
 */
ACCESS_TOKEN *PsReferencePrimaryToken(struct task *task);

/* PsDereferencePrimaryToken -- drop a reference taken by PsReferencePrimaryToken
 * (or by a self-impersonation swap). NULL-safe. */
void PsDereferencePrimaryToken(ACCESS_TOKEN *token);

/*
 * ImpersonateSelf -- duplicate the current thread's process token as an
 * impersonation token at `level` and install it on the current thread. Used
 * before adjusting privileges for a short operation. A prior impersonation
 * token on the thread is dereferenced. Self-only: it swaps the CURRENT thread's
 * own field, so there is no cross-thread writer race in this path.
 */
void ImpersonateSelf(SECURITY_IMPERSONATION_LEVEL level);

/* RevertToSelf -- clear the current thread's impersonation token (revert to the
 * process primary token) and dereference the impersonation token. */
void RevertToSelf(void);

/* --- Token mutation functions -------------------------------------------- */

#define STATUS_NOT_ALL_ASSIGNED ((int32_t)0x00000106)

/*
 * NtDuplicateToken -- deep-copy token, allocate new TokenId.
 * If effective_only is set, disabled privileges and groups are stripped.
 */
ACCESS_TOKEN *NtDuplicateToken(const ACCESS_TOKEN *existing,
                               uint32_t desired_access,
                               int effective_only,
                               TOKEN_TYPE new_type);

/*
 * NtAdjustPrivilegesToken -- enable/disable/remove privileges.
 * If disable_all, disables every privilege. Otherwise walks new_state.
 * Returns STATUS_NOT_ALL_ASSIGNED if any LUID not found in the token.
 * previous_state receives the old state (can be NULL).
 */
int32_t NtAdjustPrivilegesToken(ACCESS_TOKEN *token, int disable_all,
                                const LUID_AND_ATTRIBUTES *new_state,
                                uint32_t new_count,
                                LUID_AND_ATTRIBUTES *previous_state,
                                uint32_t *prev_count);

/*
 * NtAdjustGroupsToken -- enable/disable group SIDs.
 * Cannot re-enable a SE_GROUP_USE_FOR_DENY_ONLY group.
 * If reset_to_default, restores all groups to enabled-by-default state.
 */
int32_t NtAdjustGroupsToken(ACCESS_TOKEN *token, int reset_to_default,
                             const SID_AND_ATTRIBUTES *new_state,
                             uint32_t new_count,
                             SID_AND_ATTRIBUTES *previous_state,
                             uint32_t *prev_count);

/* --- Token information classes ------------------------------------------- */

typedef enum {
    TokenUser                  = 1,
    TokenGroups                = 2,
    TokenPrivileges            = 3,
    TokenOwner                 = 4,
    TokenPrimaryGroup          = 5,
    TokenDefaultDacl           = 6,
    TokenSource                = 7,
    TokenTypeInfo              = 8,
    TokenImpersonationLevelInfo = 9,
    TokenStatistics            = 10,
    TokenElevationTypeInfo     = 18,
    TokenLinkedToken           = 19,
    TokenIsElevatedInfo        = 20,
    TokenIntegrityLevel        = 25,
} TOKEN_INFORMATION_CLASS;

/* --- NTSTATUS codes for token operations --------------------------------
 * Canonical definitions are in kernel/nt/ntstatus.h (included above).
 * STATUS_NO_TOKEN is token-specific and defined here if ntstatus.h does
 * not provide it. */

#ifndef STATUS_NO_TOKEN
#define STATUS_NO_TOKEN         ((int32_t)0xC000007C)
#endif

/* --- Token query/open functions ------------------------------------------ */

int32_t NtOpenProcessToken(ACCESS_TOKEN *process_token, HANDLE_TABLE *ht,
                           uint32_t desired_access, HANDLE *out_handle);

int32_t NtOpenThreadToken(ACCESS_TOKEN *impersonation_token, HANDLE_TABLE *ht,
                          uint32_t desired_access, HANDLE *out_handle);

int32_t NtQueryInformationToken(const ACCESS_TOKEN *token,
                                TOKEN_INFORMATION_CLASS info_class,
                                void *buf, uint32_t buf_len,
                                uint32_t *ret_len);
