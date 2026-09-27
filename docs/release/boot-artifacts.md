<!-- docs: covers=todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md,todo/15-installer-release/TODO-01-release-artifacts.md sources=scripts/release/build-image.sh,scripts/release/to-vhdx.sh,scripts/release/build-iso.sh,scripts/deploy/write-usb.sh,tools/bootimg/bootimg.py,scripts/ci/boot-matrix.sh reviewed=2026-09-28 -->
# Boot Artifacts: Build, Verify, Write

> Owner: [Release Checklist and Documentation](../../todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md#10-release-checklist-and-documentation). Manifest schema: [boot-artifact-manifest.md](boot-artifact-manifest.md). CI matrix: [scripts/ci/boot-matrix.sh](../../scripts/ci/boot-matrix.sh).

This is the **operator handbook** for every Impossible OS release artifact: how to build it, what to expect on disk, how to verify it offline, how to write it to physical media, and where to look when boot fails. Each artifact format has its own subsection with a copy-pasteable recipe; the verification flows at the end work for all formats.

## Quick reference

| Format | Build command                                                                | Boot-test                                                               | Output path                  |
| ------ | ---------------------------------------------------------------------------- | ----------------------------------------------------------------------- | ---------------------------- |
| raw (release) | `bash scripts/release/build-image.sh`                                | offline only: `python3 tools/bootimg/bootimg.py inspect build/release/disk.img --manifest <path>` | `build/release/disk.img`     |
| raw (dev)     | `bash scripts/build.sh`                                              | matrix `raw` config in `scripts/ci/boot-matrix.sh`                      | `build/system-disk.img`      |
| usb           | `bash scripts/release/build-image.sh` (same artifact as raw release) | `sudo bash scripts/deploy/write-usb.sh` (then physical boot)            | `build/release/disk.img`     |
| vhd           | `qemu-img convert -f raw -O vpc build/release/disk.img build/release/disk.vhd` (manual) | manual: Hyper-V or `qemu-system-x86_64 -drive format=vpc`               | `build/release/disk.vhd`     |
| vhdx          | `bash scripts/release/to-vhdx.sh`                                    | `bash scripts/release/boot-test-vhdx.sh`                                | `build/release/disk.vhdx`    |
| vdi           | `bash scripts/release/to-vdi.sh`                                     | `bash scripts/release/boot-test-vbox.sh` (requires VBoxManage)          | `build/release/disk.vdi`     |
| iso           | `bash scripts/release/build-iso.sh`                                  | `bash scripts/release/boot-test-iso.sh`                                 | `build/release/disk.iso`     |
| ova           | (release-pipeline owner)                                             | (release-pipeline owner)                                                | `build/release/disk.ova`     |

> **`raw (release)` vs `raw (dev)`:** the boot matrix's `raw` config boots `build/system-disk.img` (what `bash scripts/build.sh` writes during dev iteration), NOT `build/release/disk.img`. They are NOT the same file: build-image.sh stages additional content (release manifest into `/IPOS/`, etc.) and uses deterministic-build settings the dev image does not. The release `disk.img` is validated offline via `bootimg inspect` plus a manual / VHDX-conversion boot in production.

> **Run order matters:** `bash scripts/build.sh` MUST succeed first (produces `build/tools/BOOTX64.EFI` + `build/kernel.exe`). The release scripts read those artifacts; running them against a stale build prints the binary-derived ABI manifest mismatch error.

> **End-to-end matrix:** `bash scripts/ci/boot-matrix.sh` runs every per-format boot test and emits a unified PASS/SKIP/FAIL summary plus per-config logs under `build/ci/<config>.log`.

## Per-format recipes

### Raw disk image (`build/release/disk.img`)

The reproducible reference artifact. Sector-aligned GPT with three partitions: ESP (FAT32, 64 MiB), BlackBox (FAT32, 128 MiB), IXFS-System (IXFS, 318 MiB). Total size 512 MiB.

```bash
bash scripts/build.sh                  # produces build/tools/BOOTX64.EFI + build/kernel.exe
bash scripts/release/build-image.sh    # default --format raw --role normal
ls -l build/release/disk.img           # 536870912 bytes (exactly 512 MiB)
sha256sum build/release/disk.img       # byte-reproducible: same SHA on identical inputs
```

Optional flags:

- `--out PATH` write to a non-default location.
- `--role NAME` stage `/IPOS/role.txt` with `NAME` (`normal` / `installer` / `live` / `recovery` / `manufacturing` / `diagnostics`); the bootloader reads this in the boot-media role-detection feature.
- `--manifest PATH` stage a release manifest at `/IPOS/manifest.json` on the ESP so the offline inspector can verify the image without a sidecar.

Test: `bash scripts/release/test-build-image.sh` (host-side, ~5 s).

### USB-bootable image (same artifact, different medium)

The raw image IS the USB image. The bootloader handles BlockIO regardless of medium type (NVMe / SATA / USB).

Linux / WSL (writes the release image when present, else the dev image):

```bash
sudo bash scripts/deploy/write-usb.sh         # interactive; lists removable drives only; double-confirms
```

The Linux writer prefers `build/release/disk.img` over `build/system-disk.img` when both exist; override via `DISK_IMG=path bash scripts/deploy/write-usb.sh`. It lists ONLY removable drives, refuses fixed/internal disks, and requires explicit confirmation before invoking `dd`.

Windows (writes the dev image; release-image support is open work):

```bat
scripts\deploy\write-usb.bat                  :: PowerShell wrapper; hardcodes build\system-disk.img
```

> **Caveat for the Windows writer:** it hardcodes `build\system-disk.img` and the safety filter is "non-system / non-boot / under 256 GB", not removable-only. For a release media write on Windows, copy `build\release\disk.img` over `build\system-disk.img` (or use a Linux/WSL host) until the writer gains explicit release-image support and stricter removable-only filtering. Tracked under release-pipeline tooling.

### VHD / VHDX (Hyper-V)

VHDX (preferred):

```bash
bash scripts/release/build-image.sh           # prerequisite: produces build/release/disk.img
bash scripts/release/to-vhdx.sh               # qemu-img convert -O vhdx -> build/release/disk.vhdx
bash scripts/release/boot-test-vhdx.sh        # boots in QEMU's vhdx driver, asserts userspace
```

VHD (legacy fixed-size, manual conversion -- no `to-vhd.sh` script):

```bash
bash scripts/release/build-image.sh                                       # prerequisite
qemu-img convert -f raw -O vpc build/release/disk.img build/release/disk.vhd
qemu-img info build/release/disk.vhd                                      # verify format=vpc + virtual size
```

### VDI (VirtualBox)

```bash
bash scripts/release/build-image.sh           # prerequisite: produces build/release/disk.img
bash scripts/release/to-vdi.sh                # qemu-img convert -O vdi -> build/release/disk.vdi
bash scripts/release/boot-test-vbox.sh        # requires VBoxManage on PATH; SKIPs cleanly otherwise
```

### Hybrid ISO (UEFI El Torito)

```bash
bash scripts/release/build-image.sh           # prerequisite: build-iso.sh extracts the ESP from this raw image
bash scripts/release/build-iso.sh             # xorriso hybrid GPT + El Torito UEFI entry
bash scripts/release/boot-test-iso.sh         # boots in QEMU; PASS = "Boot complete in"
```

ISO carries the ESP only; the kernel does not yet have an ISO9660 driver to mount IXFS as `C:\` from CD-ROM media (the ISO9660 mount work is a fs-domain item). Pass contract for ISO is the kernel-reaches-userspace marker only, not the shell prompt.

## Offline verification (`bootimg inspect`)

The offline artifact inspector parses any release artifact (raw / VHD / VHDX / VDI / ISO) without booting it and reports a structured PASS/FAIL.

```bash
python3 tools/bootimg/bootimg.py inspect build/release/disk.img
python3 tools/bootimg/bootimg.py inspect build/release/disk.iso
python3 tools/bootimg/bootimg.py inspect build/release/disk.vhdx
python3 tools/bootimg/bootimg.py inspect build/release/disk.vdi
```

Sidecar manifest (when the image was not built with `--manifest`):

```bash
bash scripts/release/build-manifest.sh build --out /tmp/manifest.json
python3 tools/bootimg/bootimg.py inspect build/release/disk.img --manifest /tmp/manifest.json
```

Exit codes: `0` clean, `2` hash mismatch, `3` signature FAIL (host Ed25519 not yet vendored, will surface here once it is), `4` manifest absent or malformed, `5` unsupported format. Pipe `--json` for machine output.

## Secure Boot setup (key enrollment + SBAT/dbx)

Today the kernel reads firmware Secure Boot state and surfaces it via `boot_info` v18 trust-landscape fields (`secure_boot_enabled`, `sbat_level`, `dbx_size`, `degraded_trust_flags`) and the `HKLM\SYSTEM\Boot\Trust\*` Registry. Cryptographic verification of the artifact manifest (`manifest.json.sig`) is blocked on host-side Ed25519 vendor work tracked in [Crypto Primitives](../../todo/09-desktop-shell/TODO-07-cng-crypto.md) and [Code Signing release pipeline](../../todo/15-installer-release/TODO-01-release-artifacts.md).

Operator workflow today:

1. **Read current trust posture** at boot via the kernel klog stream (`UEFI: trust-landscape degraded: 0xNN`) or post-boot via `reg query "HKLM\SYSTEM\Boot\Trust"`.
2. **Set up MS-shim-style chain** if the deployment target requires Secure Boot validation; the kernel reads `SbatLevel` (under `SHIM_LOCK_GUID`) and `dbx` (under `EFI_IMAGE_SECURITY_DATABASE_GUID`) and surfaces the values without modifying them.
3. **Enroll PK / KEK / db** through the firmware's setup-mode interface; the kernel surfaces enrollment state but does not enroll keys itself.

Once host Ed25519 lands, the manifest `.sig` flow will append: build-time signing (`scripts/release/sign-release.sh`), per-image staging (`build-image.sh --manifest path/with/.sig`), and runtime verification (`boot.conf` `require_manifest=1`).

## Release checklist (artifact PR / tag readiness)

Use this list when cutting a release. Every line maps either to an automated gate already wired or to a manual visual check the operator must perform.

- [ ] Clean build: `bash scripts/build.sh clean && bash scripts/build.sh` -> `=== BUILD OK ===`.
- [ ] All artifacts built: `bash scripts/release/build-image.sh && bash scripts/release/to-vhdx.sh && bash scripts/release/to-vdi.sh && bash scripts/release/build-iso.sh`.
- [ ] Per-format manifests present + valid (each `--format` produces its own `artifact_uuid` + per-format VM metadata where applicable; check-mode rejects malformed):
    ```bash
    for fmt in raw iso vhdx vdi; do
      bash scripts/release/build-manifest.sh build --format "$fmt" --out "build/artifacts/manifest-${fmt}.json"
      bash scripts/release/build-manifest.sh check "build/artifacts/manifest-${fmt}.json"
    done
    ```
- [ ] Per-artifact SHA-256 computed: `for f in build/release/disk.{img,vhdx,vdi,iso}; do sha256sum "$f"; done`.
- [ ] Boot matrix run: `bash scripts/ci/boot-matrix.sh` -> `BOOT MATRIX: PASS` with the configurations the host can run (raw, vhdx always; iso, vdi, whpx, usb-loop SKIP cleanly when their tool/host requirement is unmet).
- [ ] Per-config logs reviewed under `build/ci/<config>.log` (no unexpected `[FAIL]` or `[ERROR]` lines, even on PASS).
- [ ] Offline inspector clean against each release artifact (each artifact paired with its per-format manifest; expect exit 0 on all):
    ```bash
    python3 tools/bootimg/bootimg.py inspect build/release/disk.img  --manifest build/artifacts/manifest-raw.json
    python3 tools/bootimg/bootimg.py inspect build/release/disk.iso  --manifest build/artifacts/manifest-iso.json
    python3 tools/bootimg/bootimg.py inspect build/release/disk.vhdx --manifest build/artifacts/manifest-vhdx.json
    python3 tools/bootimg/bootimg.py inspect build/release/disk.vdi  --manifest build/artifacts/manifest-vdi.json
    ```
- [ ] Rollback test: previous-release `disk.img` still inspects clean against its own historical manifest; on a host with Secure Boot enabled, the kernel's degraded-trust klog surfaces the same posture as the current release.
- [ ] Release notes mention every blocker still tracked in TODO files (host Ed25519 in [crypto primitives](../../todo/09-desktop-shell/TODO-07-cng-crypto.md); Windows VHDX-WHPX runner in [CI Boot Matrix](../../todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md#9-ci-boot-matrix-for-every-artifact)).

The boot matrix configurations map 1:1 to release-checklist artifact-format coverage:

| Boot-matrix config | Artifact actually validated by the matrix | Release-image coverage path                                             |
| ------------------ | ----------------------------------------- | ----------------------------------------------------------------------- |
| `raw`              | `build/system-disk.img` (dev image)       | release `build/release/disk.img` is validated offline via `bootimg inspect` + the `vhdx` leg below (same content via `qemu-img convert`). Direct release-raw boot coverage is open work; see [CI Boot Matrix](../../todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md#9-ci-boot-matrix-for-every-artifact). |
| `iso`              | `build/release/disk.iso`                  | direct                                                                  |
| `vhdx`             | `build/release/disk.vhdx`                 | direct (sibling of release `disk.img`; produced by `to-vhdx.sh` from it) |
| `vdi`              | `build/release/disk.vdi`                  | direct (best-effort -- SKIPs when VBoxManage absent)                    |
| `whpx`             | `build/release/disk.vhdx` on Windows      | always SKIP today; pending Windows runner                               |
| `usb-loop`         | `build/system-disk.img` via loop device   | dev image again; release-image USB validation is the bare-metal manual gate at the bottom of this checklist. |

> **Coverage caveat:** the matrix `raw` and `usb-loop` legs both boot the dev image (`build/system-disk.img`), NOT the release image. The release image is exercised by the matrix only indirectly via the `vhdx` leg (which `to-vhdx.sh` derives from `build/release/disk.img`) and the `iso` / `vdi` legs (which derive from it via `build-iso.sh` / `to-vdi.sh`). Direct release-raw boot coverage in the matrix is tracked as open work in the same TODO section that owns the matrix.

## Troubleshooting

**Boot fails with "boot_info: bad header"**: bootloader and kernel built from different `BOOT_INFO_VERSION` values. Rebuild both via `bash scripts/build.sh`; never ship a mismatched BOOTX64.EFI + kernel.exe pair.

**OVMF "Image must be loaded at top of memory"** on QEMU: the OVMF firmware path is wrong or stale. The dev container uses `/usr/share/OVMF/OVMF_CODE_4M.fd`; the boot-test scripts pin this. If your host has a different layout, set `OVMF_CODE` / `OVMF_VARS_SRC` in the script's preflight section.

**`bootimg inspect` reports MISMATCH on bootloader.efi**: usually means the manifest was computed against the unsigned `build/tools/BOOTX64.EFI` source while the staged ESP carries the signed binary (signing appends an EFI signature trailer). Regenerate the manifest from the staged artifacts after signing, not before.

**Inspector reports `Signature: unverified`**: expected today. The host Ed25519 toolchain is not yet vendored; the inspector cannot validate `manifest.json.sig` on the host even when present. The bootloader-side validator path is similarly blocked. Tracked in [crypto primitives](../../todo/09-desktop-shell/TODO-07-cng-crypto.md) + [release-pipeline signing](../../todo/15-installer-release/TODO-01-release-artifacts.md).

**`losetup` fails on the USB-loop matrix config**: needs root (`sudo bash scripts/ci/boot-matrix.sh`). Without root the matrix SKIPs the leg cleanly; this is not a regression.

**Boot reaches `Boot complete in` but never shows `C:\>`**: IXFS partition not staged or not reachable. Check `build-image.sh` ran with `mkfs-ixfs --populate sysroot` and the IXFS partition's first sector contains the IXFS magic (uint32 LE 0x49584653 at offset 0). The offline inspector reports this directly: `fs=ixfs label='Impossible OS'` for a healthy partition.

**Hyper-V WHPX boot hang on Windows host**: the matrix's WHPX leg is currently an always-SKIP placeholder; manual WHPX validation goes through `qemu-system-x86_64 -accel whpx ...` directly. CLAUDE.md "Bare Metal Gotchas" lists the WHPX-specific quirks (PAT/MSR resets on CR3 reload, MSR read traps, INIT-de-assert IPI hang) that may need workarounds in your QEMU command line.

## See also

- [Boot Artifact Manifest Schema](boot-artifact-manifest.md) -- field-by-field manifest reference.
- [Boot Info Fields](../boot/boot-info-fields.md) -- `boot_info` v18 ABI including the trust-landscape surface.
- [Getting Started](../getting-started/index.md) -- first-time QEMU / VirtualBox setup.
- [scripts/ci/boot-matrix.sh](../../scripts/ci/boot-matrix.sh) -- CI orchestrator source.
- [tools/bootimg/bootimg.py](../../tools/bootimg/bootimg.py) -- offline inspector source.
