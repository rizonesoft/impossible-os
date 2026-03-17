#!/usr/bin/env python3
"""convert_symmap.py — Convert nm output to a packed binary symbol table.

Reads the output of `llvm-nm -n kernel.exe` and produces a binary file
containing sorted {address, name} entries for kernel address resolution.

Binary format:
  Header:  4 bytes magic ("KSYM")
           4 bytes entry count (uint32 LE)
  Entries: 8 bytes address (uint64 LE)
           32 bytes name (null-padded, truncated at 31 chars)
"""

import struct
import sys
import os

MAGIC = b"KSYM"
NAME_LEN = 32  # Fixed-length name field (31 chars + NUL)

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <nm_output.txt> <output.bin>", file=sys.stderr)
        sys.exit(1)

    nm_file = sys.argv[1]
    out_file = sys.argv[2]

    entries = []
    with open(nm_file, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 3:
                continue

            addr_str, sym_type, name = parts[0], parts[1], parts[2]

            # Only include text (T/t) and data (D/d) symbols
            if sym_type not in ("T", "t", "D", "d"):
                continue

            # Skip symbols starting with . or $ (compiler internals)
            if name.startswith(".") or name.startswith("$"):
                continue

            try:
                addr = int(addr_str, 16)
            except ValueError:
                continue

            # Skip zero-address symbols
            if addr == 0:
                continue

            entries.append((addr, name))

    # Sort by address (should already be sorted from nm -n)
    entries.sort(key=lambda e: e[0])

    # Write binary file
    with open(out_file, "wb") as f:
        # Header
        f.write(MAGIC)
        f.write(struct.pack("<I", len(entries)))

        # Entries
        for addr, name in entries:
            f.write(struct.pack("<Q", addr))
            # Truncate and null-pad name to NAME_LEN bytes
            name_bytes = name[:NAME_LEN - 1].encode("ascii", errors="replace")
            name_bytes = name_bytes + b"\x00" * (NAME_LEN - len(name_bytes))
            f.write(name_bytes)

    print(f"[SYMMAP] {out_file}: {len(entries)} symbols ({os.path.getsize(out_file)} bytes)")

if __name__ == "__main__":
    main()
