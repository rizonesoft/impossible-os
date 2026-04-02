# TODO-09 — Zero-Delay USB Boot (Pre-ExitBootServices Driver Loading)

> **Goal:** Eliminate all post-ExitBootServices USB initialization latency by loading the xHCI driver's DMA structures inside the UEFI bootloader while firmware is still active. The bootloader allocates DCBAA, device contexts, and transfer rings in `EfiLoaderData` memory (survives ExitBootServices), performs USBLEGSUP handoff while firmware USB is running, and passes persistent controller state to the kernel. The kernel inherits the controller without halt/reset — USB devices are available instantly. This is how Windows `winload.efi` + `iusb3xhc.sys` achieves zero-delay USB boot.

> [!NOTE]
> **Non-blocking.** The current TODO-07 §1-§4 path (halt/reset/enumerate) works on all hardware. TODO-07 §5 Phase A/B add USBLEGSUP handoff and EHCI detection. This TODO is a pure performance optimization — it can be done at any time without breaking existing functionality.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) — UEFI bootloader (Phase A USB discovery already implemented)
- [`src/kernel/drivers/xhci.c`](../../src/kernel/drivers/xhci.c) — xHCI controller driver (USBLEGSUP handoff already implemented)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) — boot_info USB device array (already defined)
- [`include/kernel/drivers/xhci.h`](../../include/kernel/drivers/xhci.h) — xHCI controller and device context structures
- → XREF: `01-boot-platform/TODO-07-xhci-usb-boot.md §5` — current handover (Phase A/B done, Phase C pending)
- → XREF: `04-drivers-hardware/TODO-02-core-driver-enhancements.md §5` — MSI/MSI-X (hot-plug in TODO-07 §5C)
- → XREF: `04-drivers-hardware/TODO-09-usb-stack.md` — advanced USB features (builds on top of this handover)

## Outcome

- USB boot drive mounted as C:\ within 1ms of kernel start (zero halt/reset/enumerate delay)
- No 500ms Intel port routing delay (no XUSB2PR needed — firmware already routed)
- No SCSI INQUIRY/READ CAPACITY (geometry from bootloader via EFI_BLOCK_IO_PROTOCOL)
- Kernel inherits fully-configured xHCI controller with active device slots
- Fallback: if handover state is corrupt, kernel falls back to TODO-07 §1-§4 path automatically
- Works on QEMU `run-usb`, bare metal i5-4210U (EHCI+xHCI), bare metal i5-11600K (xHCI only)

## Implementation Order

| ⭐  | Order | Deliverable                                           | Depends On | Status |
| --- | :---: | ----------------------------------------------------- | ---------- | :----: |
| ⭐  |   1   | Bootloader allocates xHCI DMA structures              | —          |  [x]   |
| ⭐  |   2   | Bootloader performs USBLEGSUP + controller takeover   | §1         |  [x]   |
| ⭐  |   3   | Bootloader enumerates devices with persistent state   | §2         |  [-]   |
| ⭐  |   4   | boot_info passes controller + device DMA state        | §3         |  [ ]   |
| ⭐  |   5   | Kernel inherits controller without halt/reset         | §1, §2     |  [x]   |
| ⭐  |   6   | Kernel registers MSC devices from boot_info geometry  | §5         |  [ ]   |
| 💎  |   7   | Fallback: detect corrupt state, revert to §1-§4 path  | §5         |  [ ]   |

> All ⭐ rows — this is a competitive advantage over Linux (which always re-enumerates after kexec/boot). Windows does this via winload.efi but it's invisible to users. Making it visible in boot timing would be a first.

---

## 1. Bootloader Allocates xHCI DMA Structures
Allocate DCBAA, device output contexts, and transfer rings using `gBS->AllocatePages(EfiLoaderData)` so the memory survives ExitBootServices. The kernel PMM must be aware these pages are in use.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

- [x] Allocate DCBAA: 1 page `EfiLoaderData`, zeroed (fits max_slots+1 entries)
- [x] Allocate scratchpad buffers (array + individual pages, if `HCSPARAMS2.MaxScratchpadBufs > 0`)
- [ ] Allocate device output context per enumerated device — deferred to §3 (enumeration)
- [ ] Allocate EP0 transfer ring per device — deferred to §3 (enumeration)
- [ ] Allocate bulk-IN/OUT transfer rings for MSC devices — deferred to §3 (enumeration)
- [x] Record all physical addresses in `boot_info.usb_controller` struct
- [x] Mark allocated pages in `dma_pages[]` array; kernel PMM calls `pmm_mark_region_used()` for each
- [x] PCI config space access added to bootloader (`bl_pci_read8/16/32` via 0xCF8/0xCFC)
- [x] xHCI capability registers read (HCSPARAMS1/2, HCCPARAMS1, DBOFF, RTSOFF)
- [x] Commit: `"boot: allocate persistent xHCI DMA structures in EfiLoaderData"`

**Test checkpoint:** Serial shows allocated DMA addresses. POST code 0xB082. Verify `EfiLoaderData` pages survive ExitBootServices by reading back from kernel. Test on: QEMU `run-usb`, bare metal.

**Regression risk:** Allocating extra `EfiLoaderData` pages reduces available RAM. If allocation fails (low memory), fall back to not allocating and let kernel use TODO-07 path. Rollback: skip bootloader DMA allocation entirely.

## 2. Bootloader Performs USBLEGSUP + Controller Takeover
While firmware USB stack is still active, take xHCI ownership via USBLEGSUP and configure the controller to use our persistent DMA structures.

**Files:** `src/boot/uefi/bootx64.c`

- [x] Read xHCI BAR0 from PCI config space — reuses §1 `allocate_xhci_dma()` discovery
- [x] MMIO identity-mapped by firmware — direct volatile pointer access
- [x] USBLEGSUP handoff (walk extended caps for ID=1, set OS Owned, wait 1s)
- [x] Halt controller (USBCMD.RS=0, poll HCH=1, 16ms timeout)
- [x] Reset controller (HCRST=1, poll HCRST=0 AND CNR=0) — clean state for our DMA
- [x] Write DCBAAP, CRCR (with Link TRB + cycle=1), ERST, ERDP, ERSTBA, CONFIG
- [x] Start controller (USBCMD.RS=1 + INTE=1, IMAN.IE=1)
- [x] `usb_handover_complete` set to 1 on success
- [x] Commit: `"boot: USBLEGSUP handoff + controller takeover in bootloader"`

> [!IMPORTANT]
> After halting the firmware's USB stack, `EFI_USB_IO_PROTOCOL` and `EFI_BLOCK_IO_PROTOCOL` are no longer usable. Phase A device discovery must complete BEFORE this step. Order: Phase A (discover via EFI) → §2 (takeover) → §3 (enumerate with our state).

**Test checkpoint:** Controller running with bootloader-allocated DMA. POST code 0xB083. If halt/takeover fails, log error and skip (kernel falls back to TODO-07). Test on: QEMU `run-usb`, bare metal.

**Regression risk:** HIGH — halting the firmware USB stack may break EFI services that depend on USB (e.g., EFI console on USB keyboard). Must happen late in bootloader, after kernel load and config table copy. Rollback: if takeover fails, don't modify controller and let kernel handle it.

## 3. Bootloader Enumerates Devices with Persistent State
Enumerate connected devices in the bootloader so slot contexts and endpoint rings are in persistent memory. This is the true Windows-style zero-delay — devices have pre-configured slots when the kernel inherits.

> [!NOTE]
> **Deferred.** Requires porting the full 9-step USB enumeration sequence (~350 lines of xhci_dev.c) into the bootloader. The pragmatic approach is to share the code (compile xhci_dev.c for both bootloader and kernel contexts) rather than duplicate it. For now, §5 handles kernel inheritance of DMA structures and the kernel runs its own enumeration on the persistent DCBAA/rings — still saves all allocation time + Intel 500ms.

**Files:** `src/boot/uefi/bootx64.c`

- [ ] For each port with CCS=1: port reset, Enable Slot, Address Device
- [ ] GET_DESCRIPTOR (device + configuration) — store in persistent buffers
- [ ] SET_CONFIGURATION — device is now configured
- [ ] For MSC: Configure Endpoint (bulk-IN/OUT) using persistent transfer rings
- [ ] Record slot_id, port, speed, VID:PID, endpoint addresses in boot_info
- [ ] Commit: `"boot: enumerate USB devices with persistent DMA state"`

**Test checkpoint:** Serial shows enumerated devices with slot IDs. POST code 0xB086. Device descriptors match Phase A discovery. Test on: QEMU `run-usb`, bare metal i5-4210U, bare metal i5-11600K.

**Regression risk:** MEDIUM — USB enumeration is complex (9-step sequence). If any step fails, the device is skipped and kernel re-enumerates it via TODO-07 path. No system-level risk.

## 4. boot_info Passes Controller + Device DMA State
Extend boot_info to carry the full controller state: DCBAA physical address, scratchpad pointers, per-device slot contexts, transfer ring addresses.

**Files:** `include/kernel/boot_info.h`, `src/boot/uefi/bootx64.c`

- [ ] Add `struct boot_usb_controller` to boot_info: BAR0, DCBAA phys, scratchpad phys, ring addresses
- [ ] Add per-device: slot_id, output context phys, EP0 ring phys, bulk ring phys addresses
- [ ] Add `usb_handover_complete` flag — 1 if bootloader successfully configured the controller
- [ ] Commit: `"boot: extend boot_info with xHCI controller DMA state"`

**Test checkpoint:** Kernel reads boot_info, logs controller state with matching physical addresses. POST code 0xD754. Test on: QEMU `run-usb`, bare metal i5-4210U (EHCI+xHCI), bare metal i5-11600K (xHCI only).

**Regression risk:** LOW — boot_info extension is additive. If `usb_handover_complete` is 0 (not set), kernel uses TODO-07 path unchanged. No existing fields affected.

## 5. Kernel Inherits Controller Without Halt/Reset
When `boot_info.usb_handover_complete` is set, the kernel skips the entire xhci_init_controller() halt/reset/DCBAA/rings sequence and directly uses the bootloader's DMA structures.

**Files:** `src/kernel/drivers/xhci.c`

- [x] If `usb_handover_complete`: skip halt/reset/DCBAA alloc/ring init/Intel routing
- [x] Point `xhci_controller` struct at boot_info's DMA addresses (DCBAA, cmd/evt rings, ERST, scratchpads)
- [x] Verify controller is running (USBSTS.HCH=0) — if HCH=1, fall back via `goto full_init`
- [ ] Issue a No-Op command to verify command ring — deferred (enumeration validates it implicitly)
- [x] Go straight to `xhci_enumerate_ports()` — skip Intel routing entirely
- [x] Commit: `"drivers: xHCI zero-delay handover — inherit bootloader DMA state"`

**Test checkpoint:** Serial shows "zero-delay handover: controller inherited". No halt/reset/500ms in log. USB device available within 1ms of kernel start. POST code 0xD755. If crash at 0xD755: controller state corrupt — fall back to TODO-07 path. Test on: QEMU `run-usb`, bare metal i5-4210U, bare metal i5-11600K.

**Regression risk:** HIGH — modifies xhci_init_controller() core path. If handover detection is wrong (false positive), controller has stale DMA pointers and all USB fails. Rollback: if `usb_handover_complete` check causes any issue, set it to 0 in kernel entry and the entire TODO-07 path runs unchanged.

## 6. Kernel Registers MSC Devices from boot_info Geometry
Skip INQUIRY + READ CAPACITY for MSC devices — use sector count/size from Phase A's EFI_BLOCK_IO_PROTOCOL query.

**Files:** `src/kernel/drivers/usb_msc.c`, `src/kernel/main/blkdev_adapters.c`

- [ ] When handover is active, populate `usb_msc_info` from boot_info geometry
- [ ] Register block device with `blkdev_register()` using boot_info sector count/size
- [ ] Verify by reading sector 0 — must match expected MBR/GPT
- [ ] Commit: `"drivers: USB MSC instant registration from boot_info geometry"`

**Test checkpoint:** `usb0` block device registered. Sector 0 read matches expected content. No INQUIRY/READ CAPACITY commands in serial log. POST code 0xD756. Test on: QEMU `run-usb`, bare metal i5-4210U, bare metal i5-11600K.

## 7. Fallback: Detect Corrupt State, Revert to TODO-07 Path
If any handover validation fails, transparently fall back to the proven halt/reset/enumerate path.

**Files:** `src/kernel/drivers/xhci.c`

- [ ] Check: USBSTS.HCH should be 0 (controller running)
- [ ] Check: No-Op command completes within 100ms
- [ ] Check: DCBAAP matches boot_info value
- [ ] If any check fails: log warning, halt/reset, full re-enumerate (TODO-07 §1-§4)
- [ ] Commit: `"drivers: xHCI handover fallback — detect corrupt state and recover"`

**Test checkpoint:** Force-fail handover (corrupt boot_info), verify clean fallback to TODO-07 path with no crash. POST code 0xD757. Test on: QEMU `run-usb`, bare metal i5-4210U, bare metal i5-11600K.

**Regression risk:** LOW — fallback is the proven TODO-07 path. If fallback detection itself crashes, the issue is in the validation code, not the USB stack. Rollback: disable handover validation, always use TODO-07 path.

---

## OS Comparison

| ⭐ | Feature             | 🪟 Win11             | 🐧 Linux            | 🚀 Impossible OS         |
|----|---------------------|-------------------|------------------|-----------------------|
| ⭐ | Pre-boot USB driver | ✅ winload.efi   | ❌ Post-boot     | 🔄 §1-§2 DMA only   |
| ⭐ | Zero-delay handover | ✅ Seamless      | ❌ Halt/reset    | ✅ §5 DMA inherit    |
| ⭐ | Persistent DMA      | ✅ Kernel memory | ❌ Reallocates   | ✅ §1 EfiLoaderData  |
| 💎 | USBLEGSUP handoff   | ✅ Automatic     | ✅ xhci-pci.c    | ✅ TODO-07 §5B done  |
| ⭐ | Boot USB timing VPD | ❌ Not exposed   | ❌ Not exposed   | ⬜ TODO-07 planned   |
| 💎 | Handover fallback   | ✅ Automatic     | ✅ Always fresh  | ⬜ §7 planned        |
| ⭐ | Handover + EHCI     | ✅ usbehci.sys   | ❌ Always reset  | ⬜ §2 EHCI path      |

> Matches Windows USB boot speed, exceeds Linux. VPD timing visibility would be a competitive first.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_usb_handover()` (-> XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.
> Zero-delay handover tests require USB hardware. Use `bash scripts/build.sh run-usb` for QEMU tests. Tests gracefully skip when handover is not active (fallback to TODO-07 path).

- [ ] Create `src/kernel/test/test_usb_handover.c` with:
  - `boot_info.usb_handover_complete` is 0 or 1 (valid flag, not garbage)
  - When handover active: controller is running (`USBSTS.HCH == 0`) without kernel halt/reset
  - When handover active: `boot_info.usb_controller.dcbaa_phys` is page-aligned and non-zero
  - When handover active: `boot_info.usb_controller.dma_page_count > 0` (DMA pages allocated by bootloader)
  - When handover active: PMM marked DMA pages as used (`pmm_is_allocated(dcbaa_phys)` returns true)
  - When handover active: scratchpad buffer count matches `HCSPARAMS2.MaxScratchpadBufs` from capability registers
  - Fallback validation: set `boot_info.usb_handover_complete = 0` in test, call `xhci_init_controller()`, verify full init path runs (halt/reset/enumerate)
  - When handover active: USB device available within 5ms of kernel entry (TSC delta check against `boot_info.timing.kernel_entry_tsc`)
- [ ] Add to `scripts/test-smoke.sh` (with `run-usb` target):
  - Grep serial for `zero-delay handover` (handover path taken) or `Controller halted` (fallback path)
  - When handover active: no `500ms` delay string in serial log
  - Grep serial for `DMA pages:` (bootloader allocation logged)
- [ ] Register in `test_runner_init()`: `test_register_usb_handover()`
- [ ] Commit: `"test: add usb_handover test suite"`

## Verification

- [ ] `bash scripts/build.sh run-usb` — USB drive mounted as D:\ with zero-delay handover active
- [ ] Serial log: no "Controller halted" / "Controller reset" / "500ms" when handover is active
- [ ] Bare metal i5-4210U: handover works with EHCI+xHCI (USBLEGSUP in bootloader)
- [ ] Bare metal i5-11600K: handover works with xHCI-only
- [ ] Fallback: corrupt boot_info → clean recovery to TODO-07 path, no crash
- [ ] Boot timing: USB device available within 1ms of kernel start (measured via TSC)
