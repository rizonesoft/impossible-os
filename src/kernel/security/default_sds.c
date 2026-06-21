/* ============================================================================
 * default_sds.c -- Static default security descriptors for kernel objects
 *
 * Pre-built self-relative security descriptors for each object type.
 * Built once at boot via se_default_sds_init().
 * ============================================================================ */

#include "kernel/security/default_sds.h"
#include "kernel/security/acl.h"
#include "kernel/security/sid.h"

extern void *memset(void *s, int c, size_t n);

/* Max self-relative SD size: header(20) + owner(12) + DACL(8 + 3*24) = ~112.
 * Use 256 per blob for safety. */
#define SD_BLOB_SIZE  256
#define SD_TYPE_COUNT 4

static uint8_t  g_sd_blobs[SD_TYPE_COUNT][SD_BLOB_SIZE];
static uint32_t g_sd_sizes[SD_TYPE_COUNT];
static int      g_sd_inited = 0;

/* ---- Build a self-relative SD from components -------------------------- */

static uint32_t build_sd(uint8_t *out, uint32_t out_len,
                         const SID *owner, const ACL *dacl)
{
    SECURITY_DESCRIPTOR sd;
    uint32_t rel_len = out_len;

    RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    RtlSetOwnerSecurityDescriptor(&sd, (SID *)owner, 0);
    RtlSetDaclSecurityDescriptor(&sd, 1, (ACL *)dacl, 0);

    if (RtlAbsoluteToSelfRelativeSD(&sd, out, &rel_len) != 0)
        return 0;

    return rel_len;
}

/* ---- Type 0: Default (SY+BA=Full, WD=ReadControl) ---------------------- */

static void build_default(void)
{
    uint8_t acl_buf[128];
    ACL *dacl = (ACL *)acl_buf;

    RtlCreateAcl(dacl, sizeof(acl_buf), ACL_REVISION);
    RtlAddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_ALL, SeLocalSystemSid);
    RtlAddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_ALL, SeBuiltinAdministratorsSid);
    RtlAddAccessAllowedAce(dacl, ACL_REVISION, READ_CONTROL, SeWorldSid);

    g_sd_sizes[SE_SD_TYPE_DEFAULT] =
        build_sd(g_sd_blobs[SE_SD_TYPE_DEFAULT], SD_BLOB_SIZE,
                 SeLocalSystemSid, dacl);
}

/* ---- Type 1: Process (SY=Full, BA=0x1FFFFF, WD=0x1000) ---------------- */

static void build_process(void)
{
    uint8_t acl_buf[128];
    ACL *dacl = (ACL *)acl_buf;

    RtlCreateAcl(dacl, sizeof(acl_buf), ACL_REVISION);
    RtlAddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_ALL, SeLocalSystemSid);
    RtlAddAccessAllowedAce(dacl, ACL_REVISION, 0x001FFFFF, SeBuiltinAdministratorsSid);
    RtlAddAccessAllowedAce(dacl, ACL_REVISION, 0x00001000, SeWorldSid);

    g_sd_sizes[SE_SD_TYPE_PROCESS] =
        build_sd(g_sd_blobs[SE_SD_TYPE_PROCESS], SD_BLOB_SIZE,
                 SeLocalSystemSid, dacl);
}

/* ---- Type 2: Token (SY=Full, Owner=Query 0x0008) ----------------------- */

static void build_token(void)
{
    uint8_t acl_buf[128];
    ACL *dacl = (ACL *)acl_buf;

    RtlCreateAcl(dacl, sizeof(acl_buf), ACL_REVISION);
    RtlAddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_ALL, SeLocalSystemSid);
    RtlAddAccessAllowedAce(dacl, ACL_REVISION, 0x00000008, SeCreatorOwnerSid);

    g_sd_sizes[SE_SD_TYPE_TOKEN] =
        build_sd(g_sd_blobs[SE_SD_TYPE_TOKEN], SD_BLOB_SIZE,
                 SeLocalSystemSid, dacl);
}

/* ---- Type 3: Registry Key (SY+BA=Full, BU=Read) ------------------------ */

static void build_registry(void)
{
    uint8_t acl_buf[128];
    ACL *dacl = (ACL *)acl_buf;

    RtlCreateAcl(dacl, sizeof(acl_buf), ACL_REVISION);
    RtlAddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_ALL, SeLocalSystemSid);
    RtlAddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_ALL, SeBuiltinAdministratorsSid);
    RtlAddAccessAllowedAce(dacl, ACL_REVISION, READ_CONTROL, SeBuiltinUsersSid);

    g_sd_sizes[SE_SD_TYPE_REGISTRY_KEY] =
        build_sd(g_sd_blobs[SE_SD_TYPE_REGISTRY_KEY], SD_BLOB_SIZE,
                 SeLocalSystemSid, dacl);
}

/* ---- Public API -------------------------------------------------------- */

void se_default_sds_init(void)
{
    if (g_sd_inited)
        return;

    memset(g_sd_blobs, 0, sizeof(g_sd_blobs));
    memset(g_sd_sizes, 0, sizeof(g_sd_sizes));

    build_default();
    build_process();
    build_token();
    build_registry();

    g_sd_inited = 1;
}

const void *SeCreateDefaultSD(uint32_t type)
{
    if (!g_sd_inited)
        se_default_sds_init();

    if (type >= SD_TYPE_COUNT || g_sd_sizes[type] == 0)
        return (const void *)0;

    return g_sd_blobs[type];
}

uint32_t SeGetDefaultSDSize(uint32_t type)
{
    if (!g_sd_inited)
        se_default_sds_init();

    if (type >= SD_TYPE_COUNT)
        return 0;

    return g_sd_sizes[type];
}

/* ---- SeCreateCreatorSD -------------------------------------------------- */

int SeCreateCreatorSD(SECURITY_DESCRIPTOR *sd_out,
                      const SID *creator_sid,
                      void *dacl_buf, uint32_t dacl_buf_size)
{
    ACL *dacl;
    uint32_t ace_fixed = sizeof(ACE_HEADER) + sizeof(uint32_t); /* ACE header + Mask */
    uint32_t need;

    if (!sd_out || !creator_sid || !dacl_buf)
        return -1;

    /* Require room for the ACL header + ALL THREE ACEs. A too-small buffer would
     * make RtlAddAccessAllowedAce silently drop the trailing (World) ACE while we
     * still returned success -- a DACL missing a documented grant. Size it from
     * the actual SID lengths and reject undersized buffers up front. */
    need = (uint32_t)sizeof(ACL)
         + ace_fixed + RtlLengthSid(creator_sid)
         + ace_fixed + RtlLengthSid(SeLocalSystemSid)
         + ace_fixed + RtlLengthSid(SeWorldSid);
    if (dacl_buf_size < need)
        return -1;

    dacl = (ACL *)dacl_buf;
    RtlCreateAcl(dacl, (uint16_t)dacl_buf_size, ACL_REVISION);
    if (RtlAddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_ALL, creator_sid) != 0 ||
        RtlAddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_ALL, SeLocalSystemSid) != 0 ||
        RtlAddAccessAllowedAce(dacl, ACL_REVISION, READ_CONTROL, SeWorldSid) != 0)
        return -1;  /* incomplete DACL -- never publish a partial grant set */

    RtlCreateSecurityDescriptor(sd_out, SECURITY_DESCRIPTOR_REVISION);
    RtlSetOwnerSecurityDescriptor(sd_out, (SID *)creator_sid, 0);
    RtlSetDaclSecurityDescriptor(sd_out, 1, dacl, 0);

    return 0;
}
