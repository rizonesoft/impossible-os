/* ============================================================================
 * mic.c -- Mandatory Integrity Control (MIC)
 *
 * Integrity-level lookups + the No-Write-Up mandatory policy. See mic.h for
 * the contract. These are pure functions over a caller-supplied token /
 * security descriptor -- no global state, no locks, no allocation. The SACL
 * walk uses the bounded validators so an imported (untrusted) SD cannot cause
 * an out-of-bounds read; a malformed SACL fails closed.
 * ============================================================================ */

#include "kernel/security/mic.h"

/* True if `sid` is a well-formed S-1-16 mandatory-label SID: revision 1, the
 * mandatory-label identifier authority, and exactly one subauthority (the IL
 * RID). Rejects an arbitrary SID whose last subauthority would otherwise be
 * misread as an integrity level. */
static int sid_is_integrity_label(const SID *sid)
{
    static const uint8_t mandatory_auth[6] = SECURITY_MANDATORY_LABEL_AUTHORITY;
    uint32_t i;

    if (!sid)
        return 0;
    if (sid->Revision != SID_REVISION || sid->SubAuthorityCount != 1)
        return 0;
    for (i = 0; i < 6; i++)
        if (sid->IdentifierAuthority[i] != mandatory_auth[i])
            return 0;
    return 1;
}

/* Walk the SACL for the SYSTEM_MANDATORY_LABEL_ACE. On a valid (or absent)
 * SACL sets the il and policy outputs -- the label's RID + policy Mask, or the
 * MEDIUM / No-Write-Up defaults when no label is present -- and returns 0. -1
 * (fail closed) on a malformed SACL or a malformed label SID. Single walk,
 * shared by SeGetObjectIntegrityLevel and SeCheckMandatoryAccess. */
static int find_mandatory_label(const SECURITY_DESCRIPTOR *sd,
                                uint32_t *il, uint32_t *policy)
{
    const ACL *sacl;
    uint32_t count, i;

    *il     = SECURITY_MANDATORY_MEDIUM_RID;
    *policy = SYSTEM_MANDATORY_LABEL_NO_WRITE_UP;

    if (!sd || !sd->Sacl)
        return 0;   /* no SACL -> MEDIUM / No-Write-Up defaults */

    sacl = sd->Sacl;

    /* The SD may have been imported from an untrusted source; validate the
     * SACL before walking (bounds every ACE + inline SID). */
    if (!RtlValidAcl(sacl, sacl->AclSize))
        return -1;  /* malformed -> fail closed */

    count = (uint32_t)sacl->AceCount;
    for (i = 0; i < count; i++) {
        ACE_HEADER *ace;
        const SYSTEM_MANDATORY_LABEL_ACE *lbl;
        const SID *sid;

        if (RtlGetAceEx(sacl, i, &ace, sacl->AclSize) != 0)
            return -1;
        if (ace->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
            continue;

        lbl = (const SYSTEM_MANDATORY_LABEL_ACE *)ace;
        sid = (const SID *)&lbl->SidStart;
        /* RtlValidAcl proved the inline SID fits its ACE, but the label SID
         * must specifically be a well-formed S-1-16 mandatory-label SID
         * (one subauthority = the RID); anything else is malformed. */
        if (!sid_is_integrity_label(sid))
            return -1;

        *il     = sid->SubAuthority[0];
        *policy = lbl->Mask;
        return 0;
    }

    return 0;   /* valid SACL, no mandatory label -> MEDIUM / No-Write-Up */
}

uint32_t SeGetTokenIntegrityLevel(const ACCESS_TOKEN *token)
{
    if (!token || !sid_is_integrity_label(token->IntegrityLevelSid))
        return SECURITY_MANDATORY_MEDIUM_RID;   /* NULL/non-IL SID -> MEDIUM */

    /* The single subauthority is the IL RID (0 == Untrusted is legitimate). */
    return token->IntegrityLevelSid->SubAuthority[0];
}

uint32_t SeGetObjectIntegrityLevel(const SECURITY_DESCRIPTOR *sd)
{
    uint32_t il, policy;

    if (find_mandatory_label(sd, &il, &policy) != 0)
        return SECURITY_MANDATORY_SYSTEM_RID;   /* fail closed on malformed SACL */
    return il;
}

int SeCompareMandatoryLevels(uint32_t subject_il, uint32_t object_il)
{
    if (subject_il < object_il)
        return -1;
    if (subject_il > object_il)
        return 1;
    return 0;
}

NTSTATUS SeCheckMandatoryAccess(const ACCESS_TOKEN *token,
                                const SECURITY_DESCRIPTOR *sd,
                                ACCESS_MASK desired,
                                const GENERIC_MAPPING *mapping)
{
    uint32_t subject_il, object_il, policy;
    ACCESS_MASK write_bits, read_bits, exec_bits;

    if (find_mandatory_label(sd, &object_il, &policy) != 0)
        return STATUS_ACCESS_DENIED;   /* malformed SACL -> fail closed */

    subject_il = SeGetTokenIntegrityLevel(token);

    /* Only a lower-integrity subject is restricted (No-*-Up). */
    if (SeCompareMandatoryLevels(subject_il, object_il) >= 0)
        return STATUS_SUCCESS;

    /* Fold the object type's GENERIC_MAPPING into each access class so that
     * object-specific rights (e.g. FILE_WRITE_DATA) are classified, not just
     * the generic + standard bits -- otherwise a raw specific-right request
     * could bypass the policy. */
    write_bits = MIC_WRITE_MASK;
    read_bits  = MIC_READ_MASK;
    exec_bits  = MIC_EXECUTE_MASK;
    if (mapping) {
        write_bits |= mapping->GenericWrite;
        read_bits  |= mapping->GenericRead;
        exec_bits  |= mapping->GenericExecute;
    }

    if ((policy & SYSTEM_MANDATORY_LABEL_NO_WRITE_UP) && (desired & write_bits))
        return STATUS_ACCESS_DENIED;
    if ((policy & SYSTEM_MANDATORY_LABEL_NO_READ_UP) && (desired & read_bits))
        return STATUS_ACCESS_DENIED;
    if ((policy & SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP) && (desired & exec_bits))
        return STATUS_ACCESS_DENIED;

    return STATUS_SUCCESS;
}
