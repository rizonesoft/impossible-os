#!/usr/bin/env python3
"""Validate src/boot/uefi/efi.h protocol GUIDs against EDK2/TianoCore MdePkg.

Why this exists
---------------
src/boot/uefi/efi.h is hand-written ("Based on UEFI Specification 2.9", no
gnu-efi dependency), which is a deliberate and good choice: the bootloader
stays freestanding and owns its own types. The cost is that every protocol GUID
in it is a 128-bit constant transcribed by hand. One wrong hex digit does not
fail the build, does not fail a unit test, and does not fail under QEMU with
OVMF if the affected protocol happens to be one the loader falls back from. It
fails on somebody's real machine, as "device not found".

EDK2 is the reference implementation those constants come from, and it is
BSD-2-Clause-Patent, so it is license-compatible with this GPL-3.0-only tree.
Rather than VENDOR EDK2 -- its headers are coupled to the EDK2 build system
(Base.h, ProcessorBind.h, autogen) and adopting them would drag that whole
world into a bootloader that deliberately avoids it -- this uses it as an
external ORACLE: parse both sides, compare, report drift.

Usage
-----
    python3 tools/uefi-guid-check/check.py --edk2 <path-to-edk2-checkout>

    # Fetch the reference first (headers only, ~11 MiB):
    git clone --depth 1 --filter=blob:none --sparse \\
        https://github.com/tianocore/edk2.git /tmp/edk2
    git -C /tmp/edk2 sparse-checkout set MdePkg/Include

Exit codes: 0 = every shared GUID matches, 1 = at least one mismatch.
A GUID we define that EDK2 does not is reported as unverifiable, not as a
failure -- some are ours or come from other specs.
"""

import argparse
import pathlib
import re
import sys

# A GUID definition spanning any number of continuation lines. The NAME is
# matched loosely (any macro identifier) rather than by a *GUID suffix,
# because EDK2 names some of these without one -- EFI_GLOBAL_VARIABLE is the
# GUID of the global variable vendor namespace and carries no suffix at all.
# The shape check below (exactly 11 numbers in UEFI GUID field widths) is what
# actually decides whether a macro is a GUID, and it is far more reliable than
# the name. Both projects write the same shape, modulo whitespace and braces:
#   #define NAME \
#     { 0x11111111, 0x2222, 0x3333, { 0x44, ... 0x44 } }
GUID_RE = re.compile(
    r"#define\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\\?\s*"
    r"(?P<body>(?:[^\n]*\\\s*\n)*[^\n]*)",
    re.MULTILINE,
)
HEX_RE = re.compile(r"0[xX][0-9a-fA-F]+")


def parse_guids(text):
    """Return {name: (d1, d2, d3, b0..b7)} for every parsable GUID macro."""
    out = {}
    for m in GUID_RE.finditer(text):
        name = m.group("name")
        nums = [int(h, 16) for h in HEX_RE.findall(m.group("body"))]
        # A UEFI GUID is exactly 11 numbers: 32-bit, 16-bit, 16-bit, 8 bytes.
        # Anything else is a macro that merely ends in GUID (a count, a helper)
        # and is skipped rather than guessed at.
        if len(nums) != 11:
            continue
        if nums[0] > 0xFFFFFFFF or nums[1] > 0xFFFF or nums[2] > 0xFFFF:
            continue
        if any(b > 0xFF for b in nums[3:]):
            continue
        out[name] = tuple(nums)
    return out


# Names that differ between the two projects while naming the SAME constant.
# EDK2 predates the UEFI-era "EFI_" prefixing convention for some GUIDs, so a
# pure name match reports a false mismatch here. This bit first on the ACPI
# table pair: our EFI_ACPI_TABLE_GUID is EDK2's ACPI_TABLE_GUID (the ACPI 1.0
# RSDP), while EDK2's EFI_ACPI_TABLE_GUID is the ACPI 2.0+ one we call
# EFI_ACPI_20_TABLE_GUID. Both of our values were correct; "fixing" the
# reported mismatch would have broken ACPI 1.0 discovery on real firmware.
#
# Only add an entry here after confirming the two names denote the same
# spec-defined constant. An alias is a claim about the spec, not a way to
# silence a diff.
ALIASES = {
    "EFI_ACPI_TABLE_GUID":     "ACPI_TABLE_GUID",       # ACPI 1.0 RSDP
    "EFI_ACPI_20_TABLE_GUID":  "EFI_ACPI_TABLE_GUID",   # ACPI 2.0+ RSDP
    "EFI_SMBIOS_TABLE_GUID":   "SMBIOS_TABLE_GUID",     # SMBIOS 2.x entry point
    "EFI_SMBIOS3_TABLE_GUID":  "SMBIOS3_TABLE_GUID",    # SMBIOS 3.x 64-bit entry
    "EFI_GLOBAL_VARIABLE_GUID": "EFI_GLOBAL_VARIABLE",  # EDK2 omits the _GUID suffix
}


def fmt(g):
    return ("{%08x-%04x-%04x-" % g[:3]) + "".join("%02x" % b for b in g[3:]) + "}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--edk2", required=True,
                    help="path to an EDK2 checkout containing MdePkg/Include")
    ap.add_argument("--efi-header", default="src/boot/uefi/efi.h")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    ours_path = pathlib.Path(args.efi_header)
    if not ours_path.exists():
        print("error: %s not found" % ours_path, file=sys.stderr)
        return 2

    inc = pathlib.Path(args.edk2) / "MdePkg" / "Include"
    if not inc.is_dir():
        print("error: %s not found -- sparse-checkout MdePkg/Include first" % inc,
              file=sys.stderr)
        return 2

    ours = parse_guids(ours_path.read_text(encoding="utf-8", errors="replace"))

    ref = {}
    for h in inc.rglob("*.h"):
        for name, guid in parse_guids(
                h.read_text(encoding="utf-8", errors="replace")).items():
            # First definition wins; EDK2 does not redefine a GUID with a
            # different value, so a later duplicate is the same constant.
            ref.setdefault(name, guid)

    mismatches, verified, unverifiable = [], [], []
    for name, guid in sorted(ours.items()):
        ref_name = ALIASES.get(name, name)
        if ref_name not in ref:
            unverifiable.append(name)
        elif ref[ref_name] != guid:
            mismatches.append((name, ref_name, guid, ref[ref_name]))
        else:
            verified.append(name)

    if not args.quiet:
        print("uefi-guid-check: %d parsed from %s, %d from EDK2 MdePkg"
              % (len(ours), ours_path, len(ref)))
        print("  verified:     %d" % len(verified))
        print("  unverifiable: %d %s"
              % (len(unverifiable), ("(" + ", ".join(unverifiable) + ")")
                 if unverifiable else ""))
        print("  MISMATCHED:   %d" % len(mismatches))

    for name, ref_name, mine, theirs in mismatches:
        label = name if ref_name == name else "%s (EDK2: %s)" % (name, ref_name)
        print("MISMATCH %s\n    ours: %s\n    edk2: %s"
              % (label, fmt(mine), fmt(theirs)))

    if mismatches:
        print("\nA wrong GUID does not fail the build or the unit suite: the "
              "protocol simply is never located on real firmware.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
