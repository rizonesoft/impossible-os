# shim/ -- Shim Binaries (currently UNPINNED)

> [!IMPORTANT]
> **This directory holds no shim binaries today.** `shimx64.efi` and `mmx64.efi`
> (Ubuntu `shim-signed` 1.58) were removed in `aab6b6f64` (2026-07-01): they were
> signed by the **Microsoft UEFI CA 2011**, which expired **2026-06-30**, so
> `scripts/sign-efi.sh` hard-fails on them and aborted the whole build. With the
> directory empty the disk recipe stages our loader as `EFI\BOOT\BOOTX64.EFI` and
> **direct-boots -- there is no shim chain**. Everything below describes the layout
> a pinned shim restores, and the hashes are the historical ones for the unpinned
> 1.58 binaries.
>
> Check any given build with `bash scripts/test-secureboot-smoke.sh` (reports
> `shim chain COVERED` / `NOT COVERED`); `REQUIRE_SHIM=1` makes an uncovered chain
> a hard failure. To restore a chain: `bash scripts/secure-boot/build-shim.sh` for
> a MOK-dev shim -- which carries no Microsoft signature, so verify it with
> `SHIM_TRUST_MODE=mok-dev` -- or drop a Microsoft-**UEFI CA 2023**-signed `shimx64.efi` +
> `mmx64.efi` here and refresh `SHA256SUMS` (the production path, gated on distro
> shim-review -- tracked as the vendor watch in
> [`todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md`](../todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md)).

Ubuntu's Microsoft-signed shim from the `shim-signed` package (v1.58).
These binaries are trusted by all UEFI firmware with the Microsoft UEFI CA
in the Secure Boot db -- no custom key enrollment required at the firmware level.

The shim validates `grubx64.efi` (our bootloader) via MOK (Machine Owner Key).
First boot requires MOK enrollment via MokManager; `MOK.cer` is placed on the
ESP root by the build system for easy enrollment.

## Files

| File | Source | Role | On ESP |
|------|--------|------|--------|
| `shimx64.efi` | `shim-signed` 1.58 (MS-signed) | First-stage UEFI loader | `EFI/BOOT/BOOTX64.EFI` |
| `mmx64.efi`   | `shim-signed` 1.58 | MokManager -- MOK enrollment UI | `EFI/BOOT/mmx64.efi` |
| `MOK.cer`     | Built by user (`keys/MOK.cer`) | Copied to ESP root at build time | `\MOK.cer` |

## EFI Partition Layout (with shim)

```
\EFI\BOOT\
  +-- BOOTX64.EFI    <- shimx64.efi (MS-signed, firmware trusts this)
  +-- grubx64.efi    <- Our bootloader, signed with MOK.key
  +-- mmx64.efi      <- MokManager (MOK enrollment UI)
\MOK.cer             <- MOK certificate for enrollment
\boot\
  +-- kernel.exe     <- Kernel binary
```

## Boot Chain

1. UEFI firmware loads `BOOTX64.EFI` (shimx64.efi) -- trusted via MS UEFI CA
2. Shim verifies `grubx64.efi` against db + MOK
3. First boot: MOK not enrolled -> MokManager launches -> user enrolls `MOK.cer`
4. Subsequent boots: MOK enrolled -> shim accepts signed `grubx64.efi` -> OS boots

## SHA256 Hashes

Historical -- the hashes of the unpinned 1.58 binaries. When a `SHA256SUMS` file
is present alongside pinned binaries, the disk recipe verifies them and fails the
build on a mismatch; with the directory empty there is nothing to verify.

```
6fe6e1bcbe6cf6baec8e056d40361ca1aa715cc04ddcc2855351de060b84350b  shimx64.efi
d2fa8e52fddc99dad94a0009fee23cb2478c28373b777d50b2f784eb4e96f88e  mmx64.efi
```

## Source

- Package: `shim-signed` 1.58+15.8-0ubuntu1 (Ubuntu Noble)
- Upstream: [rhboot/shim](https://github.com/rhboot/shim)
- Installed via: `sudo apt install shim-signed`
- MS signing: submitted through Ubuntu's shim-review process
