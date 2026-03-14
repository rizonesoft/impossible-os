# Impossible OS — shim-review submission
# Branch: rizonesoft-shim-x86_64-20260314

This is the shim-review submission for **Impossible OS**, a custom x86-64 operating
system built entirely from scratch (custom UEFI bootloader, 64-bit kernel, graphical
desktop — no Linux, no GRUB).

- **Bootloader repo:** https://github.com/rizonesoft/impossible-os-bootloader
- **Shim fork:** https://github.com/rizonesoft/impossible-os-shim

## Submission Checklist

- [x] completed README.md file with the necessary information
- [x] shim.efi to be signed (`shimx64.efi`)
- [x] public portion of certificate embedded in shim (`MOK.cer`, passed as `VENDOR_CERT_FILE`)
- [x] binaries for vendor_db — **N/A** (we do not use vendor_db)
- [x] any extra patches to shim — **none** (unmodified upstream rhboot/shim)
- [x] any extra patches to grub — **N/A** (we do not use GRUB)
- [x] build logs (`build.log`)
- [x] Dockerfile to reproduce the build (`Dockerfile`)

## What is the link to your tag in a repo cloned from rhboot/shim-review?

`https://github.com/rizonesoft/shim-review/tree/rizonesoft-shim-x86_64-20260314`

## What is the SHA256 hash of your final SHIM binary?

```
d7e21770b1c8f2b977db1d533f7bba3d0de3d212e83ffd35c2509de970d6bd2f  shimx64.efi
```

## What is the link to your previous shim review request (if any)?

N/A — this is our first submission.

## If no security contacts have changed since verification, what is the link?

N/A — first submission, no prior verified contacts.

---

## Organization

**Name:** Rizonesoft  
**Contact:** Derick Payne \<derick@rizonetech.com\>  
**Project:** Impossible OS  
**Bootloader:** https://github.com/rizonesoft/impossible-os-bootloader  
**Shim source:** https://github.com/rizonesoft/impossible-os-shim

## What is this used for?

Impossible OS is a custom, from-scratch x86-64 operating system targeting real
UEFI hardware with Secure Boot support. The shim is needed so our signed UEFI
bootloader (`grubx64.efi`) is trusted without requiring manual MOK enrollment
on every machine.

This is a **legitimate OS project** — not a tool to bypass Secure Boot restrictions.
Our bootloader loads our own kernel and nothing else.

## Shim Version

- **rhboot/shim v15.8** (upstream, unmodified)
- Architecture: x86_64
- Built: 2026-03-14
- `VENDOR_CERT_FILE`: `MOK.cer` (RSA-2048, CN=Impossible OS Secure Boot Key)
- No extra patches applied

## Certificate

`MOK.cer` is an RSA-2048 self-signed X.509 certificate:

```
Subject: CN=Impossible OS Secure Boot Key
Valid: 2026-03-14 → 2036-03-11
SHA256 fingerprint: D3:6B:BA:F0:FD:56:D8:5D:B9:F6:9E:3F:29:73:C4:51:47:7A:C3:B3:40:A4:AD:13:7E:8E:67:A1:69:BC:03:D7
```

## Reproducible Build

```bash
docker build -t impossible-os-shim .
docker run --rm impossible-os-shim sha256sum /output/shimx64.efi
# Expected: d7e21770b1c8f2b977db1d533f7bba3d0de3d212e83ffd35c2509de970d6bd2f
```

## EFI Partition Layout (on ESP)

```
EFI/BOOT/
  ├── BOOTX64.EFI    ← shimx64.efi (this submission)
  ├── grubx64.efi    ← Impossible OS bootloader (signed with MOK.key)
  └── mmx64.efi      ← MokManager (first-boot enrollment)
```
