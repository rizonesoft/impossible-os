# Secure Boot — Shim Chain-Loading

> **Strategy:** Impossible OS uses the [rhboot/shim](https://github.com/rhboot/shim)
> chain-loading approach. Our vendor certificate (`MOK.cer`) is embedded in the shim binary
> so the shim trusts our signed bootloader without requiring MOK enrollment on every machine.
> For now the shim is self-signed; once submitted to Microsoft for signing via
> [shim-review](https://github.com/rhboot/shim-review), it will be trusted system-wide
> with zero user interaction.

## Chain-Loading Flow

```
UEFI Firmware (Secure Boot enabled)
  → EFI/BOOT/BOOTX64.EFI        ← shimx64.efi  (Microsoft-trusted or self-signed shim)
    └── verifies EFI/BOOT/grubx64.efi against embedded MOK.cer
        → grubx64.efi            ← Our UEFI bootloader (signed with MOK.key)
            └── loads /boot/kernel.exe
                → kernel_main()  ← 64-bit kernel
```

### When Secure Boot Is Disabled

```
UEFI Firmware (Secure Boot disabled / dev mode)
  → EFI/BOOT/BOOTX64.EFI        ← shimx64.efi  (loads without firmware verification)
    → grubx64.efi                ← Our bootloader (signature not checked, but still present)
      → kernel_main()
```

## EFI System Partition Layout

```
\EFI\BOOT\
  ├── BOOTX64.EFI      ← shimx64.efi  (firmware loads this first via fallback path)
  ├── grubx64.efi      ← Our bootloader, signed with keys/MOK.key
  └── mmx64.efi        ← MokManager (first-boot key enrollment, if needed)
\boot\
  └── kernel.exe       ← Kernel ELF
```

This layout is produced automatically by the `system-disk` Makefile target.
When `shim/shimx64.efi` is present, the Makefile uses the shim chain-load path.
When absent, it falls back to placing our bootloader directly as `BOOTX64.EFI`.

## Shim Binaries (`shim/`)

| File | Role | ESP Path |
|------|------|----------|
| `shimx64.efi` | First-stage loader, trusted by UEFI firmware | `EFI/BOOT/BOOTX64.EFI` |
| `mmx64.efi`   | MokManager — first-boot key enrollment UI | `EFI/BOOT/mmx64.efi` |

**Version:** rhboot/shim v16.1  
**Built:** 2026-03-14  
**VENDOR_CERT_FILE:** `keys/MOK.cer`

SHA256 hashes:

```
d7e21770b1c8f2b977db1d533f7bba3d0de3d212e83ffd35c2509de970d6bd2f  shimx64.efi
0141578fa3270f55afd0639a91f1d56edb1f5bec5be2462e8b2edf9918ec1248  mmx64.efi
```

## Building the Shim from Source

If `keys/MOK.cer` is rotated or the shim version needs updating, rebuild with:

```bash
bash scripts/build-shim.sh
```

This script:
1. Clones `rhboot/shim` (shallow, latest)
2. Inits the `gnu-efi` submodule
3. Builds with `VENDOR_CERT_FILE=keys/MOK.cer ARCH=x86_64`
4. Copies `shimx64.efi` and `mmx64.efi` to `shim/`

### Prerequisites

```bash
sudo apt install gnu-efi libelf-dev libssl-dev pesign
```

### Manual Build

```bash
git clone --depth=1 https://github.com/rhboot/shim /tmp/shim-build
cd /tmp/shim-build
git submodule update --init gnu-efi
make VENDOR_CERT_FILE=/path/to/keys/MOK.cer ARCH=x86_64 shimx64.efi mmx64.efi
```

## MOK Key Pair

Our Machine Owner Key (MOK) was generated as follows:

```bash
openssl req -new -x509 -newkey rsa:2048 -keyout keys/MOK.key \
    -out keys/MOK.cer -days 3650 -subj "/CN=Impossible OS Secure Boot Key/"
openssl x509 -in keys/MOK.cer -out keys/MOK.der -outform DER
```

| File | Contents | Git status |
|------|----------|------------|
| `keys/MOK.key` | RSA-2048 private key | **Gitignored — never commit** |
| `keys/MOK.cer` | X.509 certificate (PEM) | Committed |
| `keys/MOK.der` | X.509 certificate (DER, for UEFI) | Committed |

**Fingerprint:** `D3:6B:BA:F0:FD:56:D8:5D:B9:F6:9E:3F:29:73:C4:51:47:7A:C3:B3:40:A4:AD:13:7E:8E:67:A1:69:BC:03:D7`  
**Valid:** 2026-03-14 → 2036-03-11

## Signing the Bootloader

Our UEFI bootloader (`build/tools/BOOTX64.EFI`) is signed with `MOK.key` using `sbsigntool`:

```bash
sbsign --key keys/MOK.key --cert keys/MOK.cer \
    --output build/tools/BOOTX64.EFI build/tools/BOOTX64.EFI

sbverify --cert keys/MOK.cer build/tools/BOOTX64.EFI
# → Signature verification OK
```

This is the `sign-efi` Makefile target, run automatically as step [4/5] in `build.sh`.
If `keys/MOK.key` is absent, signing is skipped and the build continues (dev builds unaffected).

## First-Boot MOK Enrollment (Interim Flow)

Until our shim is signed by Microsoft:

1. Boot → shim loads → checks `grubx64.efi` → matches `MOK.cer` → ✅ boots
2. If the user's firmware doesn't trust our self-signed shim:
   - Shim falls back to MokManager (`mmx64.efi`)
   - User enrolls our `MOK.der` once (physical presence required)
   - All future boots succeed automatically

## User Experience by Audience

| Audience | Secure Boot approach |
|----------|---------------------|
| Developers | Disable Secure Boot — zero friction |
| Tech-savvy (SB enabled) | Enroll MOK once — works forever |
| General users (target) | Microsoft-signed shim — pending approval (see TODO §3.4) |

## Microsoft Shim-Review Submission

See `todo/TODO-010-Bootloader.md` §3.4 for the submission checklist.
The shim fork must be public: `https://github.com/impossible-os/impossible-os-shim`

## Testing Secure Boot in QEMU

> [!NOTE]
> Full Secure Boot QEMU testing requires OVMF with enrolled PK/KEK and an interactive
> display session. This is not feasible in headless WSL 2 mode. Test on real hardware
> or a VirtualBox VM with UEFI Secure Boot enabled.

Standard build + boot test (shim path active, SB not enforced):

```bash
bash scripts/build.sh clean run
```

The build log will show:

```
[DISK] Shim found — using Secure Boot chain-load layout
```

confirming the EFI partition uses the shim chain-load path.
