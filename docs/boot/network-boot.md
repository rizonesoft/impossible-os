<!-- docs: covers=todo/01-boot-platform/TODO-25-network-pxe-http-boot.md sources=src/boot/uefi/bootx64.c,src/boot/uefi/efi.h,include/kernel/boot_info.h,src/kernel/main/boot_decision.c reviewed=2026-09-28 order=25 -->
# Network Boot (PXE and HTTP)

## What is it?

This is the bootloader's support for a machine that firmware launched over the network, through PXE or UEFI HTTP Boot. Today the bootloader detects that it was launched over the network, captures the DHCP and PXE details it was given, and proves that a TFTP or HTTP server actually serves `boot.conf`, all before `ExitBootServices`.

It is a discovery and transport-proof layer, not a working network boot path. Nothing fetched over the network is used: each probe downloads `boot.conf` and frees it immediately. The kernel receives no network provenance, `boot_device_type` 4 (network) is never set, and nothing verifies a signature or hash on fetched data. A PXE or HTTP Boot launch still ends with Impossible OS booting from local media.

## How does it work?

Early in `efi_main`, four probes run in order: `network_boot_discover()`, `net_dhcp_capture()`, `net_tftp_probe()` and `net_http_probe()`.

`network_boot_discover()` finds the Simple Network Protocol (SNP) and PXE Base Code handles, then walks the boot device path and the loaded image's file path for MAC, IPv4, IPv6 or URI nodes (`net_scan_device_path()`). A MAC or IP node marks the launch as a network boot; a URI node also marks it as HTTP Boot. This classifies the launch from the bootloader's own path, so a machine that merely has a network card is not mistaken for a network boot.

`net_dhcp_capture()` reads the firmware's PXE DHCP acknowledgement: client address, next-server address, gateway, DHCP server and boot filename. Gateway and server come from options 3 and 54, found with the bounds-checked scanner `net_dhcp_find_option()`. The raw packet is kept for forensics, but vendor option 43 and root-path option 17 are never printed to serial.

The two transport probes only run when discovery classified the launch as network (TFTP additionally needs valid DHCP data; HTTP needs the HTTP Boot flag). `net_tftp_download()` uses the firmware's MTFTP interface with a capped buffer and four retries with backoff. `net_http_download()` uses the UEFI HTTP protocol with a bounded wait, a capped body and up to four redirects (`net_http_resolve_redirect()`). Both probes then free what they fetched: there is deliberately no consumer until fetched data can be verified.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `network_boot_discover()`, `net_scan_device_path()` | Classify the launch as network or local from the device and file paths ([`bootx64.c`](../../src/boot/uefi/bootx64.c)) |
| `net_dhcp_capture()`, `net_dhcp_find_option()` | Read and redact the PXE DHCP acknowledgement ([`bootx64.c`](../../src/boot/uefi/bootx64.c)) |
| `net_resolve_boot_pxe()` | Finds the PXE Base Code instance for the boot network card ([`bootx64.c`](../../src/boot/uefi/bootx64.c)) |
| `net_tftp_download()`, `net_tftp_probe()` | Capped, retrying TFTP client; the probe fetches and frees `boot.conf` ([`bootx64.c`](../../src/boot/uefi/bootx64.c)) |
| `net_http_download()`, `net_http_probe()` | UEFI HTTP client with redirects; the probe fetches and frees `boot.conf` ([`bootx64.c`](../../src/boot/uefi/bootx64.c)) |
| `BOOT_ERR_TFTP_NOT_FOUND`, `_TIMEOUT`, `_OVER_CAP`, `_DEVICE` | Typed TFTP failure codes ([`efi.h`](../../src/boot/uefi/efi.h)) |
| `BOOT_PAYLOAD_NETWORK_CONFIG` | Reserved `boot_info` payload type, never emitted ([`boot_info.h`](../../include/kernel/boot_info.h)) |
| `BOOT_REASON_NETWORK_INSECURE` | Reserved boot-decision reason mapped to the network path; no live decision produces it ([`boot_decision.c`](../../src/kernel/main/boot_decision.c)) |

## How do I use it?

There is nothing to enable; the probes run on every boot and each one does nothing when its precondition is not met.

On a PXE launch, serial shows lines like these:

```
[NET] boot=network MAC=... link=up SNP=y PXE=y HTTPBoot=n
[NET] DHCP: client=... gw=... server=... file=...
[NET] TFTP: boot.conf reachable (N B); kernel staging owned by network-boot selection
```

On an HTTP Boot launch, the header shows `HTTPBoot=y` and an `[NET] HTTP: boot.conf reachable (...)` line appears instead. A failed probe logs `[NET] TFTP: boot.conf probe failed (err 0x...); staying local` or `[NET] HTTP: boot.conf probe failed; staying local`, and boot continues from local media. An ordinary local boot logs a single `[NET] no network boot path (local media)` line.

None of these lines change what boots.

## What is not implemented yet?

- DHCP capture misses one case: a child boot handle without its own PXE protocol, on firmware without `DevicePathUtilities`, leaves the launch unclassified: [DHCP/PXE Provenance Capture](../../todo/01-boot-platform/TODO-25-network-pxe-http-boot.md#2-dhcppxe-provenance-capture).
- There is no trust model for fetched assets (signed manifest, digests, anti-rollback, key revocation, TPM measurement); this is a reserved security-architecture decision that blocks everything below: [Network Boot Asset Integrity](../../todo/01-boot-platform/TODO-25-network-pxe-http-boot.md#5-network-boot-asset-integrity).
- `boot_info` carries no network provenance (MAC, addresses, server URI, protocol): [boot_info Network Provenance](../../todo/01-boot-platform/TODO-25-network-pxe-http-boot.md#6-boot_info-network-provenance).
- The network is not a fallback target after local media: [Fallback Ordering with Local Media](../../todo/01-boot-platform/TODO-25-network-pxe-http-boot.md#7-fallback-ordering-with-local-media).
- Recovery and installer images cannot boot over the network: [Recovery and Installer over Network](../../todo/01-boot-platform/TODO-25-network-pxe-http-boot.md#8-recovery-and-installer-over-network).
- There is no `X:\Diag\network-boot.json` report or boot-timeline entry: [Network Boot Diagnostics and BlackBox Report](../../todo/01-boot-platform/TODO-25-network-pxe-http-boot.md#9-network-boot-diagnostics-and-blackbox-report).
- There is no QEMU PXE or HTTP Boot test fixture, and the parsers have no kernel-testable pure helpers: [PXE/HTTP Boot Tests](../../todo/01-boot-platform/TODO-25-network-pxe-http-boot.md#10-pxehttp-boot-tests).

## How does it compare with Windows 11 and Linux?

Windows 11 (through Windows Deployment Services) and Linux (through PXELINUX, iPXE or GRUB's network support) both boot complete systems over the network, with signed images under Secure Boot. Impossible OS has the discovery, DHCP capture and bounded TFTP and HTTP clients, but no trust model, no kernel-visible provenance and no fallback integration, so it cannot boot over the network yet and is behind both.

## See also

- [Network / PXE / HTTP Boot roadmap](../../todo/01-boot-platform/TODO-25-network-pxe-http-boot.md)
- [Boot Device Discovery](boot-device-discovery.md)
- [Alternate Boot Protocols](alternate-boot-protocols.md)
