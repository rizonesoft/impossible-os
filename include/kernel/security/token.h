/* ============================================================================
 * token.h — ACCESS_TOKEN kernel object
 *
 * The token represents the security context of a process or thread:
 * who the user is (SID), what groups they belong to, what privileges
 * they hold, and what integrity level they run at.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/security/sid.h"
#include "kernel/security/luid.h"
#include "kernel/security/privileges.h"
#include "kernel/security/acl.h"
#include "kernel/ob/handle_table.h"

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
} ACCESS_TOKEN;

/* --- Token Ob type ------------------------------------------------------- */

void ob_token_type_init(void);

/* --- Token creation ------------------------------------------------------ */

/*
 * SeCreateSystemToken — create the initial SYSTEM token (Phase 0).
 * UserSid = SeLocalSystemSid, all privileges enabled, IL = System.
 * Returns an Ob-allocated ACCESS_TOKEN body pointer, or NULL.
 */
ACCESS_TOKEN *SeCreateSystemToken(void);

/*
 * SeCreateUserToken — create a primary token for interactive logon.
 * user_sid: the user's SID.
 * admin:    if non-zero, creates a High IL admin token; otherwise Medium IL.
 * Returns an Ob-allocated ACCESS_TOKEN body pointer, or NULL.
 */
ACCESS_TOKEN *SeCreateUserToken(const SID *user_sid, int admin);

/* --- Token mutation functions -------------------------------------------- */

#define STATUS_NOT_ALL_ASSIGNED ((int32_t)0x00000106)

/*
 * NtDuplicateToken — deep-copy token, allocate new TokenId.
 * If effective_only is set, disabled privileges and groups are stripped.
 */
ACCESS_TOKEN *NtDuplicateToken(const ACCESS_TOKEN *existing,
                               uint32_t desired_access,
                               int effective_only,
                               TOKEN_TYPE new_type);

/*
 * NtAdjustPrivilegesToken — enable/disable/remove privileges.
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
 * NtAdjustGroupsToken — enable/disable group SIDs.
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

/* --- NTSTATUS codes for token operations -------------------------------- */

#define STATUS_SUCCESS           0
#define STATUS_NO_TOKEN         ((int32_t)0xC000007C)
#define STATUS_BUFFER_TOO_SMALL ((int32_t)0xC0000023)
#define STATUS_INVALID_HANDLE   ((int32_t)0xC0000008)
#define STATUS_INVALID_PARAMETER ((int32_t)0xC000000D)
#define STATUS_INVALID_INFO_CLASS ((int32_t)0xC0000003)

/* --- Token query/open functions ------------------------------------------ */

int32_t NtOpenProcessToken(ACCESS_TOKEN *process_token, HANDLE_TABLE *ht,
                           uint32_t desired_access, HANDLE *out_handle);

int32_t NtOpenThreadToken(ACCESS_TOKEN *impersonation_token, HANDLE_TABLE *ht,
                          uint32_t desired_access, HANDLE *out_handle);

int32_t NtQueryInformationToken(const ACCESS_TOKEN *token,
                                TOKEN_INFORMATION_CLASS info_class,
                                void *buf, uint32_t buf_len,
                                uint32_t *ret_len);
