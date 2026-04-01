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
