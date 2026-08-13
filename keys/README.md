# keys/ -- Impossible OS Secure Boot Keys

> ⚠️ **NEVER commit `MOK.key` to any repository.** It is gitignored.
> If the private key is ever compromised, an attacker can sign malware
> that the Impossible OS shim will accept on any enrolled machine.

---

## Files

| File | Format | Purpose | Committed? |
|------|--------|---------|------------|
| `MOK.key` | PEM (PKCS#8) | RSA-2048 private key -- signs `BOOTX64.EFI` at build time | ❌ **Never** |
| `MOK.cer` | PEM (X.509) | Public certificate -- embedded as `VENDOR_CERT_FILE` when a shim is built (`scripts/secure-boot/build-shim.sh`); no shim is pinned today | ✅ Yes |
| `MOK.der` | DER (X.509) | Binary form of `MOK.cer` -- required for UEFI enrollment | ✅ Yes |

**Key details:**
- Algorithm: RSA-2048
- Validity: 10 years (2026-03-14 → 2036-03-11)
- CN: `Impossible OS Secure Boot Key`
- SHA-256 fingerprint: `D3:6B:BA:F0:FD:56:D8:5D:B9:F6:9E:3F:29:73:C4:51:47:7A:C3:B3:40:A4:AD:13:7E:8E:67:A1:69:BC:03:D7`

---

## How signing works

> [!IMPORTANT]
> **No shim is pinned today.** `shim/` was emptied in `aab6b6f64` (2026-07-01) because the Microsoft UEFI CA 2011 expired 2026-06-30, so a stock build stages our loader as `EFI\BOOT\BOOTX64.EFI` and **direct-boots -- there is no shim chain**. The flow below is what a pinned shim restores. Check any build with `bash scripts/test-secureboot-smoke.sh` (`REQUIRE_SHIM=1` makes an uncovered chain a hard failure); see [`shim/README.md`](../shim/README.md).

```
UEFI Firmware
  └─ shimx64.efi  (Microsoft-signed, contains MOK.cer as VENDOR_CERT_FILE)
       └─ verifies BOOTX64.EFI signature against MOK.cer
            └─ loads kernel → OS boots
```

1. At build time: `sbsign --key keys/MOK.key --cert keys/MOK.cer --output BOOTX64.signed.efi BOOTX64.EFI` (signed output goes to a distinct path so incremental builds stay idempotent)
2. At install time: the shim (containing `MOK.cer`) is placed on the ESP -- **skipped while `shim/` is empty**
3. On boot: shim verifies our `BOOTX64.EFI` against the embedded cert -- no user interaction needed

---

## Regenerating the key

If the private key is compromised or expires, regenerate with:

```bash
# Generate new key pair
openssl req -new -x509 -newkey rsa:2048 -keyout keys/MOK.key \
  -out keys/MOK.cer -days 3650 -nodes \
  -subj "/CN=Impossible OS Secure Boot Key/"

# Convert to DER for UEFI enrollment
openssl x509 -in keys/MOK.cer -out keys/MOK.der -outform DER
```

After rotating the key:
1. Update `MOK.cer` in the shim fork (`VENDOR_CERT_FILE`)
2. Rebuild and resubmit the shim via `rhboot/shim-review`
3. All enrolled machines will need to re-enroll the new MOK (or wait for new signed shim)
4. Update the SHA-256 fingerprint in this README

---

## Storage recommendations

- `MOK.key` -- store an encrypted backup in a password manager or hardware token (YubiKey)
- Never commit, never email, never share
- The build system loads it from `keys/MOK.key` at build time -- it must exist locally
