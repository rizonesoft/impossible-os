# EIF -- Executable Impossible Format

> **Version 1.0** -- Impossible OS native binary format.
> Designed for minimal parsing overhead, integer-only import resolution,
> and mandatory code signing support. Target: < 10 us load time for a 64 KB binary.

## Normative Rules

1. **Endianness:** All multi-byte integers are little-endian.
2. **Canonical order:** sections appear in the fixed file order header -> segment table -> import table -> segment data -> metadata -> signature, and every non-zero offset MUST satisfy `segment_offset < import_offset < metadata_offset < signature_offset`. Metadata therefore ALWAYS precedes the signature. All derived ranges (see rule 6) MUST be non-overlapping and lie fully inside the file; the loader MUST reject any file that violates the order, overlaps a range, or points an offset outside the file.
3. **Signing scope:** `signature_offset` MUST be placed AFTER all loader-consumed content (header, segment table, import table, all segment data, AND metadata). The signature covers `[0, signature_offset)`, so ALL loader-consumed content -- including every metadata key/value -- is signed; there is no loader-consumed byte outside the authenticated range. A signed file MUST NOT carry any loader-consumed data at or after `signature_offset`.
4. **API versioning:** Any SSDT service number reassignment or ABI-breaking change MUST increment the OS API version. The loader rejects binaries whose `api_version` exceeds the running OS API version. Binaries are forward-compatible only within the same major version.
5. **Optional import stubs:** Skipped optional imports MUST have their dispatch table entry set to a deterministic stub that returns `STATUS_NOT_IMPLEMENTED`. User code MUST check return values for optional imports.
6. **Range derivation (overflow-safe):** for each table the loader MUST compute `end = offset + (uint64_t)count * sizeof(entry)` in 64-bit and reject the file if the multiply or add would overflow or if `end > file_size`. Never validate with 32-bit arithmetic (`segment_count`/`import_count` are 32-bit and `count * stride` can wrap). `segment_count` and `import_count` are each capped at a fixed loader maximum (mirrored in `include/kernel/eif.h`); a count above the cap is rejected before any table is walked.
7. **Segment ordering:** `eif_segment_t[]` entries MUST be sorted by ascending `vaddr`, with monotonic non-overlapping `[vaddr, vaddr + mem_size)` and `[file_offset, file_offset + file_size)` ranges. This lets the loader validate non-overlap in a single O(n) walk (compare each entry to the previous end); out-of-order or overlapping entries are rejected during that walk. The `file_size <= mem_size` relation holds for UNCOMPRESSED segments only; see rule 9 for `EIF_FLAG_COMPRESSED`.
8. **Metadata bounds:** when `metadata_offset` is non-zero its range is `[metadata_offset, signature_offset)` for a signed file, or `[metadata_offset, file_size)` for an unsigned file. Key/value record parsing MUST stay inside that bounded range and stop at the first `key_len == 0` terminator OR the range end, whichever comes first; a record whose declared length would read past the range end is rejected. The loader never scans past `signature_offset` (or EOF) while reading metadata.
9. **Compressed segments (`EIF_FLAG_COMPRESSED`, a per-file header flag):** when set, every segment's file bytes `[file_offset, file_offset + file_size)` are an LZ4 block and `mem_size` is the EXACT decompressed extent (there is no implicit BSS for a compressed segment; `file_size` MAY exceed `mem_size` for small or incompressible data). A `file_size == 0` segment is a pure-BSS region zero-filled without invoking LZ4. A non-empty compressed payload with `mem_size == 0` is rejected. The loader MUST decompress every compressed segment into a scratch buffer and confirm each produces exactly `mem_size` bytes BEFORE writing any user memory, so a corrupt or size-lying stream fails the load atomically (no segment is partially materialized). Decompression is bounds-checked (`LZ4_decompress_safe`): it never reads or writes outside the source and destination buffers even on hostile input.

## Design Goals

| Goal | Mechanism |
|------|-----------|
| Fast loading | 64-byte fixed header, no string tables at load time |
| Minimal parsing | Integer syscall IDs replace string symbol resolution |
| Security first | Mandatory signature slot; SIGNED flag gates execution |
| Small overhead | No DOS stub, no PE optional header bloat, no ELF sections |
| Native API direct | Import table maps directly to SSDT service numbers |

## Comparison with ELF and PE32+

| Feature | ELF | PE32+ | EIF |
|---------|-----|-------|-----|
| Header size | 64 bytes | ~256 bytes (DOS+PE+Optional) | 64 bytes |
| Magic | `\x7FELF` | `MZ` | `EIF!` |
| Import model | PLT/GOT (string symbols) | IAT (DLL + function names) | Syscall ID table (integers) |
| Load time | ~50 us (string resolution) | ~100 us (DLL lookup + IAT) | < 10 us (integer dispatch) |
| Signing | Optional (external) | Authenticode (complex) | Built-in (simple) |

---

## File Layout

```
+0x0000  eif_header_t       (64 bytes, fixed)
+0x0040  eif_segment_t[]    (32 bytes each, segment_count entries)
+????    eif_import_t[]     (8 bytes each, import_count entries)
+????    segment data        (raw code + data bytes)
+????    metadata            (optional key-value pairs -- BEFORE signature)
+????    signature data      (if SIGNED flag set -- LAST in file)
```

> **Note:** Metadata is placed BEFORE the signature so it is covered by the signature hash. This prevents post-signing modification of `"min_os"`, `"name"`, and other security-relevant metadata.

---

## eif_header_t (64 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| `0x00` | 4 | `magic` | file bytes `45 49 46 21` (`"EIF!"`) read as little-endian `uint32_t` = `0x21464945` (`EIF_MAGIC` in `include/kernel/eif.h`) |
| `0x04` | 2 | `version` | Format version (1 = this spec) |
| `0x06` | 2 | `arch` | Target architecture: `1` = x86_64, `2` = AArch64 |
| `0x08` | 4 | `flags` | Bitfield (see below) |
| `0x0C` | 4 | `api_version` | Minimum Impossible OS API version required |
| `0x10` | 8 | `entry_point` | Virtual address of entry point (relative to load_base) |
| `0x18` | 8 | `load_base` | Preferred load base address (0 = position-independent) |
| `0x20` | 4 | `segment_count` | Number of segments in segment table |
| `0x24` | 4 | `import_count` | Number of entries in import table |
| `0x28` | 4 | `segment_offset` | File offset of segment table |
| `0x2C` | 4 | `import_offset` | File offset of import table |
| `0x30` | 8 | `signature_offset` | File offset of signature data (0 if unsigned) |
| `0x38` | 8 | `metadata_offset` | File offset of metadata section (0 if none) |

**Total: 64 bytes.**

### Flags (offset 0x08)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | `EIF_FLAG_GUI` | Graphical application (creates windows) |
| 1 | `EIF_FLAG_CONSOLE` | Console application (uses terminal I/O) |
| 2 | `EIF_FLAG_DRIVER` | Kernel-mode driver (future) |
| 3 | `EIF_FLAG_SIGNED` | Binary has a digital signature at signature_offset |
| 4 | `EIF_FLAG_COMPRESSED` | Segment data is LZ4-compressed (see normative rule 9) |
| 5 | `EIF_FLAG_DEBUG` | Debug symbols present in metadata |
| 6-31 | Reserved | Must be zero |

---

## eif_segment_t (32 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| `0x00` | 8 | `vaddr` | Virtual address (relative to load_base) |
| `0x08` | 4 | `file_offset` | Offset of segment data in file |
| `0x0C` | 4 | `file_size` | Size of segment data in file (bytes) |
| `0x10` | 4 | `mem_size` | Size in memory (uncompressed: >= file_size, difference is BSS; compressed: exact decompressed extent, see rule 9) |
| `0x14` | 4 | `flags` | Permission flags (see below) |
| `0x18` | 8 | `reserved` | Must be zero |

**Total: 32 bytes.**

### Segment Flags (offset 0x14)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | `EIF_SEG_READ` | Readable |
| 1 | `EIF_SEG_WRITE` | Writable |
| 2 | `EIF_SEG_EXEC` | Executable |
| 3-31 | Reserved | Must be zero |

Typical segments:
- `.text` -- `EIF_SEG_READ | EIF_SEG_EXEC` (R-X)
- `.data` -- `EIF_SEG_READ | EIF_SEG_WRITE` (RW-)
- `.rodata` -- `EIF_SEG_READ` (R--)

---

## eif_import_t (8 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| `0x00` | 4 | `syscall_id` | SSDT service number (main table: 0x0000-0x03FF) |
| `0x04` | 4 | `flags` | Import flags (see below) |

**Total: 8 bytes.**

### Import Flags (offset 0x04)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | `EIF_IMP_OPTIONAL` | 0 = required (fail if missing), 1 = optional (skip if missing) |
| 1-31 | Reserved | Must be zero |

### Import Resolution

Import resolution is **integer-only** -- no string lookup at load time.

1. Kernel reads import table: N entries of `{syscall_id, flags}`.
2. For each entry, checks if `ssdt_dispatch(syscall_id, ...)` is registered (not the `STATUS_NOT_IMPLEMENTED` stub).
3. Required imports with unregistered SSDT entries: loader fails with `STATUS_NOT_IMPLEMENTED`.
4. Optional imports with unregistered entries: skipped silently; user-mode code checks return value.
5. A per-process dispatch table at a well-known user-space address maps each imported syscall_id to a kernel entry point, enabling direct `syscall` with the service number in RAX.

This eliminates all string comparison, hash lookup, and DLL search path logic from the load path.

---

## Signature Format

If `EIF_FLAG_SIGNED` is set, `signature_offset` points to:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| `0x00` | 4 | `algo` | Signature algorithm: `1` = Ed25519, `2` = RSA-2048-SHA256 |
| `0x04` | 4 | `sig_size` | Signature data size in bytes |
| `0x08` | var | `signature` | Raw signature bytes |

The signature covers bytes `[0, signature_offset)` of the file -- everything before the signature block.

### Verification Flow

1. If `EIF_FLAG_SIGNED` is clear: skip verification (unsigned binary).
2. If `EIF_FLAG_SIGNED` is set but `signature_offset == 0`: reject as corrupt.
3. Validate bounds: `signature_offset + 8 + sig_size <= file_size`. Reject if out of bounds.
4. Validate `sig_size` against algorithm constraints: Ed25519 = exactly 64 bytes; RSA-2048-SHA256 = exactly 256 bytes. Reject mismatches.
5. For Ed25519: verify signature over raw bytes `[0, signature_offset)` using pure Ed25519 (no prehash). For RSA-2048-SHA256: hash bytes `[0, signature_offset)` with SHA-256, then RSA-verify.
6. Verify against the OS trusted key (compiled into kernel or loaded from UEFI Secure Boot db).
7. On failure: reject binary with `STATUS_ACCESS_DENIED`.

---

## Metadata Section

If `metadata_offset != 0`, points to a sequence of key-value pairs:

```
[4 bytes key_len] [key_len bytes key] [4 bytes val_len] [val_len bytes value]
```

Terminated by `key_len == 0`. Common keys:

| Key | Description |
|-----|-------------|
| `"name"` | Application display name |
| `"version"` | Application version string |
| `"author"` | Developer name |
| `"icon"` | Path to application icon resource |
| `"min_os"` | Minimum OS version (e.g., "26.4") |

---

## Kernel Header Definition

The kernel header struct (for `include/kernel/eif.h`):

```c
#define EIF_MAGIC  0x21464945  /* file bytes 45 49 46 21 ("EIF!") as LE u32 */

#define EIF_ARCH_X86_64   1
#define EIF_ARCH_AARCH64  2

#define EIF_FLAG_GUI        (1u << 0)
#define EIF_FLAG_CONSOLE    (1u << 1)
#define EIF_FLAG_DRIVER     (1u << 2)
#define EIF_FLAG_SIGNED     (1u << 3)
#define EIF_FLAG_COMPRESSED (1u << 4)
#define EIF_FLAG_DEBUG      (1u << 5)

#define EIF_SEG_READ   (1u << 0)
#define EIF_SEG_WRITE  (1u << 1)
#define EIF_SEG_EXEC   (1u << 2)

#define EIF_IMP_OPTIONAL (1u << 0)

#define EIF_SIG_ED25519       1
#define EIF_SIG_RSA2048_SHA256 2

typedef struct eif_header {
    uint32_t magic;           /* 0x00: EIF_MAGIC */
    uint16_t version;         /* 0x04: format version (1) */
    uint16_t arch;            /* 0x06: EIF_ARCH_* */
    uint32_t flags;           /* 0x08: EIF_FLAG_* */
    uint32_t api_version;     /* 0x0C: min OS API version */
    uint64_t entry_point;     /* 0x10: entry VA (relative to load_base) */
    uint64_t load_base;       /* 0x18: preferred base (0 = PIC) */
    uint32_t segment_count;   /* 0x20 */
    uint32_t import_count;    /* 0x24 */
    uint32_t segment_offset;  /* 0x28 */
    uint32_t import_offset;   /* 0x2C */
    uint64_t signature_offset;/* 0x30: 0 if unsigned */
    uint64_t metadata_offset; /* 0x38: 0 if no metadata */
} __attribute__((packed)) eif_header_t;

_Static_assert(sizeof(eif_header_t) == 64,
    "eif_header_t must be exactly 64 bytes");

typedef struct eif_segment {
    uint64_t vaddr;           /* 0x00: virtual address */
    uint32_t file_offset;     /* 0x08 */
    uint32_t file_size;       /* 0x0C */
    uint32_t mem_size;        /* 0x10 */
    uint32_t flags;           /* 0x14: EIF_SEG_* */
    uint64_t reserved;        /* 0x18: must be 0 */
} __attribute__((packed)) eif_segment_t;

_Static_assert(sizeof(eif_segment_t) == 32,
    "eif_segment_t must be exactly 32 bytes");

typedef struct eif_import {
    uint32_t syscall_id;      /* 0x00: SSDT service number */
    uint32_t flags;           /* 0x04: EIF_IMP_* */
} __attribute__((packed)) eif_import_t;

_Static_assert(sizeof(eif_import_t) == 8,
    "eif_import_t must be exactly 8 bytes");
```

---

## elf2eif Conversion

The `tools/elf2eif` converter transforms standard ELF64 output into EIF:

1. Parse ELF64 `PT_LOAD` segments -> EIF segments
2. Parse symbol table for `__imp_NtXxx` symbols -> extract SSDT numbers -> EIF import table
3. Copy segment data verbatim
4. Generate 64-byte EIF header
5. Optionally sign with Ed25519 key

No custom compiler or linker modifications required -- standard `clang-19 + ld.lld` output is the input.

---

## Version 1.1 Planned Extensions

These features are reserved for version 1.1 and will use the `reserved` fields or new directory entries:

| Feature | Motivation | Notes |
|---------|-----------|-------|
| Relocation table | ASLR for non-PIC binaries | Type + target RVA + addend entries |
| TLS directory | `thread_local` support | Template VA/size, zero-fill, alignment, callbacks |
| Unwind/exception table | Stack unwinding, C++ exceptions, crash reporting | `.pdata`-style entries or DWARF-compatible |
| Stack configuration | Per-image stack reserve/commit/guard | Reserve size, commit size, guard page policy |
| Directory table | Typed `{type, offset, size}` entries | Replaces ad-hoc offset fields; extensible |
| Debug info directory | Structured debug data pointer | Replaces generic metadata key-value for debug |

These extensions will increment the format version to 2 and use a backward-compatible directory table appended after the v1.0 fields.

---

## Portability Notes

The C struct definitions use `__attribute__((packed))` (GCC/Clang). For MSVC compatibility:

```c
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

/* ... struct definitions ... */

#ifdef _MSC_VER
#pragma pack(pop)
#endif
```

The kernel codebase targets `clang-19` only, so `__attribute__((packed))` is canonical. The MSVC pragma is provided for SDK consumers building with Visual Studio.
