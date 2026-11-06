#!/usr/bin/env python3
"""BOOTX64.EFI must stay a PE32+ EFI application with its entry and SBAT intact.

binutils 2.46 dropped the `efi-app-x86_64` objcopy target, so the recipe now names
`pei-x86-64` and the subsystem separately. Firmware refuses a wrong subsystem and no
emulator run shows a missing SBAT section, so these are checked on the built image.
"""
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
EFI = REPO / "build/tools/BOOTX64.EFI"
SO = REPO / "src/boot/uefi/bootx64.so"
MK = REPO / "src/boot/uefi/Makefile"


def _objdump(*a):
    return subprocess.run(["objdump", *a, str(EFI)], capture_output=True, text=True, timeout=60).stdout


def test_recipe_names_a_target_current_binutils_has():
    text = MK.read_text()
    assert "efi-app-x86_64" not in text
    assert "-O pei-x86-64 --subsystem efi-app" in text


def test_built_image():
    if not EFI.exists():
        print("SKIP built image checks: no build/tools/BOOTX64.EFI")
        return
    p = _objdump("-p")
    assert re.search(r"Subsystem\s+0000000a", p), "not an EFI application"
    assert re.search(r"SectionAlignment\s+00001000", p), "section alignment changed"
    sections = _objdump("-h")
    for name in (".text", ".reloc", ".sbat"):
        assert re.search(rf"\s{re.escape(name)}\s", sections), f"missing {name}"
    if SO.exists():
        nm = subprocess.run(["nm", str(SO)], capture_output=True, text=True, timeout=60).stdout
        entry = re.search(r"^0*([0-9a-f]+) T efi_main$", nm, re.M)
        assert entry and re.search(rf"AddressOfEntryPoint\s+0*{entry.group(1)}\b", p), "entry is not efi_main"


if __name__ == "__main__":
    test_recipe_names_a_target_current_binutils_has()
    test_built_image()
    print("PASS test_efi_image_format")
    sys.exit(0)
