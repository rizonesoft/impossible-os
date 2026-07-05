/* ============================================================================
 * assign_security.c -- SeAssignSecurity: SD inheritance for new objects
 *
 * Builds a self-contained SECURITY_DESCRIPTOR for a newly created object by
 * selecting a DACL source (explicit creator DACL, inherited-from-parent,
 * token default, or a fail-closed default) and resolving Owner/Group. The
 * returned descriptor owns one heap block holding the SD, its DACL, and copied
 * Owner/Group SIDs; SeDeassignSecurity frees it.
 *
 * Interim: the subject is an ACCESS_TOKEN* (the SECURITY_SUBJECT_CONTEXT type
 * is owned by the deferred SeAccessCheck engine work); same convention as
 * mic.c / privileges.c. Generic-rights mapping of inherited masks, auto-inherit
 * to existing children, and SACL inheritance are out of scope here.
 * ============================================================================ */

#include "kernel/security/assign_security.h"
#include "kernel/security/acl.h"
#include "kernel/security/sid.h"
#include "kernel/security/token.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"

extern void *memcpy(void *dst, const void *src, size_t n);

#define ROUND4(n)  (((uint32_t)(n) + 3u) & ~3u)

/* One page is the hard ceiling: the whole descriptor comes from a single
 * kmalloc (<= 4 KB), so an inherited DACL that would overflow it fails closed
 * rather than silently truncating a grant/deny set. */
#define SE_ASSIGN_MAX_SD  4096u

/* The DACL region size is cast to the uint16_t AclSize field; the 4 KB cap keeps
 * every region well under 0xFFFF. Pin that coupling so raising the cap past a
 * uint16 (which would also break the kmalloc <= 4 KB rule) fails to compile
 * instead of silently truncating an ACL size. */
_Static_assert(SE_ASSIGN_MAX_SD <= 0xFFFFu,
    "SE_ASSIGN_MAX_SD must stay within the uint16_t ACL AclSize field");

/* ---- ACE field accessors (ACCESS_ALLOWED/DENIED layout: Header, Mask, SID) - */

static uint32_t ace_mask(const ACE_HEADER *h)
{
    return *(const uint32_t *)((const uint8_t *)h + sizeof(ACE_HEADER));
}

static const SID *ace_sid(const ACE_HEADER *h)
{
    return (const SID *)((const uint8_t *)h + sizeof(ACE_HEADER) + sizeof(uint32_t));
}

static int ace_is_dacl_type(uint8_t t)
{
    return t == ACCESS_ALLOWED_ACE_TYPE || t == ACCESS_DENIED_ACE_TYPE;
}

/* Validate the internal shape of a TRUSTED source ACL (an owned kernel
 * allocation whose AclSize is truthful) before we republish or inherit from it:
 * confirms the 8-byte header fits and every ACE stays within the declared
 * AclSize, so a corrupt AceSize/count cannot make the returned Dacl point at a
 * partial header a later walker reads past. This is NOT an untrusted-buffer
 * bound check: AclSize is used as the readable extent, which only holds when the
 * caller owns the allocation. An imported/attacker-controlled SD must first be
 * bounded by RtlSelfRelativeToAbsoluteSD(known_len) at the syscall boundary
 * (NtSetSecurityObject) BEFORE it reaches SeAssignSecurity. */
static int dacl_shape_ok(const ACL *acl)
{
    return acl && acl->AclSize >= sizeof(ACL) &&
           RtlValidAcl(acl, acl->AclSize);
}

/* Compute the child ACE flags when a parent ACE with flags `pf` is inherited by
 * a child of the given kind. Returns 1 and sets *out if the ACE is inherited,
 * 0 if it is not. Encodes the Windows container/object-inherit propagation
 * rules (see assign_security.h). */
static int inherit_child_flags(uint8_t pf, int is_dir, uint8_t *out)
{
    int np = (pf & NO_PROPAGATE_INHERIT_ACE) != 0;

    if (!is_dir) {
        /* File (leaf) child: inherits object-inherit ACEs; a leaf never
         * propagates, so the copy carries only INHERITED_ACE. */
        if (!(pf & OBJECT_INHERIT_ACE))
            return 0;
        *out = INHERITED_ACE;
        return 1;
    }

    /* Directory (container) child. */
    if (pf & CONTAINER_INHERIT_ACE) {
        /* Applies to the directory itself and keeps propagating unless
         * NO_PROPAGATE strips the inherit flags. INHERIT_ONLY is cleared: the
         * ACE now grants on the directory. */
        uint8_t cf = INHERITED_ACE;
        if (!np)
            cf |= (pf & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE));
        *out = cf;
        return 1;
    }
    if (pf & OBJECT_INHERIT_ACE) {
        /* Object-inherit only: does NOT grant on the directory, but must reach
         * the directory's file children. With NO_PROPAGATE it can neither grant
         * here nor propagate, so it is not inherited at all. */
        if (np)
            return 0;
        *out = INHERITED_ACE | INHERIT_ONLY_ACE | OBJECT_INHERIT_ACE;
        return 1;
    }
    return 0;
}

/* Does the parent DACL contain at least one ACE that inherits to this child? */
static int parent_has_inheritable(const SECURITY_DESCRIPTOR *parent, int is_dir)
{
    ACE_HEADER *ace;
    uint32_t i;
    uint8_t cf;

    if (!parent || !dacl_shape_ok(parent->Dacl))
        return 0;
    for (i = 0; i < parent->Dacl->AceCount; i++) {
        if (RtlGetAce(parent->Dacl, i, &ace) != 0)
            break;
        if (ace_is_dacl_type(ace->AceType) &&
            inherit_child_flags(ace->AceFlags, is_dir, &cf))
            return 1;
    }
    return 0;
}

/* True for every CONDITIONAL (callback) ACE type RtlValidAcl accepts as a valid
 * SID-bearing ACE: allow/deny (0x09/0x0A) plus audit/alarm (0x0D/0x0E). */
static int ace_is_callback_type(uint8_t t)
{
    return t == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
           t == ACCESS_DENIED_CALLBACK_ACE_TYPE ||
           t == SYSTEM_AUDIT_CALLBACK_ACE_TYPE ||
           t == SYSTEM_ALARM_CALLBACK_ACE_TYPE;
}

/* Does the parent DACL carry an inheritable CONDITIONAL (callback) ACE? Callback
 * ACEs are SID-bearing and pass RtlValidAcl, but inheriting them correctly needs
 * the conditional-expression evaluation that lives in the deferred SeAccessCheck
 * engine. Rebuilding them as basic ACEs would drop the condition, and silently
 * skipping them would drop an inheritable DENY -- so callers fail closed when
 * this returns 1. Covers every callback type the validator accepts (including a
 * misplaced audit/alarm callback ACE in a DACL) so none is silently dropped. */
static int parent_has_inheritable_callback(const SECURITY_DESCRIPTOR *parent,
                                           int is_dir)
{
    ACE_HEADER *ace;
    uint32_t i;
    uint8_t cf;

    if (!parent || !dacl_shape_ok(parent->Dacl))
        return 0;
    for (i = 0; i < parent->Dacl->AceCount; i++) {
        if (RtlGetAce(parent->Dacl, i, &ace) != 0)
            break;
        if (ace_is_callback_type(ace->AceType) &&
            inherit_child_flags(ace->AceFlags, is_dir, &cf))
            return 1;
    }
    return 0;
}

/* Byte size of the inherited DACL (ACL header + every inheriting ACE, each
 * 8 bytes of header+Mask plus its SID). */
static uint32_t inherited_dacl_bytes(const SECURITY_DESCRIPTOR *parent, int is_dir)
{
    ACE_HEADER *ace;
    uint32_t i, bytes = (uint32_t)sizeof(ACL);
    uint8_t cf;

    for (i = 0; i < parent->Dacl->AceCount; i++) {
        if (RtlGetAce(parent->Dacl, i, &ace) != 0)
            break;
        if (ace_is_dacl_type(ace->AceType) &&
            inherit_child_flags(ace->AceFlags, is_dir, &cf))
            bytes += (uint32_t)(sizeof(ACE_HEADER) + sizeof(uint32_t)) +
                     RtlLengthSid(ace_sid(ace));
    }
    return bytes;
}

/* Emit one canonical-order pass of inherited ACEs into `dst`, restricted to the
 * given ACE type (deny pass then allow pass keeps denies before allows). */
static int emit_inherited_pass(ACL *dst, const SECURITY_DESCRIPTOR *parent,
                               int is_dir, uint8_t want_type)
{
    ACE_HEADER *ace, *added;
    uint32_t i;
    uint8_t cf;

    for (i = 0; i < parent->Dacl->AceCount; i++) {
        if (RtlGetAce(parent->Dacl, i, &ace) != 0)
            return -1;
        if (ace->AceType != want_type)
            continue;
        if (!inherit_child_flags(ace->AceFlags, is_dir, &cf))
            continue;
        if (want_type == ACCESS_DENIED_ACE_TYPE) {
            if (RtlAddAccessDeniedAce(dst, ACL_REVISION, ace_mask(ace),
                                      ace_sid(ace)) != 0)
                return -1;
        } else {
            if (RtlAddAccessAllowedAce(dst, ACL_REVISION, ace_mask(ace),
                                       ace_sid(ace)) != 0)
                return -1;
        }
        /* The Rtl adders write AceFlags = 0; stamp the computed child flags. */
        if (RtlGetAce(dst, (uint32_t)dst->AceCount - 1, &added) != 0)
            return -1;
        added->AceFlags = cf;
    }
    return 0;
}

/* Build the fail-closed default DACL (owner + SYSTEM full, World read) into
 * `dst`, sized `cap`. Mirrors SeCreateCreatorSD's grant set. */
static int build_fallback_dacl(ACL *dst, uint16_t cap, const SID *owner)
{
    if (RtlCreateAcl(dst, cap, ACL_REVISION) != 0)
        return -1;
    if (RtlAddAccessAllowedAce(dst, ACL_REVISION, GENERIC_ALL, owner) != 0 ||
        RtlAddAccessAllowedAce(dst, ACL_REVISION, GENERIC_ALL, SeLocalSystemSid) != 0 ||
        RtlAddAccessAllowedAce(dst, ACL_REVISION, READ_CONTROL, SeWorldSid) != 0)
        return -1;
    return 0;
}

/* DACL source selection. */
enum {
    DACL_EXPLICIT_COPY,   /* copy CreatorSD->Dacl verbatim */
    DACL_EXPLICIT_NULL,   /* explicit present-but-NULL DACL (grant all) */
    DACL_INHERIT,         /* build from parent inheritable ACEs */
    DACL_TOKEN_DEFAULT,   /* copy token DefaultDacl */
    DACL_FALLBACK         /* fail-closed creator/SYSTEM/World default */
};

NTSTATUS SeAssignSecurity(const SECURITY_DESCRIPTOR *ParentSD,
                          const SECURITY_DESCRIPTOR *CreatorSD,
                          int IsDirectory,
                          struct access_token *CreatorToken,
                          SECURITY_DESCRIPTOR **NewSD)
{
    ACCESS_TOKEN *tok = (ACCESS_TOKEN *)CreatorToken;
    const SID *owner_sid, *group_sid;
    const ACL *copy_src = (const ACL *)0;
    uint32_t owner_len, group_len, dacl_bytes = 0;
    uint32_t owner_region, group_region, dacl_region, total;
    int source, creator_explicit, child_protected, dacl_defaulted = 0;
    uint8_t *block, *cursor;
    SECURITY_DESCRIPTOR *sd;
    ACL *dacl = (ACL *)0;

    if (!NewSD)
        return STATUS_INVALID_PARAMETER;
    *NewSD = (SECURITY_DESCRIPTOR *)0;

    /* Owner is mandatory: resolve from the creator SD, else the token user.
     * Fail closed rather than emit an ownerless descriptor. */
    owner_sid = (CreatorSD && CreatorSD->Owner) ? CreatorSD->Owner
              : (tok ? tok->UserSid : (SID *)0);
    if (!owner_sid)
        return STATUS_INVALID_PARAMETER;
    group_sid = (CreatorSD && CreatorSD->Group) ? CreatorSD->Group
              : (tok ? tok->PrimaryGroup : (SID *)0);

    /* Validate the SIDs up front: the fallback DACL and the size pre-pass both
     * call RtlLengthSid/RtlAddAccessAllowedAce on the owner before the later
     * RtlCopySid would catch a malformed SID, so a bad SubAuthorityCount would
     * otherwise drive an out-of-bounds read. */
    if (!RtlValidSid(owner_sid))
        return STATUS_INVALID_PARAMETER;
    if (group_sid && !RtlValidSid(group_sid))
        return STATUS_INVALID_PARAMETER;

    /* Pick the DACL source. */
    creator_explicit = CreatorSD && (CreatorSD->Control & SE_DACL_PRESENT) &&
                       !(CreatorSD->Control & SE_DACL_DEFAULTED);
    child_protected  = CreatorSD && (CreatorSD->Control & SE_DACL_PROTECTED);

    /* If we would consult the parent for inheritance, refuse to proceed when it
     * carries an inheritable conditional (callback) ACE: we cannot evaluate or
     * faithfully copy it yet, and dropping an inheritable callback DENY would
     * grant access the parent intended to deny. Fail closed. */
    if (!creator_explicit && !child_protected &&
        parent_has_inheritable_callback(ParentSD, IsDirectory))
        return STATUS_INVALID_PARAMETER;

    if (creator_explicit) {
        if (CreatorSD->Dacl) {
            /* The caller explicitly asked for this DACL; a malformed one is a
             * caller error and cannot be safely substituted -> fail closed. */
            if (!dacl_shape_ok(CreatorSD->Dacl))
                return STATUS_INVALID_PARAMETER;
            source = DACL_EXPLICIT_COPY;
            copy_src = CreatorSD->Dacl;
            dacl_bytes = copy_src->AclSize;
        } else {
            source = DACL_EXPLICIT_NULL;   /* explicit NULL DACL = grant all */
        }
    } else if (!child_protected && parent_has_inheritable(ParentSD, IsDirectory)) {
        source = DACL_INHERIT;
        dacl_bytes = inherited_dacl_bytes(ParentSD, IsDirectory);
    } else if (tok && dacl_shape_ok(tok->DefaultDacl)) {
        source = DACL_TOKEN_DEFAULT;
        copy_src = tok->DefaultDacl;
        dacl_bytes = copy_src->AclSize;
        dacl_defaulted = 1;
    } else {
        source = DACL_FALLBACK;
        dacl_bytes = (uint32_t)sizeof(ACL)
                   + 3u * (uint32_t)(sizeof(ACE_HEADER) + sizeof(uint32_t))
                   + RtlLengthSid(owner_sid)
                   + RtlLengthSid(SeLocalSystemSid)
                   + RtlLengthSid(SeWorldSid);
        dacl_defaulted = 1;
    }

    if (dacl_bytes > 0xFFFFu)   /* AclSize is a uint16_t field */
        return STATUS_INSUFFICIENT_RESOURCES;

    owner_len    = RtlLengthSid(owner_sid);
    group_len    = group_sid ? RtlLengthSid(group_sid) : 0;
    dacl_region  = ROUND4(dacl_bytes);
    owner_region = ROUND4(owner_len);
    group_region = ROUND4(group_len);
    total = ROUND4(sizeof(SECURITY_DESCRIPTOR)) + dacl_region +
            owner_region + group_region;
    if (total > SE_ASSIGN_MAX_SD)
        return STATUS_INSUFFICIENT_RESOURCES;

    block = (uint8_t *)kmalloc_zeroed(total);
    if (!block)
        return STATUS_NO_MEMORY;

    sd = (SECURITY_DESCRIPTOR *)block;
    RtlCreateSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION);
    cursor = block + ROUND4(sizeof(SECURITY_DESCRIPTOR));

    /* Build/copy the DACL. */
    if (source != DACL_EXPLICIT_NULL) {
        dacl = (ACL *)cursor;
        cursor += dacl_region;
    }
    switch (source) {
    case DACL_EXPLICIT_COPY:
    case DACL_TOKEN_DEFAULT:
        memcpy(dacl, copy_src, copy_src->AclSize);
        break;
    case DACL_INHERIT:
        if (RtlCreateAcl(dacl, (uint16_t)dacl_region, ACL_REVISION) != 0 ||
            emit_inherited_pass(dacl, ParentSD, IsDirectory,
                                ACCESS_DENIED_ACE_TYPE) != 0 ||
            emit_inherited_pass(dacl, ParentSD, IsDirectory,
                                ACCESS_ALLOWED_ACE_TYPE) != 0) {
            kfree(block);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        break;
    case DACL_FALLBACK:
        if (build_fallback_dacl(dacl, (uint16_t)dacl_region, owner_sid) != 0) {
            kfree(block);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        break;
    default:
        break;   /* DACL_EXPLICIT_NULL: no DACL body */
    }

    /* Copy Owner + Group into the owned block (never alias the caller/token). */
    if (RtlCopySid(cursor, owner_len, owner_sid) != 0) {
        kfree(block);
        return STATUS_INVALID_PARAMETER;
    }
    RtlSetOwnerSecurityDescriptor(sd, (SID *)cursor, 0);
    cursor += owner_region;

    if (group_sid) {
        if (RtlCopySid(cursor, group_len, group_sid) != 0) {
            kfree(block);
            return STATUS_INVALID_PARAMETER;
        }
        RtlSetGroupSecurityDescriptor(sd, (SID *)cursor, 0);
        cursor += group_region;
    }

    /* Present with a body, or explicit-present-NULL (grant all). */
    RtlSetDaclSecurityDescriptor(sd, 1, dacl, dacl_defaulted);
    if (source == DACL_INHERIT)
        sd->Control |= SE_DACL_AUTO_INHERITED;
    /* Preserve the creator's no-inherit request on the output descriptor so a
     * later auto-inherit / security-query path does not re-inherit into it. */
    if (child_protected)
        sd->Control |= SE_DACL_PROTECTED;

    *NewSD = sd;
    return STATUS_SUCCESS;
}

void SeDeassignSecurity(SECURITY_DESCRIPTOR **NewSD)
{
    if (NewSD && *NewSD) {
        kfree(*NewSD);
        *NewSD = (SECURITY_DESCRIPTOR *)0;
    }
}
