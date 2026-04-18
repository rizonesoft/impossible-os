/* ============================================================================
 * token.c -- ACCESS_TOKEN Ob type registration and token creation
 *
 * ACCESS_TOKEN Ob type registration and token creation helpers.
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

/* ============================================================================
 * Token mutation functions (§4.4)
 * ============================================================================ */

/* --- NtDuplicateToken ---------------------------------------------------- */

ACCESS_TOKEN *NtDuplicateToken(const ACCESS_TOKEN *existing,
                               uint32_t desired_access,
                               int effective_only,
                               TOKEN_TYPE new_type)
{
    ACCESS_TOKEN *dup;
    uint32_t i, j;

    (void)desired_access;

    if (!existing)
        return (ACCESS_TOKEN *)0;

    dup = (ACCESS_TOKEN *)ob_alloc_object(ObpTokenType);
    if (!dup)
        return (ACCESS_TOKEN *)0;

    /* Copy scalar fields */
    dup->UserSid          = existing->UserSid;
    dup->PrimaryGroup     = existing->PrimaryGroup;
    dup->DefaultDacl      = existing->DefaultDacl;
    dup->TokenType        = new_type;
    dup->ImpersonationLevel = existing->ImpersonationLevel;
    dup->TokenId          = NtAllocateLocallyUniqueId();  /* new unique ID */
    dup->AuthenticationId = existing->AuthenticationId;
    dup->ModifiedId       = NtAllocateLocallyUniqueId();
    dup->SessionId        = existing->SessionId;
    dup->IntegrityLevelSid = existing->IntegrityLevelSid;
    dup->IntegrityPolicy  = existing->IntegrityPolicy;
    dup->IsElevated       = existing->IsElevated;
    dup->LinkedTokenId    = existing->LinkedTokenId;
    dup->ElevationType    = existing->ElevationType;
    dup->RestrictedSids   = existing->RestrictedSids;
    dup->RestrictedSidCount = existing->RestrictedSidCount;
    dup->Flags            = existing->Flags;

    /* Copy groups -- strip disabled if effective_only */
    j = 0;
    for (i = 0; i < existing->GroupCount && j < TOKEN_MAX_GROUPS; i++) {
        if (effective_only &&
            !(existing->Groups[i].Attributes & SE_GROUP_ENABLED))
            continue;
        dup->Groups[j] = existing->Groups[i];
        j++;
    }
    dup->GroupCount = j;

    /* Copy privileges -- strip disabled if effective_only */
    j = 0;
    for (i = 0; i < existing->PrivilegeCount && j < TOKEN_MAX_PRIVS; i++) {
        if (effective_only &&
            !(existing->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED))
            continue;
        dup->Privileges[j] = existing->Privileges[i];
        j++;
    }
    dup->PrivilegeCount = j;

    return dup;
}

/* --- NtAdjustPrivilegesToken --------------------------------------------- */

int32_t NtAdjustPrivilegesToken(ACCESS_TOKEN *token, int disable_all,
                                const LUID_AND_ATTRIBUTES *new_state,
                                uint32_t new_count,
                                LUID_AND_ATTRIBUTES *previous_state,
                                uint32_t *prev_count)
{
    uint32_t i, j, prev_idx = 0;
    int all_found = 1;

    if (!token)
        return STATUS_INVALID_HANDLE;

    /* DisableAll: disable every privilege */
    if (disable_all) {
        for (i = 0; i < token->PrivilegeCount; i++) {
            if (previous_state && prev_idx < (prev_count ? *prev_count : 0)) {
                previous_state[prev_idx] = token->Privileges[i];
                prev_idx++;
            }
            token->Privileges[i].Attributes &= ~SE_PRIVILEGE_ENABLED;
        }
        token->ModifiedId = NtAllocateLocallyUniqueId();
        if (prev_count) *prev_count = prev_idx;
        return STATUS_SUCCESS;
    }

    if (!new_state)
        return STATUS_INVALID_PARAMETER;

    /* Apply each requested change */
    for (j = 0; j < new_count; j++) {
        int found = 0;
        for (i = 0; i < token->PrivilegeCount; i++) {
            if (RtlEqualLuid(&token->Privileges[i].Luid, &new_state[j].Luid)) {
                /* Save previous state */
                if (previous_state && prev_idx < (prev_count ? *prev_count : 0)) {
                    previous_state[prev_idx] = token->Privileges[i];
                    prev_idx++;
                }
                /* Apply new attributes */
                if (new_state[j].Attributes & SE_PRIVILEGE_REMOVED) {
                    /* Remove: shift remaining privileges down */
                    uint32_t k;
                    for (k = i; k + 1 < token->PrivilegeCount; k++)
                        token->Privileges[k] = token->Privileges[k + 1];
                    token->PrivilegeCount--;
                } else if (new_state[j].Attributes & SE_PRIVILEGE_ENABLED) {
                    token->Privileges[i].Attributes |= SE_PRIVILEGE_ENABLED;
                } else {
                    token->Privileges[i].Attributes &= ~SE_PRIVILEGE_ENABLED;
                }
                found = 1;
                break;
            }
        }
        if (!found)
            all_found = 0;
    }

    token->ModifiedId = NtAllocateLocallyUniqueId();
    if (prev_count) *prev_count = prev_idx;

    return all_found ? STATUS_SUCCESS : STATUS_NOT_ALL_ASSIGNED;
}

/* --- NtAdjustGroupsToken ------------------------------------------------- */

int32_t NtAdjustGroupsToken(ACCESS_TOKEN *token, int reset_to_default,
                             const SID_AND_ATTRIBUTES *new_state,
                             uint32_t new_count,
                             SID_AND_ATTRIBUTES *previous_state,
                             uint32_t *prev_count)
{
    uint32_t i, j, prev_idx = 0;

    if (!token)
        return STATUS_INVALID_HANDLE;

    if (reset_to_default) {
        for (i = 0; i < token->GroupCount; i++) {
            if (previous_state && prev_idx < (prev_count ? *prev_count : 0)) {
                previous_state[prev_idx] = token->Groups[i];
                prev_idx++;
            }
            /* Restore enabled-by-default state, but never re-enable deny-only */
            if (token->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY)
                continue;
            if (token->Groups[i].Attributes & SE_GROUP_ENABLED_BY_DEFAULT)
                token->Groups[i].Attributes |= SE_GROUP_ENABLED;
            else
                token->Groups[i].Attributes &= ~SE_GROUP_ENABLED;
        }
        token->ModifiedId = NtAllocateLocallyUniqueId();
        if (prev_count) *prev_count = prev_idx;
        return STATUS_SUCCESS;
    }

    if (!new_state)
        return STATUS_INVALID_PARAMETER;

    for (j = 0; j < new_count; j++) {
        for (i = 0; i < token->GroupCount; i++) {
            if (!RtlEqualSid(token->Groups[i].Sid, new_state[j].Sid))
                continue;

            /* Cannot re-enable a deny-only group */
            if ((token->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY) &&
                (new_state[j].Attributes & SE_GROUP_ENABLED))
                continue;

            if (previous_state && prev_idx < (prev_count ? *prev_count : 0)) {
                previous_state[prev_idx] = token->Groups[i];
                prev_idx++;
            }

            if (new_state[j].Attributes & SE_GROUP_ENABLED)
                token->Groups[i].Attributes |= SE_GROUP_ENABLED;
            else
                token->Groups[i].Attributes &= ~SE_GROUP_ENABLED;
            break;
        }
    }

    token->ModifiedId = NtAllocateLocallyUniqueId();
    if (prev_count) *prev_count = prev_idx;
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Token query functions (§4.3)
 * ============================================================================ */

extern void *memcpy(void *dst, const void *src, size_t n);

/* --- NtOpenProcessToken -------------------------------------------------- */

int32_t NtOpenProcessToken(ACCESS_TOKEN *process_token, HANDLE_TABLE *ht,
                           uint32_t desired_access, HANDLE *out_handle)
{
    HANDLE h;

    if (!process_token || !ht || !out_handle)
        return STATUS_INVALID_PARAMETER;

    h = ObpAllocateHandle(ht, process_token, desired_access, 0);
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_INVALID_HANDLE;

    *out_handle = h;
    return STATUS_SUCCESS;
}

/* --- NtOpenThreadToken --------------------------------------------------- */

int32_t NtOpenThreadToken(ACCESS_TOKEN *impersonation_token, HANDLE_TABLE *ht,
                          uint32_t desired_access, HANDLE *out_handle)
{
    HANDLE h;

    if (!ht || !out_handle)
        return STATUS_INVALID_PARAMETER;

    if (!impersonation_token)
        return STATUS_NO_TOKEN;

    h = ObpAllocateHandle(ht, impersonation_token, desired_access, 0);
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_INVALID_HANDLE;

    *out_handle = h;
    return STATUS_SUCCESS;
}

/* --- NtQueryInformationToken --------------------------------------------- */

int32_t NtQueryInformationToken(const ACCESS_TOKEN *token,
                                TOKEN_INFORMATION_CLASS info_class,
                                void *buf, uint32_t buf_len,
                                uint32_t *ret_len)
{
    uint32_t needed;

    if (!token)
        return STATUS_INVALID_HANDLE;

    switch (info_class) {

    case TokenUser: {
        /* Returns SID_AND_ATTRIBUTES for the user */
        needed = sizeof(SID_AND_ATTRIBUTES);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        SID_AND_ATTRIBUTES *out = (SID_AND_ATTRIBUTES *)buf;
        out->Sid = token->UserSid;
        out->Attributes = 0;
        return STATUS_SUCCESS;
    }

    case TokenGroups: {
        needed = sizeof(uint32_t) + token->GroupCount * sizeof(SID_AND_ATTRIBUTES);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        *(uint32_t *)buf = token->GroupCount;
        memcpy((uint8_t *)buf + sizeof(uint32_t), token->Groups,
               token->GroupCount * sizeof(SID_AND_ATTRIBUTES));
        return STATUS_SUCCESS;
    }

    case TokenPrivileges: {
        needed = sizeof(uint32_t) + token->PrivilegeCount * sizeof(LUID_AND_ATTRIBUTES);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        *(uint32_t *)buf = token->PrivilegeCount;
        memcpy((uint8_t *)buf + sizeof(uint32_t), token->Privileges,
               token->PrivilegeCount * sizeof(LUID_AND_ATTRIBUTES));
        return STATUS_SUCCESS;
    }

    case TokenOwner: {
        needed = sizeof(SID *);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        *(SID **)buf = token->UserSid;
        return STATUS_SUCCESS;
    }

    case TokenPrimaryGroup: {
        needed = sizeof(SID *);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        *(SID **)buf = token->PrimaryGroup;
        return STATUS_SUCCESS;
    }

    case TokenDefaultDacl: {
        needed = sizeof(ACL *);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        *(ACL **)buf = token->DefaultDacl;
        return STATUS_SUCCESS;
    }

    case TokenTypeInfo: {
        needed = sizeof(TOKEN_TYPE);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        *(TOKEN_TYPE *)buf = token->TokenType;
        return STATUS_SUCCESS;
    }

    case TokenImpersonationLevelInfo: {
        needed = sizeof(SECURITY_IMPERSONATION_LEVEL);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        *(SECURITY_IMPERSONATION_LEVEL *)buf = token->ImpersonationLevel;
        return STATUS_SUCCESS;
    }

    case TokenStatistics: {
        /* Return TokenId, AuthenticationId, ModifiedId, GroupCount, PrivilegeCount */
        needed = 3 * sizeof(LUID) + 2 * sizeof(uint32_t);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        {
            uint8_t *p = (uint8_t *)buf;
            memcpy(p, &token->TokenId, sizeof(LUID)); p += sizeof(LUID);
            memcpy(p, &token->AuthenticationId, sizeof(LUID)); p += sizeof(LUID);
            memcpy(p, &token->ModifiedId, sizeof(LUID)); p += sizeof(LUID);
            *(uint32_t *)p = token->GroupCount; p += sizeof(uint32_t);
            *(uint32_t *)p = token->PrivilegeCount;
        }
        return STATUS_SUCCESS;
    }

    case TokenIntegrityLevel: {
        needed = sizeof(SID_AND_ATTRIBUTES);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        SID_AND_ATTRIBUTES *out = (SID_AND_ATTRIBUTES *)buf;
        out->Sid = token->IntegrityLevelSid;
        out->Attributes = SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED;
        return STATUS_SUCCESS;
    }

    case TokenElevationTypeInfo: {
        needed = sizeof(TOKEN_ELEVATION_TYPE);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        *(TOKEN_ELEVATION_TYPE *)buf = token->ElevationType;
        return STATUS_SUCCESS;
    }

    case TokenLinkedToken: {
        needed = sizeof(LUID);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        *(LUID *)buf = token->LinkedTokenId;
        return STATUS_SUCCESS;
    }

    case TokenIsElevatedInfo: {
        needed = sizeof(uint32_t);
        if (ret_len) *ret_len = needed;
        if (buf_len < needed) return STATUS_BUFFER_TOO_SMALL;
        *(uint32_t *)buf = token->IsElevated;
        return STATUS_SUCCESS;
    }

    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}
