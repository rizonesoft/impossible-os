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
| `keys/MOK.key` | PEM PKCS#8 | **Never** | Private key — signs EFI binaries at build time |
| `keys/MOK.cer` | PEM X.509  | Yes | Public cert — embedded in shim as `VENDOR_CERT_FILE` |
| `keys/MOK.der` | DER X.509  | Yes | Binary form of cert — used for UEFI manual enrollment |

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
1. Commit `keys/MOK.cer` and `keys/MOK.der` — never commit `keys/MOK.key`
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

1. Boot the machine — the shim shows the blue **MokManager** screen
2. Select **Enroll key from disk**
3. Navigate to `EFI\BOOT\MOK.der` on the EFI partition
4. Confirm enrollment and reboot

After enrollment the shim skips the MOK screen on future boots.

---

## Testing Secure Boot in QEMU

Use the dedicated Secure Boot test runner:

```
scripts\vm\run-qemu-kvm-secureboot.bat   (Windows/WHPX)
```

This uses `OVMF_CODE_4M.secboot.fd` + `OVMF_VARS_4M.snakeoil.fd` (test keys
pre-enrolled). The helper script `scripts/secure-boot/build-sb-test-disk.sh`
re-signs `shimx64.efi` with the OVMF snakeoil test key automatically so no
manual BIOS key enrollment is needed for the QEMU test run.

For the OVMF snakeoil key, see `/usr/share/ovmf/PkKek-1-snakeoil.pem` (passphrase: `snakeoil`).

---

## Submitting to Microsoft shim-review (long-term)

Once a release candidate is tagged, submit to [rhboot/shim-review](https://github.com/rhboot/shim-review).
This replaces the MOK enrollment popup with transparent Secure Boot on all hardware.
See `shim/README.md` for the current shim build details.
