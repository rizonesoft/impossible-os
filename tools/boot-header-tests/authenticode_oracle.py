#!/usr/bin/env python3
"""Independent implementation of the Authenticode PE image hash.

This exists to be a SECOND opinion, not a helper. `include/boot/pe_authenticode.h`
computes the same digest for the loader; if the two were derived from each
other, agreement between them would prove only that the code is deterministic.
So this file is transcribed directly from the numbered steps of "Windows
Authenticode Portable Executable Signature Format" v1.0 (2008-03-21), section
"Calculating the PE Image Hash", and deliberately shares no code with the header.

Two modes:

    authenticode_oracle.py              build the synthetic fixture, print its
                                        Authenticode and flat digests
    authenticode_oracle.py <file.efi>   print the Authenticode digest of a real
                                        PE image

The fixture mode backs the compiled-in expectation in test_boot_headers.c; the
file mode backs the smoke test's cross-check against the staged BOOTX64.EFI,
which is what turns "our two implementations agree" into "our implementation
agrees with the spec as read by someone else".
"""

import hashlib
import sys

FSZ = 1632


def build():
    """The synthetic fixture, byte-for-byte what build_pe() in the C test makes."""
    b = bytearray(FSZ)
    for i in range(FSZ):
        b[i] = (i * 7 + 3) & 0xFF
    b[0:2] = b'MZ'
    b[0x3C:0x40] = (64).to_bytes(4, 'little')
    pe = 64
    b[pe:pe + 4] = b'PE\0\0'
    coff = pe + 4                                            # 68
    b[coff + 0:coff + 2] = (0x8664).to_bytes(2, 'little')    # Machine
    b[coff + 2:coff + 4] = (3).to_bytes(2, 'little')         # NumberOfSections
    b[coff + 16:coff + 18] = (240).to_bytes(2, 'little')     # SizeOfOptionalHeader
    opt = coff + 20                                          # 88
    b[opt:opt + 2] = (0x20B).to_bytes(2, 'little')           # PE32+
    b[opt + 60:opt + 64] = (512).to_bytes(4, 'little')       # SizeOfHeaders
    b[opt + 64:opt + 68] = (0xDEADBEEF).to_bytes(4, 'little')  # CheckSum
    b[opt + 108:opt + 112] = (16).to_bytes(4, 'little')      # NumberOfRvaAndSizes
    dd = opt + 112                                           # 200
    sec_entry = dd + 4 * 8                                   # 232
    b[sec_entry:sec_entry + 4] = (1600).to_bytes(4, 'little')      # cert VA
    b[sec_entry + 4:sec_entry + 8] = (32).to_bytes(4, 'little')    # cert Size
    sectab = opt + 240                                       # 328

    def sec(n, ptr, size):
        o = sectab + n * 40
        b[o + 16:o + 20] = size.to_bytes(4, 'little')   # SizeOfRawData
        b[o + 20:o + 24] = ptr.to_bytes(4, 'little')    # PointerToRawData

    sec(0, 1024, 512)   # later in the file than section 1: the sort matters
    sec(1, 512, 512)
    sec(2, 2048, 0)     # SizeOfRawData 0 -> dropped at table build (step 9)
    return bytes(b)


def u16(b, o):
    return int.from_bytes(b[o:o + 2], 'little')


def u32(b, o):
    return int.from_bytes(b[o:o + 4], 'little')


def authenticode(b):
    """Steps 1-15, read straight off the spec."""
    if b[0:2] != b'MZ':
        raise ValueError('not an MZ image')
    pe = u32(b, 0x3C)
    if b[pe:pe + 4] != b'PE\0\0':
        raise ValueError('no PE signature')
    coff = pe + 4
    nsec = u16(b, coff + 2)
    opt_size = u16(b, coff + 16)
    opt = coff + 20

    magic = u16(b, opt)
    if magic == 0x10B:
        datadir = 96
    elif magic == 0x20B:
        datadir = 112
    else:
        raise ValueError('unknown optional header magic 0x%X' % magic)

    checksum_off = opt + 64                       # steps 3-4
    sec_entry = opt + datadir + 4 * 8             # steps 5-7
    size_of_headers = u32(b, opt + 60)
    nrva = u32(b, opt + datadir - 4)
    cert_size = u32(b, sec_entry + 4) if nrva > 4 else 0

    h = hashlib.sha256()
    h.update(b[0:checksum_off])                        # step 3
    h.update(b[checksum_off + 4:sec_entry])            # step 5 (skip checksum)
    h.update(b[sec_entry + 8:size_of_headers])         # step 7 (skip cert entry)

    total = size_of_headers                            # step 8
    sectab = opt + opt_size
    extents = []
    for i in range(nsec):                              # step 9
        o = sectab + i * 40
        size = u32(b, o + 16)
        ptr = u32(b, o + 20)
        if size == 0:
            continue
        extents.append((ptr, size))
    for ptr, size in sorted(extents):                  # steps 10-13
        h.update(b[ptr:ptr + size])
        total += size

    if len(b) > total:                                 # step 14
        end = len(b) - cert_size
        if end > total:
            h.update(b[total:end])
    return h.hexdigest()                               # step 15


def main():
    if len(sys.argv) > 1:
        with open(sys.argv[1], 'rb') as fh:
            print(authenticode(fh.read()))
        return 0
    img = build()
    print("file_size =", len(img))
    print("authenticode =", authenticode(img))
    print("flat         =", hashlib.sha256(img).hexdigest())
    return 0


if __name__ == '__main__':
    sys.exit(main())
