/* ============================================================================
 * sid.c -- SID primitives, well-known SID table, and utility functions
 *
 * SID type and helpers for the security reference monitor.
 * ============================================================================ */

#include "kernel/security/sid.h"
#include "kernel/klog.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);
extern int   memcmp(const void *a, const void *b, size_t n);
extern int   snprintf(char *buf, size_t size, const char *fmt, ...);
extern size_t strlen(const char *s);

/* ============================================================================
 * Well-known SID constants
 *
 * SID is a variable-length struct (flexible array member), so we use
 * concrete fixed-size structs for static initialization, then cast to
 * const SID * via extern declarations.
 * ============================================================================ */

/*
 * Concrete fixed-size SID structs for static initialization.
 * The SID type has a flexible array member, so we define concrete
 * fixed-size structs and expose const pointers to them.
 */
typedef struct { uint8_t R; uint8_t C; uint8_t A[6]; uint32_t S[1]; } SID1;
typedef struct { uint8_t R; uint8_t C; uint8_t A[6]; uint32_t S[2]; } SID2;
typedef struct { uint8_t R; uint8_t C; uint8_t A[6]; } SID0;

/* --- Null authority (0) -------------------------------------------------- */
static const SID1 g_NullSid           = { 1, 1, {0,0,0,0,0,0}, {0} };
static const SID1 g_WorldSid          = { 1, 1, {0,0,0,0,0,1}, {0} };
static const SID1 g_CreatorOwnerSid   = { 1, 1, {0,0,0,0,0,3}, {0} };

/* --- NT authority (5) ---------------------------------------------------- */
static const SID0 g_NtAuthoritySid    = { 1, 0, {0,0,0,0,0,5} };
static const SID1 g_InteractiveSid    = { 1, 1, {0,0,0,0,0,5}, {4} };
static const SID1 g_ServiceSid        = { 1, 1, {0,0,0,0,0,5}, {6} };
static const SID1 g_AnonymousLogonSid = { 1, 1, {0,0,0,0,0,5}, {7} };
static const SID1 g_LocalSystemSid    = { 1, 1, {0,0,0,0,0,5}, {18} };
static const SID1 g_LocalServiceSid   = { 1, 1, {0,0,0,0,0,5}, {19} };
static const SID1 g_NetworkServiceSid = { 1, 1, {0,0,0,0,0,5}, {20} };

/* --- Builtin domain (5-32) ----------------------------------------------- */
static const SID2 g_BuiltinAdminsSid  = { 1, 2, {0,0,0,0,0,5}, {32, 544} };
static const SID2 g_BuiltinUsersSid   = { 1, 2, {0,0,0,0,0,5}, {32, 545} };
static const SID2 g_BuiltinGuestsSid  = { 1, 2, {0,0,0,0,0,5}, {32, 546} };

/* --- Mandatory label authority (16) -------------------------------------- */
static const SID1 g_ILUntrusted       = { 1, 1, {0,0,0,0,0,16}, {0} };
static const SID1 g_ILLow             = { 1, 1, {0,0,0,0,0,16}, {4096} };
static const SID1 g_ILMedium          = { 1, 1, {0,0,0,0,0,16}, {8192} };
static const SID1 g_ILHigh            = { 1, 1, {0,0,0,0,0,16}, {12288} };
static const SID1 g_ILSystem          = { 1, 1, {0,0,0,0,0,16}, {16384} };

/* --- Exported pointers --------------------------------------------------- */
const SID *const SeNullSid                  = (const SID *)&g_NullSid;
const SID *const SeWorldSid                 = (const SID *)&g_WorldSid;
const SID *const SeCreatorOwnerSid          = (const SID *)&g_CreatorOwnerSid;
const SID *const SeNtAuthoritySid           = (const SID *)&g_NtAuthoritySid;
const SID *const SeInteractiveSid           = (const SID *)&g_InteractiveSid;
const SID *const SeServiceSid              = (const SID *)&g_ServiceSid;
const SID *const SeAnonymousLogonSid        = (const SID *)&g_AnonymousLogonSid;
const SID *const SeLocalSystemSid           = (const SID *)&g_LocalSystemSid;
const SID *const SeLocalServiceSid          = (const SID *)&g_LocalServiceSid;
const SID *const SeNetworkServiceSid        = (const SID *)&g_NetworkServiceSid;
const SID *const SeBuiltinAdministratorsSid = (const SID *)&g_BuiltinAdminsSid;
const SID *const SeBuiltinUsersSid          = (const SID *)&g_BuiltinUsersSid;
const SID *const SeBuiltinGuestsSid         = (const SID *)&g_BuiltinGuestsSid;
const SID *const SeILUntrusted              = (const SID *)&g_ILUntrusted;
const SID *const SeILLow                    = (const SID *)&g_ILLow;
const SID *const SeILMedium                 = (const SID *)&g_ILMedium;
const SID *const SeILHigh                   = (const SID *)&g_ILHigh;
const SID *const SeILSystem                 = (const SID *)&g_ILSystem;

/* ============================================================================
 * SID utility functions
 * ============================================================================ */

uint32_t RtlLengthSid(const SID *sid)
{
    if (!sid)
        return 0;
    return 8 + 4 * (uint32_t)sid->SubAuthorityCount;
}

int RtlValidSid(const SID *sid)
{
    if (!sid)
        return 0;
    if (sid->Revision != SID_REVISION)
        return 0;
    if (sid->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES)
        return 0;
    return 1;
}

int RtlEqualSid(const SID *a, const SID *b)
{
    uint32_t len_a, len_b;

    if (!a || !b)
        return 0;

    /* Reject malformed SIDs before computing length from SubAuthorityCount */
    if (!RtlValidSid(a) || !RtlValidSid(b))
        return 0;

    len_a = RtlLengthSid(a);
    len_b = RtlLengthSid(b);
    if (len_a != len_b)
        return 0;

    return memcmp(a, b, len_a) == 0;
}

int RtlCopySid(void *buf, uint32_t buf_size, const SID *src)
{
    uint32_t len;

    if (!buf || !src)
        return -1;

    if (!RtlValidSid(src))
        return -1;

    len = RtlLengthSid(src);
    if (buf_size < len)
        return -1;

    memcpy(buf, src, len);
    return 0;
}

void RtlInitializeSid(SID *sid, const uint8_t authority[6], uint8_t count)
{
    uint8_t i;

    if (!sid || !authority)
        return;

    /* Clamp to SID spec maximum to prevent out-of-bounds writes */
    if (count > SID_MAX_SUB_AUTHORITIES)
        count = SID_MAX_SUB_AUTHORITIES;

    sid->Revision = SID_REVISION;
    sid->SubAuthorityCount = count;
    for (i = 0; i < 6; i++)
        sid->IdentifierAuthority[i] = authority[i];

    /* Zero sub-authorities */
    for (i = 0; i < count; i++)
        sid->SubAuthority[i] = 0;
}

uint32_t *RtlSubAuthoritySid(SID *sid, uint32_t n)
{
    if (!sid || n >= sid->SubAuthorityCount)
        return (uint32_t *)0;

    return &sid->SubAuthority[n];
}

int RtlConvertSidToString(char *buf, uint32_t buf_size, const SID *sid)
{
    uint64_t authority;
    int written;
    uint32_t pos;
    uint8_t i;

    if (!buf || !sid || buf_size < 8)
        return -1;

    if (!RtlValidSid(sid))
        return -1;

    /* Extract 48-bit authority as a single value */
    authority = 0;
    for (i = 0; i < 6; i++)
        authority = (authority << 8) | sid->IdentifierAuthority[i];

    /* Format: S-<Revision>-<Authority>
     * Per Windows spec: authorities <= 2^32 use decimal, above use 0x hex.
     * All well-known authorities (0-16) fit in 32 bits. */
    if (authority > 0xFFFFFFFFULL)
        written = snprintf(buf, buf_size, "S-%u-0x%llx",
                           (uint32_t)sid->Revision, (unsigned long long)authority);
    else
        written = snprintf(buf, buf_size, "S-%u-%u",
                           (uint32_t)sid->Revision, (uint32_t)authority);
    if (written < 0 || (uint32_t)written >= buf_size)
        return -1;

    pos = (uint32_t)written;

    /* Append sub-authorities */
    for (i = 0; i < sid->SubAuthorityCount; i++) {
        written = snprintf(buf + pos, buf_size - pos, "-%u",
                           sid->SubAuthority[i]);
        if (written < 0 || pos + (uint32_t)written >= buf_size)
            return -1;
        pos += (uint32_t)written;
    }

    return (int)pos;
}

/* --- RtlCreateServiceSid ------------------------------------------------- */

/*
 * Simple FNV-1a hash for service name → 5 sub-authority values.
 * Service SIDs have the form S-1-5-80-<h0>-<h1>-<h2>-<h3>-<h4>.
 */
static void fnv1a_hash_name(const char *name, uint32_t out[5])
{
    uint32_t hash = 0x811c9dc5;  /* FNV offset basis */
    const char *p;
    uint32_t i;

    for (i = 0; i < 5; i++)
        out[i] = 0;

    for (p = name; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 0x01000193;  /* FNV prime */
    }

    /* Spread hash into 5 sub-authorities by re-hashing */
    out[0] = hash;
    for (i = 1; i < 5; i++) {
        hash ^= (hash >> 16);
        hash *= 0x01000193;
        out[i] = hash;
    }
}

int RtlCreateServiceSid(const char *service_name, SID *sid_buf, uint32_t *sid_len)
{
    uint32_t needed;
    uint32_t hash[5];
    uint8_t authority[6] = SECURITY_NT_AUTHORITY;
    uint32_t i;

    if (!service_name || !sid_buf || !sid_len)
        return -1;

    /* S-1-5-80-<h0>-<h1>-<h2>-<h3>-<h4> = 6 sub-authorities */
    needed = 8 + 4 * 6;  /* 32 bytes */
    if (*sid_len < needed) {
        *sid_len = needed;
        return -1;
    }

    fnv1a_hash_name(service_name, hash);

    RtlInitializeSid(sid_buf, authority, 6);
    sid_buf->SubAuthority[0] = 80;  /* SECURITY_SERVICE_ID_BASE_RID */
    for (i = 0; i < 5; i++)
        sid_buf->SubAuthority[i + 1] = hash[i];

    *sid_len = needed;
    return 0;
}
