# Microsoft Shim Review Submission

Impossible OS uses the [rhboot/shim-review](https://github.com/rhboot/shim-review)
process to get our shim (`shimx64.efi`) signed by Microsoft's UEFI CA. Once signed,
the shim is trusted by all UEFI firmware worldwide — no manual key enrollment needed.

## Status

| Step | Status |
|------|--------|
| Fork `rhboot/shim` → `rizonesoft/impossible-os-shim` | ✅ Done |
| Fork `rhboot/shim-review` → `rizonesoft/shim-review` | ✅ Done |
| Push submission branch `rizonesoft-shim-x86_64-20260314` | ✅ Done |
| Open issue on `rhboot/shim-review` | ✅ Done |
| Respond to reviewer questions | ⏳ Pending |
| Receive signed `shimx64.efi` from Microsoft | ⏳ Pending |
| Replace unsigned shim in `shim/shimx64.efi` and rebuild | ⏳ Pending |

## Submission Details

- **Issue title:** `rizonesoft: Impossible OS shim 16.1 submission (x86_64, 2026-03-14)`
- **Branch:** `https://github.com/rizonesoft/shim-review/tree/rizonesoft-shim-x86_64-20260314`
- **Shim version:** rhboot/shim v16.1 (unmodified upstream)
- **Architecture:** x86_64
- **Submitted:** 2026-03-14

### Files on the Submission Branch

| File | Purpose |
|------|---------|
| `shimx64.efi` | The binary to be signed |
| `MOK.cer` | Vendor certificate (`VENDOR_CERT_FILE`) |
| `Dockerfile` | Reproducible build instructions |
| `build.log` | Actual build output |
| `README.md` | Issue body / submission description |

### SHA256 of Submitted Binary

```
d7e21770b1c8f2b977db1d533f7bba3d0de3d212e83ffd35c2509de970d6bd2f  shimx64.efi
0141578fa3270f55afd0639a91f1d56edb1f5bec5be2462e8b2edf9918ec1248  mmx64.efi
```

## What Microsoft Reviews

Microsoft reviews **only the shim** — not the OS, kernel, or bootloader. They check:

- The shim source is unmodified upstream rhboot/shim
- `VENDOR_CERT_FILE` is a real X.509 cert belonging to the submitter
- The build is transparent and reasonably reproducible
- The use case is legitimate (real OS, not malware or SB bypass)

## Build Reproducibility Note

Shim builds embed host paths in DWARF debug info and are **not** bit-for-bit
reproducible across different host environments. The Dockerfile in the submission
branch demonstrates the exact build parameters used (`gcc 13`, `Ubuntu 24.04`,
`make VENDOR_CERT_FILE=vendor-cert/MOK.cer ARCH=x86_64`). The submitted binary
hash may differ from a fresh Docker build — this is expected and acknowledged in
the submission.

## Vendor Certificate

`keys/MOK.cer` — RSA-2048 self-signed X.509 certificate.

```
Subject: CN=Impossible OS Secure Boot Key
Valid:    2026-03-14 → 2036-03-11
SHA256 fingerprint: D3:6B:BA:F0:FD:56:D8:5D:B9:F6:9E:3F:29:73:C4:51:47:7A:C3:B3:40:A4:AD:13:7E:8E:67:A1:69:BC:03:D7
```

The **private key** (`keys/MOK.key`) is never committed. Only the public certificate
is embedded in the shim and committed publicly.

## UEFI CA Key Context

| Key | Expires | Used for |
|-----|---------|---------|
| Microsoft UEFI CA 2011 | ~June 2026 | Legacy shims only |
| Microsoft UEFI CA 2023 | ~2075 | **All new submissions, including ours** |

Submitting now means our shim will be signed with the 2023 CA — valid for decades.
Note: firmware older than ~2022 may not trust the 2023 CA by default; users on
such hardware should either update their firmware or disable Secure Boot.

## Timeline

The review process typically takes **2–4 weeks**. Reviewers are volunteers.
Responding to questions quickly improves chances of faster approval.

## After Receiving the Signed Binary

1. Download the Microsoft-signed `shimx64.efi` from the review issue
2. Replace `shim/shimx64.efi` with the signed version
3. Rebuild: `bash scripts/build.sh clean`
4. Verify `[DISK] Shim found — using Secure Boot chain-load layout` in build log
5. Commit: `"boot: replace with Microsoft-signed shimx64.efi"`

## Public vs Private

| Component | Public? | Why |
|-----------|---------|-----|
| `rizonesoft/impossible-os-shim` | ✅ Yes | Microsoft reviews this fork |
| `rizonesoft/shim-review` branch | ✅ Yes | Submission branch |
| `keys/MOK.cer` | ✅ Yes (committed) | Public certificate — safe to share |
| `keys/MOK.key` | ❌ Never | Private signing key |
| OS source / kernel | ❌ No | Irrelevant to Secure Boot review |
