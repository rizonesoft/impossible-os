# Secure Boot Key Management

This guide covers the Impossible OS MOK (Machine Owner Key) pair used to sign
`BOOTX64.EFI` and the shim chain-load path for hardware Secure Boot compatibility.

---

## How the signing chain works

```
UEFI Firmware (Microsoft UEFI CA in db -- factory default on most hardware)
  └── shimx64.efi (Ubuntu shim-signed 1.58, Microsoft-signed)
        └── grubx64.efi   <- our bootloader, signed with MOK.key
              └── kernel.exe
```

The shipped shim is the **Ubuntu shim-signed 1.58 binary**, signed by
Microsoft. It is committed at `shim/shimx64.efi` (with SHA256 verified
against `shim/SHA256SUMS`) and is NOT rebuilt locally. Firmware trusts
this shim out of the box because the Microsoft UEFI CA is in nearly
every shipping firmware's Secure Boot db.

The shim does NOT carry an embedded vendor certificate -- the previous
self-built-shim flow that embedded `MOK.cer` as `VENDOR_CERT_FILE` was
retired when we switched to the MS-signed binary. To trust
`grubx64.efi`, the shim consults the MOK list, which the user populates
via MokManager on first boot (see "Enrolling on real hardware" below).
After enrollment the chain runs without interaction on every subsequent
boot.

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

The normal system disk already carries the full shim chain -- every normal
QEMU or VirtualBox boot exercises it:

```
UEFI firmware (SB off in QEMU/VBox; SB on in OVMF-secure / real hardware)
  └── EFI\BOOT\BOOTX64.EFI  = shimx64.efi (Ubuntu shim-signed 1.58, MS-signed)
        └── EFI\BOOT\grubx64.efi  = our bootloader, signed with MOK.key
              └── kernel
```

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
