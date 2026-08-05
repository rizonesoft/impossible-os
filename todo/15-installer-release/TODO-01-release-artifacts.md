---
schema_version: 1
id: release-artifacts
domain: 15-installer-release
status: active
title: "TODO-01 -- Disk Image, USB & Release Artifacts"
---

# TODO-01 -- Disk Image, USB & Release Artifacts

> **Goal:** Turn development build outputs into versioned, signed, distributable release
> artifacts -- compressed disk images, bootable ISOs, USB writer, VM image variants, code-
> signed binaries, an artifact manifest, and reproducible builds. This is the difference
> between a development build and a product release.

> [!IMPORTANT]
> **Version header already exists**: `include/kernel/version.h` (with `build_info.h`)
> defines `VERSION_MAJOR.MINOR.PATCH.BUILD` and `version_string()`. This TODO extends
> the scheme to the `MAJOR.MINOR.BUILD` Windows-style surface and adds the
> `set-version.sh` script, Registry baking, and `winver.exe`. Do not re-specify the
> base `version.h` constants.
>
> **ISO script already specced**: `10-platform-services/TODO-11-installer-iso.md §6`
> owns `scripts/make-iso.sh` (El Torito + EFI, no GRUB, `make iso` target,
> `sha256sum`). §4 here extends it with Joliet+Rock Ridge, versioned filename, and
> `README.txt` -- do not duplicate the base ISO build.
>
> **Code signing already specced**: `09-desktop-shell/TODO-07-cng-crypto.md §9` owns
> Ed25519 PE32+ COSI-trailer signing + `codesign_sign/verify()` + optional enforcement
> in `task_create_user()`. §5 here adds the **release script** that calls those functions
> and the **bootloader-side kernel verification** path -- do not re-specify the crypto or
> the PE trailer format.
>
> **Artifact manifest consumer**: `10-platform-services/TODO-03` update check API parses
> the `release-{version}.json` produced by §6.

---

## Inputs

- `include/kernel/version.h` + `include/build_info.h` -- existing version scheme; extend in §1
- `scripts/build.sh` -- existing build script; extend with `make release` / `make iso` hooks
- `scripts/make-iso.sh` (from `TODO-11 §6`) -- base ISO build; extend in §8
- `09-desktop-shell/TODO-07-cng-crypto.md §9` (→ XREF) -- `codesign_sign(path, priv_key)` / `codesign_verify(path)`; used in §5
- `10-platform-services/TODO-11-installer-iso.md §6` (→ XREF) -- `make iso` target; §8 extends it
- `10-platform-services/TODO-03-update-delivery.md` (→ XREF) -- consumes `release-{version}.json` from §4
- `src/boot/uefi/bootx64.c` -- bootloader source; extend with optional kernel signature check in §5
- `include/kernel/uefi_runtime.h` -- UEFI variable access for Secure Boot toggle check -- §5
- `tools/` (host-side build tools pattern) -- `usb_creator.c` follows same pattern -- §3
- → XREF: `D00 T01 §3, §6` -- repo-local wrapper and artifact policy own the developer-facing build/CI contract that release scripts consume

---

## Outcome

`make release` produces a complete set of versioned, zstd-compressed, SHA-256-verified
artifacts: `impossible-os-1.0.22000.img.zst`, `.iso`, `.vmdk`, `.vhd`, `.vhdx`, and
`release-1.0.22000.json`. `codesign_sign` is called on `kernel.exe` and `BOOTX64.EFI`.
`make verify-reproducible` confirms byte-identical output from same source. `winver.exe`
shows `Impossible OS 1.0 (Build 22000)`.

---

## Implementation Order

| Step | Section                                  | 💎/⭐ | Dependency                               |
| ---- | ---------------------------------------- | ----- | ---------------------------------------- |
| 1    | Versioning scheme (`set-version.sh`, `winver.exe`) | 💎    | Extends existing `version.h` + `build_info.h` |
| 2    | GPT disk image release (`release-image.sh`, zstd) | 💎    | §1 version baked; existing `build/system-disk.img` |
| 3    | USB-bootable image (`make-usb.sh` + `usb_creator.c`) | 💎    | §2 compressed image                      |
| 4    | Bootable ISO (Joliet+Rock Ridge, versioned, `README.txt`) | 💎    | `D10T11 §6` base ISO; §6 version         |
| 5    | Code signing (`sign-release.sh`, bootloader verify) | 💎    | `D09T07 §7` `codesign_sign/verify`; §1–§4 artifacts |
| 6    | Artifact manifest (`release-{version}.json`) | ⭐    | §2–§5 all artifacts; `TODO-03` consumer  |
| 7    | VM image variants (VMDK/VHD/VHDX + `.ovf`) | 💎    | §2 raw image; `qemu-img` installed       |
| 8    | Reproducible builds (`SOURCE_DATE_EPOCH`) | ⭐    | §1–§7 all scripts; `make verify-reproducible` |

---

## 1. Versioning Scheme `[Sonnet]`

**Source:** `scripts/set-version.sh`; extends `include/version.h` + `include/build_info.h`

- [ ] **`MAJOR.MINOR.BUILD` Windows-style surface**: `BUILD` = `VERSION_BUILD` from `build_info.h` (sequential CI build number, auto-incremented by `scripts/increment-build.sh`); short form `1.0` = `MAJOR.MINOR`; full form `1.0.22000` = `MAJOR.MINOR.BUILD`
- [ ] **`OS_VERSION_STRING`** addition to `include/kernel/version.h`:
  ```c
  #define OS_VERSION_STRING \
      "Impossible OS " VER_STR(VERSION_MAJOR) "." VER_STR(VERSION_MINOR) \
      " (Build " VER_STR(VERSION_BUILD) ")"
  /* e.g. "Impossible OS 1.0 (Build 22000)" */
  ```
- [ ] **`scripts/set-version.sh <major> <minor> <patch>`**: write `#define VERSION_MAJOR`, `VERSION_MINOR`, `VERSION_PATCH` into `include/build_info.h`; leave `VERSION_BUILD` to be set by CI; commit `"build: bump version to {major}.{minor}.{patch}"` (no push; used by maintainer)
- [ ] **`scripts/increment-build.sh`**: read `VERSION_BUILD` from `build_info.h`; increment by 1; write back; called by `scripts/build.sh` at the start of every build; commit-free (local update only)
- [ ] **Registry baking at boot**: in `version_print()` (or kernel init, after Registry init): `reg_set_string(HKLM, "SOFTWARE\\Impossible\\Version", OS_VERSION_STRING)` + `reg_set_dword(HKLM, "SOFTWARE\\Impossible\\BuildNumber", VERSION_BUILD)`; sets the same key read by `winver.exe` and `sysinfo.exe`
- [ ] **`winver.exe`** (`src/tools/winver.c`): small GUI app; `MessageBoxA(NULL, OS_VERSION_STRING "\n\nCopyright © 2026 Rizonetech (Pty) Ltd", "About Impossible OS", MB_OK | MB_ICONINFORMATION)` style dialog (or IxUI window); reads `HKLM\SOFTWARE\Impossible\Version` at runtime
- [ ] **Kernel boot log**: `version_print()` already prints version; confirm it outputs `OS_VERSION_STRING` format to serial; visible in `build/serial.log`

---

## 2. GPT Disk Image Release `[Sonnet]`

**Source:** `scripts/release-image.sh`

- [ ] **`scripts/release-image.sh`**:
  1. Assert `build/system-disk.img` exists and size > 64 MiB; fail fast otherwise
  2. **Integrity checks**:
     - EFI partition check: `fdisk -l build/system-disk.img` → confirm EFI System Partition present; `mcopy -i "${EFI_OFFSET}" -s :: | grep BOOTX64.EFI` → confirm bootloader present
     - Kernel check: scan IXFS partition for `boot/kernel.exe` via `fdisk` + `dd` + raw path scan; confirm > 1 MiB
     - GPT signature check: `dd if=build/system-disk.img bs=512 count=1 | xxd | grep "4546 4920 5061 7274"` (EFI Part magic at LBA 1)
  3. Read `VERSION=$(grep VERSION_BUILD include/build_info.h | awk '{print $3}')` and `MAJOR/MINOR`
  4. **Compress**: `zstd -T0 -9 -o "build/impossible-os-${MAJOR}.${MINOR}.${BUILD}.img.zst" build/system-disk.img`
  5. **SHA-256**: `sha256sum "build/impossible-os-${MAJOR}.${MINOR}.${BUILD}.img.zst" > "build/impossible-os-${MAJOR}.${MINOR}.${BUILD}.img.zst.sha256"`
  6. Print: `Release image: impossible-os-{ver}.img.zst ({size} bytes, SHA-256: {hash})`
- [ ] **`make release-image`** target: calls `scripts/release-image.sh`; depends on `make` (build) completing first
- [ ] **Decompression instructions** in `docs/infrastructure/install-from-image.md`: `zstd -d impossible-os-{ver}.img.zst -o impossible-os.img && dd if=impossible-os.img of=/dev/sdX bs=4M status=progress`
- [ ] **Deterministic-image XREF** -- `release-image.sh` calls `scripts/release/build-image.sh` (owner: `D01 T06 §2`) and runs `verify-esp.sh` as the pre-compress gate.

---

## 3. USB-Bootable Image `[Sonnet]`

**Source:** `scripts/make-usb.sh`; `tools/usb-creator/usb_creator.c` (host-side, GCC)

- [ ] **`scripts/make-usb.sh <device>`**:
  1. Validate `<device>` is a block device (`test -b "$1"`); reject if not removable (`cat /sys/block/$(basename $1)/removable` must be `1`)
  2. Print: `WARNING: This will erase all data on $1 ($(lsblk -ndo SIZE $1)). Type "yes" to continue:`; read confirmation; abort if not `yes`
  3. Decompress if passed `.img.zst`: `zstd -d "$IMG" -o /tmp/impossible-os-usb.img`
  4. Write: `dd if=/tmp/impossible-os-usb.img of=$1 bs=4M conv=fsync status=progress`
  5. Verify: `dd if=$1 bs=512 count=1 | cmp - <(dd if=/tmp/impossible-os-usb.img bs=512 count=1)` → print `Verify: OK` or `Verify: FAILED`
  6. `sync && eject $1` -- safely eject
- [ ] **`tools/usb-creator/usb_creator.c`** (Windows host tool, compiled with `gcc -mwindows`):
  - List removable drives: `SetupDiGetClassDevs` + `GUID_DEVCLASS_DISKDRIVE`; filter `BusType == BusTypeUsb`; display as `Removable: E:\ (SanDisk 32GB)`
  - Browse for `.img.zst` or `.img` via `GetOpenFileNameA`
  - Write button: `CreateFile(\\.\\PhysicalDriveN, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL)`; `DeviceIoControl(IOCTL_DISK_SET_DRIVE_LAYOUT_EX, ...)` to clear partition table first; `WriteFile` in 4 MiB chunks; progress bar
  - Requires running as Administrator (manifest `requireAdministrator`); show UAC prompt on launch
  - Compiled from host: `gcc -O2 -mwindows -o usb_creator.exe usb_creator.c -lsetupapi`
- [ ] **Post-write verify XREF** -- `make-usb.sh` runs the `D01 T06 §2` post-write hash-check (sgdisk -p re-read + per-file sha256 of all required ESP files) before declaring success.

---

## 4. Bootable ISO (Joliet + Rock Ridge) `[Sonnet]`

> Extends `10-platform-services/TODO-11-installer-iso.md §6` -- adds Joliet+Rock Ridge
> extensions, versioned output filename, and `README.txt` at ISO root.

**Modification to `scripts/make-iso.sh`**

- [ ] **Add Joliet + Rock Ridge to `xorriso` invocation**:
  ```bash
  xorriso -as mkisofs \
    -R -J \                    # Rock Ridge (-R) + Joliet (-J)
    --joliet-long \            # allow long Joliet filenames
    -V "ImpossibleOS_${VER}" \ # volume label
    -e EFI/BOOT/BOOTX64.EFI \ # EFI boot entry (no GRUB)
    -no-emul-boot \
    -eltorito-alt-boot \       # El Torito for legacy BIOS
    -b boot/boot.img \
    -no-emul-boot \
    -o "build/impossible-os-${VER}.iso" \
    staging/
  ```
- [ ] **Versioned output filename**: `build/impossible-os-{MAJOR}.{MINOR}.{BUILD}.iso` (not `os-build.iso`); update `make iso` target in `Makefile`
- [ ] **`README.txt`** at ISO root (`staging/README.txt`):
  ```
  Impossible OS {version}
  =======================
  To install: boot from this disc (UEFI mode required).
  Minimum requirements: x86-64 CPU, 512 MB RAM, 4 GB disk.
  For help: https://github.com/rizonetech/impossible-os
  ```
- [ ] **SHA-256** alongside ISO: `sha256sum "build/impossible-os-${VER}.iso" > "build/impossible-os-${VER}.iso.sha256"`
- [ ] **Verify**: `file build/impossible-os-*.iso` must match `ISO 9660 CD-ROM filesystem`; `isoinfo -d -i *.iso` shows Joliet + Rock Ridge present; ISO boots in QEMU with `-cdrom` flag

---

## 5. Code Signing `[Opus]`

> Security-critical: bootloader-side kernel signature verification is novel (no prior
> Impossible OS bootloader verification path). Extends `TODO-07 §9` release pipeline.

**Source:** `scripts/sign-release.sh`; modification to `src/boot/uefi/bootx64.c`

- [ ] **`scripts/sign-release.sh`**:
  1. Read Ed25519 private key from environment: `CODESIGN_PRIV_KEY` (base64-encoded 64 bytes; set as GitHub Actions secret); fail if not set with `"CODESIGN_PRIV_KEY not set -- cannot sign release"`
  2. Decode key: `echo "$CODESIGN_PRIV_KEY" | base64 -d > /tmp/priv.key` (0600 permissions); trap EXIT to `rm -f /tmp/priv.key`
  3. Call `codesign_sign` for each binary (via a thin host wrapper `tools/codesign_host.c` -- same Ed25519 COSI logic compiled for the host, operating on raw files):
     - `tools/codesign_host build/os/kernel.exe /tmp/priv.key`
     - `tools/codesign_host build/esp/EFI/BOOT/BOOTX64.EFI /tmp/priv.key`
  4. **GPG-sign artifacts** for release integrity:
     - `gpg --batch --detach-sign --armor "build/impossible-os-${VER}.iso"` → `.iso.asc`
     - `gpg --batch --detach-sign --armor "build/impossible-os-${VER}.img.zst"` → `.img.zst.asc`
     - GPG key from `GPG_SIGNING_KEY` GitHub Actions secret
  5. Print manifest of signed artifacts
- [ ] **`tools/codesign_host.c`**: host-side signing tool using the same Ed25519 COSI-trailer logic as `codesign_sign()` (→ XREF `TODO-07 §9`); compiled with `gcc -O2`; reads PE/ELF file, signs with `crypto_ed25519_sign` (monocypher linked as host library), appends 100-byte trailer; replaces input file in-place
- [ ] **Bootloader-side kernel signature verification** in `src/boot/uefi/bootx64.c`:
  - After loading `kernel.exe` into memory (existing step): check if last 4 bytes of ELF data == `0x434F5349` ("COSI" magic)
  - Read `HKLM\SYSTEM\SecureBoot\Enforce` (or scan UEFI variable `ImpossibleOSSecureBoot` if Registry not yet mounted): value 0 = log only, 1 = enforce (halt on bad signature)
  - If magic present: verify Ed25519 signature using embedded public key (`tools/pubkey.h` generated at build time from private key); log result
  - If `Enforce=1` and verification fails: `Print(L"FATAL: kernel signature invalid\r\n")` + `EFI_ABORTED` halt; no jump to kernel
  - If magic absent and `Enforce=0`: log warning + continue (dev mode)
  - Public key baked into `BOOTX64.EFI` at build time via `tools/bake_pubkey.sh` that generates `include/boot/codesign_pubkey.h`
- [ ] **`HKLM\SYSTEM\SecureBoot\Enforce`** Registry DWORD: 0 by default in all release images; set to 1 only in production/OEM builds; document in `docs/infrastructure/code-signing.md`
- [ ] Sign per-image `manifest.json` with release key; emit detached `manifest.json.sig` next to it on ESP/BlackBox under `/IPOS/`. Consumer: 01-boot-platform/TODO-06 §7 items 1-3.

---

## 6. Artifact Manifest `[Sonnet]`

**Source:** `scripts/make-manifest.sh`; output: `build/release-{version}.json`

- [ ] **`scripts/make-manifest.sh`**:
  - Read version from `build_info.h`; read git commit from `git rev-parse HEAD`
  - For each artifact in `build/impossible-os-${VER}.*`: compute `sha256sum`, `stat -c%s` for size, `stat -c%Y` for mtime
  - Emit `build/release-${VER}.json`:
    ```json
    {
      "version": "1.0.22000",
      "major": 1, "minor": 0, "build": 22000,
      "git_commit": "a1b2c3d4e5f6...",
      "build_time": "2026-03-26T12:00:00Z",
      "artifacts": [
        {
          "name": "impossible-os-1.0.22000.img.zst",
          "type": "disk_image",
          "size_bytes": 123456789,
          "sha256": "abc123...",
          "download_url": "https://github.com/rizonetech/impossible-os/releases/download/v1.0.22000/impossible-os-1.0.22000.img.zst"
        },
        { "name": "impossible-os-1.0.22000.iso", "type": "iso", ... },
        { "name": "impossible-os-1.0.22000.vmdk", "type": "vm_vmdk", ... },
        { "name": "release-1.0.22000.json", "type": "manifest", ... }
      ]
    }
    ```
  - `download_url` uses `https://github.com/rizonetech/impossible-os/releases/download/v{VER}/` prefix
- [ ] **Consumed by `TODO-03` update check**: `update_check()` fetches `https://sdk.impossible-os.dev/releases/latest.json` (which redirects to or mirrors the GitHub Release manifest); parses `version` field; compares to `HKLM\SYSTEM\Version`
- [ ] **Boot-platform schema XREF** -- run `bash scripts/release/build-manifest.sh build` per image; include each emitted `manifest.json` in `release-{version}.json` as `type: "boot_manifest"`. Schema owner: `D01 T06 §1`.
- [ ] **`make manifest`** target: runs `scripts/make-manifest.sh`; depends on `make release-image`, `make iso`, `make vm-images`

---

## 7. VM Image Variants `[Sonnet]`

**Source:** `scripts/make-vm-images.sh`

- [ ] **`scripts/make-vm-images.sh`**:
  ```bash
  VER="${MAJOR}.${MINOR}.${BUILD}"
  SRC="build/system-disk.img"

  # VirtualBox / VMware
  qemu-img convert -p -O vmdk "$SRC" "build/impossible-os-${VER}.vmdk"
  sha256sum "build/impossible-os-${VER}.vmdk" > "build/impossible-os-${VER}.vmdk.sha256"

  # Hyper-V legacy (VHD, fixed size, max 127 GB)
  qemu-img convert -p -O vpc "$SRC" "build/impossible-os-${VER}.vhd"
  sha256sum "build/impossible-os-${VER}.vhd" > "build/impossible-os-${VER}.vhd.sha256"

  # Hyper-V modern (VHDX)
  qemu-img convert -p -O vhdx "$SRC" "build/impossible-os-${VER}.vhdx"
  sha256sum "build/impossible-os-${VER}.vhdx" > "build/impossible-os-${VER}.vhdx.sha256"
  ```
- [ ] **`.ovf` descriptor** (`build/impossible-os-${VER}.ovf`) for VirtualBox import:
  ```xml
  <VirtualSystem ovf:id="ImpossibleOS">
    <Name>Impossible OS 1.0</Name>
    <VirtualHardwareSection>
      <Item><!-- 4 GB RAM --><rasd:VirtualQuantity>4096</rasd:VirtualQuantity></Item>
      <Item><!-- 2 vCPU --><rasd:VirtualQuantity>2</rasd:VirtualQuantity></Item>
    </VirtualHardwareSection>
    <!-- EFI firmware, Secure Boot disabled -->
    <vbox:Machine firmware="EFI" />
  </VirtualSystem>
  ```
  - Paired with `.vmdk` into `.ova` archive: `tar -cf "impossible-os-${VER}.ova" impossible-os-${VER}.ovf impossible-os-${VER}.vmdk`
- [ ] **`make vm-images`** target: calls `scripts/make-vm-images.sh`; depends on `make` build completing
- [ ] **Sizes**: VMDK typically 60–70% of raw image size due to sparse format; print final sizes after generation

---

## 8. Reproducible Builds `[Sonnet]`

**Source:** modifications to `scripts/build.sh` + `Makefile`; new `docs/infrastructure/reproducible-builds.md`

- [ ] **`SOURCE_DATE_EPOCH`**: in `scripts/build.sh`, set before any compilation:
  ```bash
  export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
  ```
  Pass to compiler: `CFLAGS += -DBUILD_TIMESTAMP=${SOURCE_DATE_EPOCH}` (already used in `build_info.h` for `BUILD_TIMESTAMP`); ensure `version_timestamp()` returns epoch in ISO-8601 from `SOURCE_DATE_EPOCH` instead of `__TIME__`/`__DATE__`
- [ ] **Strip non-deterministic inputs**:
  - Replace `__TIME__` / `__DATE__` uses in source with `BUILD_TIMESTAMP` macro
  - Pass `-fno-record-gcc-switches` equivalent for clang-19: none needed (already deterministic)
  - Ensure `llvm-ar-19` archives are deterministic: use `llvm-ar-19 rcsD` (`D` = deterministic mode, strips timestamps from archive)
  - `ld.lld-19` is deterministic by default; confirm `--build-id=none` or `--build-id=sha1` (consistent hash)
- [ ] **Release build strips test code**: add `RELEASE=1` omitting `-DKERNEL_TESTS` (today every artifact ships the full test suite + seams) plus an `nm` assertion that no `*_for_test` / `test_register_*` symbols remain
- [ ] **`make verify-reproducible`** target:
  ```bash
  bash scripts/build.sh clean
  cp build/system-disk.img /tmp/build1.img
  bash scripts/build.sh clean
  diff /tmp/build1.img build/system-disk.img && echo "REPRODUCIBLE: OK" || echo "REPRODUCIBLE: DIFFERS"
  sha256sum /tmp/build1.img build/system-disk.img
  ```
- [ ] **`docs/infrastructure/reproducible-builds.md`**:
  - What is reproducibility; why it matters for trust
  - How `SOURCE_DATE_EPOCH` is set from git commit timestamp
  - How to verify: `make verify-reproducible` instructions
  - Known non-reproducible elements (if any): document and track as issues
  - Reference: `https://reproducible-builds.org/`
- [ ] **Disk-image reproducibility XREF** -- `D01 T06 §2` already ships byte-identical `disk.img` via `build-image.sh`; this section's `make verify-reproducible` target invokes it twice and compares.

---

## OS Comparison


| ⭐ | Feature                                                 | 🪟 Win11                                                                | 🐧 Linux                                           | 🚀 Impossible OS                                                                            |
|----|---------------------------------------------------------|----------------------------------------------------------------------|-------------------------------------------------|------------------------------------------------------------------------------------------|
| 💎 | `MAJOR.MINOR.BUILD` versioning baked into OS + registry | ✅ `10.0.22000`; `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion` | ✅ `/etc/os-release`; kernel `uname -r`         | ⬜ §1 -- `OS_VERSION_STRING`; `winver.exe`; baked into `HKLM\SOFTWARE\Impossible\Version` |
| 💎 | Compressed disk image with SHA-256                      | ✅ Windows ISO (no zstd); WinGet                                     | ✅ `xz`/`zstd` compressed images (Fedora, Arch) | ⬜ §2 -- `zstd -T0 -9` + SHA-256                                                          |
| 💎 | Verified USB writer + cross-platform creator tool       | ✅ Rufus (3rd party); Windows Media                                  | ✅ `dd`; Etcher; Fedora Media Writer            | ⬜ §3 -- `make-usb.sh` with removable-only guard; `usb_creator.exe`                       |
| 💎 | Joliet+Rock Ridge ISO                                   | ✅ Windows ISO uses Joliet                                           | ✅ Most distros use `-R -J`                     | ⬜ §4 -- extends `TODO-11 §6`; versioned filename                                         |
| 💎 | Ed25519 code signing + bootloader verification          | ✅ Authenticode RSA; Secure Boot UEFI                                | ✅ GRUB + shim + kernel                         | ⬜ §5 -- `codesign_sign` on `kernel.exe`+`BOOTX64.EFI`; bootloader verify                 |
| ⭐ | Artifact manifest JSON                                  | ✅ Windows Update XML feeds (private)                                | ✅ Flatpak/Snap manifests; APT Packages         | ⬜ §6 -- `release-{ver}.json`; consumed by `TODO-03` update                               |
| 💎 | VM image variants                                       | ✅ Hyper-V VHD; VMware tools                                         | ✅ cloud images (QCOW2, VMDK, AMI)              | ⬜ §7 -- `qemu-img convert` to VMDK/VHD/VHDX; `.ovf`                                      |
| ⭐ | Byte-reproducible builds                                | ❌ Windows builds are not reproducible                               | ✅ Debian/NixOS reproducible builds             | ⬜ §8 -- `make verify-reproducible`; `llvm-ar rcsD`; documented                           |

Impossible OS's `⭐` advantages: the **artifact manifest JSON** closes the loop to the
on-OS update check (no separate update metadata infrastructure needed), **Ed25519 code
signing** in the bootloader is faster and uses a smaller key than Windows Authenticode
RSA, and **reproducible builds** from day one establish a trust baseline that most
operating systems never achieve.

---

## Verification

Run `bash scripts/build.sh` then each release step.

- [ ] **Versioning**: `winver.exe` on-OS shows `Impossible OS 1.0 (Build {N})`; `HKLM\SOFTWARE\Impossible\Version` has matching string; serial log shows version on boot
- [ ] **Release image**: `make release-image` → `build/impossible-os-1.0.*.img.zst` exists; `zstd -d *.img.zst -o /tmp/test.img && sha256sum -c *.sha256` passes; integrity check confirms `BOOTX64.EFI` and `kernel.exe` present
- [ ] **USB writer**: `scripts/make-usb.sh /dev/null` → aborts with "not a block device"; interactive test on a loop device confirms prompt + write + verify cycle
- [ ] **ISO**: `make iso` produces `build/impossible-os-1.0.*.iso`; `file *.iso` shows ISO 9660; `isoinfo -d -i *.iso` shows Joliet+Rock Ridge; boots in `qemu-system-x86_64 -cdrom *.iso -bios /usr/share/ovmf/OVMF.fd`
- [ ] **Code signing**: `scripts/sign-release.sh` (with test key): `BOOTX64.EFI` and `kernel.exe` have COSI trailer (last 4 bytes `49 53 4F 43`); `tools/codesign_host --verify kernel.exe` prints `Signature: VALID`; bootloader with `Enforce=0` boots signed kernel; with `Enforce=1` rejects tampered kernel (flip one byte → halt)
- [ ] **Manifest**: `make manifest` → `build/release-1.0.*.json` valid JSON; `jq .version *.json` returns version string; all artifact sha256 entries match `sha256sum` of respective files
- [ ] **VM images**: `make vm-images` → `.vmdk`, `.vhd`, `.vhdx`, `.ova` exist; `qemu-img info *.vmdk` shows correct virtual size; `.ova` unpacks to `.ovf` + `.vmdk`
- [ ] **Reproducible**: `make verify-reproducible` → `REPRODUCIBLE: OK`; `sha256sum` of both builds match
- [ ] Commit: `"release: versioning, release-image, USB writer, ISO Joliet+RR, code signing, manifest, VM images, reproducible builds"`
