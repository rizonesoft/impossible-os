#!/usr/bin/env python3
"""
make-initrd.py — Pack files into an Impossible OS initrd archive.

Archive format:
    [initrd_header]                  — 4-byte magic ("IXRD") + 4-byte file count
    [initrd_file_entry] × count      — 64-byte name + 4-byte offset + 4-byte size
    [file data...]                   — raw file contents

Usage:
    python3 scripts/make-initrd.py -o build/initrd.img file1.txt file2.txt ...
"""

import struct
import sys
import os
import argparse

MAGIC = 0x44525849  # "IXRD"
MAX_NAME = 64
HEADER_SIZE = 8     # magic(4) + file_count(4)
ENTRY_SIZE = 72     # name(64) + offset(4) + size(4)


def main():
    parser = argparse.ArgumentParser(description="Pack files into an initrd image")
    parser.add_argument("-o", "--output", required=True, help="Output initrd file")
    parser.add_argument("files", nargs="*", help="Files to include")
    args = parser.parse_args()

    files = []
    for filepath in args.files:
        if not os.path.isfile(filepath):
            print(f"Warning: '{filepath}' not found, skipping", file=sys.stderr)
            continue
        with open(filepath, "rb") as f:
            data = f.read()
        name = os.path.basename(filepath)
        if len(name) >= MAX_NAME:
            name = name[:MAX_NAME - 1]
        files.append((name, data))

    file_count = len(files)

    # Calculate data offset (after header + all entries)
    data_offset = HEADER_SIZE + ENTRY_SIZE * file_count

    # Build the archive
    with open(args.output, "wb") as out:
        # Write header
        out.write(struct.pack("<II", MAGIC, file_count))

        # Write file entries
        current_offset = data_offset
        for name, data in files:
            name_bytes = name.encode("utf-8")[:MAX_NAME - 1]
            name_padded = name_bytes + b"\x00" * (MAX_NAME - len(name_bytes))
            out.write(name_padded)
            out.write(struct.pack("<II", current_offset, len(data)))
            current_offset += len(data)

        # Write file data
        for name, data in files:
            out.write(data)

    print(f"Created initrd: {args.output} ({file_count} files, {current_offset} bytes)")


if __name__ == "__main__":
    main()
