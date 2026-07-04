/* ============================================================================
 * nls.h -- NLS table file format (nls_table_v1) + boot loader + query authority
 *
 * The kernel-owned National Language Support (NLS) table layer. Section 4 of the
 * atom/NLS/locale subsystem defines a compact on-disk table format, a boot-time
 * loader that reads a table from C:\Impossible\System\NLS\ and publishes it, and
 * a small compiled-in invariant fallback served when no valid table is present.
 *
 * Two case-fold authorities coexist by DESIGN, not by accident:
 *   - rtl_upcase_char (nt_rtlstr.h) is the INVARIANT, always-compiled authority
 *     for ASCII + Latin-1 (< U+0100). The Object Manager, Registry, and atom
 *     table compare object names through it. A corrupt or hostile on-disk NLS
 *     table can NEVER alter this range: nls_upcase_char() delegates every code
 *     unit < U+0100 straight to rtl_upcase_char and only consults the loaded
 *     UPCASE chunk for U+0100 and above. Security-critical namespace comparison
 *     therefore never depends on disk-sourced data (CRC32 detects accidental
 *     corruption only; it is not an authenticity guarantee).
 *   - nls_upcase_char() is the FULL-BMP superset that adds U+0100..U+FFFF from
 *     the loaded table. It is for NON-SECURITY paths only (display, sort keys,
 *     FoldStringW); it must NEVER back a security name comparison.
 *     TRUST BOUNDARY (DECIDED): the U+0100+ fold is disk-sourced, and the loader
 *     authenticates a table only by CRC32 + the system-volume ACL (the Windows
 *     l_intl.nls trust model, not a signature), so a tampered table could
 *     collapse two distinct names. Therefore SECURITY-sensitive namespace
 *     comparison (OB / Registry / atom) MUST use the COMPILED
 *     rtl_upcase_char / rtl_upcase_char_inline authority and MUST NOT route
 *     through nls_upcase_char. The retrofitted consumers (dir_name_eq,
 *     reg_stricmp/reg_fnv1a, atom_name_eq/atom_name_hash) all fold through the
 *     compiled authority.
 *
 * The full-BMP GetStringTypeW/GetStringTypeEx and GetNLSVersionEx PUBLIC syscall
 * surface is owned by section 8; this layer only defines the format chunks and
 * the kernel-resident query accessors those syscalls build on.
 *
 * Threading: nls_init() runs once at Phase 2 boot. SMP is already online at that
 * point, so the active descriptor is published with a release store and read by
 * accessors with an acquire load; the descriptor and its backing blob are
 * immutable after publish and never freed.
 * ============================================================================ */
#ifndef KERNEL_NT_NLS_H
#define KERNEL_NT_NLS_H

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"

/* ---- On-disk nls_table_v1 format (little-endian) ------------------------- */

/* Magic "NLS1" as a little-endian uint32 (bytes 'N','L','S','1'). */
#define NLS_TABLE_V1_MAGIC    (((uint32_t)'N') | ((uint32_t)'L' << 8) | \
                               ((uint32_t)'S' << 16) | ((uint32_t)'1' << 24))
#define NLS_TABLE_V1_VERSION  1u

/* Hard cap on a loaded table so a corrupt on-disk total_size cannot drive a
 * huge contiguous PMM allocation before the header is trusted. 2 MiB comfortably
 * holds an UPCASE + three CTYPE chunks over the full BMP (4 * 0x10000 * 2 bytes
 * = 512 KiB) plus the reserved fold chunks. */
#define NLS_TABLE_V1_MAX_SIZE (2u * 1024u * 1024u)

/* Maximum chunk-directory entries accepted from an on-disk table. */
#define NLS_TABLE_V1_MAX_CHUNKS 32u

/* BMP code point count -- every per-code-point chunk is indexed [0, 0x10000). */
#define NLS_BMP_CODEPOINTS    0x10000u

/* Chunk type IDs. UPCASE + the three CTYPE namespaces are consumed here; the
 * FOLD_* chunks are RESERVED so section 7 (FoldStringW) can add payload without
 * a format break -- they are validated and carried, not interpreted, by v1. */
#define NLS_CHUNK_UPCASE      1u  /* uint16[N] invariant upcase fold */
#define NLS_CHUNK_CTYPE1      2u  /* uint16[N] C1_* character-type flags */
#define NLS_CHUNK_CTYPE2      3u  /* uint16[N] C2_* bidirectional flags */
#define NLS_CHUNK_CTYPE3      4u  /* uint16[N] C3_* text-processing flags */
#define NLS_CHUNK_FOLD_COMPAT 5u  /* reserved: MAP_FOLDCZONE compatibility fold */
#define NLS_CHUNK_FOLD_DIGIT  6u  /* reserved: MAP_FOLDDIGITS digit fold */
#define NLS_CHUNK_FOLD_WIDTH  7u  /* reserved: width fold */
#define NLS_CHUNK_TYPE_MAX    7u

/* Fixed 32-byte header, followed by chunk_count x nls_chunk_desc, then bodies. */
typedef struct nls_table_header {
    uint32_t magic;        /* NLS_TABLE_V1_MAGIC */
    uint32_t version;      /* NLS_TABLE_V1_VERSION */
    uint32_t lcid;         /* locale ID this table serves (0x007F = invariant) */
    uint32_t code_page;    /* associated code page ID, 0 for Unicode-only */
    uint32_t nls_version;  /* NLS/collation version (GetNLSVersionEx) */
    uint32_t total_size;   /* total blob size in bytes; MUST equal bytes loaded */
    uint32_t crc32;        /* kcrc32 over the whole blob with this field zeroed */
    uint32_t chunk_count;  /* number of directory entries following the header */
} nls_table_header_t;

typedef struct nls_chunk_desc {
    uint32_t type;         /* NLS_CHUNK_* */
    uint32_t offset;       /* byte offset from blob start to the chunk body */
    uint32_t size;         /* chunk body length in bytes */
} nls_chunk_desc_t;

_Static_assert(sizeof(nls_table_header_t) == 32, "nls_table_v1 header is 32 bytes");
_Static_assert(sizeof(nls_chunk_desc_t) == 12, "nls_chunk_desc is 12 bytes");
_Static_assert(__builtin_offsetof(nls_table_header_t, crc32) == 24,
               "crc32 field offset pins the on-disk contract");
_Static_assert(__builtin_offsetof(nls_table_header_t, chunk_count) == 28,
               "chunk_count field offset pins the on-disk contract");

/* ---- Win32 C1 (CT_CTYPE1) character-type flags --------------------------- */
/* Values match winnls.h so a populated CTYPE1 chunk / the compiled fallback can
 * feed GetStringTypeW (section 8) without translation. */
#define NLS_C1_UPPER   0x0001u
#define NLS_C1_LOWER   0x0002u
#define NLS_C1_DIGIT   0x0004u
#define NLS_C1_SPACE   0x0008u
#define NLS_C1_PUNCT   0x0010u
#define NLS_C1_CNTRL   0x0020u
#define NLS_C1_BLANK   0x0040u
#define NLS_C1_XDIGIT  0x0080u
#define NLS_C1_ALPHA   0x0100u
#define NLS_C1_DEFINED 0x0200u

/* ---- Published in-memory descriptor -------------------------------------- */
/* Filled by the loader (or left as the compiled fallback), then published once
 * with a release store. Immutable after publish. */
typedef struct nls_published {
    const uint8_t  *blob;         /* PMM-backed table body, or NULL for fallback */
    uint32_t        blob_pages;   /* page count backing blob (0 for fallback) */
    uint32_t        lcid;
    uint32_t        code_page;
    uint32_t        nls_version;
    const uint16_t *upcase;       uint32_t upcase_count;
    const uint16_t *ctype1;       uint32_t ctype1_count;
    const uint16_t *ctype2;       uint32_t ctype2_count;
    const uint16_t *ctype3;       uint32_t ctype3_count;
} nls_published_t;

/* ---- API ----------------------------------------------------------------- */

/* Boot-time initializer (Phase 2, after VFS + C: mount). Loads and publishes
 * C:\Impossible\System\NLS\invariant.nls when present and valid; otherwise logs
 * a degraded-state warning, sets BOOT_DEGRADED, and serves the compiled invariant
 * fallback. Never halts boot. Marks SUBSYS_NLS ready. */
void nls_init(void);

/*
 * nls_table_parse -- PURE validation + chunk resolution over a caller-provided
 * blob (no VFS, no allocation, no globals). `blob` points at `len` bytes already
 * read from disk (or a test buffer). On STATUS_SUCCESS, `out` is filled with the
 * resolved chunk pointers (aliasing into blob) and element counts. Validates:
 * magic, version, total_size == len and <= NLS_TABLE_V1_MAX_SIZE, chunk_count
 * bound, CRC32, and that every chunk lies wholly after the directory and inside
 * the blob with no offset+size overflow and no duplicate type. Returns
 * STATUS_INVALID_IMAGE_FORMAT on any structural violation, STATUS_INVALID_PARAMETER
 * on a NULL argument or a len below the header size.
 */
NTSTATUS nls_table_parse(const uint8_t *blob, uint32_t len, nls_published_t *out);

/*
 * nls_upcase_char -- full-BMP invariant uppercase fold. Code units below U+0100
 * always route through rtl_upcase_char (compiled authority; never disk-sourced).
 * U+0100 and above use the published UPCASE chunk when it covers the code unit,
 * else fall back to returning the code unit unchanged (matches rtl_upcase_char's
 * >= U+0100 behavior).
 */
uint16_t nls_upcase_char(uint16_t c);

/*
 * nls_char_type -- Win32 character-type classification. `which` selects the flag
 * namespace: 1 = CT_CTYPE1 (C1_*), 2 = CT_CTYPE2, 3 = CT_CTYPE3. Uses the
 * published CTYPE chunk when it covers the code unit; for CTYPE1 below U+0100 a
 * compiled ASCII + Latin-1 classification is always available. Returns 0 when no
 * classification is known (undefined code point / no table for CTYPE2/CTYPE3).
 */
uint16_t nls_char_type(uint16_t c, int which);

/* NLS/collation version of the active table (GetNLSVersionEx backing value); the
 * compiled fallback reports version 1. */
uint32_t nls_get_version(void);

#ifdef KERNEL_TESTS
/* Test-only: publish a parsed descriptor / reset to the compiled fallback so a
 * unit test can exercise the accessor table path without booting a loader. The
 * caller owns `desc` lifetime; pass NULL to revert to the compiled fallback. */
void nls_test_set_active(const nls_published_t *desc);
#endif

#endif /* KERNEL_NT_NLS_H */
