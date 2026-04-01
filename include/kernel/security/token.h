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

/* --- Token information classes (for NtQueryInformationToken) ------------- */

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
    TokenIntegrityLevel        = 25,
    TokenElevationTypeInfo     = 18,
    TokenLinkedToken           = 19,
    TokenIsElevatedInfo        = 20,
} TOKEN_INFORMATION_CLASS;
