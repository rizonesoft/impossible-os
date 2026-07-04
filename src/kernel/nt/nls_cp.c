/* ============================================================================
 * nls_cp.c -- Code page conversion providers (UTF-8 / CP437 / CP850 / CP1252)
 *
 * See nls_cp.h for the conversion contract and mode semantics. Design points
 * adopted from the pre-code review:
 *  - Encode (UTF-16 -> UTF-8/SBCS) validates UTF-16 well-formedness: lone or
 *    invalid surrogates fail in STRICT and emit one replacement/default per
 *    invalid scalar in REPLACE; a valid surrogate pair is one scalar.
 *  - SBCS decode rejects undefined bytes (CP1252 has 0x81/0x8D/0x8F/0x90/0x9D):
 *    STRICT fails, REPLACE emits U+FFFD.
 *  - Best-fit substitutions on SBCS are restricted to exactly one output byte so
 *    the max_char_size == 1 sizing contract holds.
 * ============================================================================ */

#include "kernel/types.h"
#include "libc/string.h"           /* memset */
#include "kernel/nt/nls_cp.h"
#include "kernel/klog.h"
#include "registry.h"

/* ---- Single-byte code-page tables (byte -> UTF-16; NLS_CP_UNDEFINED == gap) - */

/* Windows-1252. 0x00-0x7F ASCII; 0x80-0x9F Windows extensions (0x81/0x8D/0x8F/
 * 0x90/0x9D undefined); 0xA0-0xFF Latin-1 identity. */
static const uint16_t s_cp1252[256] = {
    0x0000,0x0001,0x0002,0x0003,0x0004,0x0005,0x0006,0x0007,0x0008,0x0009,0x000A,0x000B,0x000C,0x000D,0x000E,0x000F,
    0x0010,0x0011,0x0012,0x0013,0x0014,0x0015,0x0016,0x0017,0x0018,0x0019,0x001A,0x001B,0x001C,0x001D,0x001E,0x001F,
    0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
    0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
    0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
    0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,
    0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
    0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x007F,
    0x20AC,0xFFFF,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0xFFFF,0x017D,0xFFFF,
    0xFFFF,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0xFFFF,0x017E,0x0178,
    0x00A0,0x00A1,0x00A2,0x00A3,0x00A4,0x00A5,0x00A6,0x00A7,0x00A8,0x00A9,0x00AA,0x00AB,0x00AC,0x00AD,0x00AE,0x00AF,
    0x00B0,0x00B1,0x00B2,0x00B3,0x00B4,0x00B5,0x00B6,0x00B7,0x00B8,0x00B9,0x00BA,0x00BB,0x00BC,0x00BD,0x00BE,0x00BF,
    0x00C0,0x00C1,0x00C2,0x00C3,0x00C4,0x00C5,0x00C6,0x00C7,0x00C8,0x00C9,0x00CA,0x00CB,0x00CC,0x00CD,0x00CE,0x00CF,
    0x00D0,0x00D1,0x00D2,0x00D3,0x00D4,0x00D5,0x00D6,0x00D7,0x00D8,0x00D9,0x00DA,0x00DB,0x00DC,0x00DD,0x00DE,0x00DF,
    0x00E0,0x00E1,0x00E2,0x00E3,0x00E4,0x00E5,0x00E6,0x00E7,0x00E8,0x00E9,0x00EA,0x00EB,0x00EC,0x00ED,0x00EE,0x00EF,
    0x00F0,0x00F1,0x00F2,0x00F3,0x00F4,0x00F5,0x00F6,0x00F7,0x00F8,0x00F9,0x00FA,0x00FB,0x00FC,0x00FD,0x00FE,0x00FF,
};

/* IBM CP437 (DOS US). 0x00-0x7F ASCII; 0x80-0xFF accented + box-drawing. All 256
 * bytes are defined. */
static const uint16_t s_cp437[256] = {
    0x0000,0x0001,0x0002,0x0003,0x0004,0x0005,0x0006,0x0007,0x0008,0x0009,0x000A,0x000B,0x000C,0x000D,0x000E,0x000F,
    0x0010,0x0011,0x0012,0x0013,0x0014,0x0015,0x0016,0x0017,0x0018,0x0019,0x001A,0x001B,0x001C,0x001D,0x001E,0x001F,
    0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
    0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
    0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
    0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,
    0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
    0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x007F,
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0,
};

/* IBM CP850 (DOS Latin-1). 0x00-0x7F ASCII; 0x80-0xFF Latin-1 + box-drawing. All
 * 256 bytes are defined. */
static const uint16_t s_cp850[256] = {
    0x0000,0x0001,0x0002,0x0003,0x0004,0x0005,0x0006,0x0007,0x0008,0x0009,0x000A,0x000B,0x000C,0x000D,0x000E,0x000F,
    0x0010,0x0011,0x0012,0x0013,0x0014,0x0015,0x0016,0x0017,0x0018,0x0019,0x001A,0x001B,0x001C,0x001D,0x001E,0x001F,
    0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
    0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
    0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
    0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,
    0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
    0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x007F,
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00F8,0x00A3,0x00D8,0x00D7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x00AE,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x00C1,0x00C2,0x00C0,0x00A9,0x2563,0x2551,0x2557,0x255D,0x00A2,0x00A5,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x00E3,0x00C3,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x00A4,
    0x00F0,0x00D0,0x00CA,0x00CB,0x00C8,0x0131,0x00CD,0x00CE,0x00CF,0x2518,0x250C,0x2588,0x2584,0x00A6,0x00CC,0x2580,
    0x00D3,0x00DF,0x00D4,0x00D2,0x00F5,0x00D5,0x00B5,0x00FE,0x00DE,0x00DA,0x00DB,0x00D9,0x00FD,0x00DD,0x00AF,0x00B4,
    0x00AD,0x00B1,0x2017,0x00BE,0x00B6,0x00A7,0x00F7,0x00B8,0x00B0,0x00A8,0x00B7,0x00B9,0x00B3,0x00B2,0x25A0,0x00A0,
};

/* ---- 1-byte best-fit substitutions (encode) ------------------------------ */
/* Restricted to exactly one output byte so SBCS max_char_size == 1 holds. Maps
 * common Windows-1252 typographic scalars to their ASCII lookalikes. */
struct nls_bestfit { uint16_t scalar; uint8_t byte; };
static const struct nls_bestfit s_bestfit[] = {
    { 0x2018, 0x27 }, { 0x2019, 0x27 },   /* ' ' -> ' */
    { 0x201C, 0x22 }, { 0x201D, 0x22 },   /* " " -> " */
    { 0x2013, 0x2D }, { 0x2014, 0x2D },   /* en/em dash -> - */
    { 0x2022, 0x2A },                     /* bullet -> * */
    { 0x00A0, 0x20 },                     /* nbsp -> space */
};

/* ---- Providers ----------------------------------------------------------- */
static const nls_cp_provider_t s_providers[] = {
    { NLS_CP_UTF8, "utf-8",        0,         NLS_CP_UTF8_MAX_CHAR, NLS_CP_DEFAULT_BYTE },
    { NLS_CP_1252, "windows-1252", s_cp1252,  1,                    NLS_CP_DEFAULT_BYTE },
    { NLS_CP_437,  "ibm437",       s_cp437,   1,                    NLS_CP_DEFAULT_BYTE },
    { NLS_CP_850,  "ibm850",       s_cp850,   1,                    NLS_CP_DEFAULT_BYTE },
};
#define NLS_CP_PROVIDER_COUNT (sizeof(s_providers) / sizeof(s_providers[0]))

/* ---- Policy -------------------------------------------------------------- */

/* ACP/OEMCP are snapshotted at boot (nls_cp_register_defaults) into these caches
 * so the SMP-hot pseudo-page resolution path (nls_cp_resolve) NEVER opens a
 * registry handle: the registry handle pool is not lock-protected, so a per-call
 * RegOpenKeyEx would race concurrent conversions. The system ANSI/OEM code page
 * is fixed at boot (Windows requires a reboot to change it), so a boot snapshot
 * is the correct semantics. 0 == not yet cached -> compiled default. */
static uint32_t s_acp_cache = 0;
static uint32_t s_oemcp_cache = 0;

static uint32_t nls_cp_read_reg_dword(const char *value, uint32_t fallback)
{
    HKEY k;
    uint32_t v;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Nls", 0, KEY_READ, &k) != 0)
        return fallback;
    if (RegGetDword(k, value, &v) != 0)
        v = fallback;
    RegCloseKey(k);
    return v;
}

uint32_t nls_cp_get_acp(void)
{
    uint32_t v = __atomic_load_n(&s_acp_cache, __ATOMIC_ACQUIRE);
    return v ? v : NLS_CP_DEFAULT_ACP;
}

uint32_t nls_cp_get_oemcp(void)
{
    uint32_t v = __atomic_load_n(&s_oemcp_cache, __ATOMIC_ACQUIRE);
    return v ? v : NLS_CP_DEFAULT_OEMCP;
}

uint32_t nls_cp_resolve(uint32_t cp)
{
    switch (cp) {
    case NLS_CP_ACP:
    case NLS_CP_THREAD_ACP:   /* no per-thread locale yet: thread ACP == system ACP */
        return nls_cp_get_acp();
    case NLS_CP_OEMCP:
        return nls_cp_get_oemcp();
    default:
        return cp;
    }
}

const nls_cp_provider_t *nls_cp_get_provider(uint32_t cp)
{
    uint32_t i, r = nls_cp_resolve(cp);
    for (i = 0; i < NLS_CP_PROVIDER_COUNT; i++)
        if (s_providers[i].code_page == r)
            return &s_providers[i];
    return 0;
}

/* True when `cp` names a concrete supported provider (not a pseudo page). Used
 * to validate a registry policy value before it is cached. */
static int nls_cp_is_concrete_provider(uint32_t cp)
{
    uint32_t i;
    for (i = 0; i < NLS_CP_PROVIDER_COUNT; i++)
        if (s_providers[i].code_page == cp)
            return 1;
    return 0;
}

/* Snapshot a registry policy code page for the boot cache. Falls back to `deflt`
 * when the value is malformed (RegGetDword fails) OR names an unsupported /
 * pseudo code page -- a bad ACP/OEMCP must never poison the cache into BADCP. */
static uint32_t nls_cp_snapshot_policy(const char *value, uint32_t deflt)
{
    uint32_t v = nls_cp_read_reg_dword(value, deflt);
    return nls_cp_is_concrete_provider(v) ? v : deflt;
}

void nls_cp_register_defaults(void)
{
    HKEY k;
    uint32_t disp;
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Nls", 0, (const char *)0, 0,
                       KEY_ALL_ACCESS, (void *)0, &k, &disp) != 0) {
        klog(LOG_WARN, "nls", "cannot create HKLM\\SYSTEM\\Nls -- code page defaults skipped");
        /* Still seed the caches so GetACP/GetOEMCP are deterministic. */
        __atomic_store_n(&s_acp_cache, NLS_CP_DEFAULT_ACP, __ATOMIC_RELEASE);
        __atomic_store_n(&s_oemcp_cache, NLS_CP_DEFAULT_OEMCP, __ATOMIC_RELEASE);
        return;
    }
    {
        uint32_t v;
        /* Seed each value only when absent/malformed -- never clobber an existing
         * admin- or hive-supplied policy (RegGetDword fails for a missing value
         * or a non-DWORD type). */
        if (RegGetDword(k, "ACP", &v) != 0)
            RegSetDword(k, "ACP", NLS_CP_DEFAULT_ACP);
        if (RegGetDword(k, "OEMCP", &v) != 0)
            RegSetDword(k, "OEMCP", NLS_CP_DEFAULT_OEMCP);
    }
    RegCloseKey(k);
    /* Snapshot the (possibly pre-existing) values into the SMP-safe caches
     * (single boot writer, before any thread runs a pseudo-page conversion);
     * an unsupported policy value falls back to the compiled default. */
    __atomic_store_n(&s_acp_cache, nls_cp_snapshot_policy("ACP", NLS_CP_DEFAULT_ACP),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_oemcp_cache, nls_cp_snapshot_policy("OEMCP", NLS_CP_DEFAULT_OEMCP),
                     __ATOMIC_RELEASE);
}

/* ---- UTF-8 <-> UTF-16LE -------------------------------------------------- */

/* Decode one scalar starting at src[i]. On success returns bytes consumed (1..4)
 * and sets *scalar. On an invalid sequence returns 0 (caller consumes 1 byte). */
static int utf8_decode_scalar(const uint8_t *src, uint32_t len, uint32_t i, uint32_t *scalar)
{
    uint8_t b0 = src[i];
    uint32_t s;

    if (b0 < 0x80) { *scalar = b0; return 1; }
    if (b0 < 0xC2) return 0;                     /* 0x80-0xBF stray / 0xC0-0xC1 overlong */
    /* Truncation guards are subtraction-form (len - i, never i + N) so the index
     * math cannot wrap when src_len is near UINT32_MAX. i < len holds (loop
     * invariant), so len - i >= 1 and the subtraction never underflows. */
    if (b0 < 0xE0) {                             /* 2-byte */
        if (len - i < 2) return 0;
        if ((src[i+1] & 0xC0) != 0x80) return 0;
        s = ((uint32_t)(b0 & 0x1F) << 6) | (src[i+1] & 0x3F);
        if (s < 0x80) return 0;                  /* overlong */
        *scalar = s; return 2;
    }
    if (b0 < 0xF0) {                             /* 3-byte */
        if (len - i < 3) return 0;
        if ((src[i+1] & 0xC0) != 0x80 || (src[i+2] & 0xC0) != 0x80) return 0;
        s = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(src[i+1] & 0x3F) << 6) | (src[i+2] & 0x3F);
        if (s < 0x800) return 0;                 /* overlong */
        if (s >= 0xD800 && s <= 0xDFFF) return 0;/* surrogate */
        *scalar = s; return 3;
    }
    if (b0 < 0xF5) {                             /* 4-byte */
        if (len - i < 4) return 0;
        if ((src[i+1] & 0xC0) != 0x80 || (src[i+2] & 0xC0) != 0x80 || (src[i+3] & 0xC0) != 0x80)
            return 0;
        s = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(src[i+1] & 0x3F) << 12) |
            ((uint32_t)(src[i+2] & 0x3F) << 6) | (src[i+3] & 0x3F);
        if (s < 0x10000 || s > 0x10FFFF) return 0; /* overlong / out of range */
        *scalar = s; return 4;
    }
    return 0;                                    /* 0xF5-0xFF invalid lead */
}

/* Append one UTF-16 code unit; returns 0 ok, 1 = out of cap. The running count
 * is uint64 so a sizing pass (dst == NULL) over a huge input cannot wrap; the
 * conversion functions reject a count above INT_MAX before the int return. */
static int u16_put(uint16_t *dst, uint32_t cap, uint64_t *n, uint16_t v)
{
    if (dst) { if (*n >= cap) return 1; dst[*n] = v; }
    (*n)++;
    return 0;
}

/* Append one UTF-8 byte; returns 0 ok, 1 = out of cap. */
static int u8_put(uint8_t *dst, uint32_t cap, uint64_t *n, uint8_t v)
{
    if (dst) { if (*n >= cap) return 1; dst[*n] = v; }
    (*n)++;
    return 0;
}

/* Encode one scalar to UTF-16 (BMP direct, astral as a surrogate pair). */
static int u16_put_scalar(uint16_t *dst, uint32_t cap, uint64_t *n, uint32_t s)
{
    if (s < 0x10000)
        return u16_put(dst, cap, n, (uint16_t)s);
    s -= 0x10000;
    if (u16_put(dst, cap, n, (uint16_t)(0xD800 + (s >> 10)))) return 1;
    return u16_put(dst, cap, n, (uint16_t)(0xDC00 + (s & 0x3FF)));
}

/* Encode one scalar to UTF-8. */
static int u8_put_scalar(uint8_t *dst, uint32_t cap, uint64_t *n, uint32_t s)
{
    if (s < 0x80)
        return u8_put(dst, cap, n, (uint8_t)s);
    if (s < 0x800) {
        if (u8_put(dst, cap, n, (uint8_t)(0xC0 | (s >> 6)))) return 1;
        return u8_put(dst, cap, n, (uint8_t)(0x80 | (s & 0x3F)));
    }
    if (s < 0x10000) {
        if (u8_put(dst, cap, n, (uint8_t)(0xE0 | (s >> 12)))) return 1;
        if (u8_put(dst, cap, n, (uint8_t)(0x80 | ((s >> 6) & 0x3F)))) return 1;
        return u8_put(dst, cap, n, (uint8_t)(0x80 | (s & 0x3F)));
    }
    if (u8_put(dst, cap, n, (uint8_t)(0xF0 | (s >> 18)))) return 1;
    if (u8_put(dst, cap, n, (uint8_t)(0x80 | ((s >> 12) & 0x3F)))) return 1;
    if (u8_put(dst, cap, n, (uint8_t)(0x80 | ((s >> 6) & 0x3F)))) return 1;
    return u8_put(dst, cap, n, (uint8_t)(0x80 | (s & 0x3F)));
}

/* A conversion mode must be exactly one of the three defined values -- an
 * unknown mode must fail closed (NLS_CP_ERR_PARAM), never silently downgrade to
 * lossy replacement. */
static int nls_cp_mode_ok(int mode)
{
    return mode == NLS_CP_STRICT || mode == NLS_CP_REPLACE || mode == NLS_CP_BESTFIT;
}

int nls_cp_utf8_to_utf16(const uint8_t *src, uint32_t src_len,
                         uint16_t *dst, uint32_t dst_cap, int mode)
{
    uint32_t i = 0;
    uint64_t n = 0;
    if (!src || !nls_cp_mode_ok(mode)) return NLS_CP_ERR_PARAM;
    while (i < src_len) {
        uint32_t scalar;
        int used = utf8_decode_scalar(src, src_len, i, &scalar);
        if (used == 0) {
            if (mode == NLS_CP_STRICT) return NLS_CP_ERR_INVALID;
            if (u16_put(dst, dst_cap, &n, NLS_CP_REPL_U16)) return NLS_CP_ERR_TOO_SMALL;
            i += 1;                              /* skip one bad byte */
            continue;
        }
        if (u16_put_scalar(dst, dst_cap, &n, scalar)) return NLS_CP_ERR_TOO_SMALL;
        i += (uint32_t)used;
    }
    if (n > 0x7FFFFFFFu) return NLS_CP_ERR_TOO_SMALL;   /* not representable in int */
    return (int)n;
}

/* Read one UTF-16 scalar at src[i]; returns units consumed (1 or 2), sets
 * *scalar, or 0 on a lone/invalid surrogate. */
static int u16_next_scalar(const uint16_t *src, uint32_t len, uint32_t i, uint32_t *scalar)
{
    uint16_t c = src[i];
    if (c < 0xD800 || c > 0xDFFF) { *scalar = c; return 1; }
    if (c <= 0xDBFF && i + 1 < len && src[i+1] >= 0xDC00 && src[i+1] <= 0xDFFF) {
        *scalar = 0x10000 + ((uint32_t)(c - 0xD800) << 10) + (src[i+1] - 0xDC00);
        return 2;
    }
    *scalar = 0;
    return 0;                                    /* lone high / lone low surrogate */
}

int nls_cp_utf16_to_utf8(const uint16_t *src, uint32_t src_len,
                         uint8_t *dst, uint32_t dst_cap, int mode)
{
    uint32_t i = 0;
    uint64_t n = 0;
    if (!src || !nls_cp_mode_ok(mode)) return NLS_CP_ERR_PARAM;
    while (i < src_len) {
        uint32_t scalar;
        int used = u16_next_scalar(src, src_len, i, &scalar);
        if (used == 0) {
            if (mode == NLS_CP_STRICT) return NLS_CP_ERR_INVALID;
            if (u8_put_scalar(dst, dst_cap, &n, NLS_CP_REPL_U16)) return NLS_CP_ERR_TOO_SMALL;
            i += 1;
            continue;
        }
        if (u8_put_scalar(dst, dst_cap, &n, scalar)) return NLS_CP_ERR_TOO_SMALL;
        i += (uint32_t)used;
    }
    if (n > 0x7FFFFFFFu) return NLS_CP_ERR_TOO_SMALL;
    return (int)n;
}

/* ---- Code page <-> UTF-16LE ---------------------------------------------- */

int nls_cp_to_utf16(uint32_t cp, const uint8_t *src, uint32_t src_len,
                    uint16_t *dst, uint32_t dst_cap, int mode)
{
    const nls_cp_provider_t *p = nls_cp_get_provider(cp);
    uint32_t i;
    uint64_t n = 0;
    if (!src || !nls_cp_mode_ok(mode)) return NLS_CP_ERR_PARAM;
    if (!p) return NLS_CP_ERR_BADCP;
    if (!p->sbcs_to_u16)                         /* UTF-8 */
        return nls_cp_utf8_to_utf16(src, src_len, dst, dst_cap, mode);
    for (i = 0; i < src_len; i++) {
        uint16_t u = p->sbcs_to_u16[src[i]];
        if (u == NLS_CP_UNDEFINED) {
            if (mode == NLS_CP_STRICT) return NLS_CP_ERR_INVALID;
            u = NLS_CP_REPL_U16;
        }
        if (u16_put(dst, dst_cap, &n, u)) return NLS_CP_ERR_TOO_SMALL;
    }
    if (n > 0x7FFFFFFFu) return NLS_CP_ERR_TOO_SMALL;
    return (int)n;
}

/* Reverse SBCS lookup: find the byte encoding scalar `u` (< 0x10000). Returns the
 * byte, or -1 if unmapped. Scans the 256-entry table (cold path). */
static int sbcs_encode_lookup(const uint16_t *tbl, uint16_t u)
{
    int b;
    for (b = 0; b < 256; b++)
        if (tbl[b] == u && u != NLS_CP_UNDEFINED)
            return b;
    return -1;
}

static int bestfit_lookup(uint16_t u, uint8_t *out)
{
    uint32_t i;
    for (i = 0; i < sizeof(s_bestfit) / sizeof(s_bestfit[0]); i++)
        if (s_bestfit[i].scalar == u) { *out = s_bestfit[i].byte; return 1; }
    return 0;
}

int nls_cp_from_utf16(uint32_t cp, const uint16_t *src, uint32_t src_len,
                      uint8_t *dst, uint32_t dst_cap, int mode)
{
    const nls_cp_provider_t *p = nls_cp_get_provider(cp);
    uint32_t i = 0;
    uint64_t n = 0;
    if (!src || !nls_cp_mode_ok(mode)) return NLS_CP_ERR_PARAM;
    if (!p) return NLS_CP_ERR_BADCP;
    if (!p->sbcs_to_u16)                         /* UTF-8 */
        return nls_cp_utf16_to_utf8(src, src_len, dst, dst_cap, mode);
    while (i < src_len) {
        uint32_t scalar;
        int used = u16_next_scalar(src, src_len, i, &scalar);
        uint8_t b;
        int enc;
        if (used == 0) {                         /* invalid surrogate */
            if (mode == NLS_CP_STRICT) return NLS_CP_ERR_INVALID;
            if (u8_put(dst, dst_cap, &n, p->default_char)) return NLS_CP_ERR_TOO_SMALL;
            i += 1;
            continue;
        }
        if (scalar < 0x10000 && (enc = sbcs_encode_lookup(p->sbcs_to_u16, (uint16_t)scalar)) >= 0) {
            b = (uint8_t)enc;                     /* exact mapping */
        } else if (mode == NLS_CP_STRICT) {
            return NLS_CP_ERR_INVALID;            /* astral or unmapped */
        } else if (mode == NLS_CP_BESTFIT && scalar < 0x10000 &&
                   bestfit_lookup((uint16_t)scalar, &b)) {
            /* b set by best-fit (1 byte) */
        } else {
            b = p->default_char;                 /* replacement / no best-fit */
        }
        if (u8_put(dst, dst_cap, &n, b)) return NLS_CP_ERR_TOO_SMALL;
        i += (uint32_t)used;
    }
    if (n > 0x7FFFFFFFu) return NLS_CP_ERR_TOO_SMALL;
    return (int)n;
}

/* ---- Metadata / enumeration ---------------------------------------------- */

int nls_cp_is_valid(uint32_t cp)
{
    return nls_cp_get_provider(cp) != 0;
}

int nls_cp_get_info(uint32_t cp, nls_cpinfo_t *o)
{
    const nls_cp_provider_t *p = nls_cp_get_provider(cp);
    uint32_t i;
    if (!o) return NLS_CP_ERR_PARAM;
    if (!p) return NLS_CP_ERR_BADCP;
    /* Fully zero the ABI-shaped struct first so the name[] tail after the NUL and
     * any implicit padding are deterministic -- a future CPINFOEX syscall may
     * copy this out and must not disclose stale kernel bytes. */
    memset(o, 0, sizeof(*o));
    o->code_page = p->code_page;
    o->max_char_size = p->max_char_size;
    o->default_char = p->default_char;
    o->lead_byte_count = 0;                      /* SBCS + UTF-8: no DBCS lead bytes */
    for (i = 0; i < sizeof(o->name) - 1 && p->name[i]; i++)
        o->name[i] = p->name[i];
    o->name[i] = '\0';
    return 0;
}

int nls_cp_is_dbcs_lead_byte(uint32_t cp, uint8_t b)
{
    (void)cp;
    (void)b;
    /* No DBCS providers yet: every supported page is SBCS or UTF-8, so no byte
     * is a lead byte. Extends here when a DBCS provider (e.g. CP932) is added. */
    return 0;
}

uint32_t nls_cp_enum(uint32_t *out_ids, uint32_t cap)
{
    uint32_t i;
    for (i = 0; i < NLS_CP_PROVIDER_COUNT; i++) {
        if (out_ids && i < cap)
            out_ids[i] = s_providers[i].code_page;
    }
    return (uint32_t)NLS_CP_PROVIDER_COUNT;
}
