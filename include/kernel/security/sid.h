/* ============================================================================
 * sid.h -- Security Identifier (SID) type and utilities
 *
 * A SID uniquely identifies a security principal (user, group, service,
 * machine).  Format: S-1-<Authority>-<Sub1>-<Sub2>-...
 *
 * The struct uses a flexible array member for SubAuthority so the
 * binary size varies with SubAuthorityCount.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* --- SID structure ------------------------------------------------------- */

#define SID_REVISION            1
#define SID_MAX_SUB_AUTHORITIES 15
#define SID_MAX_SIZE            (8 + 4 * SID_MAX_SUB_AUTHORITIES)  /* 68 bytes */

typedef struct {
    uint8_t  Revision;                   /* always SID_REVISION (1) */
    uint8_t  SubAuthorityCount;          /* 0–15 */
    uint8_t  IdentifierAuthority[6];     /* big-endian 48-bit authority */
    uint32_t SubAuthority[];             /* variable-length */
} SID;

/* SID_AND_ATTRIBUTES -- used in token group lists */
#define SE_GROUP_MANDATORY          0x00000001
#define SE_GROUP_ENABLED_BY_DEFAULT 0x00000002
#define SE_GROUP_ENABLED            0x00000004
#define SE_GROUP_OWNER              0x00000008
#define SE_GROUP_USE_FOR_DENY_ONLY  0x00000010
#define SE_GROUP_INTEGRITY          0x00000020
#define SE_GROUP_INTEGRITY_ENABLED  0x00000040
#define SE_GROUP_RESOURCE           0x20000000
#define SE_GROUP_LOGON_ID           0xC0000000

typedef struct {
    SID     *Sid;
    uint32_t Attributes;
} SID_AND_ATTRIBUTES;

/* --- Well-known SID identifier authorities ------------------------------- */

#define SECURITY_NULL_SID_AUTHORITY         {0,0,0,0,0,0}
#define SECURITY_WORLD_SID_AUTHORITY        {0,0,0,0,0,1}
#define SECURITY_LOCAL_SID_AUTHORITY        {0,0,0,0,0,2}
#define SECURITY_CREATOR_SID_AUTHORITY      {0,0,0,0,0,3}
#define SECURITY_NT_AUTHORITY               {0,0,0,0,0,5}
#define SECURITY_MANDATORY_LABEL_AUTHORITY  {0,0,0,0,0,16}

/* --- Well-known SID constants (pointers to static storage) --------------- */

extern const SID *const SeNullSid;                  /* S-1-0-0       */
extern const SID *const SeWorldSid;                 /* S-1-1-0       (Everyone) */
extern const SID *const SeCreatorOwnerSid;           /* S-1-3-0       */
extern const SID *const SeNtAuthoritySid;            /* S-1-5         */
extern const SID *const SeInteractiveSid;            /* S-1-5-4       */
extern const SID *const SeServiceSid;               /* S-1-5-6       */
extern const SID *const SeAnonymousLogonSid;         /* S-1-5-7       */
extern const SID *const SeLocalSystemSid;            /* S-1-5-18      */
extern const SID *const SeLocalServiceSid;           /* S-1-5-19      */
extern const SID *const SeNetworkServiceSid;         /* S-1-5-20      */
extern const SID *const SeBuiltinAdministratorsSid;  /* S-1-5-32-544  */
extern const SID *const SeBuiltinUsersSid;           /* S-1-5-32-545  */
extern const SID *const SeBuiltinGuestsSid;          /* S-1-5-32-546  */

/* Integrity level SIDs */
extern const SID *const SeILUntrusted;               /* S-1-16-0      */
extern const SID *const SeILLow;                     /* S-1-16-4096   */
extern const SID *const SeILMedium;                  /* S-1-16-8192   */
extern const SID *const SeILHigh;                    /* S-1-16-12288  */
extern const SID *const SeILSystem;                  /* S-1-16-16384  */

/* --- SID utility functions ----------------------------------------------- */

/* Return total byte length of a SID: 8 + 4 * SubAuthorityCount */
uint32_t RtlLengthSid(const SID *sid);

/* Compare two SIDs for equality */
int RtlEqualSid(const SID *a, const SID *b);

/* Copy a SID into a caller-supplied buffer.
 * buf_size must be >= RtlLengthSid(src).  Returns 0 on success, -1 on error. */
int RtlCopySid(void *buf, uint32_t buf_size, const SID *src);

/* Initialize a SID with the given authority and sub-authority count.
 * Caller must ensure sid points to a buffer large enough for count sub-auths. */
void RtlInitializeSid(SID *sid, const uint8_t authority[6], uint8_t count);

/* Return a pointer to the Nth sub-authority (0-based).  NULL if out of range. */
uint32_t *RtlSubAuthoritySid(SID *sid, uint32_t n);

/* Format a SID as "S-1-X-Y-Z-..." into a buffer.
 * Returns the number of characters written (excluding NUL), or -1 on error. */
int RtlConvertSidToString(char *buf, uint32_t buf_size, const SID *sid);

/* Validate a SID: Revision == 1 and SubAuthorityCount <= 15. */
int RtlValidSid(const SID *sid);

/* Generate a service SID: S-1-5-80-<hash of name>.
 * Writes into sid_buf which must be at least SID_MAX_SIZE bytes.
 * Returns 0 on success, -1 on error. */
int RtlCreateServiceSid(const char *service_name, SID *sid_buf, uint32_t *sid_len);
