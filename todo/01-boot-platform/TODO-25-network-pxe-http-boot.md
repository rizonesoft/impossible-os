---
schema_version: 1
id: network-pxe-http-boot
domain: 01-boot-platform
status: active
title: "TODO-25 -- Network / PXE / HTTP Boot"
---

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
- -> XREF: `TODO-07-boot-entry-store-menu-policy.md §10` -- §7 fallback ordering is consumed as a `kind: network` entry

## Outcome

- UEFI PXE, HTTP Boot, TFTP, and DHCP provenance can load kernel/config assets.
- Network boot can participate in A/B rollback and recovery policy.
- Kernel receives MAC/IP/server URI/boot asset hashes.
- Failures produce visible diagnostics rather than falling through silently.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | UEFI SNP/PXE protocol discovery | TODO-05 §5 | [x] |
| 💎 | 2 | DHCP/PXE provenance capture | §1 | [/] |
| 💎 | 3 | TFTP kernel and boot.conf load | §1, §2 | [x] |
| 💎 | 4 | UEFI HTTP Boot load path | §1, §2 | [ ] |
| 💎 | 5 | Network boot asset integrity | §3, §4, TODO-13 | [ ] |
| 💎 | 6 | boot_info network provenance | TODO-01 §4, §12 | [ ] |
| 💎 | 7 | Fallback ordering with local media | TODO-05 §5, §6 | [ ] |
| ⭐ | 8 | Recovery and installer over network | §3-§7, TODO-22 | [ ] |
| 💎 | 9 | Network boot diagnostics and BlackBox report | §1-§8 | [ ] |
| 💎 | 10 | PXE/HTTP boot tests | §1-§9 | [ ] |

## 1. UEFI SNP/PXE Protocol Discovery

- [x] `network_boot_discover()` (`bootx64.c`) locates SNP + PXE Base Code handles via `gBS->LocateHandleBuffer(ByProtocol, ...)`; GUIDs + layout-exact `EFI_SIMPLE_NETWORK_PROTOCOL`/`EFI_SIMPLE_NETWORK_MODE` (offset-asserted) added to `efi.h`.
- [x] `net_scan_device_path()` walks BOTH the DeviceHandle device path AND `LoadedImage->FilePath` for MESSAGING MAC/IPv4/IPv6/URI nodes -> `booted_from_network` (Codex design D1: URI/MAC nodes can live in the file path).
- [x] HTTP Boot detection: a URI messaging node (`EFI_DP_MSG_URI` 0x18) sets `http_boot`.
- [x] `[NET]` serial log: boot source, MAC (boot-path MAC node or SNP `CurrentAddress`, capped 6 bytes), link state (SNP `MediaPresent`), SNP/PXE/HTTPBoot availability; local launch logs `[NET] no network boot path`, no error.
- [x] Commit: `"boot: discover UEFI network boot protocols"` (commit `6f6f474f`)

**Test checkpoint:** On a firmware network launch, serial logs the client MAC, link state, and which protocols are available (SNP / PXE Base Code / HTTP Boot device path); on a local-media launch the discovery is skipped with no error. Verify on QEMU WHPX (PXE), bare metal.

> **Test runner:** N/A (bootloader-only UEFI discovery, no kernel-side fields until §6) | validation: serial `[NET]` line on QEMU WHPX/OVMF PXE + bare metal

> **Notes:**
> - Shipped `network_boot_discover()` + `net_scan_device_path()` (`bootx64.c`, efi_main Step 1a pre-EBS) + SNP/PXE GUIDs, layout-exact `EFI_SIMPLE_NETWORK_PROTOCOL`/`MODE` (offset asserts), `EFI_DP_MSG_MAC/URI/DNS` in `efi.h`.
> - `booted_from_network` is authoritative from OUR boot path (DeviceHandle path + `LoadedImage->FilePath`), not SNP presence; result in file-static `g_net_discovery` seeding §2/§6.
> - Codex design review (2 HIGH adopted, evidence in commit): D1 walk FilePath too; D2 layout-exact SNP struct with offset asserts + HwAddressSize cap.
> - Scope: §1 is discovery + logging only; DHCP provenance §2, TFTP/HTTP load §3/§4, boot_info handoff + `boot_device_type=network` test §6.

> **Verified:** 2026-06-17 | commit `a9f39862` | 4/4 items | build OK | smoke PASS (KVM 2.8s)
> **Quality reviewed:** 2026-06-17 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2M fixed | scope: boot-code-quality

---

## 2. DHCP/PXE Provenance Capture

- [x] `net_dhcp_capture()` (`bootx64.c`) reads firmware PXE `Mode->DhcpAck`: client IP (yiaddr), next-server (siaddr), gateway (opt 3 / giaddr), DHCP server (opt 54), boot filename; MAC from §1 `g_net_discovery`.
- [x] Preserves the raw 1472-byte DhcpAck in `g_net_dhcp_raw` for diagnostics; the boot_info `BOOT_PAYLOAD_NETWORK_CONFIG` wiring is §6.
- [x] Redacted log: prints only IPs + boot filename; vendor option 43 + root-path option 17 stay in the raw copy, never logged.
- [x] Stages `g_net_dhcp` + raw packet for the §6 boot_info handoff; Registry `HKLM\SYSTEM\Boot\Network` output owned by §6, BlackBox `X:\Diag\network-boot.json` by §9.
- [ ] Best-effort residual: a child boot DeviceHandle without PXE on firmware lacking DevicePathUtilities skips the sole-handle fallback (`booted_from_network` unset); add a DevicePathUtilities-independent boot-NIC signal so capture is exact.
- [x] Commit: `"boot: capture PXE DHCP provenance"` (commit `e230bda1`)

**Test checkpoint:** Serial logs the captured DHCP/PXE provenance (MAC, assigned IP, gateway, DHCP + boot server, boot filename) with sensitive vendor options redacted; the raw packet blob is retained in the typed boot payload for diagnostics. Verify on QEMU WHPX (PXE).

> **Test runner:** N/A (bootloader-only UEFI capture, no kernel-side fields until §6) | validation: serial `[NET] DHCP:` line on a QEMU PXE boot; `test_pxe_dhcp_provenance_parse` owned by §10 harness

> **Notes:**
> - Shipped `net_dhcp_capture()` + `net_dhcp_find_option()` (`bootx64.c`, efi_main Step 1a) + layout-exact `EFI_PXE_BASE_CODE_MODE`/`DHCPV4_PACKET`/`PACKET` (offset asserts: DhcpAck@1524, Mode@104) in `efi.h`.
> - Binds to the boot NIC's PXE handle (boot-handle preferred, sole-handle fallback); reads `Mode->DhcpAckReceived`, parses BOOTP fixed fields + DHCP options 3/54 bounded against the 1472-byte packet; local boot / no DhcpAck is a clean no-op.
> - Parsed provenance in `g_net_dhcp`, raw packet in `g_net_dhcp_raw`; both seed §6 boot_info + §9 diagnostics.
> - Scope: §2 captures + stages in the bootloader; boot_info handoff §6, Registry §6, BlackBox JSON §9.

> **Verified:** 2026-06-17 | commit `7d32370a` | 4/5 items | build OK | smoke PASS (KVM 2.8s)
> **Deferred:** [M] child boot DeviceHandle without PXE + firmware lacking DevicePathUtilities skips the sole-handle fallback (best-effort capture) -> XREF: 01-boot-platform/TODO-25 §2 (item: "Best-effort residual" at line 75)
> **Quality reviewed:** 2026-06-17 | Codex 3x (adversarial, consistency, perf) | 1M fixed | scope: boot-code-quality

---

## 3. TFTP Kernel and boot.conf Load

- [x] `net_tftp_download()` (`bootx64.c`): bounded TFTP RRQ via firmware PXE `Mtftp` (GET_FILE_SIZE early refusal + READ_FILE capped); sized EfiLoaderData buffer; shared `net_resolve_boot_pxe()` boot-NIC resolver (D2, refactors §2).
- [x] `net_tftp_probe()` downloads `boot.conf` at Step 1a as an end-to-end client proof then FREES it; kernel.exe fetch/stage/retention is owned by §7 (no §3 consumer, so no untrusted 64 MiB is pinned pre-EBS; Codex adversarial).
- [x] Retry/backoff (`NET_TFTP_RETRIES`=4 + `gBS->Stall` on transient TIMEOUT/DEVICE_ERROR/NO_RESPONSE) + per-file caps; READ-side `EFI_BUFFER_TOO_SMALL` <= cap reallocs + retries (GET_SIZE/READ race), over-cap only when > cap.
- [x] `BOOT_ERR_TFTP_*` codes + `net_tftp_boot_err()` mapping + UEFI status constants in `efi.h`; `net_tftp_download` returns a TYPED status -- boot_fatal QR vs local fallback is §7 policy (a failure stays local, never a hard fatal here).
- [ ] Commit: `"boot: load kernel assets over TFTP"`

**Test checkpoint:** A QEMU TFTP server serves `kernel.exe` + `boot.conf`; serial shows a bounded RRQ download with retry/backoff and the per-file size cap enforced; a missing or oversized file produces a typed failure (`BOOT_ERR_TFTP_*`) and stays local, not a hang. Verify on QEMU TCG (TFTP), bare metal PXE.

> **Test runner:** N/A (bootloader-only UEFI TFTP client, no kernel-side fields until §6) | validation: serial `[NET] TFTP:` staged line on a QEMU PXE/TFTP boot; `test_tftp_retry_limits` owned by §10 harness

> **Notes:**
> - Shipped `net_tftp_download()` (bounded TFTP client) + `net_tftp_probe()` (boot.conf proof) + shared `net_resolve_boot_pxe()` (`bootx64.c`, efi_main Step 1a) + typed `Mtftp` / TFTP opcode / `MTFTP_INFO` + UEFI status codes + `BOOT_ERR_TFTP_*` in `efi.h`.
> - GET_FILE_SIZE early-refusal then READ_FILE with capped BufferSize + retry/backoff + realloc-on-grow; the probe frees boot.conf (no consumer yet); local boot / no boot-NIC PXE is a clean no-op staying local.
> - `net_tftp_download` returns a typed status (`net_tftp_boot_err()` -> `BOOT_ERR_TFTP_*`); boot_fatal-vs-fallback is §7, integrity §5, boot_info staging §6.
> - Scope: §3 is the TFTP client + boot.conf proof; kernel fetch/stage/retention + booting from the staged kernel is §7, asset verification §5.

---

## 4. UEFI HTTP Boot Load Path

- [ ] Use EFI_HTTP_PROTOCOL or firmware HTTP Boot device path to fetch assets.
- [ ] Support HTTPS only when firmware exposes TLS policy; otherwise require signature verification in §5.
- [ ] Add URI parsing and redirect limits.
- [ ] Persist final asset URLs in boot_info.
- [ ] Commit: `"boot: load kernel assets over UEFI HTTP"`

**Test checkpoint:** Firmware HTTP Boot (or `EFI_HTTP_PROTOCOL`) fetches the kernel + boot.conf; serial shows the parsed URI, an enforced redirect limit, and the final asset URLs persisted in `boot_info`. Verify on QEMU WHPX (OVMF HTTP Boot).

---

## 5. Network Boot Asset Integrity

- [ ] Require signed manifest for all network-booted assets.
- [ ] Verify kernel, boot.conf, initrd, and modules by digest.
- [ ] Bind manifest to Secure Boot/TPM state when available.
- [ ] Refuse unsigned network boot unless `network_boot_insecure=1` and physical console confirms.
- [ ] Manifest anti-rollback: monotonic version counter (NVRAM/TPM-NV backed) refuses a signed-but-older manifest than the last booted, so a valid stale manifest cannot be replayed.
- [ ] Key revocation + allowlist: SBAT-style revocation list + per-device key allowlist; refuse assets signed by a revoked or non-allowlisted key.
- [ ] Attestation chain: extend a TPM PCR with the manifest identity + asset digests; tie §6 boot_info + §9 `network-boot.json` to that identity (verifiable chain, not bare hashes); fail closed to local when firmware TLS/time is unavailable.
- [ ] Commit: `"boot: verify network boot assets"`

**Test checkpoint:** A network asset with a bad/absent manifest digest is REFUSED with a serial refusal line (boot halts to local fallback) unless `network_boot_insecure=1` AND the physical console confirms; a correctly signed manifest boots and binds to Secure Boot/TPM state when available. Verify on QEMU WHPX, bare metal.

---

## 6. boot_info Network Provenance

- [ ] Extend boot_info with network boot descriptor through TODO-01 §4 and the shared boot-path decision record through TODO-01 §12. Use `BOOT_PAYLOAD_NETWORK_CONFIG` from [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) for the TFTP/HTTP config blob pointer; the TODO-01 §4 validator rejects overlap with any other retained boot region before the kernel dereferences the MAC/IP/URI fields. -> XREF: [`01-boot-platform/TODO-01 §4`](TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array)
- [ ] Include MAC, IP, server URI, protocol, manifest digest, and insecure flag.
- [ ] Expose `HKLM\SYSTEM\Boot\Network`.
- [ ] Add `boot_device_type=network` test coverage.
- [ ] Network media-role: publish `boot_media_role=network` and parse vendor DHCP `media_role=NAME` option; slots as source #2 in the UKI > DHCP > role.txt precedence. Consumer: 01-boot-platform/TODO-06 §11.
- [ ] Test `test_media_role_network_dhcp_option` (mock DHCP option, pure data parser) wired under TEST_CAT_BOOT. Consumer: 01-boot-platform/TODO-06 §11.
- [ ] Commit: `"boot: hand off network boot provenance"`

**Test checkpoint:** After a network boot, `boot_info` carries the network descriptor (MAC, IP, server URI, protocol, manifest digest, insecure flag), `HKLM\SYSTEM\Boot\Network` is populated, `boot_device_type=network`, and `test_media_role_network_dhcp_option` passes. Verify on QEMU WHPX.

---

## 7. Fallback Ordering with Local Media

- [ ] Extend fallback chain policy: BootNext, boot device, A/B slot, recovery, network.
- [ ] Allow boot.conf to prefer local, removable, or network.
- [ ] Prevent network fallback loops after repeated download failure.
- [ ] Interlock with watchdog and rollback counters.
- [ ] Commit: `"boot: network fallback ordering"`

**Test checkpoint:** With local + network sources present, the documented order (BootNext, boot device, A/B slot, recovery, network) is honored and `boot.conf` can re-prefer local/removable/network; after repeated download failures the boot falls back to local media without a network retry loop, interlocked with the watchdog + rollback counters. Verify on QEMU WHPX.

---

## 8. Recovery and Installer over Network

- [ ] Allow recovery image download when local recovery partition is missing or corrupt.
- [ ] Add installer network boot mode: fetch assets over §3 TFTP/§4 HTTP (verified by §5), set `InstallerMode`; the same-kernel installer launches `installer.exe`. -> XREF: 10-platform-services/TODO-11-installer-iso.md §2
- [ ] No separate WinPE/initramfs or netroot mount: Impossible OS deliberately reuses the §3/§4 transports + the installed kernel (TODO-11 decision), not a WDS boot.wim ramdisk or dracut NFS/iSCSI/NBD root.
- [ ] Cache downloaded recovery assets to BlackBox when possible.
- [ ] Add progress UI for large downloads.
- [ ] Commit: `"recovery: network boot repair path"`

**Test checkpoint:** With the local recovery partition missing/corrupt, a network recovery image downloads and boots; `InstallerMode` network boot enters the installer; downloaded recovery assets are cached to BlackBox when present, with progress UI for large downloads. Verify on QEMU WHPX (network recovery).

---

## 9. Network Boot Diagnostics and BlackBox Report

- [ ] Write `X:\Diag\network-boot.json` with DHCP, protocol, timings, retries, file hashes.
- [ ] Include network path in VPD and boot timeline.
- [ ] Add QR code payload for network failures.
- [ ] Add host decoder support.
- [ ] Commit: `"boot: network boot diagnostics"`

**Test checkpoint:** Post-boot, `X:\Diag\network-boot.json` exists with DHCP, protocol, timings, retries, and per-file hashes; the network path appears in VPD + the boot timeline; a network failure emits a host-decodable QR payload. Verify on QEMU WHPX/TCG.

---

## 10. PXE/HTTP Boot Tests

- [ ] Add QEMU PXE/TFTP test harness.
- [ ] Add HTTP Boot mock server test.
- [ ] Test missing file, bad manifest, timeout, retry, and fallback.
- [ ] Test bare-metal firmware PXE where available.
- [ ] Commit: `"test: PXE and HTTP boot coverage"`

**Test checkpoint:** The QEMU PXE/TFTP harness + HTTP-Boot mock-server test pass; the missing-file, bad-manifest, timeout, retry, and fallback cases each assert the expected refusal/fallback behavior; bare-metal firmware PXE is exercised where available. Verify on QEMU WHPX/TCG, bare metal.

---

## OS Comparison

| ⭐  | Feature                       | 🪟 Win11                  | 🐧 Linux                   | 🚀 Impossible OS                   |
| --- | ----------------------------- | ------------------------- | -------------------------- | ---------------------------------- |
| 💎  | PXE boot                      | ✅ WDS/MDT                | ✅ PXELINUX/iPXE           | ✅ §1-§3 discover + DHCP + TFTP     |
| 💎  | HTTP boot                     | ✅ UEFI HTTP Boot         | ✅ iPXE/systemd-boot       | ⬜ Planned §4                      |
| 💎  | Signed network manifest       | ✅ Secure Boot policies   | ✅ shim/grub signatures    | ⬜ Planned §5                      |
| ⭐  | On-device network-boot report | ❌ event logs only        | ❌ external server logs     | ⭐ Planned §9 (X:\Diag JSON + QR)  |

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
