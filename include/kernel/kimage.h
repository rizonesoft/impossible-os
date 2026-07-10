/* kimage.h -- Kernel Image & Module Registry: KIMAGE_ENTRY data model.
 *
 * The canonical loaded-image record for the whole OS: kernel image, boot
 * modules, drivers, kernel modules, process main images, DLLs, and EIF
 * modules. One KIMAGE_ENTRY per loaded executable image. Crash dumps, KD,
 * exception dispatch (unwind), code integrity, ETW, and process
 * introspection all read the same record.
 *
 * This header defines ONLY the data model. The global / per-process
 * registries, address index, symbol and unwind providers, loader
 * integration, and the rest are separate build-out stages; the optional
 * metadata handles below are their hook points and stay 0 until those stages
 * populate them.
 *
 * KIMAGE_ENTRY is the planned superset of the existing loaded_module_t
 * (exec.h). Four layout invariants that later sections depend on:
 *   - the lifetime gate is an embedded EX_RUNDOWN_REF (one 64-bit CAS word:
 *     rundown-bit + refcount), NOT a separate flag + int32 -- a
 *     check-flag-then-increment split races the section-9 unload drain;
 *   - `format` is a canonical kimage_format_t (UNKNOWN=0), NOT the EXEC_FMT_*
 *     ids: the kernel is deliberately registered as EXEC_FMT_PE for WinDbg
 *     presentation (boot_interrupts.c), so reusing those would misclassify the
 *     ELF kernel's actual format and pick the wrong symbol/unwind provider;
 *   - durable build/debug identity bytes (ELF build-id / PE CodeView GUID+age
 *     / EIF build_id) are owned inline so a tombstone or serialized dump can
 *     resolve symbols after the image and its process-local handles are gone;
 *   - full_path is VFS_MAX_PATH, not a smaller cap -- a truncated path lets
 *     two distinct images collide in notifications, provenance, and tombstones.
 */
#ifndef KERNEL_KIMAGE_H
#define KERNEL_KIMAGE_H

#include "kernel/types.h"
#include "kernel/ex.h"       /* EX_RUNDOWN_REF -- lifetime gate */
#include "kernel/fs/vfs.h"   /* VFS_MAX_PATH -- canonical path capacity */

/* Fixed-capacity string / digest sizes (including NUL for the char arrays). */
#define KIMAGE_NAME_MAX      64            /* base name, e.g. "kernel.exe" */
#define KIMAGE_PATH_MAX      VFS_MAX_PATH  /* canonical path, full VFS capacity */
#define KIMAGE_HASH_MAX      32            /* SHA-256 content digest (provenance) */
#define KIMAGE_SIGNER_MAX    64            /* signer subject ("" if unsigned) */
#define KIMAGE_IDENTITY_MAX  32            /* durable build/debug identity bytes */

/* full_path must be able to hold any path the VFS accepts, or two distinct
 * images can truncate to the same string. */
_Static_assert(KIMAGE_PATH_MAX >= VFS_MAX_PATH,
    "KIMAGE_PATH_MAX must cover the full VFS path length");

/* ---- Image type (role) -------------------------------------------------
 * The role the image plays. A DISTINCT namespace from `format`: an ELF file
 * can be a driver, a kmod, or a process main image; a PE file can be the
 * kernel, a DLL, or a driver. Never conflate type with format. */
typedef enum kimage_type {
    KIMAGE_TYPE_UNKNOWN     = 0,
    KIMAGE_TYPE_KERNEL      = 1,  /* the kernel image itself */
    KIMAGE_TYPE_HAL         = 2,  /* HAL / platform module */
    KIMAGE_TYPE_BOOT_MODULE = 3,  /* bootloader-supplied module */
    KIMAGE_TYPE_DRIVER      = 4,  /* PE32+ / EIF driver */
    KIMAGE_TYPE_KMOD        = 5,  /* ELF relocatable kernel module (.kmod) */
    KIMAGE_TYPE_PROCESS     = 6,  /* process main image */
    KIMAGE_TYPE_DLL         = 7,  /* DLL / shared library */
    KIMAGE_TYPE_EIF_MODULE  = 8,  /* EIF native module */
    KIMAGE_TYPE_SYNTHETIC   = 9,  /* synthetic / stub (no backing file) */
    KIMAGE_TYPE_MAX
} kimage_type_t;

/* ---- Binary format (canonical actual-format) ---------------------------
 * The image's REAL on-disk format. Deliberately NOT the EXEC_FMT_* ids: those
 * are conflated with WinDbg presentation (the ELF kernel registers as
 * EXEC_FMT_PE). The crash-dump / KD serializer maps this to whatever
 * presentation a debugger expects; the registry stores the truth. */
typedef enum kimage_format {
    KIMAGE_FMT_UNKNOWN = 0,  /* zero-initialized entry is explicitly unknown */
    KIMAGE_FMT_ELF     = 1,
    KIMAGE_FMT_PE      = 2,
    KIMAGE_FMT_EIF     = 3,
    KIMAGE_FMT_MAX
} kimage_format_t;

/* ---- Durable build/debug identity kind --------------------------------
 * A content hash is not a debug identity. This is the stable id a debugger /
 * symbol server keys on, copied inline so it survives into a tombstone or a
 * serialized dump after the live image and its metadata handles are gone. */
typedef enum kimage_identity_kind {
    KIMAGE_ID_NONE        = 0,  /* no durable identity captured */
    KIMAGE_ID_ELF_BUILDID = 1,  /* ELF .note.gnu.build-id */
    KIMAGE_ID_PE_CODEVIEW = 2,  /* PE CodeView GUID(16) + age(4) */
    KIMAGE_ID_EIF_BUILDID = 3,  /* EIF producer build_id */
    KIMAGE_ID_MAX
} kimage_identity_kind_t;

/* ---- Code-integrity decision ------------------------------------------ */
typedef enum kimage_ci_decision {
    KIMAGE_CI_UNKNOWN  = 0,  /* not yet evaluated */
    KIMAGE_CI_UNSIGNED = 1,  /* no signature present */
    KIMAGE_CI_VALID    = 2,  /* signature verified, trusted */
    KIMAGE_CI_REVOKED  = 3,  /* signer / hash on a revocation list */
    KIMAGE_CI_REJECTED = 4,  /* signature present but invalid */
    KIMAGE_CI_MAX
} kimage_ci_decision_t;

/* ---- Flags (non-overlapping single-bit bitmask) ------------------------
 * KIMAGE_FLAG_GOING is a SERIALIZED MIRROR of the `life` rundown state for
 * dumps/notifications; the authoritative unload gate is `life`, never a
 * flag test. */
#define KIMAGE_FLAG_GLOBAL     (1u << 0)  /* in the global registry (else per-process) */
#define KIMAGE_FLAG_STRIPPED   (1u << 1)  /* no symbols available */
#define KIMAGE_FLAG_SIGNED     (1u << 2)  /* a CI signature is present */
#define KIMAGE_FLAG_HAS_UNWIND (1u << 3)  /* unwind metadata registered */
#define KIMAGE_FLAG_ALIAS      (1u << 4)  /* alias mapping (overlap is intentional) */
#define KIMAGE_FLAG_GOING      (1u << 5)  /* unload in progress (mirrors life rundown) */
#define KIMAGE_FLAG_TOMBSTONE  (1u << 6)  /* recently-unloaded ring entry, not live */

#define KIMAGE_FLAG_ALL \
    (KIMAGE_FLAG_GLOBAL | KIMAGE_FLAG_STRIPPED | KIMAGE_FLAG_SIGNED | \
     KIMAGE_FLAG_HAS_UNWIND | KIMAGE_FLAG_ALIAS | KIMAGE_FLAG_GOING | \
     KIMAGE_FLAG_TOMBSTONE)

/* ---- KIMAGE_ENTRY ------------------------------------------------------
 * One record per loaded image. Field offsets are pinned by the static
 * asserts below because the crash-dump / KD module-stream serializers
 * (section 8) read this layout; update the serializers if it changes. */
typedef struct kimage_entry {
    uint64_t base;          /* load VA */
    uint64_t size;          /* total mapped size in bytes */
    uint64_t entry_point;   /* entry function VA (0 if none) */
    uint64_t timestamp;     /* load time (monotonic ticks) */

    EX_RUNDOWN_REF life;    /* lifetime gate: rundown-bit + refcount in one CAS
                             * word. Acquire before touching a looked-up entry;
                             * section 9 unload runs rundown to drain. */

    uint32_t type;          /* kimage_type_t */
    uint32_t format;        /* kimage_format_t (canonical actual format) */
    uint32_t flags;         /* KIMAGE_FLAG_* bitmask */
    uint32_t ci_decision;   /* kimage_ci_decision_t */
    uint32_t owner_pid;     /* owning process (0 = global / kernel) */
    uint32_t section_count; /* number of image sections */
    uint32_t load_order;    /* monotonic load index */
    uint32_t checksum;      /* image checksum (PE CheckSum / CRC) */

    /* Optional metadata handles, 0 until the owning section populates them.
     * Process-local: NOT durable across unload (tombstones use `identity`). */
    uint64_t symbols;       /* symbol provider handle (section 4) */
    uint64_t unwind_ranges; /* unwind metadata handle (section 5) */
    uint64_t exports;       /* export table */
    uint64_t imports;       /* import table */
    uint64_t relocations;   /* relocation info */
    uint64_t debug_info;    /* debug info */

    uint8_t  hash[KIMAGE_HASH_MAX];          /* SHA-256 content digest */
    uint8_t  identity[KIMAGE_IDENTITY_MAX];  /* durable build/debug identity */
    uint8_t  identity_kind;                  /* kimage_identity_kind_t */
    uint8_t  identity_len;                   /* valid bytes in identity[] */
    uint16_t full_path_len;                  /* valid chars in full_path */
    uint16_t name_len;                       /* valid chars in name */
    uint16_t _pad0;                          /* pad -> 8-byte align signer */

    char     signer[KIMAGE_SIGNER_MAX];      /* signer subject ("" if unsigned) */
    char     full_path[KIMAGE_PATH_MAX];     /* canonical Windows-style path */
    char     name[KIMAGE_NAME_MAX];          /* base name */
} kimage_entry_t;

/* ---- Record invariants (enforced by the section-2 registration validator) --
 * A producer must not publish a kimage_entry_t that violates these, and a
 * consumer/serializer may assume them:
 *   - Copy safety: `life` (a live atomic) and the six metadata handles are
 *     NOT copy-safe. A tombstone / dump snapshot (section 8) must field-copy
 *     the durable fields and RE-INITIALIZE a fresh gate -- never raw-memcpy a
 *     live entry (the copy would read `life` non-atomically and inherit stale
 *     rundown/refcount state).
 *   - String bounds: full_path, name, and signer are NUL-terminated bounded
 *     strings. full_path_len / name_len exclude the NUL and satisfy
 *     len < capacity with buffer[len] == '\0'. signer[KIMAGE_SIGNER_MAX-1] is
 *     always '\0'. identity_len <= KIMAGE_IDENTITY_MAX. A serializer trusting
 *     a length must still not read past the buffer. */

/* Layer 1 (5-layer defense): pin size + load-bearing offsets. The crash-dump
 * and KD module-stream serializers (section 8) depend on this layout. */
_Static_assert(sizeof(kimage_entry_t) == 832,
    "kimage_entry_t size -- update KD/crash-dump serializers if changed");
_Static_assert(__builtin_offsetof(kimage_entry_t, base) == 0,
    "base must be first (KD reads it at offset 0)");
_Static_assert(__builtin_offsetof(kimage_entry_t, life) == 32,
    "lifetime gate is 8-byte aligned for atomic CAS");
_Static_assert(sizeof(((kimage_entry_t *)0)->life) == 8,
    "EX_RUNDOWN_REF must stay a single 64-bit word");
_Static_assert(__builtin_offsetof(kimage_entry_t, symbols) == 72,
    "metadata pointer block is 8-byte aligned");
_Static_assert(__builtin_offsetof(kimage_entry_t, hash) == 120,
    "hash precedes durable identity");
_Static_assert(__builtin_offsetof(kimage_entry_t, identity) == 152,
    "durable identity bytes -- tombstones copy these");
_Static_assert(__builtin_offsetof(kimage_entry_t, full_path) == 256,
    "full_path offset -- serializers read the path here");

/* type (a role enum) and format (kimage_format_t) are INDEPENDENT fields --
 * both zero-base at their own UNKNOWN, and an image's role does not constrain
 * its binary format. The flags bitmask is single-bit non-overlapping. The
 * field round-trip and flags tests in test_kimage.c exercise both. */

#endif /* KERNEL_KIMAGE_H */
