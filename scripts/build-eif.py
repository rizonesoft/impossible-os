#!/usr/bin/env python3
# =============================================================================
# build-eif.py -- Assemble a minimal EIF binary from a raw code blob
#
# Usage:
#   python3 scripts/build-eif.py <code.bin> <output.exe>
#
# Produces an EIF-format executable with:
#   - 64-byte header (magic "EIF!" + segment/import table offsets)
#   - One R+X segment at USER_ELF_BASE (0x800000) containing the
#     entire input file's contents verbatim
#   - Entry point at segment start (offset 0 within the segment)
#   - Zero imports (no SSDT dispatch table needed -- the test binary
#     uses INT 0x80 for its single SYS_EXIT call, which does not go
#     through the EIF import dispatch table)
#
# Layout (file offsets):
#   0x000 - 0x040   EIF header (sizeof(eif_header_t) = 64)
#   0x040 - 0x060   Segment table: 1 x eif_segment_t (32 bytes)
#   0x060 - end     Code (input file contents)
#
# Constants mirror include/kernel/eif.h + include/kernel/mm/user_range.h.
# Keep them in sync manually -- the kernel Static_asserts the struct
# sizes, so any drift surfaces as a build-time error there, and the
# EIF binary would then simply fail to load on the next boot. Adding
# a cross-language assertion would require generating these from the
# C header, which is out of scope for this section's test runner.
#
# The input file must be a RAW BINARY (no ELF/PE wrapper). Produce
# one via: `nasm -f bin entry.asm -o entry.bin`.
# =============================================================================

import struct
import sys

EIF_MAGIC          = 0x21464945  # "EIF!" file-order LE u32 (bytes 0x45 0x49 0x46 0x21)
EIF_VERSION        = 1
EIF_ARCH_X86_64    = 1

EIF_FLAG_CONSOLE   = 1 << 1

EIF_SEG_READ       = 1 << 0
EIF_SEG_WRITE      = 1 << 1
EIF_SEG_EXEC       = 1 << 2

USER_ELF_BASE      = 0x800000

HEADER_SIZE        = 64
SEGMENT_SIZE       = 32


def build_eif(code_path: str, out_path: str) -> None:
    with open(code_path, 'rb') as f:
        code = f.read()
    if not code:
        raise SystemExit(f"build-eif.py: {code_path} is empty")

    segment_offset = HEADER_SIZE
    code_file_offset = HEADER_SIZE + SEGMENT_SIZE

    # eif_header_t (packed, 64 bytes): see include/kernel/eif.h
    #   0x00 u32 magic, 0x04 u16 version, 0x06 u16 arch,
    #   0x08 u32 flags, 0x0C u32 api_version,
    #   0x10 u64 entry_point, 0x18 u64 load_base,
    #   0x20 u32 segment_count, 0x24 u32 import_count,
    #   0x28 u32 segment_offset, 0x2C u32 import_offset,
    #   0x30 u64 signature_offset, 0x38 u64 metadata_offset
    header = struct.pack(
        '<I H H I I Q Q I I I I Q Q',
        EIF_MAGIC,          # magic
        EIF_VERSION,        # version
        EIF_ARCH_X86_64,    # arch
        EIF_FLAG_CONSOLE,   # flags
        0,                  # api_version (no minimum)
        0,                  # entry_point (relative to load_base; _start at 0)
        USER_ELF_BASE,      # load_base
        1,                  # segment_count
        0,                  # import_count
        segment_offset,     # segment_offset
        0,                  # import_offset (no imports)
        0,                  # signature_offset (unsigned)
        0,                  # metadata_offset (no metadata)
    )
    assert len(header) == HEADER_SIZE

    # eif_segment_t (packed, 32 bytes): see include/kernel/eif.h
    #   0x00 u64 vaddr, 0x08 u32 file_offset, 0x0C u32 file_size,
    #   0x10 u32 mem_size, 0x14 u32 flags, 0x18 u64 reserved
    # vaddr in EIF is RELATIVE to load_base (eif.c adds load_base to
    # seg->vaddr when mapping). A single segment at offset 0 covers
    # the whole code blob; mem_size == file_size because the raw
    # code blob has no BSS component.
    segment = struct.pack(
        '<Q I I I I Q',
        0,                                                  # vaddr (load_base-relative)
        code_file_offset,                                   # file_offset
        len(code),                                          # file_size
        len(code),                                          # mem_size
        EIF_SEG_READ | EIF_SEG_EXEC,                        # flags (R-X)
        0,                                                  # reserved
    )
    assert len(segment) == SEGMENT_SIZE

    with open(out_path, 'wb') as f:
        f.write(header)
        f.write(segment)
        f.write(code)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 1
    build_eif(sys.argv[1], sys.argv[2])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
