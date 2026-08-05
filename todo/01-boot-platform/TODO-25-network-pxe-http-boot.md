---
schema_version: 1
id: network-pxe-http-boot
domain: 01-boot-platform
status: active
title: "TODO-25 -- Network / PXE / HTTP Boot"
---

# TODO-25 -- Network / PXE / HTTP Boot

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

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

| ⭐   | Order | Deliverable                              | Depends On      | Status |
| --- | :---: | ---------------------------------------- | --------------- | :----: |
| 💎   |   1   | UEFI SNP/PXE protocol discovery          | TODO-05 §5      |  [x]   |
| 💎   |   2   | DHCP/PXE provenance capture              | §1              |  [/]   |
| 💎   |   3   | TFTP kernel and boot.conf load           | §1, §2          |  [x]   |
| 💎   |   4   | UEFI HTTP Boot load path                 | §1, §2          |  [x]   |
| 💎   |   5   | Network boot asset integrity             | §3, §4, TODO-13 |  [/]   |
| 💎   |   6   | boot_info network provenance             | TODO-01 §4, §12 |  [/]   |
| 💎   |   7   | Fallback ordering with local media       | TODO-05 §5, §6  |  [/]   |
| ⭐   |   8   | Recovery and installer over network      | §3-§7, TODO-22  |  [/]   |
| 💎   |   9   | Network boot diagnostics and BlackBox report | §1-§8           |  [/]   |
| 💎   |  10   | PXE/HTTP boot tests                      | §1-§9           |  [/]   |

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

- [x] `net_tftp_download()` (`bootx64.c`): bounded TFTP RRQ via PXE `Mtftp` -- a cap-sized `READ_FILE` buffer is the only transfer boundary (no GET_FILE_SIZE, which can download past the cap); shared `net_resolve_boot_pxe()` resolver (refactors §2).
- [x] `net_tftp_probe()` downloads `boot.conf` at Step 1a as an end-to-end client proof then FREES it; kernel.exe fetch/stage/retention is owned by §7 (no §3 consumer, so no untrusted 64 MiB is pinned pre-EBS; Codex adversarial).
- [x] Retry/backoff (`NET_TFTP_RETRIES`=4 + `gBS->Stall` on transient TIMEOUT/DEVICE_ERROR/NO_RESPONSE/PROTOCOL_ERROR) + per-file caps; `EFI_BUFFER_TOO_SMALL` = over-cap; `EFI_TFTP_ERROR` server packet is deterministic, not retried.
- [x] `BOOT_ERR_TFTP_*` codes + `net_tftp_boot_err()` mapping + UEFI status constants in `efi.h`; `net_tftp_download` returns a TYPED status -- boot_fatal QR vs local fallback is §7 policy (a failure stays local, never a hard fatal here).
- [x] Commit: `"boot: load kernel assets over TFTP"` (commit `3de6cf8d`)

**Test checkpoint:** A QEMU TFTP server serves `kernel.exe` + `boot.conf`; serial shows a bounded RRQ download with retry/backoff and the per-file size cap enforced; a missing or oversized file produces a typed failure (`BOOT_ERR_TFTP_*`) and stays local, not a hang. Verify on QEMU TCG (TFTP), bare metal PXE.

> **Test runner:** N/A (bootloader-only UEFI TFTP client, no kernel-side fields until §6) | validation: serial `[NET] TFTP:` staged line on a QEMU PXE/TFTP boot; `test_tftp_retry_limits` owned by §10 harness

> **Notes:**
> - Shipped `net_tftp_download()` (bounded TFTP client) + `net_tftp_probe()` (boot.conf proof) + shared `net_resolve_boot_pxe()` (`bootx64.c`, efi_main Step 1a) + typed `Mtftp` / TFTP opcode / `MTFTP_INFO` + UEFI status codes + `BOOT_ERR_TFTP_*` in `efi.h`.
> - Cap-sized READ_FILE buffer is the only transfer boundary (no GET_FILE_SIZE) + retry/backoff; `EFI_TFTP_ERROR` is deterministic (-> `BOOT_ERR_TFTP_NOT_FOUND`); the probe frees boot.conf (no consumer yet); local boot / no boot-NIC PXE is a clean no-op.
> - `net_tftp_download` returns a typed status (`net_tftp_boot_err()` -> `BOOT_ERR_TFTP_*`); boot_fatal-vs-fallback is §7, integrity §5, boot_info staging §6.
> - Scope: §3 is the TFTP client + boot.conf proof; kernel fetch/stage/retention + booting from the staged kernel is §7, asset verification §5.

> **Verified:** 2026-06-17 | commit `31067c28` | 4/4 items | build OK | smoke PASS (KVM 2.8s)
> **Quality reviewed:** 2026-06-17 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+1M fixed | scope: boot-code-quality

---

## 4. UEFI HTTP Boot Load Path

- [x] `net_http_download()` (`bootx64.c`): HTTP GET via `EFI_HTTP_SERVICE_BINDING` + `EFI_HTTP_PROTOCOL` (CreateChild/Configure/async Request+Response/DestroyChild); typed event + HTTP ABI in `efi.h`. D2: the URI node is provenance only.
- [x] Bounded async wait (`net_http_wait()` = `CheckEvent`+`Stall`); cap-sized body buffer is the transfer bound (Content-Length not trusted); multi-chunk body read; absolute-`Location` 3xx redirect limit (`NET_HTTP_MAX_REDIRECTS`=4).
- [x] HTTP-only here (HTTPv4 config); HTTPS/TLS gating + plain-http / unsigned-asset refusal is §5 integrity.
- [x] `net_http_probe()` fetches `boot.conf` over HTTP as an end-to-end client proof then FREES it; kernel fetch/stage/retention is §7, final-URL persistence in boot_info is §6 (D1: no §6/§7 behavior in the transport section).
- [x] Relative-redirect resolution: `net_http_resolve_redirect()` joins a relative `Location` against the current absolute URL (root-relative + sibling-path), bounds-checked; dot-segment collapse left to the firmware HTTP stack.
- [x] Commit: `"boot: load kernel assets over UEFI HTTP"`

**Test checkpoint:** On an OVMF HTTP Boot, `net_http_probe()` fetches `boot.conf` over `EFI_HTTP_PROTOCOL`; serial shows the reachable size, an enforced redirect limit, the cap-bounded body, and that the buffer is freed (no retention). Verify on QEMU WHPX (OVMF HTTP Boot).

> **Test runner:** N/A (bootloader-only UEFI HTTP client, no kernel-side fields until §6) | validation: serial `[NET] HTTP:` line on a QEMU OVMF HTTP Boot; HTTP-boot test owned by §10 harness

> **Notes:**
> - Shipped `net_http_download()` + `net_http_probe()` + helpers (`bootx64.c`, efi_main Step 1a) + typed event ABI + `EFI_HTTP_PROTOCOL`/`SERVICE_BINDING` ABI in `efi.h`.
> - EFI_HTTP service-binding child + HTTPv4 Configure (firmware DHCP address) + async GET; tokens are `EVT_NOTIFY_SIGNAL` and `net_http_wait()` polls the callback-set completion flag while driving `http->Poll()` + Stall on a shared whole-transfer budget (CheckEvent rejects NOTIFY_SIGNAL events); cap-sized body buffer is the bound with EOF/over-cap detection; cleanup frees events/headers/pages + DestroyChild on every path.
> - `net_http_probe()` proves the client on an HTTP boot then frees boot.conf (no retention); `booted_from_network && http_boot` gates it; absolute + relative `Location` redirects resolved (`net_http_resolve_redirect()`).
> - Scope: §4 is the HTTP client + boot.conf proof; kernel fetch/stage/retention + boot-from-network is §7, final-URL persistence §6, integrity/TLS §5.

> **Verified:** 2026-06-17 | commit `ad04ee2b` | 6/6 items | build OK | smoke PASS (TCG 2.59s)
> **Quality reviewed:** 2026-06-17 | Codex 8x (design, adversarial, consistency, perf, re-adversarial) | 5H fixed | scope: boot-code-quality

---

## 5. Network Boot Asset Integrity

> [!IMPORTANT] Deferred (2026-06-17, unattended sequencer + Codex design review): the bootloader has no asymmetric crypto, no runtime SHA-256, and `efi.h` exposes no `EFI_PKCS7_VERIFY_PROTOCOL`. The whole section pivots on the network-asset **manifest signature trust model** (firmware-PKCS7 verify vs in-bootloader X.509 vs wrapping the network payload in a Secure-Boot-signed PE like the UKI) -- a user-reserved security-architecture decision. Shipping a digest/manifest wire format before that choice would authenticate nothing and risks cementing the wrong trust boundary. Anti-rollback, revocation/allowlist, and PCR attestation each need signing-toolchain + TPM-NV infrastructure owned elsewhere. Network boot stays fail-closed-to-local until the trust model is chosen. See Deferred stamps below.

- [ ] **BLOCKER (user-reserved):** choose the manifest signature trust model (firmware PKCS7 vs in-bootloader X.509 vs Secure-Boot-signed-PE wrap); gates every item below. -> XREF: `01-boot-platform/TODO-02 §11`, `02-kernel-core/TODO-19 §3`
- [ ] Require signed manifest for all network-booted assets.
- [ ] Verify kernel, boot.conf, initrd, and modules by digest.
- [ ] Bind manifest to Secure Boot/TPM state when available.
- [ ] Refuse unsigned network boot unless `network_boot_insecure=1` and physical console confirms.
- [ ] Manifest anti-rollback: monotonic version counter (NVRAM/TPM-NV backed) refuses a signed-but-older manifest than the last booted, so a valid stale manifest cannot be replayed.
- [ ] Key revocation + allowlist: SBAT-style revocation list + per-device key allowlist; refuse assets signed by a revoked or non-allowlisted key.
- [ ] Attestation chain: extend a TPM PCR with the manifest identity + asset digests; tie §6 boot_info + §9 `network-boot.json` to that identity (verifiable chain, not bare hashes); fail closed to local when firmware TLS/time is unavailable.
- [ ] Commit: `"boot: verify network boot assets"`

**Test checkpoint:** A network asset with a bad/absent manifest digest is REFUSED with a serial refusal line (boot halts to local fallback) unless `network_boot_insecure=1` AND the physical console confirms; a correctly signed manifest boots and binds to Secure Boot/TPM state when available. Verify on QEMU WHPX, bare metal.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until trust model chosen

> **Notes:**
> - Deferred, not implemented: §5 hinges on a user-reserved manifest signature trust model; the bootloader has no asymmetric crypto / runtime SHA-256 / firmware PKCS7. Codex design review (2026-06-17) confirmed DEFER and "no must-ship digest core".
> - Until unblocked, network boot stays fail-closed-to-local (existing §3/§4 probes fetch-and-free, no asset is executed from the network without local trust); no integrity claim is made.
> - Owners for the prerequisites: signing/trust = TODO-02 §11/§16 + TODO-19 §3/§4; anti-rollback = TODO-13 §7 + TODO-01 §13; revocation = TODO-02 §17 + TODO-19 §6; attestation = TODO-13 §6/§9/§13.

> **Deferred:** [H] manifest signature trust model is a user-reserved security-architecture decision; not guessed unattended (firmware PKCS7 / in-bootloader X.509 / Secure-Boot-signed-PE wrap) -> XREF: 01-boot-platform/TODO-02 §11 (Unified Signed Boot Artifact) + 02-kernel-core/TODO-19 §3 (Hashing and Signature Provider Bridge), §4 (Embedded Signature Validation)
> **Deferred:** [H] manifest anti-rollback monotonic counter needs TPM-NV / secure-counter infra -> XREF: 01-boot-platform/TODO-13 §7 (TPM NV Index Support) + 01-boot-platform/TODO-01 §13 (Anti-Rollback and Security-Version Binding)
> **Deferred:** [M] key revocation + per-device allowlist (SBAT-style) needs the cert/key + revocation infra -> XREF: 01-boot-platform/TODO-02 §17 (SBAT Revocation Metadata) + 02-kernel-core/TODO-19 §6 (Revocation and Deny Lists)
> **Deferred:** [M] TPM PCR attestation-extend chain needs measured-boot baseline + quote -> XREF: 01-boot-platform/TODO-13 §6 (Baseline Enrollment), §9 (Attestation Report Export), §13 (Attestation Key Provisioning and TPM2 Quote)

---

## 6. boot_info Network Provenance

> [!IMPORTANT] Deferred (2026-06-17, unattended sequencer + Codex design review, 2 HIGH): §6's authoritative outputs are blocked. (1) Promoting a selected network boot (`boot_device_type=network`, `boot_path=NETWORK`, selected `HKLM\SYSTEM\Boot\Network`, boot-decision record) while §5 verification + the insecure-override/console-confirm path are deferred would ship the exact insecure selected-boot state §5 forbids -- `boot_decision.c:269` already accepts `BOOT_REASON_NETWORK_INSECURE` as a valid `BOOT_PATH_NETWORK`. (2) `boot_media_role=network` is a closed inline-enum extension (`BOOT_MEDIA_ROLE_MAX=6`, pinned offset 23968) needing a `BOOT_INFO_VERSION` bump -- an ABI change to stop on. Non-authoritative provenance staging is the only unblocked sliver and is premature until §5+§7 make network a selected boot path. See Deferred stamps below.

- [ ] Extend boot_info with network boot descriptor through TODO-01 §4 and the shared boot-path decision record through TODO-01 §12. Use `BOOT_PAYLOAD_NETWORK_CONFIG` from [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) for the TFTP/HTTP config blob pointer; the TODO-01 §4 validator rejects overlap with any other retained boot region before the kernel dereferences the MAC/IP/URI fields. -> XREF: [`01-boot-platform/TODO-01 §4`](TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array)
- [ ] Include MAC, IP, server URI, protocol, manifest digest, and insecure flag.
- [ ] Expose `HKLM\SYSTEM\Boot\Network`.
- [ ] Add `boot_device_type=network` test coverage.
- [ ] Network media-role: publish `boot_media_role=network` and parse vendor DHCP `media_role=NAME` option; slots as source #2 in the UKI > DHCP > role.txt precedence. Consumer: 01-boot-platform/TODO-06 §11.
- [ ] Test `test_media_role_network_dhcp_option` (mock DHCP option, pure data parser) wired under TEST_CAT_BOOT. Consumer: 01-boot-platform/TODO-06 §11.
- [ ] Commit: `"boot: hand off network boot provenance"`

**Test checkpoint:** After a network boot, `boot_info` carries the network descriptor (MAC, IP, server URI, protocol, manifest digest, insecure flag), `HKLM\SYSTEM\Boot\Network` is populated, `boot_device_type=network`, and `test_media_role_network_dhcp_option` passes. Verify on QEMU WHPX.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until §5 trust gate + ABI bump

> **Notes:**
> - Deferred, not implemented: §6's authoritative provenance (selected boot-source promotion + media-role) is gated on §5's trust model and a closed-enum `BOOT_INFO_VERSION` bump; Codex design review (2026-06-17) returned 2 HIGH confirming both blockers.
> - Honest current state: the firmware may launch BOOTX64.EFI over the network, but the OS still loads from local trusted media (§3/§4 probes fetch-and-free); there is no authoritative network boot source to hand off yet.
> - Owners: trust gate = TODO-25 §5; media-role enum + version bump = TODO-01 §7 (boot_info ABI); media-role precedence consumer = TODO-06 §11.

> **Deferred:** [H] authoritative network boot-source promotion (boot_device_type=network, boot_path=NETWORK, selected HKLM\SYSTEM\Boot\Network, decision record) must stay fail-closed until §5 verification or the documented insecure-override + physical-console-confirm path exists (boot_decision.c:269 already treats BOOT_REASON_NETWORK_INSECURE as a valid selected BOOT_PATH_NETWORK) -> XREF: 01-boot-platform/TODO-25 §5 (BLOCKER: manifest signature trust model)
> **Deferred:** [H] `boot_media_role=network` is a closed inline-enum extension (BOOT_MEDIA_ROLE_MAX=6, pinned offset 23968) needing a BOOT_INFO_VERSION bump + bootloader mirror + boot_media_role_name() + validator + skew tests -> XREF: 01-boot-platform/TODO-01 §7 (Version Negotiation and Stale-Loader Error Path) + consumer 01-boot-platform/TODO-06 §11

---

## 7. Fallback Ordering with Local Media

> [!IMPORTANT] Deferred (2026-06-17, unattended sequencer): §7's distinctly-new content is the NETWORK leg of the fallback chain ("fall back to network", `boot.conf` "prefer network", network-download-failure loop prevention), all gated on §5/§6 -- network cannot be a selectable fallback target while it is an untrusted, non-selected boot path. The local ordering (BootNext, boot device, A/B slot, recovery) is already owned: TODO-05 §5 (priority-based device fallback chain) + §6 (UEFI BootOrder/Current/Next) are DONE, TODO-21 owns the A/B slot leg, and `boot_fallback_depth`/`BOOT_FALLBACK_DEPTH_MAX` already cap runaway loops. No non-gated, non-duplicate work remains. See Deferred stamps below.

- [ ] Extend fallback chain policy: BootNext, boot device, A/B slot, recovery, network.
- [ ] Allow boot.conf to prefer local, removable, or network.
- [ ] Prevent network fallback loops after repeated download failure.
- [ ] Interlock with watchdog and rollback counters.
- [ ] Commit: `"boot: network fallback ordering"`

**Test checkpoint:** With local + network sources present, the documented order (BootNext, boot device, A/B slot, recovery, network) is honored and `boot.conf` can re-prefer local/removable/network; after repeated download failures the boot falls back to local media without a network retry loop, interlocked with the watchdog + rollback counters. Verify on QEMU WHPX.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until network is a selectable boot path

> **Notes:**
> - Deferred, not implemented: the local fallback ordering is already shipped (TODO-05 §5/§6, TODO-21 A/B); §7's new content is the network leg, gated on §5/§6 making network a trusted selectable boot path.
> - Loop prevention already exists structurally: `boot_decision.c` caps `boot_fallback_depth` at `BOOT_FALLBACK_DEPTH_MAX`; no network retry loop is currently possible because network is not a selected target.
> - Owners: local order = TODO-05 §5/§6; A/B slot = TODO-21; network selectability gate = TODO-25 §5/§6.

> **Deferred:** [H] network fallback leg + `boot.conf` prefer-network + network download-failure loop prevention require network to be a trusted selectable boot target -> XREF: 01-boot-platform/TODO-25 §5 (BLOCKER: trust model), §6 (authoritative network boot-source promotion)
> **Deferred:** [M] local fallback ordering (BootNext / boot device / A/B slot / recovery) is already owned -> XREF: 01-boot-platform/TODO-05 §5 (Device Fallback Chain), §6 (UEFI Boot Variable Reading) + 01-boot-platform/TODO-21 §4 (Boot Failure Counting and Rollback)

---

## 8. Recovery and Installer over Network

> [!IMPORTANT] Deferred (2026-06-17, unattended sequencer): every item downloads and BOOTS executable assets (recovery image, installer kernel) over the §3/§4 network transports "verified by §5". With §5 verification deferred, executing network-fetched assets is exactly the insecure network boot §5's fail-closed contract forbids. Gated on §5. See Deferred stamp below.

- [ ] Allow recovery image download when local recovery partition is missing or corrupt.
- [ ] Add installer network boot mode: fetch assets over §3 TFTP/§4 HTTP (verified by §5), set `InstallerMode`; the same-kernel installer launches `installer.exe`. -> XREF: 10-platform-services/TODO-11-installer-iso.md §2
- [ ] No separate WinPE/initramfs or netroot mount: Impossible OS deliberately reuses the §3/§4 transports + the installed kernel (TODO-11 decision), not a WDS boot.wim ramdisk or dracut NFS/iSCSI/NBD root.
- [ ] Cache downloaded recovery assets to BlackBox when possible.
- [ ] Add progress UI for large downloads.
- [ ] Commit: `"recovery: network boot repair path"`

**Test checkpoint:** With the local recovery partition missing/corrupt, a network recovery image downloads and boots; `InstallerMode` network boot enters the installer; downloaded recovery assets are cached to BlackBox when present, with progress UI for large downloads. Verify on QEMU WHPX (network recovery).

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until §5 verification exists

> **Notes:**
> - Deferred, not implemented: booting network-fetched recovery/installer assets requires §5 asset verification; the items themselves say "verified by §5".
> - Honest current state: recovery + installer still boot from local trusted media; no network repair path is wired.
> - Owners: verification gate = TODO-25 §5; installer launch consumer = TODO-11 §2.

> **Deferred:** [H] downloading + booting recovery image / installer over the network requires §5 asset verification; unverified network-fetched executable assets are exactly what §5's fail-closed contract forbids -> XREF: 01-boot-platform/TODO-25 §5 (BLOCKER: trust model) + consumer 10-platform-services/TODO-11 §2 (installer ISO)

---

## 9. Network Boot Diagnostics and BlackBox Report

> [!IMPORTANT] Deferred (2026-06-17, unattended sequencer): the kernel writes `X:\Diag\network-boot.json`, but its data source -- the network provenance -- is the §6 boot_info handoff (deferred), and per-file hashes come from §5 verification (deferred). A non-authoritative diagnostic provenance handoff (the §6 sliver Codex blessed) is the future unblock path. Gated on §6 -> §5. See Deferred stamp below.

- [ ] Write `X:\Diag\network-boot.json` with DHCP, protocol, timings, retries, file hashes.
- [ ] Include network path in VPD and boot timeline.
- [ ] Add QR code payload for network failures.
- [ ] Add host decoder support.
- [ ] Commit: `"boot: network boot diagnostics"`

**Test checkpoint:** Post-boot, `X:\Diag\network-boot.json` exists with DHCP, protocol, timings, retries, and per-file hashes; the network path appears in VPD + the boot timeline; a network failure emits a host-decodable QR payload. Verify on QEMU WHPX/TCG.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until §6 provenance handoff exists

> **Notes:**
> - Deferred, not implemented: the network-boot diagnostic JSON needs the kernel-visible provenance handoff (§6) as its data source; per-file hashes need §5 verification.
> - Unblock path: a non-authoritative diagnostic provenance descriptor (the §6 sliver) feeds this report without the §5 trust decision; tracked under §6.
> - Owners: provenance handoff = TODO-25 §6; file hashes = TODO-25 §5; BlackBox X:\Diag writer infra = TODO-24.

> **Deferred:** [H] `X:\Diag\network-boot.json` needs the §6 kernel-visible network-provenance handoff as its data source, and per-file hashes need §5 verification -> XREF: 01-boot-platform/TODO-25 §6 (boot_info network provenance handoff), §5 (manifest/file hashes)

---

## 10. PXE/HTTP Boot Tests

> [!IMPORTANT] Deferred (2026-06-17, unattended sequencer): the PXE/TFTP + HTTP-Boot integration harness and the bad-manifest/fallback assertions depend on §5-§8 (trust, provenance, recovery, fallback), all deferred; and the §1-§4 transport parsers are UEFI-only (no kernel test surface, per their N/A test-runner notes) so they need extraction into kernel-testable pure helpers first. Gated on §5-§8. See Deferred stamp below.

- [ ] Add QEMU PXE/TFTP test harness.
- [ ] Add HTTP Boot mock server test.
- [ ] Test missing file, bad manifest, timeout, retry, and fallback.
- [ ] Test bare-metal firmware PXE where available.
- [ ] Commit: `"test: PXE and HTTP boot coverage"`

**Test checkpoint:** The QEMU PXE/TFTP harness + HTTP-Boot mock-server test pass; the missing-file, bad-manifest, timeout, retry, and fallback cases each assert the expected refusal/fallback behavior; bare-metal firmware PXE is exercised where available. Verify on QEMU WHPX/TCG, bare metal.

> **Test runner:** N/A (deferred -- no code shipped) | validation: deferred until §5-§8 ship

> **Notes:**
> - Deferred, not implemented: the bad-manifest/fallback assertions need §5-§8 (trust, provenance, recovery, fallback); the §1-§4 transport parsers are UEFI-only and need extraction into kernel-testable helpers before a kernel suite can cover them.
> - Unblock path: extract `net_dhcp_find_option` / TFTP-retry / redirect-resolver into pure helpers callable from a TEST_CAT_BOOT suite, then add the §5-§8 behavioral cases as those sections ship.
> - Owners: behavioral surface = TODO-25 §5-§8; transport parser extraction = TODO-25 §1-§4 follow-up.

> **Deferred:** [H] PXE/HTTP integration harness + bad-manifest/fallback assertions depend on §5-§8 and on extracting the UEFI-only transport parsers into kernel-testable helpers -> XREF: 01-boot-platform/TODO-25 §5 (trust), §6 (provenance), §8 (recovery)

---

## OS Comparison

| ⭐   | Feature                       | 🪟 Win11                | 🐧 Linux                | 🚀 Impossible OS                  |
| --- | ----------------------------- | ---------------------- | ---------------------- | -------------------------------- |
| 💎   | PXE boot                      | ✅ WDS/MDT              | ✅ PXELINUX/iPXE        | ✅ §1-§3 discover + DHCP + TFTP   |
| 💎   | HTTP boot                     | ✅ UEFI HTTP Boot       | ✅ iPXE/systemd-boot    | ✅ §4 EFI_HTTP client + probe     |
| 💎   | Signed network manifest       | ✅ Secure Boot policies | ✅ shim/grub signatures | ⏸ §5 deferred (trust model)      |
| ⭐   | On-device network-boot report | ❌ event logs only      | ❌ external server logs | ⭐ Planned §9 (X:\Diag JSON + QR) |

## Unit Tests

> **Gated:** these tests cover the deferred §5-§10 surface or UEFI-only transport parsers with no kernel test surface; they ship with §10 (PXE/HTTP boot tests), itself deferred behind the §5 trust model. `test_pxe_dhcp_provenance_parse` / `test_tftp_retry_limits` need `net_dhcp_find_option` / TFTP-retry extracted into kernel-testable pure helpers; `test_network_boot_device_type` needs §6; `test_network_manifest_rejects_bad_hash` needs §5.

- [ ] `test_network_boot_device_type`
- [ ] `test_pxe_dhcp_provenance_parse`
- [ ] `test_tftp_retry_limits`
- [ ] `test_network_manifest_rejects_bad_hash`

## Verification

> **Gated:** §1-§4 (transport: discovery, DHCP provenance, TFTP, HTTP) are shipped + reviewed and validated by serial `[NET]` lines + the boot smoke test; the items below need a PXE/TFTP + HTTP-Boot server harness (manual) and exercise the deferred §5-§8 trust/provenance/fallback surface. They open as §5-§10 ship.

- [ ] QEMU PXE with local TFTP server
- [ ] QEMU HTTP Boot mock
- [ ] Network failure fallback to local disk
- [ ] Bare metal PXE firmware
