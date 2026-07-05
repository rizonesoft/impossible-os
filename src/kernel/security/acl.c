/* ============================================================================
 * acl.c -- ACL construction and debug helpers
 *
 * Provides RtlCreateAcl, RtlAddAccessAllowedAce, RtlAddAccessDeniedAce,
 * RtlAddMandatoryAce, RtlGetAce, RtlAclToCStr.
 * ============================================================================ */

#include "kernel/security/acl.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);
extern int   snprintf(char *buf, size_t size, const char *fmt, ...);

/* ============================================================================
 * SECURITY_DESCRIPTOR helpers
 * ============================================================================ */

int RtlCreateSecurityDescriptor(SECURITY_DESCRIPTOR *sd, uint8_t rev)
{
    if (!sd)
        return -1;
    memset(sd, 0, sizeof(*sd));
    sd->Revision = rev;
    return 0;
}

int RtlSetOwnerSecurityDescriptor(SECURITY_DESCRIPTOR *sd, SID *owner, int defaulted)
{
    if (!sd)
        return -1;
    sd->Owner = owner;
    if (defaulted)
        sd->Control |= SE_OWNER_DEFAULTED;
    else
        sd->Control &= ~SE_OWNER_DEFAULTED;
    return 0;
}

int RtlGetOwnerSecurityDescriptor(const SECURITY_DESCRIPTOR *sd, SID **owner, int *defaulted)
{
    if (!sd || !owner)
        return -1;
    *owner = sd->Owner;
    if (defaulted)
        *defaulted = (sd->Control & SE_OWNER_DEFAULTED) ? 1 : 0;
    return 0;
}

int RtlSetGroupSecurityDescriptor(SECURITY_DESCRIPTOR *sd, SID *group, int defaulted)
{
    if (!sd)
        return -1;
    sd->Group = group;
    if (defaulted)
        sd->Control |= SE_GROUP_DEFAULTED;
    else
        sd->Control &= ~SE_GROUP_DEFAULTED;
    return 0;
}

int RtlSetDaclSecurityDescriptor(SECURITY_DESCRIPTOR *sd, int present, ACL *dacl, int defaulted)
{
    if (!sd)
        return -1;
    if (present) {
        sd->Control |= SE_DACL_PRESENT;
        sd->Dacl = dacl;
    } else {
        sd->Control &= ~SE_DACL_PRESENT;
        sd->Dacl = (ACL *)0;
    }
    if (defaulted)
        sd->Control |= SE_DACL_DEFAULTED;
    else
        sd->Control &= ~SE_DACL_DEFAULTED;
    return 0;
}

int RtlGetDaclSecurityDescriptor(const SECURITY_DESCRIPTOR *sd, int *present, ACL **dacl, int *defaulted)
{
    if (!sd || !present)
        return -1;
    *present = (sd->Control & SE_DACL_PRESENT) ? 1 : 0;
    if (dacl)
        *dacl = sd->Dacl;
    if (defaulted)
        *defaulted = (sd->Control & SE_DACL_DEFAULTED) ? 1 : 0;
    return 0;
}

int RtlSetSaclSecurityDescriptor(SECURITY_DESCRIPTOR *sd, int present, ACL *sacl, int defaulted)
{
    if (!sd)
        return -1;
    if (present) {
        sd->Control |= SE_SACL_PRESENT;
        sd->Sacl = sacl;
    } else {
        sd->Control &= ~SE_SACL_PRESENT;
        sd->Sacl = (ACL *)0;
    }
    if (defaulted)
        sd->Control |= SE_SACL_DEFAULTED;
    else
        sd->Control &= ~SE_SACL_DEFAULTED;
    return 0;
}

/* ---- Self-relative marshalling ----------------------------------------- */

/*
 * Self-relative layout (flat buffer):
 *   [SD header: Rev, Sbz1, Control | SE_SELF_RELATIVE, OffsetOwner,
 *    OffsetGroup, OffsetSacl, OffsetDacl]
 *   [Owner SID bytes]
 *   [Group SID bytes]
 *   [SACL bytes]
 *   [DACL bytes]
 *
 * Offsets are uint32_t from the start of the buffer (0 = not present).
 * The SECURITY_DESCRIPTOR_RELATIVE header type + its size/offset asserts now
 * live in acl.h so untrusted-input consumers can parse offsets safely.
 */

int RtlAbsoluteToSelfRelativeSD(const SECURITY_DESCRIPTOR *abs,
                                void *rel_buf, uint32_t *rel_len)
{
    uint32_t needed, offset;
    uint32_t owner_len = 0, group_len = 0, sacl_len = 0, dacl_len = 0;
    SECURITY_DESCRIPTOR_RELATIVE *rel;
    uint8_t *p;

    if (!abs || !rel_len)
        return -1;

    if (abs->Owner) owner_len = RtlLengthSid(abs->Owner);
    if (abs->Group) group_len = RtlLengthSid(abs->Group);
    if (abs->Sacl)  sacl_len  = abs->Sacl->AclSize;
    if (abs->Dacl)  dacl_len  = abs->Dacl->AclSize;

    needed = sizeof(SECURITY_DESCRIPTOR_RELATIVE) +
             owner_len + group_len + sacl_len + dacl_len;

    if (!rel_buf || *rel_len < needed) {
        *rel_len = needed;
        return -1;
    }

    memset(rel_buf, 0, needed);
    rel = (SECURITY_DESCRIPTOR_RELATIVE *)rel_buf;
    rel->Revision = abs->Revision;
    rel->Control  = abs->Control | SE_SELF_RELATIVE;

    offset = sizeof(SECURITY_DESCRIPTOR_RELATIVE);
    p = (uint8_t *)rel_buf + offset;

    if (abs->Owner && owner_len) {
        rel->OffsetOwner = offset;
        memcpy(p, abs->Owner, owner_len);
        p += owner_len; offset += owner_len;
    }
    if (abs->Group && group_len) {
        rel->OffsetGroup = offset;
        memcpy(p, abs->Group, group_len);
        p += group_len; offset += group_len;
    }
    if (abs->Sacl && sacl_len) {
        rel->OffsetSacl = offset;
        memcpy(p, abs->Sacl, sacl_len);
        p += sacl_len; offset += sacl_len;
    }
    if (abs->Dacl && dacl_len) {
        rel->OffsetDacl = offset;
        memcpy(p, abs->Dacl, dacl_len);
        offset += dacl_len;
    }

    *rel_len = needed;
    return 0;
}

/* Validate one self-relative component offset: 0 (absent) is handled by the
 * caller. A present offset must clear the fixed header and stay inside the
 * source buffer. Returns the bytes available at the offset (>= 1), or 0 if
 * the offset is out of range. Bounds are subtraction-form (no uint32 wrap). */
static uint32_t sr_offset_avail(uint32_t off, uint32_t rel_len)
{
    if (off < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || off >= rel_len)
        return 0;
    return rel_len - off;
}

int RtlSelfRelativeToAbsoluteSD(const void *rel, uint32_t rel_len,
                                SECURITY_DESCRIPTOR *abs,
                                void *abs_buf, uint32_t abs_buf_len)
{
    const SECURITY_DESCRIPTOR_RELATIVE *sr;
    const uint8_t *base;
    uint8_t *wp;
    uint32_t used = 0;
    uint32_t off, avail, len;

    if (!rel || !abs || !abs_buf)
        return -1;

    /* The fixed header must be fully readable before any offset field is
     * touched. Everything below is bounds-checked against rel_len -- rel may
     * point at an untrusted (attacker-controlled) buffer. */
    if (rel_len < sizeof(SECURITY_DESCRIPTOR_RELATIVE))
        return -1;

    sr   = (const SECURITY_DESCRIPTOR_RELATIVE *)rel;
    base = (const uint8_t *)rel;
    wp   = (uint8_t *)abs_buf;

    /* NT input-shape invariants (RtlValidSecurityDescriptor model): the header
     * revision must match, and a self-relative descriptor MUST carry
     * SE_SELF_RELATIVE. Reject malformed/absolute headers rather than parsing
     * them with self-relative offset semantics. */
    if (sr->Revision != SECURITY_DESCRIPTOR_REVISION)
        return -1;
    if (!(sr->Control & SE_SELF_RELATIVE))
        return -1;

    memset(abs, 0, sizeof(*abs));
    abs->Revision = sr->Revision;
    abs->Control  = sr->Control & ~SE_SELF_RELATIVE;

    /* Copy Owner SID (bounded) */
    off = sr->OffsetOwner;
    if (off) {
        const SID *src;
        avail = sr_offset_avail(off, rel_len);
        if (!avail) return -1;
        src = (const SID *)(base + off);
        len = RtlLengthSidBounded(src, avail);
        if (!len) return -1;
        /* used <= abs_buf_len invariant holds -> subtraction cannot wrap */
        if (len > abs_buf_len - used) return -1;
        memcpy(wp + used, src, len);
        abs->Owner = (SID *)(wp + used);
        used += len;
    }

    /* Copy Group SID (bounded) */
    off = sr->OffsetGroup;
    if (off) {
        const SID *src;
        avail = sr_offset_avail(off, rel_len);
        if (!avail) return -1;
        src = (const SID *)(base + off);
        len = RtlLengthSidBounded(src, avail);
        if (!len) return -1;
        if (len > abs_buf_len - used) return -1;
        memcpy(wp + used, src, len);
        abs->Group = (SID *)(wp + used);
        used += len;
    }

    /* Copy SACL (bounded) */
    off = sr->OffsetSacl;
    if (off) {
        const ACL *src;
        avail = sr_offset_avail(off, rel_len);
        if (!avail) return -1;
        src = (const ACL *)(base + off);
        if (!RtlValidAcl(src, avail)) return -1;
        len = (uint32_t)src->AclSize;   /* RtlValidAcl proved AclSize <= avail */
        if (len > abs_buf_len - used) return -1;
        memcpy(wp + used, src, len);
        abs->Sacl = (ACL *)(wp + used);
        used += len;
    }

    /* Copy DACL (bounded) */
    off = sr->OffsetDacl;
    if (off) {
        const ACL *src;
        avail = sr_offset_avail(off, rel_len);
        if (!avail) return -1;
        src = (const ACL *)(base + off);
        if (!RtlValidAcl(src, avail)) return -1;
        len = (uint32_t)src->AclSize;
        if (len > abs_buf_len - used) return -1;
        memcpy(wp + used, src, len);
        abs->Dacl = (ACL *)(wp + used);
        used += len;
    }

    return 0;
}

/* ============================================================================
 * ACL helpers
 * ============================================================================ */

/* ---- RtlCreateAcl ------------------------------------------------------ */

int RtlCreateAcl(ACL *acl, uint16_t size, uint8_t rev)
{
    if (!acl || size < sizeof(ACL))
        return -1;

    memset(acl, 0, size);
    acl->AclRevision = rev;
    acl->AclSize     = size;
    acl->AceCount    = 0;
    return 0;
}

/* ---- Internal: append a generic ACE ------------------------------------ */

static int acl_append_ace(ACL *acl, uint8_t ace_type, uint8_t ace_flags,
                          uint32_t mask, const SID *sid)
{
    uint32_t sid_len, ace_size, used, avail;
    uint8_t *p;
    ACE_HEADER *hdr;
    uint32_t i;

    if (!acl || !sid)
        return -1;

    sid_len  = RtlLengthSid(sid);
    ace_size = sizeof(ACE_HEADER) + sizeof(uint32_t) + sid_len;  /* header + Mask + SID */

    /* Walk existing ACEs to find end */
    p = (uint8_t *)acl + sizeof(ACL);
    for (i = 0; i < acl->AceCount; i++)
        p += ((ACE_HEADER *)p)->AceSize;

    used  = (uint32_t)(p - (uint8_t *)acl);
    avail = acl->AclSize - used;

    if (ace_size > avail)
        return -1;  /* ACL buffer full */

    hdr = (ACE_HEADER *)p;
    hdr->AceType  = ace_type;
    hdr->AceFlags = ace_flags;
    hdr->AceSize  = (uint16_t)ace_size;

    /* Mask field immediately after header */
    *(uint32_t *)(p + sizeof(ACE_HEADER)) = mask;

    /* SID immediately after Mask */
    memcpy(p + sizeof(ACE_HEADER) + sizeof(uint32_t), sid, sid_len);

    acl->AceCount++;
    return 0;
}

/* ---- Public ACE append functions --------------------------------------- */

int RtlAddAccessAllowedAce(ACL *acl, uint8_t rev, uint32_t mask, const SID *sid)
{
    (void)rev;
    return acl_append_ace(acl, ACCESS_ALLOWED_ACE_TYPE, 0, mask, sid);
}

int RtlAddAccessDeniedAce(ACL *acl, uint8_t rev, uint32_t mask, const SID *sid)
{
    (void)rev;
    return acl_append_ace(acl, ACCESS_DENIED_ACE_TYPE, 0, mask, sid);
}

int RtlAddMandatoryAce(ACL *acl, uint8_t rev, uint8_t flags,
                       uint32_t mask, uint8_t type, const SID *integrity_sid)
{
    (void)rev;
    (void)type;  /* always SYSTEM_MANDATORY_LABEL_ACE_TYPE */
    return acl_append_ace(acl, SYSTEM_MANDATORY_LABEL_ACE_TYPE, flags,
                          mask, integrity_sid);
}

/* ---- RtlGetAce --------------------------------------------------------- */

int RtlGetAce(const ACL *acl, uint32_t index, ACE_HEADER **ace)
{
    const uint8_t *p;
    uint32_t i;

    if (!acl || !ace || index >= acl->AceCount)
        return -1;

    p = (const uint8_t *)acl + sizeof(ACL);
    for (i = 0; i < index; i++)
        p += ((const ACE_HEADER *)p)->AceSize;

    *ace = (ACE_HEADER *)p;
    return 0;
}

/* ---- Bounded ACL validation (untrusted input) -------------------------- */

/* ACE types that carry an inline SID immediately after ACE_HEADER + Mask.
 * The callback variants (0x09/0x0A/0x0D/0x0E) share the same Header+Mask+
 * SidStart prefix as the basic ACEs -- the SID is at +8; a conditional-
 * expression BLOB (if any) follows the SID, still inside AceSize -- so their
 * inline SID must be bounds-validated exactly like the basic types. Object
 * ACEs (variable Flags+GUID prefix before the SID) are deliberately NOT here:
 * they have no fixed SID offset and are treated bounds-only until supported. */
static int ace_is_sid_bearing(uint8_t ace_type)
{
    switch (ace_type) {
    case ACCESS_ALLOWED_ACE_TYPE:
    case ACCESS_DENIED_ACE_TYPE:
    case SYSTEM_AUDIT_ACE_TYPE:
    case SYSTEM_ALARM_ACE_TYPE:
    case SYSTEM_MANDATORY_LABEL_ACE_TYPE:
    case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
    case ACCESS_DENIED_CALLBACK_ACE_TYPE:
    case SYSTEM_AUDIT_CALLBACK_ACE_TYPE:
    case SYSTEM_ALARM_CALLBACK_ACE_TYPE:
        return 1;
    default:
        return 0;
    }
}

/* Validate one ACE against the bytes remaining in the ACL from this ACE's
 * start: header fits, AceSize is non-zero / header-sized / within `remaining`,
 * and (for SID-bearing types) the inline SID after header + 4-byte Mask is
 * well-formed and fits inside THIS ACE (AceSize - 8), not merely the ACL.
 * Returns the ACE size (>= sizeof(ACE_HEADER)) on success, 0 on failure.
 * Shared by RtlValidAcl and RtlGetAceEx so both apply identical bounds. */
static uint32_t ace_body_valid(const ACE_HEADER *hdr, uint32_t remaining)
{
    uint32_t ace_size, sid_off;
    const SID *sid;

    if (remaining < sizeof(ACE_HEADER))
        return 0;

    ace_size = (uint32_t)hdr->AceSize;
    if (ace_size < sizeof(ACE_HEADER) || ace_size > remaining)
        return 0;

    /* Every ACE type we support is SID-bearing (fixed Header+Mask+SidStart).
     * Reject any other type: object/unknown ACEs have a variable Flags+GUID
     * prefix we do not parse yet, and approving one would let a consumer that
     * reads Mask/SID at fixed offsets (e.g. RtlAclToCStr) overread past
     * AceSize on a validator-approved ACL. */
    if (!ace_is_sid_bearing(hdr->AceType))
        return 0;

    /* The inline SID (after header + 4-byte Mask) must be well-formed AND fit
     * inside THIS ACE (AceSize - 8), not merely inside the ACL. */
    sid_off = sizeof(ACE_HEADER) + sizeof(uint32_t); /* 8 */
    if (ace_size < sid_off)
        return 0;
    sid = (const SID *)((const uint8_t *)hdr + sid_off);
    if (RtlLengthSidBounded(sid, ace_size - sid_off) == 0)
        return 0;

    return ace_size;
}

/* Validate the fixed ACL header for UNTRUSTED input: the 8-byte header must be
 * readable, AclRevision must be one of the two defined revisions (ACL_REVISION
 * for basic ACEs, ACL_REVISION_DS when object ACEs are present -- NT RtlValidAcl
 * model), and AclSize must cover the header without exceeding `avail`. Returns
 * the widened AclSize on success, 0 on failure. Shared by RtlValidAcl and
 * RtlGetAceEx so both untrusted-input APIs agree on what a valid ACL header is. */
static uint32_t acl_header_valid(const ACL *acl, uint32_t avail)
{
    uint32_t acl_size;

    if (!acl || avail < sizeof(ACL))
        return 0;
    if (acl->AclRevision != ACL_REVISION && acl->AclRevision != ACL_REVISION_DS)
        return 0;
    acl_size = (uint32_t)acl->AclSize;   /* widen from uint16_t */
    if (acl_size < sizeof(ACL) || acl_size > avail)
        return 0;
    return acl_size;
}

int RtlValidAcl(const ACL *acl, uint32_t avail)
{
    uint32_t acl_size, ace_count, cursor, i;
    const uint8_t *p;

    acl_size = acl_header_valid(acl, avail);
    if (!acl_size)
        return 0;
    ace_count = (uint32_t)acl->AceCount;

    /* Walk every ACE, keeping the cursor within AclSize (subtraction-form). */
    p      = (const uint8_t *)acl + sizeof(ACL);
    cursor = sizeof(ACL);
    for (i = 0; i < ace_count; i++) {
        uint32_t ace_size = ace_body_valid((const ACE_HEADER *)p,
                                           acl_size - cursor);
        if (!ace_size)
            return 0;
        p      += ace_size;
        cursor += ace_size;
    }

    return 1;
}

int RtlGetAceEx(const ACL *acl, uint32_t index, ACE_HEADER **ace, uint32_t avail)
{
    uint32_t acl_size, cursor, i;
    const uint8_t *p;

    if (!ace)
        return -1;

    /* Same untrusted ACL-header validation as RtlValidAcl (incl. AclRevision). */
    acl_size = acl_header_valid(acl, avail);
    if (!acl_size)
        return -1;
    if (index >= (uint32_t)acl->AceCount)
        return -1;

    p      = (const uint8_t *)acl + sizeof(ACL);
    cursor = sizeof(ACL);
    for (i = 0; i < index; i++) {
        uint32_t ace_size = ace_body_valid((const ACE_HEADER *)p,
                                           acl_size - cursor);
        if (!ace_size)
            return -1;
        p      += ace_size;
        cursor += ace_size;
    }

    /* The target ACE must be FULLY valid (size + inline SID), not merely have
     * a readable header -- callers read the mask/SID from the returned ACE. */
    if (!ace_body_valid((const ACE_HEADER *)p, acl_size - cursor))
        return -1;

    *ace = (ACE_HEADER *)p;
    return 0;
}

/* ---- RtlAclToCStr ------------------------------------------------------ */

/* Well-known SID → short alias for SDDL output */
static const char *sid_to_alias(const SID *sid)
{
    if (!sid) return "??";

    /* Match by authority + sub-authority pattern */
    if (sid->IdentifierAuthority[5] == 5 && sid->SubAuthorityCount >= 1) {
        if (sid->SubAuthorityCount == 1) {
            if (sid->SubAuthority[0] == 18) return "SY";  /* LocalSystem */
            if (sid->SubAuthority[0] == 19) return "LS";  /* LocalService */
            if (sid->SubAuthority[0] == 20) return "NS";  /* NetworkService */
        }
        if (sid->SubAuthorityCount == 2 && sid->SubAuthority[0] == 32) {
            if (sid->SubAuthority[1] == 544) return "BA";  /* Builtin\\Admins */
            if (sid->SubAuthority[1] == 545) return "BU";  /* Builtin\\Users */
            if (sid->SubAuthority[1] == 546) return "BG";  /* Builtin\\Guests */
        }
    }
    if (sid->IdentifierAuthority[5] == 1 && sid->SubAuthorityCount == 1 &&
        sid->SubAuthority[0] == 0)
        return "WD";  /* Everyone */
    if (sid->IdentifierAuthority[5] == 3 && sid->SubAuthorityCount == 1 &&
        sid->SubAuthority[0] == 0)
        return "CO";  /* Creator Owner */

    return (const char *)0;  /* no alias -- caller must format full SID */
}

int RtlAclToCStr(const ACL *acl, char *buf, uint32_t len)
{
    uint32_t pos = 0;
    uint32_t i;
    int w;

    if (!acl || !buf || len < 4)
        return -1;

    buf[0] = '\0';

    for (i = 0; i < acl->AceCount && pos < len - 1; i++) {
        ACE_HEADER *hdr;
        const char *type_str;
        uint32_t mask;
        const SID *sid;
        const char *alias;

        if (RtlGetAce(acl, i, &hdr) != 0)
            break;

        switch (hdr->AceType) {
        case ACCESS_ALLOWED_ACE_TYPE:          type_str = "A";  break;
        case ACCESS_DENIED_ACE_TYPE:           type_str = "D";  break;
        case SYSTEM_AUDIT_ACE_TYPE:            type_str = "AU"; break;
        case SYSTEM_MANDATORY_LABEL_ACE_TYPE:  type_str = "ML"; break;
        default:                               type_str = "?";  break;
        }

        mask = *(uint32_t *)((uint8_t *)hdr + sizeof(ACE_HEADER));
        sid  = (const SID *)((uint8_t *)hdr + sizeof(ACE_HEADER) + sizeof(uint32_t));
        alias = sid_to_alias(sid);

        if (alias) {
            w = snprintf(buf + pos, len - pos, "(%s;;0x%x;;;%s)",
                         type_str, mask, alias);
        } else {
            char sid_str[80];
            if (RtlConvertSidToString(sid_str, sizeof(sid_str), sid) < 0)
                snprintf(sid_str, sizeof(sid_str), "<invalid-sid>");
            w = snprintf(buf + pos, len - pos, "(%s;;0x%x;;;%s)",
                         type_str, mask, sid_str);
        }

        if (w < 0 || pos + (uint32_t)w >= len)
            break;
        pos += (uint32_t)w;
    }

    return (int)pos;
}
