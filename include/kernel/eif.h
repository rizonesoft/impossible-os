/* ============================================================================
 * eif.h -- Executable Impossible Format (EIF) definitions
 *
 * Native Impossible OS binary format. 64-byte header, integer-only syscall
 * imports, optional code signing. Spec: docs/specs/eif-format.md
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Magic and architecture --------------------------------------------- */

#define EIF_MAGIC         0x45494621  /* "EIF!" little-endian */
#define EIF_VERSION       1           /* current format version */

#define EIF_ARCH_X86_64   1
#define EIF_ARCH_AARCH64  2

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

/* ---- Loader API --------------------------------------------------------- */

/* Load an EIF binary from a raw buffer.
 * Returns entry point address on success, 0 on failure. */
uint64_t eif_load(const uint8_t *data, uint64_t size);
