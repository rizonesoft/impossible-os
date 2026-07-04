/* ============================================================================
 * nls_cp.h -- Code page conversion providers (UTF-8 / CP437 / CP850 / CP1252)
 *
 * The code-page conversion layer of the atom/NLS/locale subsystem. Kernel-
 * internal code-page <-> UTF-16LE conversion, a provider registry keyed by code
 * page ID, the registry-backed system ANSI/OEM code-page policy (GetACP/
 * GetOEMCP), and the CPINFO/IsDBCSLeadByte/IsValidCodePage/EnumSystemCodePages
 * metadata surface.
 *
 * Like the NLS fold/ctype accessors, these are kernel-resident C functions
 * consumed by kernel-mode code (VFS/console/future syscalls); the Win32
 * MultiByteToWideChar / WideCharToMultiByte / GetACP export + SSDT surface is
 * owned by the NLS native-syscall section, not here.
 *
 * Conversion contract: pass dst == NULL to run a sizing pass (returns the
 * required dst element count without writing). A non-NULL dst writes up to
 * dst_cap elements and returns the count written, or NLS_CP_ERR_TOO_SMALL. All
 * validation (UTF-8 well-formedness, UTF-16 surrogate well-formedness, SBCS
 * undefined-byte rejection) is fail-closed in STRICT mode; REPLACE emits one
 * replacement per invalid scalar; BESTFIT tries a 1-byte substitution first.
 * ============================================================================ */
#ifndef KERNEL_NT_NLS_CP_H
#define KERNEL_NT_NLS_CP_H

#include "kernel/types.h"

/* ---- Conversion modes ---------------------------------------------------- */
#define NLS_CP_STRICT     0   /* fail (NLS_CP_ERR_INVALID) on any invalid/unmapped */
#define NLS_CP_REPLACE    1   /* U+FFFD on decode, default char on encode-unmapped */
#define NLS_CP_BESTFIT    2   /* REPLACE + a 1-byte best-fit substitution on encode */

/* ---- Code page IDs (real + Win32 pseudo pages) --------------------------- */
#define NLS_CP_ACP        0u      /* system ANSI (GetACP) */
#define NLS_CP_OEMCP      1u      /* system OEM (GetOEMCP) */
#define NLS_CP_MACCP      2u      /* Macintosh (unsupported -> unknown) */
#define NLS_CP_THREAD_ACP 3u      /* thread ANSI (== system ACP until per-thread locale lands) */
#define NLS_CP_SYMBOL     42u     /* symbol (unsupported -> unknown) */
#define NLS_CP_437        437u
#define NLS_CP_850        850u
#define NLS_CP_1252       1252u
#define NLS_CP_UTF8       65001u

/* Registry-absent fallbacks for the system ANSI / OEM code pages. */
#define NLS_CP_DEFAULT_ACP    NLS_CP_1252
#define NLS_CP_DEFAULT_OEMCP  NLS_CP_437

/* ---- Sentinels / constants ----------------------------------------------- */
#define NLS_CP_UNDEFINED    0xFFFFu  /* SBCS table entry: byte undefined in this CP */
#define NLS_CP_REPL_U16     0xFFFDu  /* U+FFFD replacement scalar */
#define NLS_CP_DEFAULT_BYTE 0x3Fu    /* '?' default single-byte char */
#define NLS_CP_UTF8_MAX_CHAR 4u      /* max UTF-8 bytes per scalar */

/* ---- Error codes (negative; >=0 is a written/required element count) ------ */
#define NLS_CP_ERR_INVALID   (-1)  /* strict-mode invalid or unmapped input */
#define NLS_CP_ERR_TOO_SMALL (-2)  /* output buffer too small */
#define NLS_CP_ERR_BADCP     (-3)  /* unknown / unsupported code page */
#define NLS_CP_ERR_PARAM     (-4)  /* NULL / bad argument */

/* ---- Provider ------------------------------------------------------------ */
/* An SBCS provider carries a 256-entry byte->UTF-16 table (NLS_CP_UNDEFINED for
 * undefined bytes). A UTF-8 provider has sbcs_to_u16 == NULL and is handled by
 * the algorithmic codec. */
typedef struct nls_cp_provider {
    uint32_t        code_page;
    const char     *name;
    const uint16_t *sbcs_to_u16;   /* 256 entries, or NULL for UTF-8 */
    uint32_t        max_char_size; /* 1 for SBCS, NLS_CP_UTF8_MAX_CHAR for UTF-8 */
    uint8_t         default_char;  /* single-byte default substitution */
} nls_cp_provider_t;

/* Provider lookup by code page ID. Resolves the pseudo pages (ACP/OEMCP/
 * THREAD_ACP) to the concrete system page. Returns NULL for an unknown or
 * unsupported code page. */
const nls_cp_provider_t *nls_cp_get_provider(uint32_t cp);

/* ---- CPINFO / CPINFOEX metadata ------------------------------------------ */
typedef struct nls_cpinfo {
    uint32_t code_page;
    uint32_t max_char_size;
    uint8_t  default_char;
    uint8_t  lead_byte_count;   /* number of lead-byte range endpoints (0 = SBCS) */
    uint8_t  lead_bytes[12];    /* DBCS lead-byte ranges (Win32 CPINFO.LeadByte) */
    char     name[32];          /* CPINFOEX.CodePageName */
} nls_cpinfo_t;

/* ---- UTF-8 <-> UTF-16LE (algorithmic) ------------------------------------ */
/* src_len / return counts are in ELEMENTS (u8 bytes for UTF-8, u16 units for
 * UTF-16). dst == NULL runs a sizing pass. */
int nls_cp_utf8_to_utf16(const uint8_t *src, uint32_t src_len,
                         uint16_t *dst, uint32_t dst_cap, int mode);
int nls_cp_utf16_to_utf8(const uint16_t *src, uint32_t src_len,
                         uint8_t *dst, uint32_t dst_cap, int mode);

/* ---- Code page <-> UTF-16LE (MultiByteToWideChar / WideCharToMultiByte) --- */
int nls_cp_to_utf16(uint32_t cp, const uint8_t *src, uint32_t src_len,
                    uint16_t *dst, uint32_t dst_cap, int mode);
int nls_cp_from_utf16(uint32_t cp, const uint16_t *src, uint32_t src_len,
                      uint8_t *dst, uint32_t dst_cap, int mode);

/* ---- Policy (GetACP / GetOEMCP) ------------------------------------------ */
/* Read HKLM\SYSTEM\Nls ACP/OEMCP DWORDs (RegGetDword); fall back to the compiled
 * default when the registry is unavailable or the key is absent. */
uint32_t nls_cp_get_acp(void);
uint32_t nls_cp_get_oemcp(void);
/* Resolve a pseudo code page (ACP/OEMCP/THREAD_ACP) to a concrete page. */
uint32_t nls_cp_resolve(uint32_t cp);

/* ---- Metadata / enumeration ---------------------------------------------- */
int      nls_cp_is_valid(uint32_t cp);                  /* IsValidCodePage */
int      nls_cp_get_info(uint32_t cp, nls_cpinfo_t *o); /* GetCPInfoEx (0 ok, err<0) */
int      nls_cp_is_dbcs_lead_byte(uint32_t cp, uint8_t b); /* IsDBCSLeadByte */
uint32_t nls_cp_enum(uint32_t *out_ids, uint32_t cap);  /* EnumSystemCodePages -> count */

/* Seed HKLM\SYSTEM\Nls ACP/OEMCP defaults. Called from registry_populate_defaults. */
void nls_cp_register_defaults(void);

#endif /* KERNEL_NT_NLS_CP_H */
