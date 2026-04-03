/* ============================================================================
 * acl.h -- SECURITY_DESCRIPTOR, ACL, and ACE types
 *
 * Defines the core security data structures used by the SRM to make
 * access decisions.  A SECURITY_DESCRIPTOR owns an optional Owner SID,
 * Group SID, DACL (discretionary), and SACL (system/audit).
 *
 * The struct tag "security_descriptor" matches the forward declaration
 * in ob.h so OBJECT_HEADER can point to it without including this file.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/security/sid.h"

/* ---- SECURITY_DESCRIPTOR Control flags --------------------------------- */

#define SE_OWNER_DEFAULTED       0x0001
#define SE_GROUP_DEFAULTED       0x0002
#define SE_DACL_PRESENT          0x0004
#define SE_DACL_DEFAULTED        0x0008
#define SE_SACL_PRESENT          0x0010
#define SE_SACL_DEFAULTED        0x0020
#define SE_DACL_AUTO_INHERIT_REQ 0x0100
#define SE_SACL_AUTO_INHERIT_REQ 0x0200
#define SE_DACL_AUTO_INHERITED   0x0400
#define SE_SACL_AUTO_INHERITED   0x0800
#define SE_DACL_PROTECTED        0x1000
#define SE_SACL_PROTECTED        0x2000
#define SE_SELF_RELATIVE         0x8000

#define SECURITY_DESCRIPTOR_REVISION  1

/* ---- Forward-declare ACL (used by SECURITY_DESCRIPTOR) ----------------- */

typedef struct acl ACL;

/* ---- SECURITY_DESCRIPTOR (absolute form -- pointers) -------------------- */

struct security_descriptor {
    uint8_t   Revision;    /* SECURITY_DESCRIPTOR_REVISION (1) */
    uint8_t   Sbz1;        /* padding */
    uint16_t  Control;     /* SE_* control flags */
    SID      *Owner;       /* owner SID (NULL = not set) */
    SID      *Group;       /* primary group SID (NULL = not set) */
    ACL      *Sacl;        /* system ACL (NULL = not present) */
    ACL      *Dacl;        /* discretionary ACL (NULL = not present) */
};

/* Provide the typedef here so users of acl.h don't need ob.h.
 * ob.h has a matching forward declaration -- both are compatible. */
typedef struct security_descriptor SECURITY_DESCRIPTOR;

/* ---- ACL ---------------------------------------------------------------- */

#define ACL_REVISION  2

struct acl {
    uint8_t  AclRevision;  /* ACL_REVISION (2) */
    uint8_t  Sbz1;         /* padding */
    uint16_t AclSize;      /* total size in bytes (header + all ACEs) */
    uint16_t AceCount;     /* number of ACEs */
    uint16_t Sbz2;         /* padding */
};

/* Bulletproofing: ACL must be exactly 8 bytes (Windows ABI) */
_Static_assert(sizeof(ACL) == 8, "ACL header must be 8 bytes (Windows ABI)");

/* ---- ACE types --------------------------------------------------------- */

#define ACCESS_ALLOWED_ACE_TYPE          0x00
#define ACCESS_DENIED_ACE_TYPE           0x01
#define SYSTEM_AUDIT_ACE_TYPE            0x02
#define SYSTEM_ALARM_ACE_TYPE            0x03
#define SYSTEM_MANDATORY_LABEL_ACE_TYPE  0x11

/* ---- ACE flags --------------------------------------------------------- */

#define OBJECT_INHERIT_ACE           0x01
#define CONTAINER_INHERIT_ACE        0x02
#define NO_PROPAGATE_INHERIT_ACE     0x04
#define INHERIT_ONLY_ACE             0x08
#define INHERITED_ACE                0x10
#define SUCCESSFUL_ACCESS_ACE_FLAG   0x40
#define FAILED_ACCESS_ACE_FLAG       0x80

/* ---- ACE_HEADER -------------------------------------------------------- */

typedef struct {
    uint8_t  AceType;
    uint8_t  AceFlags;
    uint16_t AceSize;      /* total size of ACE (header + body) */
} ACE_HEADER;

/* Bulletproofing: ACE_HEADER must be 4 bytes (Windows ABI) */
_Static_assert(sizeof(ACE_HEADER) == 4, "ACE_HEADER must be 4 bytes (Windows ABI)");

/* ---- Concrete ACE types ------------------------------------------------ */

/* ACCESS_ALLOWED_ACE / ACCESS_DENIED_ACE: the SID follows immediately
 * after SidStart in memory.  SidStart is the first uint32_t of the SID
 * (i.e., the SID is at &ace->SidStart). */

typedef struct {
    ACE_HEADER Header;
    uint32_t   Mask;       /* ACCESS_MASK -- granted/denied rights */
    uint32_t   SidStart;   /* first dword of the SID (SID follows in-line) */
} ACCESS_ALLOWED_ACE;

typedef struct {
    ACE_HEADER Header;
    uint32_t   Mask;
    uint32_t   SidStart;
} ACCESS_DENIED_ACE;

typedef struct {
    ACE_HEADER Header;
    uint32_t   Mask;
    uint32_t   SidStart;
} SYSTEM_AUDIT_ACE;

typedef struct {
    ACE_HEADER Header;
    uint32_t   Mask;       /* SYSTEM_MANDATORY_LABEL_NO_WRITE_UP etc. */
    uint32_t   SidStart;   /* integrity level SID (S-1-16-XXXX) */
} SYSTEM_MANDATORY_LABEL_ACE;

/* Bulletproofing: ACE struct sizes (Windows ABI) */
_Static_assert(sizeof(ACCESS_ALLOWED_ACE) == 12,
    "ACCESS_ALLOWED_ACE must be 12 bytes (4 header + 4 mask + 4 SidStart)");
_Static_assert(sizeof(ACCESS_DENIED_ACE) == 12,
    "ACCESS_DENIED_ACE must be 12 bytes");
_Static_assert(sizeof(SYSTEM_MANDATORY_LABEL_ACE) == 12,
    "SYSTEM_MANDATORY_LABEL_ACE must be 12 bytes");

/* ---- Mandatory label policy masks -------------------------------------- */

#define SYSTEM_MANDATORY_LABEL_NO_WRITE_UP    0x01
#define SYSTEM_MANDATORY_LABEL_NO_READ_UP     0x02
#define SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP  0x04

/* ---- Generic access rights (mapped per object type) -------------------- */

#define DELETE                   0x00010000
#define READ_CONTROL             0x00020000
#define WRITE_DAC                0x00040000
#define WRITE_OWNER              0x00080000
#define SYNCHRONIZE              0x00100000
#define STANDARD_RIGHTS_ALL      (DELETE | READ_CONTROL | WRITE_DAC | WRITE_OWNER | SYNCHRONIZE)

#define GENERIC_READ             0x80000000
#define GENERIC_WRITE            0x40000000
#define GENERIC_EXECUTE          0x20000000
#define GENERIC_ALL              0x10000000

/* ---- Helper: get SID pointer from an ACE ------------------------------- */

/* The SID starts at &ace->SidStart. Cast to SID* for access. */
#define ACE_SID(ace)  ((SID *)&(ace)->SidStart)

/* ---- SECURITY_DESCRIPTOR helpers --------------------------------------- */

/* Initialize an absolute SD. rev = SECURITY_DESCRIPTOR_REVISION (1). */
int RtlCreateSecurityDescriptor(SECURITY_DESCRIPTOR *sd, uint8_t rev);

/* Set/get Owner SID. defaulted = SE_OWNER_DEFAULTED flag. */
int RtlSetOwnerSecurityDescriptor(SECURITY_DESCRIPTOR *sd, SID *owner, int defaulted);
int RtlGetOwnerSecurityDescriptor(const SECURITY_DESCRIPTOR *sd, SID **owner, int *defaulted);

/* Set/get Group SID. */
int RtlSetGroupSecurityDescriptor(SECURITY_DESCRIPTOR *sd, SID *group, int defaulted);

/* Set/get DACL. present = whether DACL is set (SE_DACL_PRESENT). */
int RtlSetDaclSecurityDescriptor(SECURITY_DESCRIPTOR *sd, int present, ACL *dacl, int defaulted);
int RtlGetDaclSecurityDescriptor(const SECURITY_DESCRIPTOR *sd, int *present, ACL **dacl, int *defaulted);

/* Set SACL. */
int RtlSetSaclSecurityDescriptor(SECURITY_DESCRIPTOR *sd, int present, ACL *sacl, int defaulted);

/* Marshal absolute SD to a flat self-relative buffer.
 * *rel_len is in/out: on entry the buffer size, on exit the required size.
 * Returns 0 on success, -1 if buffer too small (rel_len set to needed). */
int RtlAbsoluteToSelfRelativeSD(const SECURITY_DESCRIPTOR *abs,
                                void *rel_buf, uint32_t *rel_len);

/* Unmarshal a self-relative SD back to absolute form.
 * Allocates SID/ACL copies from the supplied abs_buf workspace.
 * abs_buf_len must be large enough for the SD struct + all SIDs + ACLs.
 * Returns 0 on success, -1 on error. */
int RtlSelfRelativeToAbsoluteSD(const void *rel,
                                SECURITY_DESCRIPTOR *abs,
                                void *abs_buf, uint32_t abs_buf_len);

/* ---- ACL utility functions --------------------------------------------- */

/* Initialize an empty ACL in a caller-supplied buffer.
 * size = total buffer size (must be >= sizeof(ACL)).
 * rev  = ACL_REVISION (2). Returns 0 on success, -1 on error. */
int RtlCreateAcl(ACL *acl, uint16_t size, uint8_t rev);

/* Append an ACCESS_ALLOWED_ACE to the ACL. Returns 0 or -1 if full. */
int RtlAddAccessAllowedAce(ACL *acl, uint8_t rev, uint32_t mask, const SID *sid);

/* Append an ACCESS_DENIED_ACE to the ACL. Returns 0 or -1 if full. */
int RtlAddAccessDeniedAce(ACL *acl, uint8_t rev, uint32_t mask, const SID *sid);

/* Append a SYSTEM_MANDATORY_LABEL_ACE to the SACL. Returns 0 or -1. */
int RtlAddMandatoryAce(ACL *acl, uint8_t rev, uint8_t flags,
                       uint32_t mask, uint8_t type, const SID *integrity_sid);

/* Get the Nth ACE (0-based). Returns 0 and sets *ace, or -1 if OOB. */
int RtlGetAce(const ACL *acl, uint32_t index, ACE_HEADER **ace);

/* Format an ACL as SDDL-like string: "(A;;FA;;;SY)(D;;GA;;;BA)..."
 * Returns chars written (excluding NUL), or -1 on error. */
int RtlAclToCStr(const ACL *acl, char *buf, uint32_t len);
