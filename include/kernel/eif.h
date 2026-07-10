/* ============================================================================
 * eif.h -- Executable Impossible Format (EIF) definitions
 *
 * Native Impossible OS binary format. 64-byte header, integer-only syscall
 * imports, optional code signing. Spec: specs/eif-format.md
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Magic and architecture --------------------------------------------- */

/* EIF magic bytes "EIF!" in file order at offset 0:
 *   file bytes: 'E' (0x45), 'I' (0x49), 'F' (0x46), '!' (0x21)
 *   read as little-endian uint32:
 *     p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)
 *     = 0x45 | (0x49<<8) | (0x46<<16) | (0x21<<24)
 *     = 0x21464945
 * Previous value 0x45494621 was the "EIF!" string packed as a
 * uint32 MSB-first (big-endian) which produced file bytes
 * 0x21 0x46 0x49 0x45 (= "!FIE") -- contradicting exec.c's magic
 * byte-array {'E','I','F','!'} which expects the ASCII order in
 * the file. Kernel-side tests never fed a real EIF binary through
 * the exec dispatcher + eif_load, so the mismatch was latent until
 * the user-mode binary format loader probe shipped a real EIF
 * test binary. */
#define EIF_MAGIC         0x21464945  /* "EIF!" as file-order LE u32 */
#define EIF_VERSION       1           /* current format version */
#define EIF_CURRENT_API_VERSION 1     /* OS API version the loader provides; reject binaries requiring newer */

#define EIF_ARCH_X86_64   1
#define EIF_ARCH_AARCH64  2

/* Count caps (spec rule 6): segment_count/import_count are 32-bit and bounded
 * only by table-fits-file, so an uncapped count lets a crafted EIF drive the
 * loader's per-entry walk for billions of iterations -- an exec-path DoS. Both
 * are rejected before any table is walked. EIF_MAX_SEGMENTS 64 matches
 * ELF_MAX_PHNUM and the module-registry cap; EIF_MAX_IMPORTS mirrors the
 * dispatch table size (asserted equal to EIF_DISPATCH_TABLE_MAX below). Both
 * are far above any real binary. */
#define EIF_MAX_SEGMENTS  64
#define EIF_MAX_IMPORTS   1024

/* Metadata record cap (spec rule 8): the metadata range bound prevents overreads
 * but not an attacker-controlled record count -- a crafted file can pack the range
 * with minimal >=9-byte records and drive the exec-path key/value walk (twice: once
 * in eif_load, once in the task.c module-name re-parse). Mirrors the segment/import
 * count caps: reject a file with more than this many records before any mutation. */
#define EIF_MAX_METADATA_RECORDS 64

/* ---- Header flags ------------------------------------------------------- */

#define EIF_FLAG_GUI        (1u << 0)
#define EIF_FLAG_CONSOLE    (1u << 1)
#define EIF_FLAG_DRIVER     (1u << 2)
#define EIF_FLAG_SIGNED     (1u << 3)
#define EIF_FLAG_COMPRESSED (1u << 4)
#define EIF_FLAG_DEBUG      (1u << 5)

/* ---- Segment flags ------------------------------------------------------ */

#define EIF_SEG_READ   (1u << 0)
#define EIF_SEG_WRITE  (1u << 1)
#define EIF_SEG_EXEC   (1u << 2)

/* ---- Import flags ------------------------------------------------------- */

#define EIF_IMP_OPTIONAL (1u << 0)

/* ---- Structures --------------------------------------------------------- */

typedef struct eif_header {
    uint32_t magic;            /* 0x00: EIF_MAGIC */
    uint16_t version;          /* 0x04: format version (1) */
    uint16_t arch;             /* 0x06: EIF_ARCH_* */
    uint32_t flags;            /* 0x08: EIF_FLAG_* */
    uint32_t api_version;      /* 0x0C: min OS API version */
    uint64_t entry_point;      /* 0x10: entry VA (relative to load_base) */
    uint64_t load_base;        /* 0x18: preferred base (0 = PIC) */
    uint32_t segment_count;    /* 0x20 */
    uint32_t import_count;     /* 0x24 */
    uint32_t segment_offset;   /* 0x28 */
    uint32_t import_offset;    /* 0x2C */
    uint64_t signature_offset; /* 0x30: 0 if unsigned */
    uint64_t metadata_offset;  /* 0x38: 0 if no metadata */
} __attribute__((packed)) eif_header_t;

_Static_assert(sizeof(eif_header_t) == 64,
    "eif_header_t must be exactly 64 bytes");

typedef struct eif_segment {
    uint64_t vaddr;            /* 0x00: virtual address */
    uint32_t file_offset;      /* 0x08 */
    uint32_t file_size;        /* 0x0C */
    uint32_t mem_size;         /* 0x10 */
    uint32_t flags;            /* 0x14: EIF_SEG_* */
    uint64_t reserved;         /* 0x18: must be 0 */
} __attribute__((packed)) eif_segment_t;

_Static_assert(sizeof(eif_segment_t) == 32,
    "eif_segment_t must be exactly 32 bytes");

typedef struct eif_import {
    uint32_t syscall_id;       /* 0x00: SSDT service number */
    uint32_t flags;            /* 0x04: EIF_IMP_* */
} __attribute__((packed)) eif_import_t;

_Static_assert(sizeof(eif_import_t) == 8,
    "eif_import_t must be exactly 8 bytes");

/* ---- Parsed metadata ---------------------------------------------------- */

/* Decoded form of the optional key-value metadata section. Populated by
 * eif_parse_metadata() from the wire records (spec: specs/eif-format.md
 * "Metadata Section"). String fields are always NUL-terminated; values longer
 * than the field are truncated (the fields are display/identity hints, not a
 * security surface). build_id is a raw byte blob (NOT a string): build_id_len
 * gives its length, 0 when absent. This is an in-memory decode target only --
 * NOT an on-disk struct, so no size/offset _Static_assert applies.
 *
 * All fields here are ADVISORY (display / identity / diagnostics). None gate the
 * load: the sole enforced version check is the numeric header api_version in
 * eif_validate(). In particular min_os is informational, matching how Windows
 * (subsystem/OS-version fields) and Linux treat file-level version strings --
 * not a hard load gate. A real min_os policy, if ever wanted, would be a
 * separate enforcement step, not a change to this decoder. */
typedef struct eif_metadata {
    char     name[64];        /* "name" key: application display name */
    char     version[32];     /* "version" key */
    char     author[64];      /* "author" key */
    char     min_os[16];      /* "min_os" key, e.g. "26.4" (advisory, not gated) */
    uint8_t  build_id[32];    /* "build_id" key: raw bytes (crash/debug id) */
    uint32_t build_id_len;    /* bytes used in build_id (0 = no build_id) */
} eif_metadata_t;

/* ---- Per-process dispatch table ----------------------------------------- */

/* Fixed user-space address for the EIF import dispatch table.
 * Sits between ELF code (~0x815000) and user stack (0x8FC000).
 * Each entry is 8 bytes: { syscall_id (4), status (4) }.
 * Status: 1 = available (registered in SSDT), 0 = unavailable.
 * User code reads this table to check import availability before SYSCALL.
 * Maximum 1024 entries = 8 KB. */
#define EIF_DISPATCH_TABLE_ADDR  0x8F0000UL
#define EIF_DISPATCH_TABLE_MAX   1024

typedef struct eif_dispatch_entry {
    uint32_t syscall_id;       /* SSDT service number */
    uint32_t available;        /* 1 = registered, 0 = not available */
} eif_dispatch_entry_t;

_Static_assert(sizeof(eif_dispatch_entry_t) == 8,
    "eif_dispatch_entry_t must be 8 bytes");

/* The import-count cap and the dispatch table capacity are the same limit:
 * every accepted import writes one dispatch entry, so a count the validator
 * admits must fit the table the loader writes. Pin them together. */
_Static_assert(EIF_MAX_IMPORTS == EIF_DISPATCH_TABLE_MAX,
    "EIF_MAX_IMPORTS must equal EIF_DISPATCH_TABLE_MAX");

/* ---- Loader API --------------------------------------------------------- */

/* Load an EIF binary from a raw buffer.
 * Returns entry point address on success, 0 on failure. */
uint64_t eif_load(const uint8_t *data, uint64_t size);

/* Parse the optional key-value metadata section into *out (fully zeroed first).
 * Pure: reads only the caller buffer, allocates nothing, emits no log (safe to
 * call inside eif_load's timed region and again from the exec module-registration
 * path). Returns 1 on success (including metadata_offset == 0, which yields an
 * all-zero *out), 0 if any record is malformed (declared length runs past the
 * metadata range, trailing bytes too short for a length prefix, or the record
 * count exceeds EIF_MAX_METADATA_RECORDS). A 0 return means the caller must
 * reject the binary before mutation. `hdr` must already have passed eif_validate. */
int eif_parse_metadata(const uint8_t *data, uint64_t size,
                       const eif_header_t *hdr, eif_metadata_t *out);
