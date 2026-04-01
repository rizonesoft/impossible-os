/* ============================================================================
 * token.c — ACCESS_TOKEN Ob type registration and token creation
 *
 * Implements TODO-11 §4.1 + §4.2.
 * ============================================================================ */

#include "kernel/security/token.h"
#include "kernel/ob/ob.h"
#include "kernel/klog.h"

/* --- Callbacks ----------------------------------------------------------- */

static void token_on_delete(void *body)
{
    (void)body;
    /* Token SID/ACL pointers reference either static well-known SIDs
     * or kmalloc'd copies.  Deep-free of dynamic allocations will be
     * added when per-user SID allocation is implemented. */
}

/* --- Type registration --------------------------------------------------- */

void ob_token_type_init(void)
{
    extern const OBJECT_TYPE *ObpTokenType;

    ObpTokenType = ob_create_type(&(OBJECT_TYPE){
        .name      = "Token",
        .body_size = sizeof(ACCESS_TOKEN),
        .on_close  = NULL,
        .on_delete = token_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,
    });

    if (!ObpTokenType)
        klog(LOG_ERROR, "security", "Failed to register ObpTokenType");
}

/* --- Helper: add a privilege to a token ---------------------------------- */

static void token_add_priv(ACCESS_TOKEN *tok, const LUID *luid, uint32_t attrs)
{
    if (tok->PrivilegeCount >= TOKEN_MAX_PRIVS)
        return;
    tok->Privileges[tok->PrivilegeCount].Luid = *luid;
    tok->Privileges[tok->PrivilegeCount].Attributes = attrs;
    tok->PrivilegeCount++;
}

/* --- Helper: add a group to a token -------------------------------------- */

static void token_add_group(ACCESS_TOKEN *tok, const SID *sid, uint32_t attrs)
{
    if (tok->GroupCount >= TOKEN_MAX_GROUPS)
        return;
    tok->Groups[tok->GroupCount].Sid = (SID *)sid;
    tok->Groups[tok->GroupCount].Attributes = attrs;
    tok->GroupCount++;
}

/* --- SeCreateSystemToken ------------------------------------------------- */

ACCESS_TOKEN *SeCreateSystemToken(void)
{
    ACCESS_TOKEN *tok;
    uint32_t enabled = SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT;

    tok = (ACCESS_TOKEN *)ob_alloc_object(ObpTokenType);
    if (!tok)
        return (ACCESS_TOKEN *)0;

    tok->UserSid          = (SID *)SeLocalSystemSid;
    tok->PrimaryGroup     = (SID *)SeBuiltinAdministratorsSid;
    tok->DefaultDacl      = (ACL *)0;
    tok->TokenType        = TokenPrimary;
    tok->ImpersonationLevel = SecurityImpersonation;
    tok->TokenId          = NtAllocateLocallyUniqueId();
    tok->AuthenticationId = (LUID){ 999, 0 };  /* SYSTEM_LUID */
    tok->ModifiedId       = NtAllocateLocallyUniqueId();
    tok->SessionId        = 0;
    tok->IntegrityLevelSid = (SID *)SeILSystem;
    tok->IntegrityPolicy  = TOKEN_MANDATORY_POLICY_NO_WRITE_UP;
    tok->IsElevated       = 1;
    tok->LinkedTokenId    = (LUID){ 0, 0 };
    tok->ElevationType    = TokenElevationTypeDefault;
    tok->RestrictedSids   = (SID *)0;
    tok->RestrictedSidCount = 0;
    tok->Flags            = 0;

    /* Groups */
    token_add_group(tok, SeBuiltinAdministratorsSid,
                    SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT |
                    SE_GROUP_MANDATORY | SE_GROUP_OWNER);
    token_add_group(tok, SeWorldSid,
                    SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT |
                    SE_GROUP_MANDATORY);

    /* All well-known privileges enabled */
    token_add_priv(tok, &SeCreateTokenPrivilege, enabled);
    token_add_priv(tok, &SeAssignPrimaryTokenPrivilege, enabled);
    token_add_priv(tok, &SeLockMemoryPrivilege, enabled);
    token_add_priv(tok, &SeIncreaseQuotaPrivilege, enabled);
    token_add_priv(tok, &SeTcbPrivilege, enabled);
    token_add_priv(tok, &SeSecurityPrivilege, enabled);
    token_add_priv(tok, &SeTakeOwnershipPrivilege, enabled);
    token_add_priv(tok, &SeLoadDriverPrivilege, enabled);
    token_add_priv(tok, &SeSystemProfilePrivilege, enabled);
    token_add_priv(tok, &SeSystemtimePrivilege, enabled);
    token_add_priv(tok, &SeProfileSingleProcessPrivilege, enabled);
    token_add_priv(tok, &SeIncreaseBasePriorityPrivilege, enabled);
    token_add_priv(tok, &SeCreatePagefilePrivilege, enabled);
    token_add_priv(tok, &SeBackupPrivilege, enabled);
    token_add_priv(tok, &SeRestorePrivilege, enabled);
    token_add_priv(tok, &SeShutdownPrivilege, enabled);
    token_add_priv(tok, &SeDebugPrivilege, enabled);
    token_add_priv(tok, &SeAuditPrivilege, enabled);
    token_add_priv(tok, &SeChangeNotifyPrivilege, enabled);
    token_add_priv(tok, &SeUndockPrivilege, enabled);
    token_add_priv(tok, &SeManageVolumePrivilege, enabled);
    token_add_priv(tok, &SeImpersonatePrivilege, enabled);
    token_add_priv(tok, &SeCreateGlobalPrivilege, enabled);
    token_add_priv(tok, &SeCreateSymbolicLinkPrivilege, enabled);

    klog(LOG_INFO, "security", "SYSTEM token created: %u groups, %u privileges, IL=System",
         (uint64_t)tok->GroupCount, (uint64_t)tok->PrivilegeCount);

    return tok;
}

/* --- SeCreateUserToken --------------------------------------------------- */

ACCESS_TOKEN *SeCreateUserToken(const SID *user_sid, int admin)
{
    ACCESS_TOKEN *tok;
    uint32_t enabled = SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT;
    uint32_t disabled = SE_PRIVILEGE_ENABLED_BY_DEFAULT;  /* present but not enabled */

    if (!user_sid)
        return (ACCESS_TOKEN *)0;

    tok = (ACCESS_TOKEN *)ob_alloc_object(ObpTokenType);
    if (!tok)
        return (ACCESS_TOKEN *)0;

    tok->UserSid          = (SID *)user_sid;
    tok->PrimaryGroup     = (SID *)SeBuiltinUsersSid;
    tok->DefaultDacl      = (ACL *)0;
    tok->TokenType        = TokenPrimary;
    tok->ImpersonationLevel = SecurityImpersonation;
    tok->TokenId          = NtAllocateLocallyUniqueId();
    tok->AuthenticationId = NtAllocateLocallyUniqueId();
    tok->ModifiedId       = NtAllocateLocallyUniqueId();
    tok->SessionId        = 1;
    tok->IntegrityPolicy  = TOKEN_MANDATORY_POLICY_NO_WRITE_UP;
    tok->RestrictedSids   = (SID *)0;
    tok->RestrictedSidCount = 0;
    tok->Flags            = 0;

    /* Groups: everyone + interactive + builtin users */
    token_add_group(tok, SeWorldSid,
                    SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT |
                    SE_GROUP_MANDATORY);
    token_add_group(tok, SeInteractiveSid,
                    SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT |
                    SE_GROUP_MANDATORY);
    token_add_group(tok, SeBuiltinUsersSid,
                    SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT |
                    SE_GROUP_MANDATORY);

    if (admin) {
        /* Admin user: High IL, elevated, admin group */
        tok->IntegrityLevelSid = (SID *)SeILHigh;
        tok->IsElevated        = 1;
        tok->ElevationType     = TokenElevationTypeFull;

        token_add_group(tok, SeBuiltinAdministratorsSid,
                        SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT |
                        SE_GROUP_MANDATORY | SE_GROUP_OWNER);

        /* Admin privileges: some enabled, some disabled by default */
        token_add_priv(tok, &SeChangeNotifyPrivilege, enabled);
        token_add_priv(tok, &SeShutdownPrivilege, enabled);
        token_add_priv(tok, &SeUndockPrivilege, enabled);
        token_add_priv(tok, &SeIncreaseBasePriorityPrivilege, enabled);
        token_add_priv(tok, &SeBackupPrivilege, disabled);
        token_add_priv(tok, &SeRestorePrivilege, disabled);
        token_add_priv(tok, &SeLoadDriverPrivilege, disabled);
        token_add_priv(tok, &SeDebugPrivilege, disabled);
        token_add_priv(tok, &SeSystemtimePrivilege, disabled);
        token_add_priv(tok, &SeSecurityPrivilege, disabled);
        token_add_priv(tok, &SeTakeOwnershipPrivilege, disabled);
        token_add_priv(tok, &SeManageVolumePrivilege, disabled);
        token_add_priv(tok, &SeImpersonatePrivilege, disabled);
        token_add_priv(tok, &SeCreateGlobalPrivilege, disabled);
        token_add_priv(tok, &SeCreateSymbolicLinkPrivilege, disabled);
    } else {
        /* Standard user: Medium IL, not elevated */
        tok->IntegrityLevelSid = (SID *)SeILMedium;
        tok->IsElevated        = 0;
        tok->ElevationType     = TokenElevationTypeDefault;

        /* Standard privileges */
        token_add_priv(tok, &SeChangeNotifyPrivilege, enabled);
        token_add_priv(tok, &SeShutdownPrivilege, enabled);
        token_add_priv(tok, &SeUndockPrivilege, enabled);
    }

    tok->LinkedTokenId = (LUID){ 0, 0 };

    klog(LOG_DEBUG, "security", "User token created: admin=%d, %u groups, %u privs, IL=%s",
         (uint64_t)admin, (uint64_t)tok->GroupCount, (uint64_t)tok->PrivilegeCount,
         admin ? "High" : "Medium");

    return tok;
}
