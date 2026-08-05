---
schema_version: 1
id: usb-zero-delay-handover
domain: 01-boot-platform
status: active
title: "TODO-20 -- Zero-Delay USB Boot (Pre-ExitBootServices Driver Loading)"
---

# TODO-20 -- Zero-Delay USB Boot (Pre-ExitBootServices Driver Loading)

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Eliminate all post-ExitBootServices USB initialization latency by loading the xHCI driver's DMA structures inside the UEFI bootloader while firmware is still active. The bootloader allocates DCBAA, device contexts, and transfer rings in `EfiLoaderData` memory (survives ExitBootServices), performs USBLEGSUP handoff while firmware USB is running, and passes persistent controller state to the kernel. The kernel inherits the controller without halt/reset -- USB devices are available instantly. This is how Windows `winload.efi` + `iusb3xhc.sys` achieves zero-delay USB boot.

> [!NOTE]
> **Non-blocking.** The current TODO-17 §5-§1 path (halt/reset/enumerate) works on all hardware, and TODO-17 §2 owns post-boot hot-plug and runtime lifecycle. This TODO is now the single owner for pre-ExitBootServices USB handover and persistent DMA inheritance. It is a performance optimization, not a correctness prerequisite.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) -- UEFI bootloader (Phase A USB discovery already implemented)
- [`src/kernel/drivers/xhci.c`](../../src/kernel/drivers/xhci.c) -- xHCI controller driver (USBLEGSUP handoff already implemented)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) -- boot_info USB device array (already defined)
- [`include/kernel/drivers/xhci.h`](../../include/kernel/drivers/xhci.h) -- xHCI controller and device context structures
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §4` -- typed boot payload and boot_info ownership work owns the persistent xHCI handover descriptor schema
- → XREF: `01-boot-platform/TODO-17-xhci-usb-boot.md §5-§2` -- baseline xHCI boot path and post-boot hot-plug that this handover layer accelerates but does not replace
- → XREF: `04-drivers-hardware/TODO-08-core-driver-enhancements.md §3` -- MSI/MSI-X (runtime hot-plug path remains in TODO-17 §2)
- → XREF: `04-drivers-hardware/TODO-10-usb-stack.md` -- advanced USB features (builds on top of this handover)

## Outcome

- USB boot drive mounted as C:\ within 1ms of kernel start (zero halt/reset/enumerate delay)
- No 500ms Intel port routing delay (no XUSB2PR needed -- firmware already routed)
- No SCSI INQUIRY/READ CAPACITY (geometry from bootloader via EFI_BLOCK_IO_PROTOCOL)
- Kernel inherits fully-configured xHCI controller with active device slots
- Fallback: if handover state is corrupt, kernel falls back to TODO-17 §5-§1 path automatically
- Works on QEMU `run-usb`, bare metal i5-4210U (EHCI+xHCI), bare metal i5-11600K (xHCI only)

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On | Status |
| --- | :---: | ---------------------------------------- | ---------- | :----: |
| ⭐   |   1   | Bootloader allocates xHCI DMA structures | --         |  [/]   |
| ⭐   |   2   | Bootloader performs USBLEGSUP + controller takeover | §1         |  [/]   |
| ⭐   |   3   | Bootloader enumerates devices with persistent state | §2         |  [/]   |
| ⭐   |   4   | boot_info passes controller + device DMA state | §3, T01 §4 |  [/]   |
| ⭐   |   5   | Kernel inherits controller DMA (partial; full zero-delay needs §3) | §1, §2     |  [/]   |
| ⭐   |   6   | Kernel registers MSC devices from boot_info geometry | §5         |  [/]   |
| 💎   |   7   | Fallback: detect corrupt state, revert to §1-§4 path | §5         |  [/]   |

> All ⭐ rows -- this is a competitive advantage over Linux (which always re-enumerates after kexec/boot). Windows does this via winload.efi but it's invisible to users. Making it visible in boot timing would be a first.

---

## 1. Bootloader Allocates xHCI DMA Structures
Allocate DCBAA, device output contexts, and transfer rings using `gBS->AllocatePages(EfiLoaderData)` so the memory survives ExitBootServices. The kernel PMM must be aware these pages are in use.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

- [x] Allocate DCBAA: 1 page `EfiLoaderData`, zeroed (fits max_slots+1 entries)
- [x] Allocate scratchpad buffers (array + individual pages, if `HCSPARAMS2.MaxScratchpadBufs > 0`)
- [ ] **USB scratchpad clamp:** `src/boot/uefi/bootx64.c` writes the raw `HCSPARAMS2.MaxScratchpadBufs` value into `boot_usb_controller.max_scratchpads` and allocates exactly that many pages without clamping against `BOOT_USB_MAX_SCRATCHPADS` (16). Controllers reporting more than 16 scratchpads would allocate beyond our documented ABI surface, and the canonical `docs/boot/boot-info-fields.md` row for `max_scratchpads` currently documents "not clamped" as the real producer behavior. Work: clamp in `src/boot/uefi/bootx64.c` near the HCSParams2 read (around line 4160), log a serial warning if the clamp trips, and update the `max_scratchpads` row in the canonical matrix + this item to `[x]`. Not a functional defect today because all target hardware reports <= 16, but an unbounded ABI field is the kind of thing an uncommon controller can trip into an allocator overrun. Filed during the TODO-01 §1 post-impl review (2026-04-17); see [canonical matrix -- boot_usb_controller table](../../docs/boot/boot-info-fields.md#nested-struct-boot_usb_controller).
- [ ] Allocate device output context per enumerated device -- deferred to §3 (enumeration)
- [ ] Allocate EP0 transfer ring per device -- deferred to §3 (enumeration)
- [ ] Allocate bulk-IN/OUT transfer rings for MSC devices -- deferred to §3 (enumeration)
- [x] Record all physical addresses in `boot_info.usb_controller` struct
- [x] Mark allocated pages in `dma_pages[]` array; kernel PMM calls `pmm_mark_region_used()` for each
- [x] PCI config space access added to bootloader (`bl_pci_read8/16/32` via 0xCF8/0xCFC)
- [x] xHCI capability registers read (HCSPARAMS1/2, HCCPARAMS1, DBOFF, RTSOFF)
- [x] **HIGH (DMA cap):** `bootx64.c:10163-10333` uses `AllocateAnyPages` -- DMA can land above the 4 GiB kernel identity map (or violate `AC64=0`), faulting the kernel. Use `AllocateMaxAddress` capped at `0xFFFFFFFF`; fall back on failure
- [x] **HIGH (BAR decode):** `bootx64.c:10255-10262` treats BAR1 as BAR0's 64-bit high half unconditionally -- a 32-bit memory BAR fabricates a bogus MMIO base. Decode BAR0 type `(bar0&0x6)==0x4` before using BAR1; skip handover if invalid
- [ ] **HIGH (payload overlap):** `boot_payload_region_overlap()` (`boot_payload.c:203-214`) treats each `dma_pages[]` entry as one page, missing multi-page scratchpad spans. Use `boot_reserved_populate_from_info()` extents + add a 2nd-page test
- [x] Commit: `"boot: allocate persistent xHCI DMA structures in EfiLoaderData"`

**Test checkpoint:** Serial shows allocated DMA addresses. POST code 0xB082. Verify `EfiLoaderData` pages survive ExitBootServices by reading back from kernel. Test on: QEMU `run-usb`, bare metal.

**Regression risk:** Allocating extra `EfiLoaderData` pages reduces available RAM. If allocation fails (low memory), fall back to not allocating and let kernel use TODO-17 path. Rollback: skip bootloader DMA allocation entirely.

> **Test runner:** N/A (pre-ExitBootServices UEFI bootloader DMA allocation; no kernel test surface -- the payload-overlap test is the open HIGH item) | validation: serial on bare metal

> **Notes:**
> - **What shipped:** §1 bootloader xHCI DMA allocation in `bootx64.c` (DCBAA + scratchpad `EfiLoaderData` pages, PCI config via 0xCF8/0xCFC, cap-register reads) + `boot_usb_controller` struct in `boot_info.h`.
> - **How it runs:** pre-ExitBootServices; the kernel inherits the recorded phys addresses (§5) and PMM reserves `dma_pages[]`; `usb_handover_complete` gates the whole path.
> - **Downstream effects:** foundation for §5's partial DMA inherit; DONE-UNSTAMPED review filed 3 HIGH bare-metal bugs -- 2 now fixed (DMA `AllocateMaxAddress` 4 GiB cap, BAR-width decode) via re-adversarial-converged patch; payload-overlap span check remains open.
> - **Canonical doc:** `include/kernel/boot_info.h` `boot_usb_controller` + `docs/boot/boot-info-fields.md`.
> - **Scope boundary:** §1 owns DMA allocation; device-context/ring alloc is §3; the typed-payload schema is §4.

> **Verified:** 2026-06-15 | commit `832af15c` | 8/13 items | build OK + smoke PASS (KVM 1.96s) | DONE-UNSTAMPED review + 2H fix
> **Deferred:** [H] payload-overlap validator misses multi-page scratchpad spans (DMA-above-4G + BAR1 mis-decode RESOLVED 2026-06-15 by AllocateMaxAddress cap + BAR-width decode; re-adversarial converged 2 rounds) -> XREF: 01-boot-platform/TODO-20 §1 (item: "HIGH (payload overlap)")
> **Quality reviewed:** 2026-06-15 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 2H+1M fixed, 1H open | scope: boot-code-quality

---

## 2. Bootloader Performs USBLEGSUP + Controller Takeover
While firmware USB stack is still active, take xHCI ownership via USBLEGSUP and configure the controller to use our persistent DMA structures.

**Files:** `src/boot/uefi/bootx64.c`

- [x] Read xHCI BAR0 from PCI config space -- reuses §1 `allocate_xhci_dma()` discovery
- [x] MMIO identity-mapped by firmware -- direct volatile pointer access
- [x] USBLEGSUP handoff (walk extended caps for ID=1, set OS Owned, wait 1s)
- [x] Halt controller (USBCMD.RS=0, poll HCH=1, 16ms timeout)
- [x] Reset controller (HCRST=1, poll HCRST=0 AND CNR=0) -- clean state for our DMA
- [x] Write DCBAAP, CRCR (with Link TRB + cycle=1), ERST, ERDP, ERSTBA, CONFIG
- [x] Start controller (USBCMD.RS=1 + INTE=1, IMAN.IE=1)
- [x] `usb_handover_complete` set to 1 on success
- [ ] **HIGH (USBLEGSUP timeout):** `bl_usblegsup_handoff()` (`bootx64.c:10456-10462`) force-clears BIOS-Owned + returns success on the 1s timeout, so takeover sets `usb_handover_complete=1` anyway. Return failure + abort on timeout
- [ ] **MEDIUM (ext-cap MMIO bound):** the USBLEGSUP cap walk (`bootx64.c:10417-10428`) is iteration-bounded but not MMIO-bounded -- a malformed chain reads unrelated MMIO. Require `xecp_off+4 <= mmio_size` per read
- [x] **HIGH (BAR-decode consumer):** §2 derives op_base/rt_base from the §1 BAR decode and does destructive pre-EBS MMIO writes -- resolved by the §1 BAR-decode fix (32-bit BARs no longer fabricate a base)
- [x] Commit: `"boot: USBLEGSUP handoff + controller takeover in bootloader"`

> [!IMPORTANT]
> After halting the firmware's USB stack, `EFI_USB_IO_PROTOCOL` and `EFI_BLOCK_IO_PROTOCOL` are no longer usable. Phase A device discovery must complete BEFORE this step. Order: Phase A (discover via EFI) → §2 (takeover) → §3 (enumerate with our state).

**Test checkpoint:** Controller running with bootloader-allocated DMA. POST code 0xB083. If halt/takeover fails, log error and skip (kernel falls back to TODO-17). Test on: QEMU `run-usb`, bare metal.

**Regression risk:** HIGH -- halting the firmware USB stack may break EFI services that depend on USB (e.g., EFI console on USB keyboard). Must happen late in bootloader, after kernel load and config table copy. Rollback: if takeover fails, don't modify controller and let kernel handle it.

> **Test runner:** N/A (pre-ExitBootServices UEFI bootloader takeover; no kernel test surface) | validation: serial on bare metal

> **Notes:**
> - **What shipped:** §2 USBLEGSUP handoff + controller halt/reset/takeover in `bootx64.c` (~10389-10520) -- walks extended caps for OS-Owned, programs DCBAAP/CRCR/ERST/ERDP/CONFIG, starts the controller, sets `usb_handover_complete`.
> - **How it runs:** late in the bootloader after kernel load + config-table copy (EFI USB dies after the halt); the kernel inherits via §5.
> - **Downstream effects:** this DONE-UNSTAMPED review filed 2 HIGH + 1 MEDIUM bare-metal bugs (USBLEGSUP-timeout-as-success, unbounded cap walk, destructive consumption of §1's BAR decode) + overlaps the §5 ring re-allocation; downgraded §2 to `[/]`.
> - **Canonical doc:** `src/boot/uefi/bootx64.c` takeover block + `include/kernel/boot_info.h` `boot_usb_controller`.
> - **Scope boundary:** §2 owns the USBLEGSUP/takeover; DMA allocation + BAR decode are §1; kernel inherit is §5.

> **Verified:** 2026-06-15 | commit `cf7bd197` | 9/11 items | build OK | DONE-UNSTAMPED review
> **Deferred:** [H] USBLEGSUP 1s timeout treated as success + unbounded ext-cap MMIO walk (M) (BAR-decode consumer RESOLVED 2026-06-15 by the §1 BAR-width decode fix) -> XREF: 01-boot-platform/TODO-20 §2 (item: "HIGH (USBLEGSUP timeout)" + "MEDIUM (ext-cap MMIO bound)")
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 1H fixed, 1H+1M open | scope: boot-code-quality

---

## 3. Bootloader Enumerates Devices with Persistent State
Enumerate connected devices in the bootloader so slot contexts and endpoint rings are in persistent memory. This is the true Windows-style zero-delay -- devices have pre-configured slots when the kernel inherits.

> [!NOTE]
> **Deferred.** Requires porting the full 9-step USB enumeration sequence (~350 lines of xhci_dev.c) into the bootloader. The pragmatic approach is to share the code (compile xhci_dev.c for both bootloader and kernel contexts) rather than duplicate it. For now, §5 handles kernel inheritance of DMA structures and the kernel runs its own enumeration on the persistent DCBAA/rings -- still saves all allocation time + Intel 500ms.

**Files:** `src/boot/uefi/bootx64.c`

- [ ] For each port with CCS=1: port reset, Enable Slot, Address Device
- [ ] GET_DESCRIPTOR (device + configuration) -- store in persistent buffers
- [ ] SET_CONFIGURATION -- device is now configured
- [ ] For MSC: Configure Endpoint (bulk-IN/OUT) using persistent transfer rings
- [ ] Record slot_id, port, speed, VID:PID, endpoint addresses in boot_info
- [ ] Commit: `"boot: enumerate USB devices with persistent DMA state"`

**Test checkpoint:** Serial shows enumerated devices with slot IDs. POST code 0xB086. Device descriptors match Phase A discovery. Test on: QEMU `run-usb`, bare metal i5-4210U, bare metal i5-11600K.

**Regression risk:** MEDIUM -- USB enumeration is complex (9-step sequence). If any step fails, the device is skipped and kernel re-enumerates it via TODO-17 path. No system-level risk.

> **Test runner:** N/A (pre-ExitBootServices bootloader enumeration; deferred -- not started) | validation: serial on bare metal once implemented

> **Notes:**
> - **What shipped:** nothing -- §3 is deferred. The kernel still runs its own enumeration on the inherited DCBAA/rings (§5), so the feature degrades cleanly to "DMA-alloc saved" without §3.
> - **Why deferred:** requires porting the ~350-line 9-step `xhci_dev.c` enumeration into the bootloader pre-EBS (best done by compiling `xhci_dev.c` for both contexts), and the per-device results must be carried via §4's `boot_info` schema.
> - **Blocker:** gated on the §4 `boot_info` DMA-payload ABI decision (a stop-and-ask ABI change) -- without the per-device payload layout there is nowhere to record slot/EP-ring geometry for the kernel.
> - **Canonical doc:** `src/boot/uefi/bootx64.c` (would host the port) + `src/kernel/drivers/xhci_dev.c` (the source enumeration).
> - **Scope boundary:** §3 enumerates in the bootloader; §5 inherits; §4 carries the schema.

> **Verified:** 2026-06-15 | commit `832af15c` | 0/5 items | deferred (design parked on §4 ABI) | gap-audit scoped
> **Deferred:** [feature] bootloader device enumeration (per-device slot context + EP0/bulk rings) -- needs the ~350-line xhci_dev.c port + the §4 boot_info per-device payload schema -> XREF: 01-boot-platform/TODO-20 §4 (item: "DMA-shape design (blocks typed-payload item)")

---

## 4. boot_info Passes Controller + Device DMA State
Extend boot_info to carry the full controller state: DCBAA physical address, scratchpad pointers, per-device slot contexts, transfer ring addresses.

**Files:** `include/kernel/boot_info.h`, `src/boot/uefi/bootx64.c`

- [x] Add `struct boot_usb_controller` to boot_info: BAR0, DCBAA phys, scratchpad phys, ring addresses -- shipped in §1
- [ ] Add per-device: slot_id, output context phys, EP0 ring phys, bulk ring phys addresses
- [x] Add `usb_handover_complete` flag -- 1 if bootloader successfully configured the controller -- shipped in §1
- [ ] DMA-shape design (blocks typed-payload item): §1 allocates DCBAA/cmd/evt/ERST/scratchpad as disjoint pages, so one contiguous `BOOT_PAYLOAD_USB_HANDOVER` descriptor can't cover them -- pick a contiguous arena or scatter/gather descriptors
- [ ] Publish one typed payload descriptor of type `BOOT_PAYLOAD_USB_HANDOVER` (enum in [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)) covering the contiguous xHCI DMA blob so TODO-01 §4's packed-prefix + overlap validator sees it alongside every other handoff region. -> XREF: [`01-boot-platform/TODO-01 §4`](TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array)
- [ ] Commit: `"boot: extend boot_info with xHCI controller DMA state"`

**Test checkpoint:** Kernel reads boot_info, logs controller state with matching physical addresses. POST code 0xD754. Test on: QEMU `run-usb`, bare metal i5-4210U (EHCI+xHCI), bare metal i5-11600K (xHCI only).

**Regression risk:** LOW -- boot_info extension is additive. If `usb_handover_complete` is 0 (not set), kernel uses TODO-17 path unchanged. No existing fields affected.

> **Test runner:** N/A (boot_info ABI schema; per-device payload deferred) | validation: kernel reads controller state, serial on bare metal

> **Notes:**
> - **What shipped:** the controller-level `struct boot_usb_controller` (BAR0, DCBAA, scratchpad, ring phys) + `usb_handover_complete` flag, both landed in §1 and consumed by §5.
> - **Open (ABI decision):** the per-device payload (slot_id, output-context/EP0/bulk-ring phys) + the typed `BOOT_PAYLOAD_USB_HANDOVER` descriptor are NOT done -- they require the DMA-shape design fork.
> - **Blocker:** §1 allocates DCBAA/cmd/evt/ERST/scratchpad as DISJOINT pages, so one contiguous descriptor can't cover them -- the contiguous-vs-scatter choice is a `boot_info` ABI change (stop-and-ask) wiring into TODO-01 §4.
> - **Canonical doc:** `include/kernel/boot_info.h` `boot_usb_controller` + `src/boot/uefi/boot_info_mirror.h`.
> - **Scope boundary:** §4 owns the schema; §3 produces per-device data into it; §5/§6 consume it.

> **Verified:** 2026-06-15 | commit `832af15c` | 2/5 items | build OK | controller-level fields shipped (§1)
> **Deferred:** [feature] per-device DMA payload + typed `BOOT_PAYLOAD_USB_HANDOVER` descriptor + the contiguous-vs-scatter DMA-shape decision -- a stop-and-ask `boot_info` ABI change needing user sign-off -> XREF: 01-boot-platform/TODO-01 §4 (item: "Optional payload descriptor array") + TODO-20 §4 (item: "DMA-shape design")

---

## 5. Kernel Inherits Controller Without Halt/Reset
When `boot_info.usb_handover_complete` is set, the kernel skips the entire xhci_init_controller() halt/reset/DCBAA/rings sequence and directly uses the bootloader's DMA structures.

**Files:** `src/kernel/drivers/xhci.c`

- [x] On `usb_handover_complete` + `boot_caps_require(BOOT_CAP_USB_HANDOVER)`: inherit the bootloader DCBAA / scratchpad / ERST / cmd+evt-ring PHYSICAL pages with no DMA re-allocation (`xhci.c:374-396`) -- saves the allocation cost
- [x] Verify controller + fall back: on handover halt / restart / ring-init failure `goto full_init` (TODO-17 path) -- `xhci.c:411,427,440`
- [ ] Issue a No-Op command to verify command ring -- deferred (enumeration validates it implicitly)
- [/] Reality is a PARTIAL inherit, not full zero-delay: the path still halts-to-sync, re-inits ring state via `xhci_rings_init()`, routes Intel USB2, and re-enumerates (`xhci.c:398-462`)
- [ ] True zero-delay: skip halt + ring re-init + Intel routing + kernel enumeration -- requires §3 bootloader enumeration so the kernel inherits configured slots
- [ ] **HIGH (controller identity):** inherit gate (`xhci.c:375-377`) matches only global caps, not `bc->pci_bus/dev/func`/`mmio_phys` -- a 2nd xHCI reuses the same DCBAA/rings. Require a pci/mmio match before inherit
- [ ] **HIGH (validate before trust):** gate programs DCBAAP from `bc->dcbaa_phys` with no nonzero/aligned/<4GiB/reserved check (`xhci.c:375-427`); a stale boot_info starts xHCI on a bad DMA target. Validate first (overlaps §7)
- [ ] **HIGH (Intel 500ms not saved):** handover still calls `xhci_route_intel_usb2_ports()` (`xhci.c:459`) = 500ms delay, so EHCI+xHCI (i5-4210U) is not zero-delay. Carry routing state in boot_info + skip XUSB2PR on inherit
- [x] Commit: `"drivers: xHCI zero-delay handover -- inherit bootloader DMA state"`

**Test checkpoint:** Serial shows "zero-delay handover: controller inherited". No halt/reset/500ms in log. USB device available within 1ms of kernel start. POST code 0xD755. If crash at 0xD755: controller state corrupt -- fall back to TODO-17 path. Test on: QEMU `run-usb`, bare metal i5-4210U, bare metal i5-11600K.

**Regression risk:** HIGH -- modifies xhci_init_controller() core path. If handover detection is wrong (false positive), controller has stale DMA pointers and all USB fails. Rollback: if `usb_handover_complete` check causes any issue, set it to 0 in kernel entry and the entire TODO-17 path runs unchanged.

> **Test runner:** N/A (handover path needs a real bootloader-configured xHCI; no in-memory test surface) | validation: serial on bare metal i5-4210U + i5-11600K

> **Notes:**
> - **What shipped:** §5 kernel handover-inherit path in `xhci.c:374-462` -- gated on `usb_handover_complete` + `BOOT_CAP_USB_HANDOVER`, copies the bootloader DCBAA/scratchpad phys, halt-syncs, falls back to `full_init` on failure.
> - **Reality (partial):** inherits DCBAA/scratchpad only; `xhci_rings_init()` re-allocates fresh cmd/evt/ERST (leaking the bootloader rings), Intel routing still pays 500ms, kernel re-enumerates -- NOT zero-delay on EHCI+xHCI.
> - **Downstream effects:** DONE-UNSTAMPED review filed 3 HIGH (controller-identity match, validate-before-trust, Intel-500ms) + the ring-realloc-leak; corrected the OS-Comparison overclaim from `✅` to `🔄 partial`.
> - **Canonical doc:** `src/kernel/drivers/xhci.c` handover branch; the validate-before-trust check overlaps §7's fallback detection.
> - **Scope boundary:** §5 owns the DMA inherit; true zero-delay (skip routing + enumeration) needs §3 + the §4 ABI schema.

> **Verified:** 2026-06-15 | commit `a6de918f` | 2/8 items | build OK | DONE-UNSTAMPED review
> **Deferred:** [H] inherit gate does not match the bootloader controller identity + trusts inherited DMA addrs unvalidated + still pays Intel 500ms; (M) re-allocated rings leak the inherited pages; true zero-delay blocked on §3/§4 -> XREF: 01-boot-platform/TODO-20 §5 (item: "HIGH (controller identity)" + "HIGH (validate before trust)" + "HIGH (Intel 500ms not saved)") + §3 + §4
> **Quality reviewed:** 2026-06-15 | Codex 3x (adversarial, consistency, perf) | 0 fixed, 3H+1M open | scope: kernel-code-quality

---

## 6. Kernel Registers MSC Devices from boot_info Geometry
Skip INQUIRY + READ CAPACITY for MSC devices -- use sector count/size from Phase A's EFI_BLOCK_IO_PROTOCOL query.

**Files:** `src/kernel/drivers/usb_msc.c`, `src/kernel/main/blkdev_adapters.c`

- [ ] When handover is active, populate `usb_msc_info` from boot_info geometry
- [ ] Register block device with `blkdev_register()` using boot_info sector count/size
- [ ] Verify by reading sector 0 -- must match expected MBR/GPT
- [ ] Commit: `"drivers: USB MSC instant registration from boot_info geometry"`

**Test checkpoint:** `usb0` block device registered. Sector 0 read matches expected content. No INQUIRY/READ CAPACITY commands in serial log. POST code 0xD756. Test on: QEMU `run-usb`, bare metal i5-4210U, bare metal i5-11600K.

> **Test runner:** N/A (deferred -- depends on §4 geometry) | validation: serial on bare metal once implemented

> **Notes:**
> - **What shipped:** nothing -- §6 is deferred. MSC devices still register via the full TODO-17 path (INQUIRY + READ CAPACITY), which works; §6 is purely the latency optimization.
> - **Why deferred:** skipping INQUIRY/READ CAPACITY needs the sector count/size captured at Phase A (EFI_BLOCK_IO) and carried in §4's per-device `boot_info` payload -- which is the ABI-gated open part of §4.
> - **Blocker:** §4 per-device DMA payload schema + a Phase A geometry capture step.
> - **Canonical doc:** `src/kernel/drivers/usb_msc.c` + `src/kernel/main/blkdev_adapters.c`.
> - **Scope boundary:** §6 consumes §4's geometry; §3 produces the per-device data.

> **Verified:** 2026-06-15 | commit `832af15c` | 0/3 items | deferred (blocked on §4 ABI) | gap-audit scoped
> **Deferred:** [feature] MSC instant registration from boot_info geometry -- blocked on the §4 per-device payload + a Phase A EFI_BLOCK_IO geometry capture -> XREF: 01-boot-platform/TODO-20 §4 (item: "Add per-device: slot_id, output context phys, EP0 ring phys, bulk ring phys addresses")

---

## 7. Fallback: Detect Corrupt State, Revert to TODO-17 Path
If any handover validation fails, transparently fall back to the proven halt/reset/enumerate path.

**Files:** `src/kernel/drivers/xhci.c`

- [ ] Check: USBSTS.HCH should be 0 (controller running)
- [ ] Check: No-Op command completes within 100ms
- [ ] Check: DCBAAP matches boot_info value
- [ ] If any check fails: log warning, halt/reset, full re-enumerate (TODO-17 §5-§1)
- [ ] IOMMU default-deny: §5 inherits a DMA-capable controller, so before default-deny remapping the inherited DMA pages must be IOMMU-mapped for the xHCI function or the handover forces the TODO-17 fallback -> XREF: `04-drivers-hardware/TODO-04 §4`
- [ ] Commit: `"drivers: xHCI handover fallback -- detect corrupt state and recover"`

**Test checkpoint:** Force-fail handover (corrupt boot_info), verify clean fallback to TODO-17 path with no crash. POST code 0xD757. Test on: QEMU `run-usb`, bare metal i5-4210U, bare metal i5-11600K.

**Regression risk:** LOW -- fallback is the proven TODO-17 path. If fallback detection itself crashes, the issue is in the validation code, not the USB stack. Rollback: disable handover validation, always use TODO-17 path.

> **Test runner:** N/A (deferred; validation gate overlaps §5) | validation: force-corrupt boot_info, serial on bare metal once implemented

> **Notes:**
> - **What shipped:** a coarse fallback only -- §5 already does `goto full_init` on halt/restart/ring-init failure (`xhci.c:411,427,440`). The explicit validation checks (HCH, No-Op completes, DCBAAP matches) are NOT implemented.
> - **Why deferred:** the explicit validation gate is the same xHCI handover-validation surgery deferred for the §5 "validate before trust" HIGH -- consolidate both into one hardening pass rather than write it twice.
> - **Blocker:** the IOMMU default-deny item is cross-blocked on `D04 T04 §4` (inherited DMA pages must be IOMMU-mapped before default-deny); the validation checks themselves are implementable now.
> - **Canonical doc:** `src/kernel/drivers/xhci.c` handover branch.
> - **Scope boundary:** §7 validates + recovers; the validation overlaps the §5 validate-before-trust item.

> **Verified:** 2026-06-15 | commit `832af15c` | 0/5 items | deferred (consolidate with §5 validate-before-trust) | gap-audit scoped
> **Deferred:** [feature] explicit handover validation (USBSTS.HCH, No-Op completes, DCBAAP matches) + IOMMU-map inherited DMA before default-deny -- validation overlaps the §5 HIGH; IOMMU cross-blocked -> XREF: 01-boot-platform/TODO-20 §5 (item: "HIGH (validate before trust)") + 04-drivers-hardware/TODO-04 §4

---

## OS Comparison

| ⭐   | Feature             | 🪟 Win11         | 🐧 Linux        | 🚀 Impossible OS                  |
| --- | ------------------- | --------------- | -------------- | -------------------------------- |
| ⭐   | Pre-boot USB driver | ✅ winload.efi   | ❌ Post-boot    | 🔄 §1-§2 DMA only                 |
| ⭐   | Zero-delay handover | ✅ Seamless      | ❌ Halt/reset   | 🔄 §5 partial (500ms+enum remain) |
| ⭐   | Persistent DMA      | ✅ Kernel memory | ❌ Reallocates  | ✅ §1 EfiLoaderData               |
| 💎   | USBLEGSUP handoff   | ✅ Automatic     | ✅ xhci-pci.c   | ✅ §2 done                        |
| ⭐   | Boot USB timing VPD | ❌ Not exposed   | ❌ Not exposed  | ⬜ TODO-17 planned                |
| 💎   | Handover fallback   | ✅ Automatic     | ✅ Always fresh | ⬜ §7 planned                     |
| ⭐   | Handover + EHCI     | ✅ usbehci.sys   | ❌ Always reset | ⬜ xHCI-only; EHCI -> T19 §11     |

> §1-§2 + §5 DMA-inherit foundation shipped; full Windows-parity zero-delay still needs §3-§6 (bootloader enumeration + the §4 boot_info DMA-payload ABI). VPD timing visibility would be a competitive first.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_usb_handover()` (-> XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.
> Zero-delay handover tests require USB hardware. Use `bash scripts/build.sh run-usb` for QEMU tests. Tests gracefully skip when handover is not active (fallback to TODO-17 path).

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

- [ ] `bash scripts/build.sh run-usb` -- USB drive mounted as D:\ with zero-delay handover active
- [ ] Serial log: no "Controller halted" / "Controller reset" / "500ms" when handover is active
- [ ] Bare metal i5-4210U: handover works with EHCI+xHCI (USBLEGSUP in bootloader)
- [ ] Bare metal i5-11600K: handover works with xHCI-only
- [ ] Fallback: corrupt boot_info → clean recovery to TODO-17 path, no crash
- [ ] Boot timing: USB device available within 1ms of kernel start (measured via TSC)
