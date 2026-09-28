<!-- docs: covers=todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md sources=src/boot/uefi/bootx64.c,include/kernel/boot_info.h,src/boot/uefi/boot_info_mirror.h,src/kernel/drivers/xhci.c,src/kernel/drivers/xhci_ring.c reviewed=2026-09-28 order=20 -->
# USB Zero-Delay Handover

## What is it?

This is the pre-`ExitBootServices` path that lets the kernel inherit a running xHCI controller instead of halting, resetting and re-enumerating it from scratch. While UEFI firmware is still active, the bootloader allocates the controller's DMA structures (DCBAA, scratchpad buffers, command ring, event ring, ERST) in `EfiLoaderData` memory that survives `ExitBootServices`, takes ownership from the firmware's USB stack via USBLEGSUP, and starts the controller on those persistent structures. The kernel then keeps the bootloader's DCBAA and scratchpad buffers instead of resetting the controller and allocating its own.

The inherit path ships today, but it is a partial inherit: devices are still enumerated by the kernel, so the "zero-delay" result the roadmap is named for is not reached yet (see "What is not implemented yet?").

## How does it work?

The bootloader's `allocate_xhci_dma()` scans PCI for an xHCI controller (class `0x0C`, subclass `0x03`, prog-if `0x30`), decodes BAR0 (checking the BAR width bits before treating BAR1 as a 64-bit high half), reads the capability registers, and allocates one page each for the DCBAA, command ring, event ring and ERST below 4 GiB as `EfiLoaderData`. When `HCSPARAMS2` reports scratchpad buffers it also allocates the scratchpad pointer array and one page per buffer. Every physical address is recorded in `boot_info.usb_controller` (a `struct boot_usb_controller`), and each page is also listed so the kernel PMM marks it used and never hands it out again.

Once the DMA structures exist, `bl_usblegsup_handoff()` walks the xHCI extended capability list for USBLEGSUP (capability ID 1), requests OS ownership, and polls up to one second for the firmware to release the controller. The bootloader then halts and resets the controller, programs DCBAAP, CRCR, ERST, ERDP and CONFIG from its own structures, restarts it, and sets `usb_handover_complete = 1`. POST codes `0xB082`/`0xB083` bracket the DMA allocation stage and `0xB084`/`0xB085` the takeover stage.

This runs after the bootloader has finished reading the kernel and configuration through UEFI, because halting the firmware's USB stack makes the EFI USB and block I/O protocols unusable for the rest of the bootloader's run.

On the kernel side, `xhci_init_controller()` trusts the handover only when three things hold: the negotiated capability `BOOT_CAP_USB_HANDOVER`, `usb_handover_complete`, and `usb_controller.active`. It then adopts the bootloader's DCBAA and scratchpad array, halts the controller just long enough to resynchronize (not a full reset, so DCBAA content is preserved), reprograms CONFIG and DCBAAP, and calls `xhci_rings_init()`, which allocates a fresh command ring, event ring and ERST in place of the bootloader's, then restarts it. Only the DCBAA and scratchpads are actually inherited today. A halt timeout, a ring-init failure or a restart timeout each fall back to the ordinary halt, reset, allocate and enumerate path. On success the kernel still runs the Intel EHCI-to-xHCI port routing and its own port enumeration. POST code `0xD750` marks the handoff; `0xD751` follows a successful inherit, but on the ordinary path it is emitted before full init, so the `handover OK` or `ready` serial line, not the code, confirms the controller is running.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `allocate_xhci_dma()` | Bootloader: finds the xHCI controller, allocates DCBAA, rings and scratchpads in `EfiLoaderData`, records addresses in `boot_info` ([`bootx64.c`](../../src/boot/uefi/bootx64.c)) |
| `bl_usblegsup_handoff()` | Bootloader: requests USBLEGSUP OS ownership and waits up to one second for firmware release ([`bootx64.c`](../../src/boot/uefi/bootx64.c)) |
| `struct boot_usb_controller` | The `boot_info` ABI struct carrying PCI identity, cached capability registers and every DMA structure's physical address ([`boot_info.h`](../../include/kernel/boot_info.h)) |
| `usb_handover_complete` | Set to 1 when the bootloader configured and started the controller ([`boot_info.h`](../../include/kernel/boot_info.h)) |
| `BOOT_CAP_USB_HANDOVER` | Capability bit the kernel requires before trusting the handover fields ([`boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h)) |
| `xhci_init_controller()` inherit branch | Kernel: adopts the bootloader's DCBAA and scratchpads, allocates fresh rings, halt-syncs and restarts, or falls back to full init ([`xhci.c`](../../src/kernel/drivers/xhci.c), [`xhci_ring.c`](../../src/kernel/drivers/xhci_ring.c)) |

## How do I use it?

Handover runs automatically on any boot with an xHCI controller; there is no flag to enable it.

```bash
bash scripts/build.sh run-usb    # QEMU with a USB mass-storage device attached
```

On a successful handover, serial shows the bootloader lines `[BOOT] xHCI DMA: found at PCI ...`, `[BOOT] xHCI DMA: allocated N pages (DCBAA=0x...` and `[BOOT] xHCI takeover: BIOS released`, then on the kernel side `Handover: inheriting bootloader DMA (N pages, N scratchpads)` followed by `xHCI vX.Y handover OK -- N slots, N ports (DMA inherited)`.

If a handover step fails, the kernel logs `Handover halt timeout -- falling back`, `Handover ring init failed -- falling back` or `Handover restart failed -- falling back` and continues through the ordinary xHCI init path, so an absent handover, or one that fails at one of those three steps, does not block boot. A handover whose addresses are corrupt is not detected: the kernel programs DCBAAP from `boot_info` without validating it (see below).

## What is not implemented yet?

- The bootloader does not enumerate devices before `ExitBootServices`; slot contexts and endpoint rings are still built by the kernel after the inherit: [Bootloader Enumerates Devices with Persistent State](../../todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md#3-bootloader-enumerates-devices-with-persistent-state).
- `boot_usb_controller.max_scratchpads` carries the raw, unclamped `MaxScratchpadBufs` value, so a controller reporting more than `BOOT_USB_MAX_SCRATCHPADS` (16) exceeds the documented ABI surface: [Bootloader Allocates xHCI DMA Structures](../../todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md#1-bootloader-allocates-xhci-dma-structures).
- `bl_usblegsup_handoff()` treats a USBLEGSUP timeout as success rather than aborting the handover: [Bootloader Performs USBLEGSUP + Controller Takeover](../../todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md#2-bootloader-performs-usblegsup--controller-takeover).
- There is no per-device payload (slot ID, output context, EP0 and bulk ring addresses) in `boot_info`, and no typed handover payload descriptor; both wait on a DMA-shape ABI decision: [boot_info Passes Controller + Device DMA State](../../todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md#4-boot_info-passes-controller--device-dma-state).
- The kernel inherit still halts to resynchronize, allocates new rings instead of using the bootloader's, pays the Intel USB 2.0 port-routing wait and re-enumerates ports. It also does not check that the handover state belongs to the controller being initialized (PCI address or MMIO base) before programming DCBAAP from it: [Kernel Inherits Controller Without Halt/Reset](../../todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md#5-kernel-inherits-controller-without-haltreset).
- USB mass-storage devices always go through the full INQUIRY and READ CAPACITY path; nothing yet registers them from bootloader-captured geometry: [Kernel Registers MSC Devices from boot_info Geometry](../../todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md#6-kernel-registers-msc-devices-from-boot_info-geometry).
- There is no explicit post-handover validation stage (halted-bit check, a No-Op command round trip, a DCBAAP match) and no IOMMU mapping step for the inherited DMA pages: [Fallback: Detect Corrupt State, Revert to TODO-17 Path](../../todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md#7-fallback-detect-corrupt-state-revert-to-todo-17-path).
- No unit test covers the handover-active path, and the smoke test does not boot the `run-usb` configuration: [Unit Tests](../../todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md#unit-tests).

## How does it compare with Windows 11 and Linux?

Windows 11 does this by design: `winload.efi` configures the controller and `iusb3xhc.sys` inherits it, with persistent DMA and automatic fallback, none of it visible to the user. Linux always halts and re-enumerates USB controllers after boot and never inherits pre-boot DMA state. Impossible OS ships the persistent-DMA foundation (`EfiLoaderData` allocations surviving `ExitBootServices`, the USBLEGSUP handoff done by the bootloader) and a kernel-side inherit with fallback, which is already ahead of Linux. It is behind Windows because the inherit is partial: the kernel still re-enumerates devices and still pays the port-routing wait, so boot does not yet skip USB bringup the way Windows does.

## See also

- [USB Zero-Delay Handover roadmap](../../todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md)
- [USB Boot Storage (xHCI)](xhci-usb-boot.md)
- [USB Boot Hardening](usb-boot-hardening.md)
- [boot_info Field Ownership](boot-info-fields.md)
- [Boot Protocol ABI Handoff](boot-protocol-abi-overview.md)
