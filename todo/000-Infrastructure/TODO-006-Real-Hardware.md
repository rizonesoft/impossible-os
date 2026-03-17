# P0006 — Real Hardware Boot & Validation

> **Goal:** Ensure Impossible OS boots reliably and performs optimally on real x86-64
> hardware via USB disk. Validate every subsystem — UEFI boot, APIC routing, AHCI
> storage, SMP bring-up, framebuffer compositor, and input devices — against the
> assumptions documented in the architecture research paper. Fill hardware gaps
> exposed by emulator-only development, and establish a repeatable USB test workflow.

> [!IMPORTANT]
> **This TODO touches multiple subsystems.** Cross-references are marked with
> `→ XREF:` to indicate work in other TODO files that must be completed to fully
> realize real hardware support.

> [!WARNING]
> **Current state (2026-03-17):** Impossible OS is primarily developed and tested in
> QEMU with VirtIO storage. Real hardware boots have exposed critical gaps:
> timer calibration hangs on some BIOSes, AHCI port enumeration assumptions fail on
> multi-port controllers, USB 2.0 vs 3.0 speed differences cause FAT32 write stalls,
> GOP framebuffer resolution negotiation may fail silently on non-standard displays,
> and the 8259 PIC mask sequence is required even on APIC-only systems due to
> spurious IRQ7 on legacy hardware.

---

## Current Architecture Audit

### Hardware Test Matrix

| Component | QEMU Status | Real Hardware Status | Known Issues |
|-----------|-------------|---------------------|--------------|
| UEFI Boot (BOOTX64.EFI) | ✅ Works | ⚠️ Machine-dependent | Some boards need Secure Boot explicitly OFF |
| GOP Framebuffer (1280×720) | ✅ Works | ⚠️ Mode negotiation | Falls back to 640×480 on some Intel NUCs |
| ACPI RSDP Discovery | ✅ Works | ⚠️ Untested on AMD | RSDP version 1 vs 2 path differences |
| LAPIC/IOAPIC Routing | ✅ Works | ⚠️ Spurious IRQ7 | Must mask PIC before APIC init on legacy HW |
| PIT Calibration | ✅ Works | ❌ Hangs on some boards | Timer test loop too tight; BIOS PIT init varies |
| AHCI Controller Detection | ✅ VirtIO only | ⚠️ Partial | Only tested Port 0; multi-port AHCI untested |
| FAT32 Read/Write (USB) | ✅ Works | ⚠️ Slow writes | Full-file overwrite on USB 2.0 = visible stall |
| Keyboard (PS/2) | ✅ Works | ✅ Works | USB keyboards need USB HID (not implemented) |
| Mouse (PS/2) | ✅ Works | ⚠️ Some laptops | Touchpad may not expose PS/2 emulation |
| SMP Bring-up (INIT/SIPI) | ✅ Works | ⚠️ Untested | AP stack allocation, SIPI vector page |
| Double-Buffered Compositor | ✅ Works | ✅ Works | VRAM write speed varies by PCI-Express generation |

### Test Machines

> Track every machine tested against for driver development and regression tracking.

| # | Machine | CPU | RAM | GPU | AHCI/NVMe | Status | Notes |
|---|---------|-----|-----|-----|-----------|--------|-------|
| 1 | _TBD — add your machine_ | | | | | | |

---

## 1. USB Boot Disk Workflow

### 1.1 USB Disk Layout & Partitioning

**Prompt:** Define and document the canonical USB disk layout for booting Impossible OS on real hardware. The USB disk must have a GPT partition table with two partitions: (1) EFI System Partition (ESP, FAT32, 64 MiB) containing `EFI\BOOT\BOOTX64.EFI`, `boot.conf`, and space for numbered boot logs (`BOOT_NNN.LOG`, `HARDWARE.TXT`); (2) Data Partition (IXFS or FAT32, remainder) containing the OS filesystem image (`C:\Impossible\*`). The UEFI firmware reads BOOTX64.EFI from the ESP, which loads the kernel ELF from the same partition (embedded in an initrd or separate file). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"infra: USB disk layout specification"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-010-Bootloader.md §1` — the UEFI bootloader must locate and load the
> kernel from the ESP. The kernel ELF and initrd paths must match between the
> bootloader and the USB write script.

> [!IMPORTANT]
> → XREF: `TODO-005-Debug.md §1.2` — numbered boot logs (`BOOT_NNN.LOG`) are written
> to the ESP (X: partition). The partition must be large enough for ~100 logs.

- [ ] Document GPT partition layout:
  ```
  USB Disk (GPT)
  ├── Partition 1: ESP (FAT32, 64 MiB, type EFI System)
  │   ├── EFI\BOOT\BOOTX64.EFI    — UEFI bootloader
  │   ├── boot.conf                — boot configuration (debug=1, verbose=0)
  │   ├── kernel.elf               — kernel binary
  │   ├── initrd.img               — initial ramdisk (IXFS image)
  │   ├── BOOT_NNN.LOG             — numbered boot logs (auto-generated)
  │   └── HARDWARE.TXT             — hardware dump (auto-generated)
  └── Partition 2: Data (FAT32, remainder)
      └── C:\Impossible\*           — OS filesystem
  ```
- [ ] `scripts/deploy/write-usb.ps1`: create GPT table, format ESP, copy files
- [ ] `scripts/deploy/write-usb.sh`: Linux variant for WSL development
- [ ] Both scripts: auto-detect USB disk (prompt user to confirm target device)
- [ ] Safety: refuse to write to fixed disks (check removable flag)
- [ ] Commit: `"infra: USB disk layout specification"`

### 1.2 USB Write Script (Windows Host)

**Prompt:** Create `scripts/deploy/write-usb.ps1` for writing the OS to a USB disk from a Windows host. The script must: (1) list available removable USB disks, (2) prompt for target selection, (3) create GPT partition table with ESP + data partitions, (4) format ESP as FAT32, (5) copy BOOTX64.EFI, kernel.elf, initrd, and boot.conf, (6) optionally format data partition. The script must handle: disk already has partitions (re-partition), disk is mounted (unmount first), and write verification (compare checksums). After completing all items, mark every item as `[x]`, and commit as `"infra: USB write script (Windows)"`. Add notes directly in this TODO section.

- [ ] Create `scripts/deploy/write-usb.ps1`
- [ ] List removable USB disks with `Get-Disk | Where-Object {$_.BusType -eq 'USB'}`
- [ ] Prompt: "Write to Disk X? THIS WILL ERASE ALL DATA. [Y/N]"
- [ ] `Clear-Disk -RemoveData -RemoveOEM` → `New-Partition` (ESP 64 MiB, Data remainder)
- [ ] Format ESP: `Format-Volume -FileSystem FAT32 -NewFileSystemLabel "BOOT"`
- [ ] Copy: `BOOTX64.EFI` → `X:\EFI\BOOT\`, `kernel.elf` → `X:\`, `boot.conf` → `X:\`
- [ ] Set `boot.conf` debug mode: default `debug=1` for development, `debug=0` for release
- [ ] Verify: compare source vs written file sizes
- [ ] Commit: `"infra: USB write script (Windows)"`

### 1.3 USB Write Script (Linux/WSL Host)

**Prompt:** Create `scripts/deploy/write-usb.sh` for writing the OS to a USB disk from Linux or WSL. Uses `fdisk`/`gdisk` for partitioning, `mkfs.fat` for formatting, and `mount`/`cp` for file copying. Must detect the correct block device, refuse to write to non-removable devices, and handle mounted partitions. After completing all items, mark every item as `[x]`, and commit as `"infra: USB write script (Linux)"`. Add notes directly in this TODO section.

- [ ] Create `scripts/deploy/write-usb.sh`
- [ ] List USB block devices: `lsblk --scsi | grep usb`
- [ ] Safety check: `cat /sys/block/sdX/removable` must be `1`
- [ ] `sgdisk --zap-all`, create ESP (ef00) + data (8300) partitions
- [ ] `mkfs.fat -F32 /dev/sdX1` (ESP), `mkfs.fat -F32 /dev/sdX2` (data)
- [ ] Mount, copy BOOTX64.EFI + kernel.elf + initrd + boot.conf, unmount
- [ ] `sync` before unmount to ensure write completion
- [ ] Commit: `"infra: USB write script (Linux)"`

---

## 2. UEFI Bootloader Hardening

### 2.1 GOP Mode Negotiation

**Prompt:** The current bootloader requests a specific GOP mode (1280×720×32bpp). On real hardware, this mode may not be available — the GOP protocol may offer different resolutions depending on the GPU and display. Implement robust mode negotiation: enumerate all available GOP modes, prefer 1280×720, fall back to the closest 16:9 mode ≥ 1024×768, and finally accept any mode ≥ 800×600. Log the selected mode to the boot params so the kernel knows the exact framebuffer dimensions. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: robust GOP mode negotiation"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-010-Bootloader.md §1.2` — GOP framebuffer initialization. This section
> hardens the existing implementation for real hardware variance.

- [ ] Enumerate all GOP modes: `gop->QueryMode()` for each index 0..MaxMode-1
- [ ] Build a sorted list of candidate modes (width×height×BPP)
- [ ] Preferred: 1280×720×32bpp (current default)
- [ ] Fallback chain: 1920×1080 → 1366×768 → 1024×768 → 800×600
- [ ] Reject modes with BPP < 24 or unusual pixel formats (BGR vs RGB — handle both)
- [ ] Set selected mode: `gop->SetMode()`
- [ ] Pass to kernel: `boot_info.fb_width`, `boot_info.fb_height`, `boot_info.fb_bpp`, `boot_info.fb_pitch`
- [ ] Log: `"GOP: Selected mode %dx%d %dbpp (mode %d of %d available)"`
- [ ] Commit: `"boot: robust GOP mode negotiation"`

### 2.2 UEFI Memory Map Edge Cases

**Prompt:** The UEFI memory map varies dramatically between firmware implementations. Some BIOSes report overlapping regions, memory holes in unexpected places, or unusual EfiMemoryType values. The PMM initialization must handle: (1) overlapping regions (merge them), (2) memory holes within the first 16 MiB (avoid for DMA buffers), (3) EfiPersistentMemory type (treat as available), (4) firmware-reserved regions above 4 GiB that appear usable but aren't. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: handle UEFI memory map edge cases"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-010-Bootloader.md §1.3` — memory map discovery and identity paging.

- [ ] Sort memory map by physical address before processing
- [ ] Handle overlapping regions: merge adjacent/overlapping EfiConventionalMemory
- [ ] Reject regions below 1 MiB (legacy area — BIOS data, video ROM)
- [ ] Log total usable memory: `"PMM: %d MiB usable across %d regions"`
- [ ] Handle EfiPersistentMemory (type 14): treat as conventional memory
- [ ] Validate page table identity mapping covers all usable regions
- [ ] Test on: QEMU (OVMF), real Intel hardware, real AMD hardware
- [ ] Commit: `"boot: handle UEFI memory map edge cases"`

### 2.3 Secure Boot Compatibility

**Prompt:** Real hardware with Secure Boot enabled will refuse to load unsigned BOOTX64.EFI. While full Authenticode signing is a long-term goal, the OS must gracefully handle this: detect Secure Boot state, display a clear error message via GOP if Secure Boot prevents loading, and document the manual BIOS settings required to disable Secure Boot. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: Secure Boot detection and guidance"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-008-Hyper-V-Runner.md §1` — Hyper-V Gen 2 also requires Secure Boot OFF.

- [ ] Query `EFI_GLOBAL_VARIABLE` for `SecureBoot` variable (UINT8: 0=off, 1=on)
- [ ] If Secure Boot ON and loader is unsigned: display GOP error screen with instructions
- [ ] Error message: "Secure Boot is enabled. Please disable in BIOS/UEFI settings."
- [ ] Document per-vendor disable steps: Dell (F2→Security), HP (F10→Security), Lenovo (F1→Security)
- [ ] *(Stretch)* Self-signed EFI binary with `sbsign` + MOK enrollment
- [ ] Commit: `"boot: Secure Boot detection and guidance"`

---

## 3. Interrupt Routing & Timer Calibration

### 3.1 PIC Mask Before APIC Init

**Prompt:** On real hardware, the 8259 PIC may fire spurious IRQ7 (or IRQ15) during the transition from PIC to APIC mode. The current code masks the PIC, but some BIOSes partially unmask it during ACPI table parsing. Ensure the PIC mask sequence is: (1) save current PIC masks, (2) mask all PIC interrupts (OCW1: 0xFF to both PICs), (3) remap PIC to vectors 0x20-0x2F (even though masked — prevents vector collision), (4) initialize LAPIC + IOAPIC, (5) verify PIC stays masked. The MADT `PCAT_COMPAT` flag (bit 0) indicates whether the PIC is present at all — on pure APIC systems (Hyper-V Gen 2), skip PIC init entirely. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: robust PIC→APIC transition for real hardware"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-080-Drivers.md §2.2` — MADT PCAT_COMPAT flag and APIC-only mode.
> → XREF: `TODO-005-Debug.md §3.4` — ACPI MADT dump must log the PCAT_COMPAT flag.
> → XREF: `TODO-008-Hyper-V-Runner.md §2` — Hyper-V Gen 2 clears PCAT_COMPAT.

- [ ] Read MADT flags offset 36, bit 0 (PCAT_COMPAT):
  - [ ] If set (1): legacy 8259 PIC exists — mask and remap before APIC init
  - [ ] If clear (0): pure APIC system — skip all PIC code
- [ ] PIC mask sequence: `outb(0x21, 0xFF)` + `outb(0xA1, 0xFF)` **immediately** in `kernel_main()`
- [ ] Remap PIC vectors to 0x20-0x2F (prevent ISA IRQ collision with CPU exceptions)
- [ ] Install spurious IRQ7 handler: log and return with no EOI (edge-triggered spurious)
- [ ] Verify: QEMU + real hardware — no spurious interrupt crashes during APIC init
- [ ] Boot log: `"APIC: PCAT_COMPAT=%d, PIC %s"` (masked / not present)
- [ ] Commit: `"kernel: robust PIC→APIC transition for real hardware"`

### 3.2 PIT Timer Calibration Fix

**Prompt:** The PIT timer calibration hangs on some real hardware (Acer laptops, older Dell desktops). The current calibration loop is too tight: it programs PIT Channel 2 and busy-waits for the counter to expire, but some BIOSes initialize PIT Channel 2 in a non-standard state. Fix: (1) explicitly program PIT Channel 2 to mode 0 (one-shot) before calibration, (2) add a timeout (max 100ms wall-clock via TSC or simple loop counter) to prevent infinite hangs, (3) fall back to a safe default frequency (100 Hz) if calibration fails. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: fix PIT calibration hang on real hardware"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-010-Bootloader.md §1.5` — the LAPIC timer uses PIT calibration results.
> A hang here blocks the entire boot sequence.

- [ ] Before calibration: program PIT Chan 2 explicitly — Command 0xB0, Mode 0 (one-shot)
- [ ] Set Gate 2 high: `inb(0x61) | 0x01`, then `outb(0x61, ...)`
- [ ] Add timeout counter: max ~50,000,000 loop iterations (≈100ms on modern CPUs)
- [ ] On timeout: log `"WARNING: PIT calibration timed out, using default 100 Hz"`
- [ ] Fall back to safe LAPIC timer frequency (1000 Hz from estimated 1 GHz bus)
- [ ] Test: QEMU (should calibrate normally), real hardware (should not hang)
- [ ] Boot log: `"LAPIC: calibrated frequency = %d Hz (PIT reference)"` or `"LAPIC: using default 1000 Hz (PIT calibration failed)"`
- [ ] Commit: `"kernel: fix PIT calibration hang on real hardware"`

### 3.3 LAPIC Timer Validation

**Prompt:** After bringing up the LAPIC timer via PIT calibration, validate that the timer is actually firing at the expected rate. On some systems, the LAPIC timer frequency varies between cores or drifts due to CPU frequency scaling. Add a sanity check: after 1 second (measured by PIT), verify the LAPIC timer has fired within ±10% of expected ticks. If outside tolerance, re-calibrate or warn. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: LAPIC timer validation"`. Add notes directly in this TODO section.

- [ ] After LAPIC timer start: count ticks for 1 PIT-measured second
- [ ] Expected: ~1000 ticks (if configured for 1kHz)
- [ ] If ticks < 900 or > 1100: log warning, re-calibrate once
- [ ] If still out of range after re-calibration: use PIT as fallback timer
- [ ] Boot log: `"LAPIC: tick validation = %d ticks/sec (expected %d, %s)"`
- [ ] Commit: `"kernel: LAPIC timer validation"`

---

## 4. AHCI Storage on Real Hardware

### 4.1 AHCI Controller Discovery via PCI

**Prompt:** The current AHCI driver assumes a specific PCI location or relies on QEMU's VirtIO. On real hardware, the AHCI controller can be on any PCI bus/device/function. Implement proper discovery: scan all PCI devices for class 01h (Mass Storage), subclass 06h (SATA), progIF 01h (AHCI 1.0). Map the ABAR (AHCI Base Address Register — BAR 5) and verify the HBA capability registers. Some systems have multiple AHCI controllers (e.g., chipset + add-in card). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ahci: PCI-based controller discovery"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-080-Drivers.md §3` — AHCI driver architecture. This section hardens
> it for real hardware variance beyond QEMU's single-controller setup.

- [ ] Scan PCI for class=01h, subclass=06h, progIF=01h (AHCI 1.0)
- [ ] Read BAR 5 (ABAR): MMIO base address for HBA registers
- [ ] Map ABAR into virtual memory (identity mapped — already done for first 4 GiB)
- [ ] Read HBA capabilities: `CAP` (number of ports, NCQ depth, 64-bit DMA support)
- [ ] Read `PI` (Ports Implemented) bitmask — don't assume port 0 exists
- [ ] For each implemented port: read `PxSSTS` (device detection and interface speed)
- [ ] Skip ports with no device (`DET != 3` or `IPM != 1`)
- [ ] Boot log: `"AHCI: controller at %02x:%02x.%d, %d ports, %d devices"`
- [ ] Commit: `"ahci: PCI-based controller discovery"`

### 4.2 AHCI Port Initialization & IDENTIFY

**Prompt:** For each attached AHCI device, perform proper port initialization: stop the port engine (CLB/FB must be stopped before modification), allocate Command List and FIS Receive structures in DMA-accessible memory (below 4 GiB if the controller doesn't support 64-bit addressing), start the port engine, then issue an ATA IDENTIFY DEVICE (0xEC) command to read the drive model string, serial number, capacity, and sector size. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ahci: port init + IDENTIFY DEVICE"`. Add notes directly in this TODO section.

> [!CAUTION]
> **Memory Rule:** Command List (1 KiB) and FIS Receive (256 bytes) must use
> `pmm_alloc_contiguous()` — they are DMA targets and must be physically contiguous
> and below 4 GiB. `kmalloc` is NOT safe for DMA buffers.

- [ ] Stop port engine: clear `PxCMD.ST` and `PxCMD.FRE`, wait for `CR=0` and `FR=0`
- [ ] Allocate Command List (1 KiB, 1 KiB aligned) via `pmm_alloc_contiguous()`
- [ ] Allocate FIS Receive buffer (256 bytes, 256-byte aligned) via `pmm_alloc_contiguous()`
- [ ] Allocate Command Table (per-slot, 128 bytes + PRDT) via `pmm_alloc_contiguous()`
- [ ] Write CLB and FB addresses to `PxCLB`/`PxCLBU` and `PxFB`/`PxFBU`
- [ ] Start port engine: set `PxCMD.FRE` then `PxCMD.ST`
- [ ] Issue ATA IDENTIFY DEVICE (cmd 0xEC): build command header + FIS + PRDT
- [ ] Parse IDENTIFY response: words 27-46 (model), 10-19 (serial), 100-103 (LBA capacity), 106 (sector size)
- [ ] Boot log: `"AHCI port %d: %s (%s), %d GiB, %d-byte sectors"`
- [ ] Commit: `"ahci: port init + IDENTIFY DEVICE"`

### 4.3 DMA Read/Write Operations

**Prompt:** Implement DMA-based sector read/write using AHCI commands. Build a Command FIS (Host-to-Device FIS, type 0x27) with the LBA address and sector count, attach a PRDT (Physical Region Descriptor Table) pointing to the target memory buffer, issue the command by setting the command slot bit in `PxCI`, and wait for completion via polling `PxCI` (or interrupt). Handle errors: check `PxIS` for task file errors, `PxTFD` for ATA error register. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ahci: DMA read/write operations"`. Add notes directly in this TODO section.

- [ ] `ahci_read(port, lba, count, buffer)` — DMA read sectors
- [ ] `ahci_write(port, lba, count, buffer)` — DMA write sectors
- [ ] Build H2D FIS: command=0x25 (READ DMA EXT) / 0x35 (WRITE DMA EXT), LBA 48-bit
- [ ] PRDT entry: physical address of buffer, byte count, interrupt-on-completion bit
- [ ] Issue command: set bit in `PxCI`, poll until `PxCI` bit clears
- [ ] Error handling: check `PxIS.TFES` (Task File Error), read `PxTFD` error register
- [ ] Timeout: 5 seconds per command — log error and return failure
- [ ] Wire into `blkdev` abstraction layer: `blkdev_read()`/`blkdev_write()` → AHCI
- [ ] Commit: `"ahci: DMA read/write operations"`

### 4.4 USB Mass Storage Fallback

**Prompt:** When booting from USB, the OS filesystem may be on the USB disk itself (partition 2). The USB mass storage class uses SCSI commands over USB bulk transfers — significantly more complex than AHCI. For initial real hardware support, the UEFI bootloader should load the entire initrd (IXFS image) into memory before `ExitBootServices()`, so the kernel doesn't need a USB driver to access the OS filesystem. The UEFI Block I/O protocol handles USB transparently. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: load initrd via UEFI Block I/O"`. Add notes directly in this TODO section.

> [!NOTE]
> A native USB mass storage driver (XHCI + BOT/UAS) is a long-term goal tracked in
> `TODO-080-Drivers.md`. For now, the UEFI-loaded initrd avoids this complexity.

- [ ] Bootloader: load initrd.img from ESP via UEFI Simple File System Protocol
- [ ] Bootloader: pass initrd physical address + size in `boot_info.initrd_*`
- [ ] Kernel: mount initrd as in-memory IXFS volume at `C:\`
- [ ] After ExitBootServices: USB is inaccessible without a native driver
- [ ] Document limitation: OS filesystem must fit in the initrd (currently ~64 MiB)
- [ ] Commit: `"boot: load initrd via UEFI Block I/O"`

---

## 5. SMP Bring-up on Real Hardware

### 5.1 AP Boot Trampoline

**Prompt:** The INIT/SIPI sequence requires a trampoline page in the first 1 MiB of physical memory (below 0x100000). The trampoline must contain 16-bit real mode code that transitions the AP through protected mode to long mode, then jumps to the kernel's AP entry point. On real hardware, the trampoline page must be reserved before PMM initialization to prevent it from being allocated. Copy the trampoline code to 0x8000 (conventional memory, safe on all x86 systems). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"smp: AP boot trampoline for real hardware"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-020-Threading-Synchronization.md` — SMP initialization and scheduling.

- [ ] Reserve trampoline page (0x8000) in PMM — mark as used before PMM init completes
- [ ] Write 16-bit trampoline code: `ap_trampoline.asm` (NASM syntax)
  - [ ] Real mode → Protected mode (set PE bit in CR0)
  - [ ] Protected mode → Long mode (set LME in IA32_EFER, enable PAE, load CR3, set PG)
  - [ ] Jump to 64-bit AP entry point in kernel
- [ ] Copy trampoline code to 0x8000 at runtime (not linked at that address)
- [ ] Each AP gets its own kernel stack (8 KiB, allocated via PMM)
- [ ] AP entry: initialize LAPIC, announce via shared `volatile` counter
- [ ] Boot log: `"SMP: %d APs online (total %d logical CPUs)"`
- [ ] Commit: `"smp: AP boot trampoline for real hardware"`

### 5.2 Per-CPU Data Structures

**Prompt:** Each CPU core needs private data: its LAPIC ID, kernel stack pointer, current task pointer, and local scheduling queue. Use the GS segment base (via MSR `IA32_GS_BASE`) to point to a per-CPU data area. The BSP sets this up during early init; each AP sets its own GS base during the SIPI bring-up. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"smp: per-CPU data via GS segment"`. Add notes directly in this TODO section.

- [ ] Define `struct per_cpu` (lapic_id, stack_top, current_task, idle_ticks, nested_cli_count)
- [ ] Allocate one `struct per_cpu` per logical CPU (PMM or static array)
- [ ] BSP: `wrmsr(IA32_GS_BASE, &per_cpu[0])` during kernel init
- [ ] Each AP: `wrmsr(IA32_GS_BASE, &per_cpu[ap_index])` during SIPI bring-up
- [ ] Accessor macros: `this_cpu()` reads GS:0 → `struct per_cpu*`
- [ ] Use in scheduler: `this_cpu()->current_task` replaces global `current_task`
- [ ] Boot log: `"CPU %d: LAPIC ID %d, stack at %p"`
- [ ] Commit: `"smp: per-CPU data via GS segment"`

### 5.3 RCU Synchronization for SMP Scalability

**Prompt:** Read-Copy-Update (RCU) enables lock-free reads of shared kernel data structures (VFS mount table, process list, Registry cache). Writers create a copy, modify the copy, atomically swap the pointer, then free the old version after all readers have finished (a "grace period"). For Impossible OS: implement a simple quiescent-state–based RCU where each CPU reports a quiescent state during context switches. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: RCU synchronization"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-020-Threading-Synchronization.md §6` — seqlocks are already implemented.
> RCU complements seqlocks for different read/write patterns.

- [ ] Define `rcu_read_lock()` / `rcu_read_unlock()` — increment/decrement per-CPU counter
- [ ] `synchronize_rcu()` — wait until all CPUs pass a quiescent state (context switch)
- [ ] `call_rcu(callback, data)` — defer callback until after grace period
- [ ] Per-CPU grace period counter: incremented on context switch
- [ ] `synchronize_rcu()`: snapshot all counters, wait until all have advanced
- [ ] Apply to VFS mount table: readers use `rcu_read_lock()`, mount/unmount use RCU update
- [ ] Commit: `"kernel: RCU synchronization"`

---

## 6. Framebuffer & Compositor on Real Hardware

### 6.1 Pixel Format Detection

**Prompt:** UEFI GOP reports pixel format via `EFI_GRAPHICS_PIXEL_FORMAT` enum: `PixelRedGreenBlueReserved8BitPerColor` (RGB), `PixelBlueGreenRedReserved8BitPerColor` (BGR), or `PixelBitMask` (custom). The compositor currently assumes BGRA (0xAARRGGBB). On some hardware (Intel integrated GPUs), the format may be RGBA. Detect the format in the bootloader and pass it to the kernel, then adapt the compositor's pixel packing. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"gfx: pixel format detection from GOP"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-110-UI-Framework.md §1` — all GFX rendering functions must respect
> the pixel format. A BGR vs RGB mismatch causes red/blue color swap.

- [ ] Bootloader: read `gop->Mode->Info->PixelFormat` and `PixelInformation` (bit masks)
- [ ] Pass to kernel: `boot_info.fb_pixel_format` (enum: RGB, BGR, BITMASK)
- [ ] If BITMASK: extract red/green/blue channel positions from `EFI_PIXEL_BITMASK`
- [ ] Compositor: use pixel format to pack ARGB → framebuffer pixel correctly
- [ ] Test: QEMU (BGRA), Intel real hardware (may be RGBA)
- [ ] Boot log: `"Framebuffer: %dx%d %dbpp, format=%s"` (RGB/BGR/BITMASK)
- [ ] Commit: `"gfx: pixel format detection from GOP"`

### 6.2 VRAM Write Performance

**Prompt:** Writing to Video RAM (VRAM) over PCI-Express is significantly slower than writing to system RAM — VRAM is uncacheable (Write-Combining at best). The double-buffer compositor mitigates this by compositing in system RAM and doing a single blit to VRAM per frame. Validate that the blit performance is acceptable on real hardware: measure the time for a full 1280×720×4 = 3.6 MiB memcpy to VRAM. If >16ms (below 60fps), consider SSE2-optimized `movntdq` (non-temporal stores) which bypass cache and use the CPU's write-combine buffers efficiently. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"gfx: optimize VRAM blit for real hardware"`. Add notes directly in this TODO section.

- [ ] Measure VRAM blit time: TSC before/after `fb_swap()`, log in milliseconds
- [ ] If blit time > 8ms: implement SSE2 non-temporal blit (`movntdq` + `sfence`)
- [ ] `fb_swap_sse2()`: 128-bit aligned stores, 16 bytes per instruction, unrolled loop
- [ ] Ensure back-buffer is 16-byte aligned (PMM allocation already page-aligned ✅)
- [ ] Dirty-rect optimization: only blit changed regions via `fb_swap_rect()`
- [ ] Boot log: `"Framebuffer: blit time = %d.%dms (%s)"` (SSE2/memcpy)
- [ ] Commit: `"gfx: optimize VRAM blit for real hardware"`

---

## 7. Input Device Compatibility

### 7.1 PS/2 Keyboard on Real Hardware

**Prompt:** PS/2 keyboard works in QEMU but real hardware may have initialization timing differences. Some BIOSes disable the PS/2 controller during UEFI boot and rely on USB HID. Ensure the PS/2 controller is explicitly re-enabled during kernel init: send 0xAE (Enable Keyboard) to port 0x64, flush the output buffer, then install IRQ1 handler. Some keyboards need a reset (0xFF) command with ACK (0xFA) response. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"keyboard: PS/2 init hardening for real hardware"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-060-Keyboard.md` — PS/2 keyboard driver implementation.

- [ ] Send 0xAE to port 0x64 (enable first PS/2 port)
- [ ] Flush output buffer: read port 0x60 until status bit 0 clears
- [ ] Send 0xFF (reset) to port 0x60, wait for ACK (0xFA) + self-test pass (0xAA)
- [ ] Timeout after 500ms if no ACK — log warning, continue without reset
- [ ] Enable scan code translation: send 0x20 to 0x64, read config, set bit 6, write back
- [ ] Enable IRQ1: set bit 0 in controller config byte (port 0x64 → 0x20/0x60)
- [ ] Test on: QEMU, real PS/2 keyboard, USB keyboard with legacy PS/2 emulation
- [ ] Commit: `"keyboard: PS/2 init hardening for real hardware"`

### 7.2 PS/2 Mouse on Real Hardware

**Prompt:** Similar to keyboard, the PS/2 mouse (IRQ12) may need explicit re-initialization on real hardware. Touchpads on laptops typically expose PS/2 emulation but may need Synaptics or ALPS-specific initialization sequences for multi-touch or scrolling. For initial support, use standard PS/2 mouse protocol only. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"mouse: PS/2 init hardening for real hardware"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-070-Mouse.md` — PS/2 mouse driver implementation.

- [ ] Send 0xA8 to port 0x64 (enable second PS/2 port — mouse)
- [ ] Send 0xF4 (enable data reporting) to mouse via port 0x64→0xD4→0x60
- [ ] Wait for ACK (0xFA) with timeout
- [ ] Enable IRQ12: set bit 1 in controller config byte
- [ ] Handle mouse packet format: 3 bytes (flags, X delta, Y delta)
- [ ] *(Stretch)* Detect IntelliMouse (scroll wheel): send magic sequence 0xF3,200,0xF3,100,0xF3,80 → device ID changes from 0 to 3
- [ ] Commit: `"mouse: PS/2 init hardening for real hardware"`

### 7.3 USB HID Input *(Stretch)*

**Prompt:** Most modern keyboards and mice use USB, not PS/2. On systems without PS/2 emulation, no input is available. The minimal USB stack needed: XHCI host controller driver (PCI class 0Ch/03h/30h), USB device enumeration, HID class driver for keyboard and mouse. This is a significant effort tracked as a stretch goal. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: USB HID keyboard and mouse"`. Add notes directly in this TODO section.

> [!NOTE]
> → XREF: `TODO-080-Drivers.md` — USB host controller is tracked there as a long-term driver.

- [ ] *(Stretch)* PCI scan for XHCI controller (class 0Ch, subclass 03h, progIF 30h)
- [ ] *(Stretch)* XHCI initialization: CRCR, DCBAAP, command ring, event ring
- [ ] *(Stretch)* USB device enumeration: address assignment, GET_DESCRIPTOR
- [ ] *(Stretch)* HID class driver: parse HID report descriptor, handle keyboard/mouse reports
- [ ] *(Stretch)* Route USB keyboard scancodes to existing keyboard event queue
- [ ] *(Stretch)* Route USB mouse packets to existing mouse event queue
- [ ] Commit: `"drivers: USB HID keyboard and mouse"`

---

## 8. Real Hardware Test Checklist

### 8.1 Pre-Flight Checklist

Before each real hardware test:

- [ ] Build clean: `bash scripts/build.sh clean` → `=== BUILD OK ===`
- [ ] QEMU regression test: `bash scripts/build.sh run` → boots to desktop
- [ ] Write USB: `bash scripts/deploy/write-usb.ps1` (Windows) or `bash scripts/deploy/write-usb.sh` (Linux)
- [ ] Verify `boot.conf` on USB: `debug=1`
- [ ] BIOS settings: Secure Boot OFF, USB Boot enabled, Legacy Boot OFF (UEFI only)

### 8.2 Boot Test Sequence

Test each stage independently, checking the numbered boot log on X: after each attempt:

| Stage | What to Check | Pass Criteria | Log Evidence |
|-------|---------------|---------------|--------------|
| 1. UEFI Load | bootx64.efi loads | Boot splash appears | — |
| 2. GOP Init | Framebuffer active | Correct resolution (1280×720 or fallback) | `GOP: Selected mode` |
| 3. Kernel Entry | kernel_main() reached | First klog message | `[000.000] Impossible OS` |
| 4. Memory Init | PMM + kmalloc work | Memory totals logged | `PMM: NNN MiB usable` |
| 5. APIC Init | No spurious IRQ crash | LAPIC timer starts | `APIC: PCAT_COMPAT=` |
| 6. Timer | PIT calibration completes | No hang | `LAPIC: calibrated frequency` |
| 7. Storage | AHCI/FAT32 mount | X: and C: accessible | `AHCI: controller at` |
| 8. Input | Keyboard works | Key presses register | Typing produces output |
| 9. Desktop | Compositor renders | Desktop visible | Wallpaper + taskbar drawn |
| 10. Logging | Logs written to USB | `BOOT_NNN.LOG` exists | File contains boot log |

### 8.3 Hardware Compatibility Log

After each test, update the test machine table (§ Current Architecture Audit → Test Machines) with results. Record:
- Machine make/model/year
- CPU vendor + model
- RAM size
- GPU (discrete vs integrated)
- Boot result: ✅ Full boot / ⚠️ Partial / ❌ Failed
- Failure point (if any)
- Notes (BIOS version, special settings)

---

## Cross-References

| This TODO Section       | Depends On                        | Other TODO File                    |
|-------------------------|-----------------------------------|------------------------------------|
| §1.1 USB Disk Layout    | UEFI bootloader + ESP             | `TODO-010-Bootloader.md §1`       |
| §1.1 Boot Logs          | Numbered boot logs on X:          | `TODO-005-Debug.md §1.2`          |
| §2.1 GOP Negotiation    | GOP framebuffer init              | `TODO-010-Bootloader.md §1.2`     |
| §2.2 Memory Map         | UEFI memory discovery             | `TODO-010-Bootloader.md §1.3`     |
| §2.3 Secure Boot        | Hyper-V Gen 2 Secure Boot         | `TODO-008-Hyper-V-Runner.md §1`   |
| §3.1 PIC/APIC           | MADT PCAT_COMPAT flag             | `TODO-080-Drivers.md §2.2`        |
| §3.1 MADT Dump          | ACPI table dump                   | `TODO-005-Debug.md §3.4`          |
| §3.2 PIT Calibration    | LAPIC timer calibration           | `TODO-010-Bootloader.md §1.5`     |
| §4.1 AHCI Discovery     | AHCI driver architecture          | `TODO-080-Drivers.md §3`          |
| §5.1 SMP Trampoline     | Threading and SMP                 | `TODO-020-Threading-Synchronization.md` |
| §5.3 RCU                | Seqlocks                          | `TODO-020-Threading-Synchronization.md §6` |
| §6.1 Pixel Format       | GFX rendering functions           | `TODO-110-UI-Framework.md §1`     |
| §7.1 PS/2 Keyboard      | Keyboard driver                   | `TODO-060-Keyboard.md`            |
| §7.2 PS/2 Mouse         | Mouse driver                      | `TODO-070-Mouse.md`               |

---

## Priority Order

| Priority | Section                         | Description                                    |
|----------|---------------------------------|------------------------------------------------|
| 🔴 P0   | 1.1 USB Disk Layout             | Can't test hardware without bootable USB       |
| 🔴 P0   | 1.2 USB Write Script (Windows)  | Primary dev environment is Windows             |
| 🔴 P0   | 3.2 PIT Calibration Fix         | Blocks boot on affected hardware               |
| 🔴 P0   | 3.1 PIC→APIC Transition         | Spurious IRQ crashes on real hardware           |
| 🟠 P1   | 2.1 GOP Mode Negotiation        | Resolution fails silently on some GPUs         |
| 🟠 P1   | 4.1 AHCI Discovery via PCI      | Storage access on real SATA hardware           |
| 🟠 P1   | 4.2 AHCI Port Init + IDENTIFY   | Read drive info, prepare for DMA               |
| 🟠 P1   | 4.3 DMA Read/Write              | Actual disk I/O on real hardware               |
| 🟠 P1   | 4.4 USB Mass Storage Fallback   | Boot from USB without native USB driver        |
| 🟠 P1   | 6.1 Pixel Format Detection      | Color swap (red↔blue) on Intel GPUs            |
| 🟠 P1   | 7.1 PS/2 Keyboard Hardening     | Input broken on some boards                    |
| 🟡 P2   | 1.3 USB Write Script (Linux)    | Secondary dev environment                      |
| 🟡 P2   | 2.2 Memory Map Edge Cases       | Rare BIOS quirks                               |
| 🟡 P2   | 2.3 Secure Boot Compatibility   | Guidance + detection                           |
| 🟡 P2   | 3.3 LAPIC Timer Validation      | Drift detection                                |
| 🟡 P2   | 5.1 AP Boot Trampoline          | Multi-core on real hardware                    |
| 🟡 P2   | 5.2 Per-CPU Data Structures     | SMP scalability                                |
| 🟡 P2   | 6.2 VRAM Write Performance      | Compositor performance tuning                  |
| 🟡 P2   | 7.2 PS/2 Mouse Hardening        | Touchpad compatibility                         |
| 🟢 P3   | 5.3 RCU Synchronization         | Advanced concurrency                           |
| 🟢 P3   | 7.3 USB HID Input               | Native USB keyboard/mouse (stretch)            |
| 🟢 P3   | 8.1-8.3 Test Checklist          | Documentation and process                      |

---

## Key Files

| File                                    | Purpose                                    |
|-----------------------------------------|--------------------------------------------|
| `scripts/deploy/write-usb.ps1`                | [NEW] USB write script (Windows)           |
| `scripts/deploy/write-usb.sh`                 | [NEW] USB write script (Linux/WSL)         |
| `src/boot/uefi/bootx64.c`              | [MODIFY] GOP negotiation, Secure Boot, initrd |
| `src/kernel/main.c`                     | [MODIFY] PIC mask, PIT fix, SMP bring-up   |
| `src/kernel/drivers/ahci.c`            | [NEW] AHCI controller + DMA driver         |
| `src/kernel/drivers/keyboard.c`        | [MODIFY] PS/2 init hardening               |
| `src/kernel/drivers/mouse.c`           | [MODIFY] PS/2 init hardening               |
| `src/kernel/smp.c`                      | [NEW] AP trampoline + per-CPU data         |
| `src/kernel/ap_trampoline.asm`          | [NEW] 16→64 bit AP startup code            |
| `src/kernel/rcu.c`                      | [NEW] Read-Copy-Update synchronization     |
| `include/kernel/per_cpu.h`             | [NEW] Per-CPU data structures              |

---

## OS Comparison

| Feature                    | Windows 11                    | Linux                        | Impossible OS                        |
|----------------------------|-------------------------------|------------------------------|--------------------------------------|
| UEFI boot (native)        | ✅ Windows Boot Manager       | ✅ GRUB2/systemd-boot        | ✅ Custom BOOTX64.EFI               |
| GOP mode negotiation      | ✅ Automatic best mode        | ✅ efifb / simplefb          | ⬜ §2.1 — hardcoded 1280×720       |
| Secure Boot support       | ✅ Signed bootloader          | ✅ shim + MOK                | ⬜ §2.3 — unsigned, detection only  |
| UEFI memory map handling  | ✅ HAL memory manager          | ✅ e820 + EFI memmap         | ⬜ §2.2 — basic, no edge cases     |
| PIC→APIC transition       | ✅ HAL handles both           | ✅ APIC driver + PIC compat  | ⬜ §3.1 — works but fragile        |
| PIT/TSC calibration       | ✅ HAL timer calibration       | ✅ Multiple fallback methods | ⬜ §3.2 — hangs on some hardware   |
| AHCI/SATA DMA             | ✅ storahci.sys               | ✅ libahci + libata          | ⬜ §4.1-4.3 — VirtIO only          |
| SMP bring-up (INIT/SIPI)  | ✅ HAL MP init                | ✅ smpboot.c                 | ⬜ §5.1 — untested on real HW      |
| RCU synchronization       | ✅ Pushlocks / EX resources   | ✅ Tree RCU / SRCU           | ⬜ §5.3 — **missing**              |
| Per-CPU data              | ✅ KPCR via GS segment       | ✅ per_cpu via GS segment    | ⬜ §5.2 — global state only        |
| Pixel format handling     | ✅ WDDM driver abstraction    | ✅ DRM/KMS format negotiation | ⬜ §6.1 — assumes BGRA            |
| VRAM blit optimization    | ✅ GPU-accelerated flip       | ✅ DRM page flip             | ⬜ §6.2 — memcpy, no SSE2         |
| USB HID keyboard/mouse    | ✅ usbhid.sys                 | ✅ usbhid + hid-generic      | ⬜ §7.3 — **missing** (stretch)    |
| USB bootable media tool   | ✅ Media Creation Tool        | ✅ dd / Ventoy               | ⬜ §1.2 — **missing**              |
| **Native PE32+ UEFI boot** | ⚠️ Via Windows Boot Mgr     | ❌ Needs GRUB/systemd-boot   | ✅ **Direct BOOTX64.EFI — zero chainloading** |
| **In-kernel compositor**  | ❌ WDDM user-mode subsystem   | ❌ X11/Wayland user process   | ✅ **Zero context-switch latency** |
| **Identity-mapped PMM**   | ❌ HAL virtual mapping         | ❌ buddy allocator + vmalloc  | ✅ **Direct physical access — no TLB miss on heap** |

> **After P0 items:** Impossible OS boots reliably on real hardware from USB with working
> keyboard, storage, and display.
> **After P1 items:** Full AHCI storage and robust input — real hardware is a first-class target.
> **After P2+P3 items:** Multi-core SMP, RCU, optimized compositor — production-grade bare metal.

---
