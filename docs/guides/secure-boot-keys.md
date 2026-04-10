# Secure Boot Key Management

This guide covers the Impossible OS MOK (Machine Owner Key) pair used to sign
`BOOTX64.EFI` and the shim chain-load path for hardware Secure Boot compatibility.

---

## How the signing chain works

```
UEFI Firmware (has Microsoft DB or user-enrolled key)
  └── shimx64.efi         ← signed by: MOK.key (or Microsoft for production)
        └── grubx64.efi   ← signed by: MOK.key  (verified via VENDOR_CERT_FILE=MOK.cer)
              └── kernel.exe
```

The shim is built with `MOK.cer` embedded as `VENDOR_CERT_FILE`. This means
the shim will trust any EFI binary signed with the corresponding `MOK.key`
without requiring interactive MOK enrollment on every machine.

---

## Key files

| File | Format | Committed | Purpose |
|------|--------|-----------|---------|
| `keys/MOK.key` | PEM PKCS#8 | **Never** | Private key -- signs EFI binaries at build time |
| `keys/MOK.cer` | PEM X.509  | Yes | Public cert -- embedded in shim as `VENDOR_CERT_FILE` |
| `keys/MOK.der` | DER X.509  | Yes | Binary form of cert -- used for UEFI manual enrollment |

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
2. Rebuild the shim: `bash scripts/secure-boot/build-shim.sh`
3. Rebuild the OS: `bash scripts/build.sh`

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

On machines without the key pre-enrolled in the shim:

1. Boot the machine -- the shim shows the blue **MokManager** screen
2. Select **Enroll key from disk**
3. Navigate to `EFI\BOOT\MOK.der` on the EFI partition
4. Confirm enrollment and reboot

After enrollment the shim skips the MOK screen on future boots.

---

## Testing the Secure Boot chain

The normal system disk already carries the full shim chain -- every normal
QEMU or VirtualBox boot exercises it:

```
UEFI firmware (SB off)
  └── EFI\BOOT\BOOTX64.EFI  = shimx64.efi (shim, VENDOR_CERT = MOK.cer)
        └── EFI\BOOT\grubx64.efi  = our bootloader, signed with MOK.key
              └── kernel
```

If `grubx64.efi` is not signed with the correct `MOK.key`, the shim refuses
to load it and the OS will not boot -- so the chain is verified on every
normal run.

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
   - Update `shim/` submodule to the new tag
   - Rebuild: `make -C shim` (produces `shimx64.efi`)
   - Re-sign with MOK: `sbsign --key keys/MOK.key --cert keys/MOK.cer --output shimx64.efi.signed shimx64.efi`
   - Verify: `sbverify --cert keys/MOK.cer shimx64.efi.signed`
   - Update `.sbat` CSV section in shimx64.efi if the SBAT generation changed

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
