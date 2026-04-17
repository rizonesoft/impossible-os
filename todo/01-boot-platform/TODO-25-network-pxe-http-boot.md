# TODO-25 -- Network / PXE / HTTP Boot

> **Goal:** Boot from the network when local media is absent, intentionally bypassed, or used only as a thin bootstrap. The boot device enum already reserves `network`, and fallback-chain prose mentions network devices, but there is no PXE/HTTP/TFTP implementation. This TODO covers firmware-assisted network boot, bootloader network clients, kernel handoff of network provenance, and recovery integration.
> **Current state:** Local ESP/FAT boot is the only real bootloader path. The kernel has an RTL8139 driver and networking work elsewhere, but bootloader network protocols are not implemented. `boot_device_type = 4` exists as a placeholder.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`src/boot/uefi/efi.h`](../../src/boot/uefi/efi.h)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- -> XREF: `TODO-05-boot-device-discovery.md §5` -- fallback chain
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §4` -- network config payload descriptors
- -> XREF: `../07-networking/INDEX.md` -- post-boot network stack

## Outcome

- UEFI PXE, HTTP Boot, TFTP, and DHCP provenance can load kernel/config assets.
- Network boot can participate in A/B rollback and recovery policy.
- Kernel receives MAC/IP/server URI/boot asset hashes.
- Failures produce visible diagnostics rather than falling through silently.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | UEFI SNP/PXE protocol discovery | TODO-05 §5 | [ ] |
| 💎 | 2 | DHCP/PXE provenance capture | §1 | [ ] |
| 💎 | 3 | TFTP kernel and boot.conf load | §1, §2 | [ ] |
| 💎 | 4 | UEFI HTTP Boot load path | §1, §2 | [ ] |
| 💎 | 5 | Network boot asset integrity | §3, §4, TODO-13 | [ ] |
| 💎 | 6 | boot_info network provenance | TODO-01 §4 | [ ] |
| 💎 | 7 | Fallback ordering with local media | TODO-05 §5, §6 | [ ] |
| ⭐ | 8 | Recovery and installer over network | §3-§7, TODO-22 | [ ] |
| 💎 | 9 | Network boot diagnostics and BlackBox report | §1-§8 | [ ] |
| 💎 | 10 | PXE/HTTP boot tests | §1-§9 | [ ] |

## 1. UEFI SNP/PXE Protocol Discovery

- [ ] Locate EFI_SIMPLE_NETWORK_PROTOCOL and EFI_PXE_BASE_CODE_PROTOCOL handles.
- [ ] Match handles against LoadedImage device path when firmware launched us from network.
- [ ] Detect firmware HTTP Boot device paths.
- [ ] Add boot log for MAC, link state, and protocol availability.
- [ ] Commit: `"boot: discover UEFI network boot protocols"`

## 2. DHCP/PXE Provenance Capture

- [ ] Capture client MAC, assigned IP, gateway, DHCP server, boot server, boot filename, and vendor options.
- [ ] Preserve raw DHCP/PXE packets in a typed boot payload for diagnostics.
- [ ] Redact sensitive options in normal logs.
- [ ] Add Registry/BlackBox output after kernel handoff.
- [ ] Commit: `"boot: capture PXE DHCP provenance"`

## 3. TFTP Kernel and boot.conf Load

- [ ] Implement bounded TFTP RRQ client using firmware PXE Base Code where available.
- [ ] Support `\EFI\ImpossibleOS\kernel.exe`, `boot.conf`, and optional initrd/module downloads.
- [ ] Add retry/backoff and per-file size caps.
- [ ] Produce boot_fatal QR codes on TFTP failure.
- [ ] Commit: `"boot: load kernel assets over TFTP"`

## 4. UEFI HTTP Boot Load Path

- [ ] Use EFI_HTTP_PROTOCOL or firmware HTTP Boot device path to fetch assets.
- [ ] Support HTTPS only when firmware exposes TLS policy; otherwise require signature verification in §5.
- [ ] Add URI parsing and redirect limits.
- [ ] Persist final asset URLs in boot_info.
- [ ] Commit: `"boot: load kernel assets over UEFI HTTP"`

## 5. Network Boot Asset Integrity

- [ ] Require signed manifest for all network-booted assets.
- [ ] Verify kernel, boot.conf, initrd, and modules by digest.
- [ ] Bind manifest to Secure Boot/TPM state when available.
- [ ] Refuse unsigned network boot unless `network_boot_insecure=1` and physical console confirms.
- [ ] Commit: `"boot: verify network boot assets"`

## 6. boot_info Network Provenance

- [ ] Extend boot_info with network boot descriptor through TODO-01 version bump.
- [ ] Include MAC, IP, server URI, protocol, manifest digest, and insecure flag.
- [ ] Expose `HKLM\SYSTEM\Boot\Network`.
- [ ] Add `boot_device_type=network` test coverage.
- [ ] Commit: `"boot: hand off network boot provenance"`

## 7. Fallback Ordering with Local Media

- [ ] Extend fallback chain policy: BootNext, boot device, A/B slot, recovery, network.
- [ ] Allow boot.conf to prefer local, removable, or network.
- [ ] Prevent network fallback loops after repeated download failure.
- [ ] Interlock with watchdog and rollback counters.
- [ ] Commit: `"boot: network fallback ordering"`

## 8. Recovery and Installer over Network

- [ ] Allow recovery image download when local recovery partition is missing or corrupt.
- [ ] Add installer network boot mode with explicit `InstallerMode`.
- [ ] Cache downloaded recovery assets to BlackBox when possible.
- [ ] Add progress UI for large downloads.
- [ ] Commit: `"recovery: network boot repair path"`

## 9. Network Boot Diagnostics and BlackBox Report

- [ ] Write `X:\Diag\network-boot.json` with DHCP, protocol, timings, retries, file hashes.
- [ ] Include network path in VPD and boot timeline.
- [ ] Add QR code payload for network failures.
- [ ] Add host decoder support.
- [ ] Commit: `"boot: network boot diagnostics"`

## 10. PXE/HTTP Boot Tests

- [ ] Add QEMU PXE/TFTP test harness.
- [ ] Add HTTP Boot mock server test.
- [ ] Test missing file, bad manifest, timeout, retry, and fallback.
- [ ] Test bare-metal firmware PXE where available.
- [ ] Commit: `"test: PXE and HTTP boot coverage"`

## OS Comparison

| ⭐ | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | PXE boot | WDS/MDT | PXELINUX/iPXE | TODO-25 |
| 💎 | HTTP boot | UEFI HTTP Boot | iPXE/systemd-boot | TODO-25 §4 |
| 💎 | signed network manifest | Secure Boot policies | shim/grub signatures | TODO-25 §5 |
| ⭐ | BlackBox network boot report | event logs | external server logs | TODO-25 §9 |

## Unit Tests

- [ ] `test_network_boot_device_type`
- [ ] `test_pxe_dhcp_provenance_parse`
- [ ] `test_tftp_retry_limits`
- [ ] `test_network_manifest_rejects_bad_hash`

## Verification

- [ ] QEMU PXE with local TFTP server
- [ ] QEMU HTTP Boot mock
- [ ] Network failure fallback to local disk
- [ ] Bare metal PXE firmware

