# Credits and Third-Party Attribution

Impossible OS is licensed under the **GNU General Public License version 3 only** (GPL-3.0-only). See [LICENSE](LICENSE) for the full text.

This file lists every third-party work redistributed in this repository or in a built Impossible OS image, with its copyright holder, license, and upstream source. It exists to satisfy the attribution obligations of the permissive licenses below, which follow the **binary** as well as the source: a shipped disk image, ISO, or USB artifact must carry these notices, not just the source tree.

Machine-readable provenance for the vendored code libraries (upstream commit, embedded version macro, vendoring commit) lives in [src/libs/PROVENANCE.md](src/libs/PROVENANCE.md). This file is the human-facing and redistribution-facing companion to it.

## License compatibility

All third-party works listed here are compatible with GPL-3.0-only, which is a narrower constraint than it first appears:

- Permissive licenses (BSD-2-Clause, BSD-3-Clause, MIT, CC0-1.0, SIL OFL 1.1) combine freely into a GPL-3.0 work.
- Apache-2.0 is compatible with GPLv3 but **not** with GPLv2, so an Apache-2.0 dependency is available to this project where it would not be to a GPL-2.0 kernel.
- GPL-2.0-**or-later** code may be used, because the "or later" clause permits relicensing under v3.
- GPL-2.0-**only** code may **not** be used. This excludes the Linux kernel and everything derived from it, including Linux device drivers. See "Sources that cannot be used" below.

## Vendored code libraries

Source under `src/libs/`. Each directory retains its upstream license file verbatim.

| Library | Copyright | License | Upstream |
| --- | --- | --- | --- |
| Mbed TLS | Copyright The Mbed TLS Contributors | Apache-2.0 OR GPL-2.0-or-later | https://github.com/Mbed-TLS/mbedtls |
| Monocypher | Copyright (c) 2017-2020, Loup Vaillant | BSD-2-Clause OR CC0-1.0 | https://monocypher.org |
| LZ4 | Copyright (c) Yann Collet | BSD-2-Clause (`lib/` only) | https://github.com/lz4/lz4 |
| miniz | Copyright 2013-2014 RAD Game Tools and Valve Software; Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC | MIT | https://github.com/richgel999/miniz |
| cJSON | Copyright (c) 2009-2017 Dave Gamble and cJSON contributors | MIT | https://github.com/DaveGamble/cJSON |

Notes:

- **LZ4** is dual-licensed upstream. Only the BSD-2-Clause `lib/` sources are vendored here; the GPL-2.0-or-later `programs/` sources are not. The effective license for this copy is BSD-2-Clause.
- **Monocypher** ships its license as `LICENCE.md` (British spelling), not `LICENSE`. Automated license sweeps that glob `LICENSE*` will miss it.

## Fonts

Redistributed under `resources/fonts/`. All three are SIL Open Font License 1.1, which permits bundling in and with software including sale, and requires that the license and copyright notice travel with the font files.

| Font | Copyright | License |
| --- | --- | --- |
| Inter | Copyright 2020 The Inter Project Authors (https://github.com/rsms/inter) | SIL OFL 1.1 |
| Selawik | Copyright 2015, Microsoft Corporation, with Reserved Font Name Selawik | SIL OFL 1.1 |
| Cascadia Code | Copyright (c) 2019, Microsoft Corporation | SIL OFL 1.1 |

Selawik is a trademark of Microsoft Corporation in the United States and/or other countries. The OFL Reserved Font Name clause means a modified derivative may not be distributed under the name "Selawik".

## Assets NOT redistributed

**Icons8 color icons** (`resources/icons/color/`) are licensed under the Icons8 Universal Multimedia Licensing Agreement, a paid subscription license that **prohibits redistribution as standalone files**. The PNG files are therefore excluded from version control via `.gitignore` and are not present in this repository. A build without them succeeds: the icon store returns NULL for missing color icons and falls back to monochrome Fluent icons. Obtaining them requires a paid Icons8 account; see [resources/icons/color/LICENSE.md](resources/icons/color/LICENSE.md).

Anyone redistributing a built Impossible OS image that includes these icons is bound by their own Icons8 agreement, not by this project's license.

## Ported source files (per-file inventory)

Whole libraries are vendored under `src/libs/` and listed above. **Individual files** adapted from another project are a different granularity and need a per-file record: upstream file path, upstream project, license, and SPDX identifier.

That inventory is planned but not yet built. `04-drivers-hardware/TODO-14-network-drivers.md §1` owns it and requires it to exist **before any porting work begins**, alongside a `LICENSES/` directory holding the full license texts. Several driver TODOs already queue entries for it: SerenityOS `VMWareFramebufferDevice.cpp` (BSD-2) in TODO-17, Intel and Realtek Bluetooth firmware (BSD-2, from linux-firmware) in TODO-16, and MediaTek MT7921 firmware (MIT) in TODO-15.

**This file is the root notice file.** Earlier TODOs refer to that artifact as `NOTICE.md`; it had not been created when this file was written. The per-file inventory should land as a section here or as a `LICENSES/` companion referenced from here, so the project has one notice set rather than two competing ones.

## Redistributable device firmware

Device microcode and configuration blobs are redistributed **inside the OS image** and carry their own licenses, separate from the driver source that loads them. No firmware blob is in this repository yet.

`04-drivers-hardware/TODO-06-firmware-loader-device-blobs.md` owns the loading subsystem and specifies a per-blob manifest carrying device ids, version, license, source URL, hash, and an explicit **redistribution flag**. That flag is the load-bearing field: some vendor firmware is freely redistributable and some is not, and the difference decides whether a blob may ship in a release image at all. Every blob that ships must gain a row here.

## Specifications and reference documents

Impossible OS implements behavior defined by external specifications. Implementing a published specification creates no license obligation, and no specification text is copied into this tree, but the specifications are acknowledged as the normative sources for the code that follows them: UEFI, ACPI, Intel SDM, PE/COFF, ELF, TCG TPM 2.0, PCIe, NVMe, AHCI, xHCI, SMBIOS, FAT32, and the relevant IETF RFCs.

## Development tools

Tools used to build or validate Impossible OS are not redistributed with it and impose no attribution obligation on the shipped artifacts. They are acknowledged here as a matter of record: clang, ld.lld and llvm-addr2line (Apache-2.0 WITH LLVM-exception), NASM (BSD-2-Clause), QEMU (GPL-2.0), swtpm and libtpms (used only as a TPM test emulator, never linked or vendored), and OVMF/EDK2 firmware used for testing (BSD-2-Clause-Patent).

## Sources that cannot be used

Recorded so the constraint is not rediscovered the hard way. Because this project is GPL-3.0-**only**, code under **GPL-2.0-only** is license-incompatible and must not be vendored, adapted, or translated into this tree. In practice that rules out:

- The Linux kernel and its device drivers, which are GPL-2.0-only by the explicit terms in its `COPYING`.
- Anything whose only upstream is a Linux driver, including out-of-tree Linux driver projects that inherit that license.

The compatible route to mature driver and stack code is the permissively-licensed ecosystem: the BSD kernels (BSD-2-Clause / BSD-3-Clause), EDK2/TianoCore (BSD-2-Clause-Patent), ACPICA (dual BSD/GPL, so the BSD arm is available), and standalone permissive stacks. Adding any of these is a dependency addition and requires explicit approval per the CLAUDE.md dependency gate, plus a row in `src/libs/PROVENANCE.md` and in this file.

## Adding a third-party work

1. Confirm license compatibility with GPL-3.0-only against the rules above. GPL-2.0-only is a hard stop.
2. Get explicit approval: dependency additions are a gated decision, not an implementation detail.
3. Vendor the source under `src/libs/<name>/`, keeping the upstream license file verbatim and unrenamed.
4. Add a row to `src/libs/PROVENANCE.md` with upstream URL, embedded version macro, license, and the vendoring commit.
5. Add a row to this file so the attribution ships with the binary.
