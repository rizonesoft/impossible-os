# shim/ -- Impossible OS Shim Binaries

Pre-built shim binaries from [rhboot/shim](https://github.com/rhboot/shim) v16.1.

These are built with `keys/MOK.cer` embedded as `VENDOR_CERT_FILE` so the shim
trusts our signed bootloader without requiring MOK enrollment on every machine.

## Files

| File | Role | On ESP |
|------|------|--------|
| `shimx64.efi` | First-stage UEFI loader, trusted by UEFI firmware | `EFI/BOOT/BOOTX64.EFI` |
| `mmx64.efi`   | MokManager -- first-boot key enrollment UI | `EFI/BOOT/mmx64.efi` |

## EFI Partition Layout (with shim)

```
\EFI\BOOT\
  ├── BOOTX64.EFI    ← shimx64.efi (firmware loads this first)
  ├── grubx64.efi    ← Our bootloader, signed with MOK.key
  └── mmx64.efi      ← MokManager (first-boot key enrollment)
```

## SHA256 Hashes

```
d7e21770b1c8f2b977db1d533f7bba3d0de3d212e83ffd35c2509de970d6bd2f  shimx64.efi
0141578fa3270f55afd0639a91f1d56edb1f5bec5be2462e8b2edf9918ec1248  mmx64.efi
```

## Rebuilding

If `keys/MOK.cer` is rotated or the shim version needs updating, rebuild with:

```bash
bash scripts/build-shim.sh
```

This will clone `rhboot/shim`, init the `gnu-efi` submodule, build with
`VENDOR_CERT_FILE=keys/MOK.cer`, and copy the binaries here.

## Shim Version

- **rhboot/shim v16.1**
- Built: 2026-03-14
- VENDOR_CERT_FILE: `keys/MOK.cer` (SHA-256: `D3:6B:BA:F0:...`)
- Status: Self-signed (pending Microsoft shim-review -- see TODO §3.4)
