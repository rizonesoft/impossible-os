/* ============================================================================
 * acl.c — ACL construction and debug helpers
 *
 * Implements TODO-11 §3.2: RtlCreateAcl, RtlAddAccessAllowedAce,
 * RtlAddAccessDeniedAce, RtlAddMandatoryAce, RtlGetAce, RtlAclToCStr.
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
 */
typedef struct {
    uint8_t  Revision;
    uint8_t  Sbz1;
    uint16_t Control;
    uint32_t OffsetOwner;
    uint32_t OffsetGroup;
    uint32_t OffsetSacl;
    uint32_t OffsetDacl;
} SECURITY_DESCRIPTOR_RELATIVE;

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

int RtlSelfRelativeToAbsoluteSD(const void *rel,
                                SECURITY_DESCRIPTOR *abs,
                                void *abs_buf, uint32_t abs_buf_len)
{
    const SECURITY_DESCRIPTOR_RELATIVE *sr;
    const uint8_t *base;
    uint8_t *wp;
    uint32_t used = 0;
    uint32_t len;

    if (!rel || !abs || !abs_buf)
        return -1;

    sr   = (const SECURITY_DESCRIPTOR_RELATIVE *)rel;
    base = (const uint8_t *)rel;
    wp   = (uint8_t *)abs_buf;

    memset(abs, 0, sizeof(*abs));
    abs->Revision = sr->Revision;
    abs->Control  = sr->Control & ~SE_SELF_RELATIVE;

    /* Copy Owner SID */
    if (sr->OffsetOwner) {
        const SID *src = (const SID *)(base + sr->OffsetOwner);
        len = RtlLengthSid(src);
        if (used + len > abs_buf_len) return -1;
        memcpy(wp + used, src, len);
        abs->Owner = (SID *)(wp + used);
        used += len;
    }

    /* Copy Group SID */
    if (sr->OffsetGroup) {
        const SID *src = (const SID *)(base + sr->OffsetGroup);
        len = RtlLengthSid(src);
        if (used + len > abs_buf_len) return -1;
        memcpy(wp + used, src, len);
        abs->Group = (SID *)(wp + used);
        used += len;
    }

    /* Copy SACL */
    if (sr->OffsetSacl) {
        const ACL *src = (const ACL *)(base + sr->OffsetSacl);
        len = src->AclSize;
        if (used + len > abs_buf_len) return -1;
        memcpy(wp + used, src, len);
        abs->Sacl = (ACL *)(wp + used);
        used += len;
    }

    /* Copy DACL */
    if (sr->OffsetDacl) {
        const ACL *src = (const ACL *)(base + sr->OffsetDacl);
        len = src->AclSize;
        if (used + len > abs_buf_len) return -1;
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

    return (const char *)0;  /* no alias — caller must format full SID */
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
            RtlConvertSidToString(sid_str, sizeof(sid_str), sid);
            w = snprintf(buf + pos, len - pos, "(%s;;0x%x;;;%s)",
                         type_str, mask, sid_str);
        }

        if (w < 0 || pos + (uint32_t)w >= len)
            break;
        pos += (uint32_t)w;
    }

    return (int)pos;
}
