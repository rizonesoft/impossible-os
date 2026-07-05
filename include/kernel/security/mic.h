/* ============================================================================
 * mic.h -- Mandatory Integrity Control (MIC)
 *
 * Windows integrity-level machinery: every token carries an integrity level
 * (Untrusted..System) and every securable object an optional integrity label
 * in its SACL. The mandatory policy (No-Write-Up by default) denies a lower-
 * integrity subject write/read/execute access to a higher-integrity object,
 * checked BEFORE the discretionary (DACL) check.
 *
 * These are the SRM building blocks consumed by the SeAccessCheck
 * access-decision engine.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/security/sid.h"
#include "kernel/security/acl.h"
#include "kernel/security/token.h"
#include "kernel/security/generic_mapping.h"  /* GENERIC_MAPPING */
#include "kernel/nt/nt_types.h"    /* ACCESS_MASK */
#include "kernel/nt/ntstatus.h"    /* NTSTATUS */

/* ---- Integrity-level RIDs (last SubAuthority of an S-1-16-x SID) --------- */

#define SECURITY_MANDATORY_UNTRUSTED_RID  0x0000  /* 0     */
#define SECURITY_MANDATORY_LOW_RID        0x1000  /* 4096  */
#define SECURITY_MANDATORY_MEDIUM_RID     0x2000  /* 8192  */
#define SECURITY_MANDATORY_HIGH_RID       0x3000  /* 12288 */
#define SECURITY_MANDATORY_SYSTEM_RID     0x4000  /* 16384 */

/* ---- MIC access classes -------------------------------------------------- */

/* Generic + standard bits that unconditionally count as write / read / execute
 * for the No-*-Up policy, regardless of object type. SeCheckMandatoryAccess
 * ALSO folds in the object's GENERIC_MAPPING (when supplied) so object-specific
 * rights (e.g. FILE_WRITE_DATA, KEY_SET_VALUE) cannot bypass the policy.
 * MAXIMUM_ALLOWED is in every class: it resolves to whatever the DACL grants,
 * so a lower-integrity subject requesting it is restricted under ANY No-*-Up. */
#define MIC_WRITE_MASK    (GENERIC_WRITE | GENERIC_ALL | WRITE_DAC | WRITE_OWNER | DELETE | MAXIMUM_ALLOWED)
#define MIC_READ_MASK     (GENERIC_READ | GENERIC_ALL | READ_CONTROL | MAXIMUM_ALLOWED)
#define MIC_EXECUTE_MASK  (GENERIC_EXECUTE | GENERIC_ALL | MAXIMUM_ALLOWED)

/* ---- API ----------------------------------------------------------------- */

/* Integrity level of a token: the last SubAuthority (RID) of its
 * IntegrityLevelSid. Returns SECURITY_MANDATORY_MEDIUM_RID when the token or
 * its IL SID is NULL, is not a well-formed S-1-16 mandatory-label SID, or
 * carries no subauthority (fail-safe default). */
uint32_t SeGetTokenIntegrityLevel(const ACCESS_TOKEN *token);

/* Integrity level of an object's security descriptor: the RID of the
 * SYSTEM_MANDATORY_LABEL_ACE in the SACL. Returns MEDIUM when there is no SACL
 * or no label on an otherwise-valid SD; returns SYSTEM (fail closed) when the
 * SACL is malformed so a lower-IL subject cannot write up through corruption. */
uint32_t SeGetObjectIntegrityLevel(const SECURITY_DESCRIPTOR *sd);

/* Compare two integrity levels: -1 if subject_il < object_il, 0 if equal,
 * +1 if subject_il > object_il. */
int SeCompareMandatoryLevels(uint32_t subject_il, uint32_t object_il);

/* Mandatory-policy check (No-Write-Up + optional No-Read-Up/No-Execute-Up).
 * When subject_il < object_il and `desired` intersects a guarded access class
 * whose policy bit is set in the object's mandatory label, returns
 * STATUS_ACCESS_DENIED; otherwise STATUS_SUCCESS. A malformed SACL fails
 * closed (STATUS_ACCESS_DENIED).
 *
 * `token` is the effective (impersonation-or-primary) token. `sd` MUST be a
 * kernel-owned / already-validated absolute descriptor -- its SACL is walked
 * defensively (bounded validators) but the pointer itself is trusted.
 * `mapping` is the object type's GENERIC_MAPPING: it lets the check treat
 * object-specific rights (FILE_WRITE_DATA, KEY_SET_VALUE, ...) as write/read/
 * execute so they cannot bypass the policy. It may be NULL, in which case only
 * the generic + standard bits (MIC_*_MASK) are classified.
 *
 * This is a conservative boolean GATE, not an access resolver: a lower-IL
 * MAXIMUM_ALLOWED request is denied outright (fail-safe) rather than resolved
 * to a reduced grant. The SeAccessCheck engine owns MAXIMUM_ALLOWED resolution
 * against the DACL; a deny here means "MIC blocks this", not the final answer. */
NTSTATUS SeCheckMandatoryAccess(const ACCESS_TOKEN *token,
                                const SECURITY_DESCRIPTOR *sd,
                                ACCESS_MASK desired,
                                const GENERIC_MAPPING *mapping);
