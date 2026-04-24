#!/usr/bin/env python3
"""check-kernel-bootproto.py -- verify the linked kernel.exe embeds a
`.bootproto` section whose 4-tuple { magic, version, struct_size,
sha256 } matches the ABI manifest.

Runs as a post-build check: opens build/kernel.exe (ELF64), walks the
section header table, finds `.bootproto`, parses the 56-byte
descriptor, and asserts each field matches the expected value
(manifest JSON for size + sha256; BOOT_INFO_VERSION grep for version).

Fails the build on any mismatch. Prevents silent drift between the
kernel TU that populates the descriptor and the manifest the bootloader
compares against.
"""

import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
KERNEL_EXE = REPO_ROOT / "build" / "kernel.exe"
MANIFEST = REPO_ROOT / "build" / "boot-info-abi.kernel.json"
BOOT_INFO_H = REPO_ROOT / "include" / "kernel" / "boot_info.h"

EXPECTED_MAGIC = 0x31445042  # "BPD1"
DESCRIPTOR_SIZE = 56


def die(msg):
    print(f"check-kernel-bootproto: FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def parse_expected():
    if not MANIFEST.exists():
        die(f"manifest not found: {MANIFEST}")
    manifest_raw = MANIFEST.read_bytes()
    expected_sha = hashlib.sha256(manifest_raw).digest()
    manifest_json = json.loads(manifest_raw)
    expected_size = manifest_json.get("struct_size")
    if expected_size is None:
        die("manifest missing struct_size")

    # Parse BOOT_INFO_VERSION from the header.
    text = BOOT_INFO_H.read_text()
    m = re.search(r"#define\s+BOOT_INFO_VERSION\s+(\d+)", text)
    if not m:
        die("could not parse BOOT_INFO_VERSION from boot_info.h")
    expected_version = int(m.group(1))
    return expected_version, expected_size, expected_sha


def find_bootproto_section(elf_bytes):
    # Elf64_Ehdr: check magic, class.
    if len(elf_bytes) < 64:
        die("kernel.exe too small for ELF header")
    magic = elf_bytes[:4]
    if magic != b"\x7fELF":
        die("kernel.exe is not ELF")
    if elf_bytes[4] != 2:
        die("kernel.exe is not ELF64")

    # Offsets per ELF64 spec.
    e_shoff = struct.unpack_from("<Q", elf_bytes, 40)[0]
    e_shentsize = struct.unpack_from("<H", elf_bytes, 58)[0]
    e_shnum = struct.unpack_from("<H", elf_bytes, 60)[0]
    e_shstrndx = struct.unpack_from("<H", elf_bytes, 62)[0]

    if e_shentsize != 64:
        die(f"unexpected sh_entsize {e_shentsize} (want 64)")

    # Load section headers.
    shdrs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type = struct.unpack_from("<II", elf_bytes, off)
        sh_offset = struct.unpack_from("<Q", elf_bytes, off + 24)[0]
        sh_size = struct.unpack_from("<Q", elf_bytes, off + 32)[0]
        shdrs.append((sh_name, sh_type, sh_offset, sh_size))

    # shstrtab bytes.
    shstr_off = shdrs[e_shstrndx][2]
    shstr_size = shdrs[e_shstrndx][3]
    shstrtab = elf_bytes[shstr_off:shstr_off + shstr_size]

    for sh_name, sh_type, sh_offset, sh_size in shdrs:
        if sh_name >= len(shstrtab):
            continue
        name_end = shstrtab.find(b"\0", sh_name)
        if name_end < 0:
            continue
        name = shstrtab[sh_name:name_end].decode("ascii", errors="replace")
        if name == ".bootproto":
            return sh_offset, sh_size

    die("`.bootproto` section not found in kernel.exe")


def main():
    if not KERNEL_EXE.exists():
        die(f"kernel.exe not found: {KERNEL_EXE} (run `make kernel`?)")

    expected_version, expected_size, expected_sha = parse_expected()

    elf_bytes = KERNEL_EXE.read_bytes()
    sh_offset, sh_size = find_bootproto_section(elf_bytes)

    if sh_size != DESCRIPTOR_SIZE:
        die(f".bootproto size {sh_size} != expected {DESCRIPTOR_SIZE}")

    desc = elf_bytes[sh_offset:sh_offset + DESCRIPTOR_SIZE]
    if len(desc) != DESCRIPTOR_SIZE:
        die(".bootproto section data truncated")

    # Parse 56-byte descriptor per boot_proto_descriptor layout.
    magic       = struct.unpack_from("<I", desc, 0)[0]
    version     = struct.unpack_from("<I", desc, 4)[0]
    struct_size = struct.unpack_from("<I", desc, 8)[0]
    sha256      = desc[12:44]
    flags       = struct.unpack_from("<I", desc, 44)[0]
    reserved    = desc[48:56]

    errors = []
    if magic != EXPECTED_MAGIC:
        errors.append(f"magic 0x{magic:08x} != expected 0x{EXPECTED_MAGIC:08x}")
    if version != expected_version:
        errors.append(f"version {version} != expected {expected_version}")
    if struct_size != expected_size:
        errors.append(f"struct_size {struct_size} != expected {expected_size}")
    if sha256 != expected_sha:
        errors.append(
            f"sha256 {sha256.hex()} != expected {expected_sha.hex()}"
        )
    if flags != 0:
        errors.append(f"flags {flags:#x} != expected 0 (reserved)")
    if reserved != b"\x00" * 8:
        errors.append(f"_reserved {reserved.hex()} != expected 00..00")

    if errors:
        for e in errors:
            print(f"check-kernel-bootproto: FAIL: {e}", file=sys.stderr)
        sys.exit(1)

    print(
        f"[TOOL] .bootproto verified in kernel.exe "
        f"(version={version}, struct_size={struct_size}, "
        f"sha256={expected_sha.hex()[:12]}...)"
    )


if __name__ == "__main__":
    main()
