<!-- docs: covers=todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md -->
# Secure Boot Key Management

This guide covers the Impossible OS MOK (Machine Owner Key) pair used to sign
`BOOTX64.EFI` and the shim chain-load path for hardware Secure Boot compatibility.

---

## How the signing chain works

```
UEFI Firmware (Microsoft UEFI CA in db -- factory default on most hardware)
  └── shimx64.efi (first stage -- see "No shim is pinned today" below)
        └── grubx64.efi   <- our bootloader, signed with MOK.key
              └── kernel.exe
```

> [!IMPORTANT]
> **No shim is pinned today, so the shipped image direct-boots.** `shim/shimx64.efi`
> and `shim/mmx64.efi` were removed from the tree in `aab6b6f64` (2026-07-01): the
> only Microsoft-signed shim available to us was signed by the **Microsoft UEFI CA
> 2011**, which expired **2026-06-30**, and `scripts/sign-efi.sh` hard-fails on it.
> With `shim/` empty the disk recipe stages `BOOTX64.EFI` directly and there is no
> shim chain at all -- everything below describing the chain applies once a shim is
> pinned again, not to the image a clean build produces right now. `bash
> scripts/test-secureboot-smoke.sh` reports this state explicitly (`shim chain NOT
> COVERED`); run it with `REQUIRE_SHIM=1` to make an uncovered chain a hard failure.

Two ways to get a shim back, and the smoke test must be told which one you used:

| Path | How | Firmware trust | Smoke invocation |
|---|---|---|---|
| **MOK-dev** (available now) | `bash scripts/secure-boot/build-shim.sh` builds our own shim from rhboot/shim with `keys/MOK.cer` as `VENDOR_CERT_FILE`, **then you sbsign it yourself** | Only after your signing key is enrolled in the firmware's db -- **not** out of the box | `SHIM_TRUST_MODE=mok-dev bash scripts/test-secureboot-smoke.sh` |
| **Stock Secure Boot** (production, vendor-gated) | a distro shim re-signed by Microsoft under the **UEFI CA 2023**, obtained through the shim-review process | Yes, out of the box | default (`SHIM_TRUST_MODE=ms-ca`) |

> [!WARNING]
> **`build-shim.sh` does not sign its output.** `VENDOR_CERT_FILE` embeds a
> certificate that the shim uses for its own MOK-list checks; that is not an
> Authenticode signature on the shim itself. Its raw `shimx64.efi`/`mmx64.efi`
> are UNSIGNED, so firmware with Secure Boot ON will refuse them and
> `SHIM_TRUST_MODE=mok-dev` reports the chain as NOT COVERED. To use this path
> with Secure Boot enabled you must `sbsign` both binaries with a key you have
> enrolled in db yourself. With Secure Boot OFF nothing needs signing -- and
> nothing is being secured either.

A self-built shim carries no Microsoft signature, so the default `ms-ca` mode
rejects it as "no recognized MS UEFI CA generation" -- that is the mode
disagreeing with the binary, not a broken shim. In `mok-dev` mode the shim is
verified against `keys/MOK.cer` instead. Either way the build pipeline itself
needs no code change: drop the binaries in `shim/`, refresh `shim/SHA256SUMS`,
and rebuild.

How `grubx64.efi` gets trusted depends on which shim is in play. A
**Microsoft-signed distro shim** carries no vendor certificate of ours, so it
consults the MOK list, which the user populates via MokManager on first boot
(see "Enrolling on real hardware" below); after enrollment the chain runs
without interaction on every subsequent boot. A **self-built shim** embeds
`keys/MOK.cer` as `VENDOR_CERT_FILE` and therefore trusts our loader with no
MokManager step -- that flow was retired in favour of the MS-signed binary in
2026-04, and became the only available option again when that binary was
unpinned. Both are described in the box above.

---

## Key files

| File | Format | Committed | Purpose |
|------|--------|-----------|---------|
| `keys/MOK.key` | PEM PKCS#8 | **Never** | Private key -- signs `grubx64.efi` (BOOTX64) at build time |
| `keys/MOK.cer` | PEM X.509  | Yes       | Public cert -- paired with MOK.key; sbverify reads this to confirm signatures |
| `keys/MOK.der` | DER X.509  | Yes       | Binary form of cert -- the same bytes the build copies to ESP root as `\MOK.cer` for MokManager enrollment |

---

## Generating a new key pair

```bash
# Generate RSA-2048 key + self-signed X.509 certificate (10-year validity)
openssl req -new -x509 -newkey rsa:2048 -keyout keys/MOK.key \
  -out keys/MOK.cer -days 3650 -nodes \
  -subj "/CN=Impossible OS Secure Boot Key/"

# Convert to DER for UEFI enrollment tools
openssl x509 -in keys/MOK.cer -out keys/MOK.der -outform DER
```

After generating a new key:
1. Commit `keys/MOK.cer` and `keys/MOK.der` -- never commit `keys/MOK.key`
2. Rebuild the OS: `bash scripts/build.sh` (signs `grubx64.efi` with the new
   key; the MS-signed shim is independent and is NOT rebuilt)
3. Re-enroll the new MOK on every machine via MokManager on next boot
   (the old MOK in the firmware MokList no longer matches signatures)

---

## Signing an EFI binary manually

```bash
# Sign (in-place)
sbsign --key keys/MOK.key --cert keys/MOK.cer \
       --output build/tools/BOOTX64.EFI build/tools/BOOTX64.EFI

# Verify
sbverify --cert keys/MOK.cer build/tools/BOOTX64.EFI
```

The `make sign-efi` target and `scripts/sign-efi.sh` do this automatically.
`bash scripts/build.sh` calls `make sign-efi` on every build.

---

## Enrolling on real hardware (one-time per machine)

On machines without the key pre-enrolled in the shim (every first boot):

1. Boot the machine -- the shim sees `grubx64.efi` is unsigned by any
   MOK in its list and launches MokManager (`mmx64.efi`) automatically.
2. Select **Enroll key from disk**.
3. Navigate to `\MOK.cer` at the **root of the EFI partition**. The file
   is DER-encoded despite the `.cer` suffix -- the build's
   `make disk` step writes it via `openssl x509 -outform DER` from
   `keys/MOK.cer`. (Earlier docs named `EFI\BOOT\MOK.der`; that path
   is not what the build produces -- enroll `\MOK.cer` at the root.)
4. Confirm the SHA1 fingerprint matches your local
   `openssl x509 -in keys/MOK.cer -fingerprint -sha1 -noout`.
5. Confirm enrollment and reboot.

After enrollment the shim skips the MOK screen on every subsequent
boot until the MOK list is cleared (firmware reset / `mokutil --reset`).

---

## Testing the Secure Boot chain

**With no shim pinned (the state today), there is no chain to test** -- the disk
recipe stages our own loader as `EFI\BOOT\BOOTX64.EFI` and boots it directly.
Confirm what a given build actually produced before assuming otherwise:

```
bash scripts/test-secureboot-smoke.sh                # reports COVERED / NOT COVERED
REQUIRE_SHIM=1 bash scripts/test-secureboot-smoke.sh # non-zero unless the chain is covered
```

Once a shim is pinned AND `keys/MOK.key` exists, the system disk carries the
full chain and every normal QEMU or VirtualBox boot exercises it:

```
UEFI firmware (SB off in QEMU/VBox; SB on in OVMF-secure / real hardware)
  └── EFI\BOOT\BOOTX64.EFI  = shimx64.efi (MS-signed, or MOK-dev self-built)
        └── EFI\BOOT\grubx64.efi  = our bootloader, signed with MOK.key
              └── kernel
```

A pinned shim WITHOUT `keys/MOK.key` does not produce that layout: the recipe
falls back to direct boot, because the shim would reject an unsigned
`grubx64.efi`. The smoke test reports that case as NOT COVERED rather than
treating the two files' presence as proof.

With Secure Boot OFF the chain runs without enforcement (every QEMU/VBox
default config). With Secure Boot ON, firmware must trust the shim's MS
signature AND the shim must have the MOK enrolled (first-boot MokManager
flow above) before it will hand off to `grubx64.efi`.

Use the standard runners:

```
scripts\machines\run-qemu-kvm.bat    # QEMU/KVM (Windows, fast)
scripts\machines\run-vbox.bat        # VirtualBox
```

---

## SBAT Bump and Shim Refresh Checklist

SBAT (Secure Boot Advanced Targeting) allows firmware and OS vendors to revoke
specific versions of boot components without revoking the entire signing
certificate. When a shim or GRUB vulnerability is disclosed:

1. **Watch for advisories:**
   - [rhboot/shim releases](https://github.com/rhboot/shim/releases) -- new shim versions bump the SBAT generation
   - [Microsoft Secure Boot program notices](https://uefi.org/revocationlistfile) -- dbx updates that revoke old shim hashes
   - UEFI Forum revocation list updates (published quarterly)

2. **When a new shim version is released:**
   - Pull the new `shim-signed` package from Ubuntu (or equivalent
     distro that ships a Microsoft-signed shim).
   - Replace `shim/shimx64.efi` and `shim/mmx64.efi` with the new
     binaries; update `shim/SHA256SUMS` to match.
   - Update `shim/README.md` with the new version + source.
   - Re-run `bash scripts/build.sh` and confirm the new shim produces
     a bootable system disk (no re-signing of the shim itself --
     it stays MS-signed).
   - We do NOT re-sign the shim; the chain trusts the MS signature
     out of the box.

3. **SBAT generation in our bootloader:**
   - The `.sbat` section in `bootx64.efi` declares our generation number
   - If Microsoft revokes a generation, all bootloaders at or below that number are blocked
   - Bump the generation in `src/boot/uefi/sbat.csv` when rebuilding after a revocation

4. **Never commit private keys:**
   - `keys/MOK.key` is in `.gitignore` -- verify before every push
   - Distribute `MOK.cer` (public) only; `MOK.key` stays on the build machine

5. **Testing after a shim refresh:**
   - Enroll the new MOK on test hardware (MokManager)
   - Verify boot with Secure Boot enabled on QEMU (OVMF + enrolled db) and bare metal
   - Check serial log for `[BOOT] ExitBootServices OK` (EBS retry covers map changes from dbx updates)

---

## Submitting to Microsoft shim-review (long-term)

Once a release candidate is tagged, submit to [rhboot/shim-review](https://github.com/rhboot/shim-review).
This replaces the MOK enrollment popup with transparent Secure Boot on all hardware.
See `shim/README.md` for the current shim build details.

## MS UEFI CA Lifecycle

Microsoft began rotating UEFI signing certificates in 2024-2025:

- **Microsoft Corporation UEFI CA 2011** -- the cert that signs every shim
  Microsoft has shipped historically. Scheduled to expire **2026-06-30**.
- **Microsoft Corporation UEFI CA 2023** -- the replacement cert, being
  enrolled into firmware DBs via Windows Update through 2025-2026.

Devices booting a shim signed only by the 2011 CA will start failing on
machines whose firmware's KEK/db has rotated to 2023-only after the
expiry window closes.

### Verifying which CA your shim uses

```bash
sbverify --list shim/shimx64.efi 2>&1 | grep -E 'Microsoft Corporation UEFI CA'
```

Look for a "Microsoft Corporation UEFI CA YYYY" subject in the certificate
chain. The newest year in the output is the active CA generation.

### Re-signing + redistributing

When Microsoft publishes the 2023-CA-signed shim binary:

1. Download the new `shimx64.efi` from the
   [rhboot/shim](https://github.com/rhboot/shim) release tagged for the
   2023 CA, or from your distribution's signed-shim package.
2. Replace `shim/shimx64.efi` in this repo.
3. Update `shim/SHA256SUMS` with the new digest.
4. Run `bash scripts/sign-efi.sh` and confirm the post-sign block emits
   `[shim] signed-by: Microsoft Corporation UEFI CA 2023`.
5. Run `bash scripts/build.sh clean` to rebuild the disk image with the
   new shim.

### Graduated deprecation policy in `scripts/sign-efi.sh`

| Today's date           | Behavior on shim signed by 2011 CA      |
|------------------------|------------------------------------------|
| pre-2026-05-01         | Silent accept (transition window not open) |
| 2026-05-01 to 2026-06-30 | `WARN` (60-day pre-expiry margin)      |
| post-2026-06-30        | `FAIL` -- signing pipeline aborts hard  |

The WARN threshold is `MS_UEFI_CA_2011_EXPIRY` minus 60 days. When
Microsoft publishes a 2023-signed shim binary, pin it in `shim/` per the
re-sign + redistribute steps above, well before the WARN window opens.

### Kernel-side audit value

`HKLM\SYSTEM\SecureBoot\ShimCA` (DWORD) surfaces the bundled shim's CA
generation at boot for audit tools (msinfo32-equivalent, security
inventory, support diagnostics):

| Value      | Meaning                                                  |
|------------|----------------------------------------------------------|
| `0`        | Shim binary not pinned (waiting on 2023-CA publication) |
| `2011`     | Shim signed by Microsoft Corporation UEFI CA 2011       |
| `2023`     | Shim signed by Microsoft Corporation UEFI CA 2023       |
| `0xFFFFFFFF` | Shim present but `sbverify` could not identify the CA |

## Unified Kernel Image (UKI)

Impossible OS produces two installable artifacts in parallel: the
classic split path (`BOOTX64.EFI` + `\\kernel.exe` + `\\boot.conf`)
and a Unified Kernel Image (`BOOTX64.UKI.efi`) per the
[UAPI Group UKI specification](https://uapi-group.org/specifications/specs/unified_kernel_image/).
The UKI bundles the bootloader stub, the kernel, the boot
configuration, and an `os-release` snippet into a single signable PE,
giving the firmware-verified Secure Boot signature whole-chain
coverage. Tampering with any embedded component invalidates the
single signature.

### Section layout

Deterministic ordering: `.osrel` -> `.cmdline` -> `.linux` -> `.initrd` -> `.recovery` -> `.modules`. Order is load-bearing for PCR measurement reproducibility (the measured-boot log consumes the section list in this order; reordering breaks attestation comparisons). Only the first three are mandatory; the last three are conditional on payload files existing in `build/uki-payloads/`.

The `.sbat` revocation-metadata section is NOT in this list: it is embedded into the `BOOTX64.EFI` stub at link time. `src/boot/uefi/sbat.asm` `incbin`s `src/boot/uefi/sbat.csv` into a `.sbat` ELF section, `uefi.lds` places it, and the ELF->PE objcopy carries it with `-j .sbat`; the UKI inherits the section from the stub. Link-time embedding is the only reliable path: post-hoc `objcopy --add-section` on the PE drops the content (GNU ELF->PE), corrupts the PE optional header so firmware rejects the image (GNU PE->PE), or produces a 0-byte section (llvm-objcopy). A build-time gate in `scripts/build.sh` validates `sbat.csv` structurally (exact header, one `impossibleos` row, every row 6 fields, positive generation) and byte-compares the embedded `.sbat` in both `BOOTX64.EFI` and `BOOTX64.UKI.efi` against the source, hard-failing on any mismatch. Bump the generation per the SBAT Bump checklist above.

| PE Section  | Source                                   | Purpose                                                       |
|-------------|------------------------------------------|---------------------------------------------------------------|
| (stub)      | `build/tools/BOOTX64.EFI`                | UEFI entry point; PE code + data, parses sections at boot      |
| `.sbat`     | `src/boot/uefi/sbat.csv` (in stub)       | SBAT revocation metadata; inherited from the stub, makes the artifact SBAT-revocable |
| `.osrel`    | `build/uki-osrel.txt` (auto-generated)   | NAME / ID / VERSION_ID / PRETTY_NAME (`os-release` form)        |
| `.cmdline`  | `resources/boot/boot.conf`               | `boot_config` key/value file (UKI-mode equivalent)             |
| `.linux`    | `build/kernel.exe`                       | The ELF kernel; consumed by `load_kernel()` directly           |
| `.initrd`   | `build/uki-payloads/initrd.img` (opt)    | Initial ramdisk; copied to `EfiLoaderData` for kernel consume   |
| `.recovery` | `build/uki-payloads/recovery.img` (opt)  | Recovery image; copied to `EfiLoaderData` for recovery loader   |
| `.modules`  | `build/uki-payloads/modules.cpio` (opt)  | CPIO of pinned kernel modules; copied to `EfiLoaderData`        |

Section virtual addresses are auto-placed by `objcopy` past the stub's
existing sections; the bootloader's `detect_uki_sections()` walks the
PE table by name so specific VAs do not need to match the UAPI Group
spec constants.

### Payload provenance contract

Anything in `build/uki-payloads/` is signed by the same `MOK_KEY` as the PE (the `sbsign` step covers the whole `BOOTX64.UKI.efi` including all embedded sections). **Out-of-tree payloads must not be staged in `build/uki-payloads/` -- staging is the trust boundary, and a misplaced file would silently get a signed-payload provenance claim it does not deserve.** Every payload that ends up in the directory must come from a tracked source (kernel-module build artifact, recovery-image generator, or a CI step that downloads + verifies an external initrd). Build pipelines that pull from external sources MUST verify the upstream signature / hash BEFORE staging into the directory.

### Cmdline rejection rule

When a UKI is invoked (`boot_info.flags & BOOT_FLAG_INVOKED_VIA_UKI`), the bootloader scans the active cmdline (which itself comes from the signed `.cmdline` PE section) for `initrd=` / `module=` / `recovery_image=` tokens. Token names match the keys consumed by `parse_conf_kv` in the bootloader's boot.conf parser exactly -- `module=` is singular, `recovery_image=` is the full key (not `modules=` or `recovery=`). If any of those tokens appear at a token-start position (start-of-buffer or preceded by whitespace), the boot is rejected via `boot_fatal(BOOT_ERR_UKI_DISK_OVERRIDE, ...)` BEFORE the kernel is invoked. Rationale: under UKI mode all load-bearing payloads come from the signed PE sections; a cmdline-driven disk path for these would defeat the whole-chain Secure Boot signature claim. The check is defense-in-depth -- the cmdline IS signed, so a hit means the build pipeline accidentally embedded an inconsistent cmdline. The split path (no UKI flag) accepts these tokens normally. The scanner is `uki_find_disk_override_token` in [`include/boot/uki_cmdline_check.h`](../../include/boot/uki_cmdline_check.h) (header-only, shared between bootloader + unit tests).

### Build pipeline

`scripts/build.sh` runs the UKI pack step between `EFI Boot` and
`EFI Signing`:

```
llvm-objcopy-19 \
    --add-section .osrel=build/uki-osrel.txt \
    --set-section-flags .osrel=alloc,readonly,data \
    --add-section .cmdline=build/uki-cmdline.txt \
    --set-section-flags .cmdline=alloc,readonly,data \
    --add-section .linux=build/kernel.exe \
    --set-section-flags .linux=alloc,readonly,data \
    [optional .initrd / .recovery / .modules when staged] \
    build/tools/BOOTX64.EFI build/tools/BOOTX64.UKI.efi
```

Optional payload sections are appended only when the corresponding file exists at `build/uki-payloads/{initrd.img,recovery.img,modules.cpio}`. Argument order pins on-disk section ordering (`llvm-objcopy --add-section` preserves it), so a missing payload does not shift the offsets of present ones.

`scripts/sign-efi.sh` then signs both artifacts in the same pass via
the shared `sign_one()` helper. The split path remains installable;
the UKI is the modern Secure Boot path used by direct-firmware-invoke
and by `shim + systemd-boot`-style chained loaders.

### Bootloader detection

At entry, `bootx64.c` calls `detect_uki_sections(loaded_image)` right
after the LoadedImage protocol is acquired. The walker validates the
DOS magic, e_lfanew bound, PE signature, COFF header, optional-header
size, section-table extent, and each section's virtual span before
storing the embedded pointers in `g_uki_kernel_ptr`,
`g_uki_cmdline_ptr`, `g_uki_osrel_ptr`, and (when present)
`g_uki_initrd_ptr`, `g_uki_recovery_ptr`, `g_uki_modules_ptr`.
After the PE walk, `uki_copy_payloads_to_loader_data()` allocates
`EfiLoaderData` pages via `gBS->AllocatePages` for each payload and
copies the section bytes out of LoadedImage memory into the new
range. The post-copy kernel-physical addresses get published into
`boot_info.uki_initrd_addr` / `_recovery_addr` / `_modules_addr` (v14
ABI) so they survive ExitBootServices. UEFI guarantees
`ImageBase` remains valid until ExitBootServices, so the scan is safe
in the pre-EBS window.

When `g_uki_kernel_ptr` is non-NULL, `load_kernel()` uses the embedded
buffer directly and skips the disk-load path entirely. The bootloader
sets `boot_info.flags |= BOOT_FLAG_INVOKED_VIA_UKI` so the kernel can
report whole-chain signature coverage in its attestation surface.

### PCR measurement order

The measured-boot log (TPM PCR replay) reconstructs the same set of
PCR values regardless of which artifact booted, because both paths
load the same kernel bytes. The UKI path measures the entire signed
PE in one go (matching firmware Secure Boot behavior); the split path
measures `BOOTX64.EFI` + `\\kernel.exe` + `\\boot.conf` as separate
events. Replay tools should canonicalize on the UKI shape when
verifying attestation reports from a UKI-booted machine.

### When to use which artifact

| Scenario                                       | Use         |
|-----------------------------------------------|-------------|
| Direct-firmware-invoke (UEFI 2.7+ Secure Boot)  | `BOOTX64.UKI.efi` |
| Confidential Computing (whole-chain signature) | `BOOTX64.UKI.efi` |
| Chained boot via `shim + systemd-boot`        | `BOOTX64.UKI.efi` |
| Legacy boot loaders expecting separate kernel | `BOOTX64.EFI` (split) |
| Development / fast iteration                  | Either      |

The split path stays in-tree as the legacy compatibility surface; the
UKI path is the modern preferred form whenever the firmware accepts
direct PE invocation.
