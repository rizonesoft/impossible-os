# shim/ -- Microsoft-Signed Shim Binaries

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

Verified at build time via `SHA256SUMS` (build fails on mismatch).

```
6fe6e1bcbe6cf6baec8e056d40361ca1aa715cc04ddcc2855351de060b84350b  shimx64.efi
d2fa8e52fddc99dad94a0009fee23cb2478c28373b777d50b2f784eb4e96f88e  mmx64.efi
```

## Source

- Package: `shim-signed` 1.58+15.8-0ubuntu1 (Ubuntu Noble)
- Upstream: [rhboot/shim](https://github.com/rhboot/shim)
- Installed via: `sudo apt install shim-signed`
- MS signing: submitted through Ubuntu's shim-review process
