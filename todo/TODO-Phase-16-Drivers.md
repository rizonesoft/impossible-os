# Phase 16 — Driver System & Hardware Modules

> **Goal:** Build a loadable kernel module system so drivers can be loaded from disk
> at boot time. Classify all drivers as **built-in** (boot-critical) or **module**
> (loaded from `C:\System\Drivers\`). Port essential drivers from permissive-licensed
> open-source projects (SerenityOS BSD-2, tinyusb MIT, FreeBSD BSD-2) and implement
> remaining drivers from scratch using hardware datasheets.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (DMA buffers, ring descriptors, device memory). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

> [!IMPORTANT]
> **Licensing Rule:** Only port code from **MIT, BSD-2, BSD-3, ISC, Apache-2.0, or public domain** sources. **Never** copy GPL-2.0 code (Linux kernel). For GPL-only hardware, do clean-room implementation from public datasheets. Each ported file must retain the original copyright + license header.


---

## 1. Kernel Module Loader Infrastructure

### 1.1 Kernel Symbol Table

**Prompt:** The kernel symbol table exports functions that loadable modules can call. Create a `struct ksym_entry { const char *name; void *addr; }` array populated at compile time. Use a `EXPORT_SYMBOL(func)` macro that adds an entry to a `.ksymtab` linker section. Export ~50 core functions: printk, kmalloc/kfree, pmm_alloc/free, pci_read/write_config, idt_register_handler, pic_unmask_irq, inb/outb/inw/outw/inl/outl, virtual memory helpers, blkdev_register, and ethernet_receive. The module loader resolves imports by searching this table. After completing all items, create `docs/architecture/modules.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: symbol table for loadable modules"`.

- [ ] Create `src/kernel/ksymtab.c` and `include/kernel/ksymtab.h`
- [ ] Define `struct ksym_entry` (name string, function pointer)
- [ ] Implement `EXPORT_SYMBOL(func)` macro using linker section `.ksymtab`
- [ ] Implement `ksym_lookup(name)` — search symbol table by name, return address
- [ ] Export core functions (~50 symbols):
  - [ ] Printing: `printk`, `serial_write`
  - [ ] Memory: `kmalloc`, `kfree`, `krealloc`, `pmm_alloc_contiguous`, `pmm_free_contiguous`
  - [ ] PCI: `pci_read_config_*`, `pci_write_config_*`, `pci_enable_bus_mastering`, `pci_find_device`
  - [ ] Interrupts: `idt_register_handler`, `pic_unmask_irq`, `pic_send_eoi`
  - [ ] I/O: `inb`, `outb`, `inw`, `outw`, `inl`, `outl`
  - [ ] Block: `blkdev_register`
  - [ ] Network: `ethernet_receive`, `net_register_nic`
  - [ ] Framebuffer: `fb_get_width`, `fb_get_height`
  - [ ] Timer: `pit_get_ticks`, `pit_sleep_ms`
- [ ] Add `.ksymtab` section to linker script
- [ ] Commit: `"kernel: symbol table for loadable modules"`

### 1.2 ELF Relocatable Object Loader

**Prompt:** Loadable modules are compiled as ELF relocatable objects (`.o` files renamed to `.kmod`). The module loader reads a `.kmod` file from disk via VFS, parses its ELF header, loads `.text` (code) and `.data`/`.bss` sections into executable kernel memory, applies x86-64 relocations (R_X86_64_64, R_X86_64_PC32, R_X86_64_32S, R_X86_64_PLT32), and resolves undefined symbols against the kernel symbol table. PMM-allocated pages are identity-mapped and need execute permission. After loading, call the module's `module_init()` function. After completing all items, update `docs/architecture/modules.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: ELF module loader"`.

- [ ] Create `src/kernel/module.c` and `include/kernel/module.h`
- [ ] Define `struct loaded_module` (name, base addr, size, init/cleanup funcs)
- [ ] Implement ELF parser for relocatable objects:
  - [ ] Parse ELF header (verify `ET_REL`, `EM_X86_64`)
  - [ ] Enumerate section headers (`.text`, `.data`, `.rodata`, `.bss`, `.rela.*`, `.symtab`, `.strtab`)
  - [ ] Calculate total memory needed (sum all ALLOC sections)
- [ ] Allocate module memory via `pmm_alloc_contiguous()` (identity-mapped, executable)
- [ ] Copy section contents to allocated memory
- [ ] Apply relocations:
  - [ ] Parse `.rela.text` and `.rela.data` sections
  - [ ] Handle relocation types: `R_X86_64_64`, `R_X86_64_PC32`, `R_X86_64_32S`, `R_X86_64_PLT32`
  - [ ] Resolve `STB_GLOBAL` + `SHN_UNDEF` symbols via `ksym_lookup()`
- [ ] Find `module_init` symbol in loaded module, call it
- [ ] Implement `module_load(path)` — full load sequence
- [ ] Implement `module_unload(name)` — call cleanup, free memory
- [ ] Test: load a trivial test module that calls `printk("Hello from module!\n")`
- [ ] Commit: `"kernel: ELF module loader"`

### 1.3 Module Build System

**Prompt:** Each loadable driver compiles as a separate `.kmod` file. Add a `src/modules/` directory with per-driver subdirectories. Each module compiles with `-ffreestanding -nostdlib -nostdinc -c -fPIC -mcmodel=kernel` to produce a relocatable object. The Makefile installs `.kmod` files to the IXFS system partition at `C:\System\Drivers\`. A module header file `include/kernel/module.h` provides `MODULE_INIT(fn)`, `MODULE_CLEANUP(fn)`, `MODULE_NAME(str)`, and `MODULE_LICENSE(str)` macros. After completing all items, update `docs/architecture/modules.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"build: module compilation and installation"`.

- [ ] Create `src/modules/` directory structure
- [ ] Create `include/kernel/module.h` with macros:
  - [ ] `MODULE_INIT(fn)` — marks init function
  - [ ] `MODULE_CLEANUP(fn)` — marks cleanup function
  - [ ] `MODULE_NAME(str)` — module name string
  - [ ] `MODULE_LICENSE(str)` — license identifier (MIT, BSD-2, etc.)
- [ ] Add Makefile rules for compiling `.kmod` files:
  - [ ] Compile flags: `-ffreestanding -nostdlib -nostdinc -c -fPIC -mcmodel=kernel`
  - [ ] Include kernel headers via `-I include/`
  - [ ] Output: `build/modules/driver_name.kmod`
- [ ] Install `.kmod` files to IXFS at `C:\System\Drivers\` during disk creation
- [ ] Commit: `"build: module compilation and installation"`

### 1.4 Driver Model & PCI Match Tables

**Prompt:** The driver model provides a standard lifecycle for PCI device drivers. `struct driver` has name, PCI match table (array of vendor/device ID pairs), probe function (called when matching device found), and remove function (called on unload). At boot, after PCI enumeration, the kernel walks all PCI devices and calls `driver_match_pci(dev)` to find a matching driver. For built-in drivers, the match table is checked at kernel init. For modules, it is checked when the module loads. `driver_register(drv)` adds a driver to the global list and scans existing PCI devices for matches. After completing all items, update `docs/architecture/modules.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: driver model with PCI match tables"`.

- [ ] Create `src/kernel/driver_model.c` and `include/kernel/driver_model.h`
- [ ] Define `struct pci_match` (vendor_id, device_id — 0 = wildcard)
- [ ] Define `struct driver` (name, license, match_table, probe, remove)
- [ ] Implement `driver_register(drv)` — add to global list, probe matching PCI devices
- [ ] Implement `driver_unregister(drv)` — call remove for each matched device
- [ ] Implement `driver_match_pci(dev)` — walk all registered drivers for a PCI device
- [ ] After PCI scan in `main.c`, call `driver_probe_all()` for built-in drivers
- [ ] After module load, call `driver_register()` from `module_init()`
- [ ] Commit: `"kernel: driver model with PCI match tables"`

### 1.5 Auto-Load Modules at Boot

**Prompt:** After the IXFS root filesystem mounts at `C:\`, scan `C:\System\Drivers\` and load all `.kmod` files via `module_load()`. Log each module load: `[OK] Loaded module: e1000.kmod (Intel PRO/1000, BSD-2)`. If a module fails to load, log a warning and continue (non-fatal). The load order doesn't matter if drivers use the PCI match model — each driver's probe runs when a matching PCI device exists. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: auto-load modules at boot"`.

- [ ] After `vfs_mount()` in `main.c`, call `module_load_all("C:\\System\\Drivers")`
- [ ] Scan directory for `.kmod` files
- [ ] Load each module via `module_load(path)`
- [ ] Log: `[OK] Loaded module: name.kmod (description, license)`
- [ ] Log: `[WARN] Failed to load module: name.kmod (reason)` — continue boot
- [ ] Commit: `"kernel: auto-load modules at boot"`

### 1.6 Proof of Concept: RTL8139 as First Module

**Prompt:** Convert the existing built-in RTL8139 NIC driver to a loadable module as the first test case. Move `src/kernel/drivers/rtl8139.c` to `src/modules/rtl8139/rtl8139.c`. Add a PCI match table entry for vendor `0x10EC`, device `0x8139`. Implement `module_init` that calls `driver_register(&rtl8139_driver)`, and `module_cleanup` that calls `driver_unregister()`. The driver's `probe()` function contains the existing init code. Verify: remove RTL8139 from the kernel, boot, confirm it loads from disk and networking works. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: convert RTL8139 to loadable module"`.

- [ ] Move `src/kernel/drivers/rtl8139.c` to `src/modules/rtl8139/rtl8139.c`
- [ ] Add PCI match table: `{ 0x10EC, 0x8139 }`
- [ ] Implement `module_init` → `driver_register(&rtl8139_driver)`
- [ ] Implement `module_cleanup` → `driver_unregister()`
- [ ] Move RTL8139 init code into `probe()` function
- [ ] Remove RTL8139 from kernel static build
- [ ] Test: boot without RTL8139 in kernel, verify it loads from `C:\System\Drivers\rtl8139.kmod`
- [ ] Verify: DHCP works, `ping` works after module loads
- [ ] Commit: `"drivers: convert RTL8139 to loadable module"`

---

## 2. Built-in Kernel Drivers (Boot-Critical)

> These drivers are statically linked into the kernel. They are needed **before**
> the filesystem is available and cannot be loaded as modules.

### Current status

| Driver | File | Status | Source |
|--------|------|--------|--------|
| PCI bus | `pci.c` | ✅ Done | Scratch |
| PIC (8259A) | `pic.c` | ✅ Done | Scratch |
| PIT timer | `pit.c` | ✅ Done | Scratch |
| Serial (UART) | `serial.c` | ✅ Done | Scratch |
| Framebuffer (VBE) | `framebuffer.c` | ✅ Done | Scratch |
| PS/2 keyboard | `keyboard.c` | ✅ Done | Scratch |
| PS/2 mouse | `mouse.c` | ✅ Done | Scratch |
| AHCI (SATA) | `ahci.c` | ✅ Done | Scratch |
| ATA/IDE | `ata.c` | ✅ Done | Scratch |
| VirtIO-blk | `virtio_blk.c` | ✅ Done | Scratch |
| RTC | `rtc.c` | ✅ Done | Scratch |
| ACPI (basic) | `acpi.c` | ✅ Done | Scratch |

### 2.1 NVMe Storage Driver (Built-in)

**Prompt:** NVMe (Non-Volatile Memory Express) is the standard for modern SSDs. Detect via PCI class `0x01`, subclass `0x08`, prog_if `0x02`. Map BAR0 for MMIO registers. NVMe uses a simple queue-pair model: Admin Queue for control commands, I/O Queues for read/write. Initialize: reset controller, configure Admin Queue (submission + completion), create I/O queue pair. Identify namespace to get disk capacity. Implement read/write via I/O submission queue entries (SQEs). Register as a block device for filesystem mounting. Port from SerenityOS NVMe driver (BSD-2). This is **built-in** because the boot disk may be NVMe. After completing all items, create `docs/architecture/nvme.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: NVMe storage (built-in)"`.

- [ ] Create `src/kernel/drivers/nvme.c` and `include/kernel/drivers/nvme.h`
- [ ] Port from: **SerenityOS** `Kernel/Devices/Storage/NVMe/` (BSD-2-Clause)
- [ ] Detect NVMe controller via PCI (class `0x01`, subclass `0x08`, prog_if `0x02`)
- [ ] Map MMIO BAR0 (controller registers)
- [ ] Initialize controller:
  - [ ] Disable controller (CC.EN = 0), wait for CSTS.RDY = 0
  - [ ] Configure Admin Queue: allocate submission + completion queue memory
  - [ ] Set AQA, ASQ, ACQ registers
  - [ ] Enable controller (CC.EN = 1), wait for CSTS.RDY = 1
- [ ] Send Identify Controller command → get capabilities
- [ ] Send Identify Namespace command → get disk size, LBA format
- [ ] Create I/O Completion Queue (Create I/O CQ command)
- [ ] Create I/O Submission Queue (Create I/O SQ command)
- [ ] Implement `nvme_read_sectors(lba, count, buf)` via I/O SQ
- [ ] Implement `nvme_write_sectors(lba, count, buf)` via I/O SQ
- [ ] Handle NVMe IRQ → process completion queue entries
- [ ] Register as block device (`blkdev_register`)
- [ ] Test: add QEMU flag `-drive file=test.img,if=none,id=nvme0 -device nvme,serial=deadbeef,drive=nvme0`
- [ ] Commit: `"drivers: NVMe storage (built-in)"`

### 2.2 APIC / IOAPIC (Built-in)

**Prompt:** The APIC (Advanced Programmable Interrupt Controller) replaces the legacy 8259 PIC for multi-core systems. The Local APIC is per-CPU (MMIO at `0xFEE00000`), handling timer, IPI, and local interrupts. The IOAPIC (typically at `0xFEC00000`) routes external device interrupts to specific CPUs. Parse ACPI MADT table for APIC and IOAPIC base addresses. Initialize: enable the Local APIC, configure the IOAPIC redirection table for all IRQs currently handled by the PIC. Disable the legacy PIC after APIC takeover. This is **built-in** because MSI interrupts (used by NVMe, xHCI, modern NICs) require APIC. After completing all items, create `docs/architecture/apic.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: APIC and IOAPIC interrupt controller"`.

- [ ] Create `src/kernel/apic.c` and `include/kernel/apic.h`
- [ ] Parse ACPI MADT table for Local APIC and IOAPIC info
- [ ] Initialize Local APIC:
  - [ ] Map MMIO at base address (typically `0xFEE00000`)
  - [ ] Enable APIC via Spurious Interrupt Vector Register
  - [ ] Configure APIC Timer (for scheduler, replacing PIT later)
- [ ] Initialize IOAPIC:
  - [ ] Map MMIO at base address (typically `0xFEC00000`)
  - [ ] Configure redirection table entries for IRQs 0-23
  - [ ] Map legacy IRQ numbers to IOAPIC inputs
- [ ] Implement `apic_send_eoi()` — end-of-interrupt for APIC
- [ ] Implement `ioapic_set_irq(irq, vector, cpu)` — route IRQ to specific CPU
- [ ] Disable legacy PIC (mask all PIC IRQs) after APIC is active
- [ ] Update existing IRQ handlers to use APIC EOI
- [ ] Commit: `"kernel: APIC and IOAPIC interrupt controller"`

### 2.3 HPET Timer (Built-in)

**Prompt:** The HPET (High Precision Event Timer) provides nanosecond-resolution timing, replacing the PIT for precise measurements. Find HPET via ACPI table (signature "HPET"). Map the MMIO registers. Read the period from the General Capabilities register, enable the main counter. Use comparator 0 for periodic timer interrupts (can replace PIT for scheduler). Provide `hpet_read_ns()` for high-resolution timestamps. This is optional but valuable for accurate profiling and timer precision. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: HPET high-precision timer"`.

- [ ] Create `src/kernel/hpet.c` and `include/kernel/hpet.h`
- [ ] Parse ACPI HPET table for base address
- [ ] Map MMIO registers
- [ ] Read main counter period from capabilities register
- [ ] Enable main counter
- [ ] Implement `hpet_read_ns()` — current time in nanoseconds
- [ ] *(Stretch)* Configure comparator 0 for periodic interrupts
- [ ] Commit: `"kernel: HPET high-precision timer"`

---

## 3. Network Drivers (Modules)

### 3.1 Intel e1000 NIC

**Prompt:** The Intel e1000 (82540EM) is the default NIC in VirtualBox and is extremely common in real hardware. Detect via PCI vendor `0x8086`, device `0x100E` (82540EM), `0x100F` (82545EM), `0x153A` (I217-LM), `0x10D3` (82574L). Map MMIO BAR0. Initialize: reset device, read MAC from EEPROM or RAL/RAH registers, configure TX/RX descriptor rings (16 entries each), enable TX/RX. TX: build descriptor with buffer pointer + length + EOP flag, update TDT tail. RX: pre-populate descriptors with buffers, poll/IRQ on RDH advance, pass frames to `ethernet_receive()`. Port from SerenityOS `E1000NetworkAdapter` (BSD-2). After completing all items, create `docs/architecture/e1000.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: Intel e1000 NIC module"`.

- [ ] Create `src/modules/e1000/e1000.c`
- [ ] Port from: **SerenityOS** `Kernel/Net/Intel/E1000NetworkAdapter.cpp` (BSD-2-Clause)
- [ ] PCI match table: `{ 0x8086, 0x100E }`, `{ 0x8086, 0x100F }`, `{ 0x8086, 0x153A }`, `{ 0x8086, 0x10D3 }`
- [ ] Map MMIO BAR0
- [ ] Reset device (set CTRL.RST bit, wait)
- [ ] Read MAC address from EEPROM or RAL/RAH registers
- [ ] Configure TX ring:
  - [ ] Allocate TX descriptor array (16 entries) + TX buffers
  - [ ] Set TDBAL/TDBAH (base address), TDLEN (length), TDH/TDT (head/tail)
- [ ] Configure RX ring:
  - [ ] Allocate RX descriptor array (16 entries) + RX buffers
  - [ ] Set RDBAL/RDBAH, RDLEN, RDH/RDT
  - [ ] Enable RX (RCTL.EN)
- [ ] Implement `e1000_send(frame, length)` — write TX descriptor, bump TDT
- [ ] Handle e1000 IRQ → process RX descriptors → `ethernet_receive()`
- [ ] Register with network stack
- [ ] Test in VirtualBox (default NIC is e1000)
- [ ] Commit: `"drivers: Intel e1000 NIC module"`

### 3.2 VirtIO-net NIC

**Prompt:** VirtIO-net is the paravirtual NIC for QEMU/KVM — much faster than RTL8139. Detect via PCI vendor `0x1AF4`, device `0x1000`. Reuse the VirtIO MMIO transport from `virtio.c`. Initialize two virtqueues: RX (queue 0) and TX (queue 1). Pre-populate RX queue with empty buffers. TX: build Ethernet frame, submit via descriptor chain, kick queue. RX interrupt: process used buffers, pass to `ethernet_receive()`. Read MAC from device config space. After completing all items, create `docs/architecture/virtio-net.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: VirtIO-net NIC module"`.

- [ ] Create `src/modules/virtio_net/virtio_net.c`
- [ ] PCI match: `{ 0x1AF4, 0x1000 }` (transitional) and `{ 0x1AF4, 0x1041 }` (modern)
- [ ] Reuse VirtIO transport from `virtio.c` (negotiate features, init virtqueues)
- [ ] Initialize RX virtqueue (queue 0) — pre-populate with empty buffers
- [ ] Initialize TX virtqueue (queue 1)
- [ ] Read MAC address from device config space
- [ ] Implement `virtio_net_send(frame, len)` — add to TX queue, kick
- [ ] Handle IRQ → process RX used buffers → `ethernet_receive()`
- [ ] Register with network stack
- [ ] QEMU flag: `-device virtio-net-pci,netdev=net0`
- [ ] Commit: `"drivers: VirtIO-net NIC module"`

### 3.3 Realtek RTL8169 Gigabit NIC

**Prompt:** The RTL8169 is a very common gigabit Ethernet controller found in budget motherboards and PCIe NICs. Detect via PCI vendor `0x10EC`, device `0x8169` (RTL8169), `0x8168` (RTL8111). Similar to RTL8139 but with DMA descriptor rings instead of a fixed buffer. Port from FreeBSD `re(4)` driver or SerenityOS RTL8168 driver (BSD-2). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: RTL8169 gigabit NIC module"`.

- [ ] Create `src/modules/rtl8169/rtl8169.c`
- [ ] Port from: **FreeBSD** `sys/dev/re/` (BSD-2) or **SerenityOS** (BSD-2)
- [ ] PCI match: `{ 0x10EC, 0x8169 }`, `{ 0x10EC, 0x8168 }`
- [ ] MMIO or I/O BAR access
- [ ] TX/RX descriptor rings with DMA
- [ ] Register with network stack
- [ ] Commit: `"drivers: RTL8169 gigabit NIC module"`

---

## 4. GPU / Display Drivers (Modules)

### 4.1 VMSVGA Display Driver

**Prompt:** VMSVGA is VirtualBox's GPU adapter, providing mode setting, 2D acceleration, and hardware cursor. Detect via PCI vendor `0x15AD`, device `0x0405`. Map I/O ports (SVGA_INDEX_PORT, SVGA_VALUE_PORT) and framebuffer BAR. Initialize: read SVGA_REG_ID, negotiate version, enable SVGA mode, set width/height/bpp. The FIFO (command buffer) at BAR2 enables 2D acceleration commands (RECT_FILL, RECT_COPY, UPDATE). Hardware cursor via SVGA_REG_CURSOR_*. Port from SerenityOS VMWareBackend (BSD-2). After completing all items, create `docs/architecture/vmsvga.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: VMSVGA display module"`.

- [ ] Create `src/modules/vmsvga/vmsvga.c`
- [ ] Port from: **SerenityOS** `Kernel/Graphics/VMWare/` (BSD-2-Clause)
- [ ] PCI match: `{ 0x15AD, 0x0405 }`
- [ ] Map I/O ports and framebuffer BAR
- [ ] Negotiate SVGA version via SVGA_REG_ID
- [ ] Set display mode: width, height, bpp
- [ ] Enable SVGA and read framebuffer offset
- [ ] Initialize FIFO command buffer (BAR2):
  - [ ] RECT_FILL — accelerated rectangle fill
  - [ ] RECT_COPY — accelerated screen blit
  - [ ] UPDATE — mark dirty regions
- [ ] Hardware cursor:
  - [ ] Define cursor image via SVGA_REG_CURSOR_*
  - [ ] Move cursor position
  - [ ] Show/hide cursor
- [ ] Register as display device (replaces VBE for page flips)
- [ ] Test in VirtualBox
- [ ] Commit: `"drivers: VMSVGA display module"`

### 4.2 VirtIO-GPU Driver

**Prompt:** VirtIO-GPU provides hardware cursor and 2D resource management in QEMU. Detect via PCI vendor `0x1AF4`, device `0x1050`. Uses controlq (VQ0) for display commands and cursorq (VQ1) for cursor updates. Commands: GET_DISPLAY_INFO, RESOURCE_CREATE_2D, RESOURCE_ATTACH_BACKING, SET_SCANOUT, TRANSFER_TO_HOST_2D, RESOURCE_FLUSH, UPDATE_CURSOR, MOVE_CURSOR. The driver code already exists (reverted) — restore and convert to a loadable module. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: VirtIO-GPU display module"`.

- [ ] Create `src/modules/virtio_gpu/virtio_gpu.c`
- [ ] Restore reverted VirtIO-GPU code as module
- [ ] PCI match: `{ 0x1AF4, 0x1050 }`
- [ ] Controlq (VQ0): display commands (scanout, resources, flush)
- [ ] Cursorq (VQ1): hardware cursor (update, move, hide)
- [ ] Register as display device
- [ ] QEMU flag: `-device virtio-vga` or `-device virtio-gpu-pci`
- [ ] Commit: `"drivers: VirtIO-GPU display module"`

### 4.3 Bochs/BGA Display Driver

**Prompt:** The Bochs Graphics Adapter (BGA) is the simplest GPU — just VBE DISPI I/O port registers for mode setting. Already used by the built-in framebuffer for page flipping. Extract the BGA-specific code to a display module that can set arbitrary resolutions, enable LFB, and do page flipping via Y_OFFSET. This works in QEMU with `-device VGA` and `-device bochs-display`. Low priority since the built-in VBE fallback already handles this. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: Bochs/BGA display module"`.

- [ ] *(Stretch)* Create `src/modules/bga/bga.c`
- [ ] *(Stretch)* Extract BGA DISPI register code from `framebuffer.c`
- [ ] *(Stretch)* Mode setting, page flipping, resolution change
- [ ] Commit: `"drivers: Bochs/BGA display module"`

---

## 5. Audio Drivers (Modules)

> Note: Audio drivers (AC97, Intel HDA) and the audio subsystem are defined in
> **Phase 08**. The following are additional audio drivers not covered there.

### 5.1 VirtualBox AC97 / Intel HDA

> These are covered by Phase 08 §1.1 (AC97) and §4.1 (Intel HDA).
> Build them as loadable modules using the module system from §1.

- [ ] Convert AC97 driver (Phase 08 §1.1) to loadable module format
- [ ] Convert Intel HDA driver (Phase 08 §4.1) to loadable module format

---

## 6. Input Drivers (Modules)

### 6.1 VirtIO-input (Tablet Mode)

**Prompt:** Convert the existing built-in VirtIO-input driver to a loadable module. Move `src/kernel/drivers/virtio_input.c` to `src/modules/virtio_input/`. Add PCI match for vendor `0x1AF4`, device `0x1052`. Module init registers the driver, probe contains current init code. After converting, remove VirtIO-input from the static kernel build. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: convert VirtIO-input to module"`.

- [ ] Move `src/kernel/drivers/virtio_input.c` to `src/modules/virtio_input/`
- [ ] PCI match: `{ 0x1AF4, 0x1052 }`
- [ ] Module init/cleanup lifecycle
- [ ] Remove from static kernel build
- [ ] Commit: `"drivers: convert VirtIO-input to module"`

---

## 7. Licensing & Attribution

### 7.1 License Tracking

**Prompt:** Create a `LICENSES/` directory in the project root with the full text of each license used by ported drivers. Create a `NOTICE.md` file listing each ported file, its original source, and its license. Each ported source file retains its original copyright header with an additional line noting the Impossible OS adaptation. After completing all items, mark every item as `[x]`, and commit as `"docs: license tracking for ported drivers"`.

- [ ] Create `LICENSES/` directory:
  - [ ] `LICENSES/MIT.txt`
  - [ ] `LICENSES/BSD-2-Clause.txt`
  - [ ] `LICENSES/BSD-3-Clause.txt`
  - [ ] `LICENSES/Apache-2.0.txt`
- [ ] Create `NOTICE.md` — table of ported files with source + license
- [ ] Ensure each ported file has original copyright + SPDX identifier:
  ```
  /* Originally from [Project] — Copyright (c) [year], [authors]
   * SPDX-License-Identifier: [license]
   *
   * Adapted for Impossible OS by Rizonesoft, 2026 */
  ```
- [ ] Commit: `"docs: license tracking for ported drivers"`

---

## Compatible Open-Source Porting Sources

| Source | License | Best Drivers | URL |
|--------|---------|-------------|-----|
| SerenityOS | BSD-2 | e1000, AC97, HDA, VMSVGA, NVMe | [github.com/SerenityOS/serenity](https://github.com/SerenityOS/serenity) |
| tinyusb | MIT | USB host (xHCI, EHCI), HID, MSC | [github.com/hathach/tinyusb](https://github.com/hathach/tinyusb) |
| FreeBSD | BSD-2 | Intel NIC (em/igb), RTL8169 (re) | [github.com/freebsd/freebsd-src](https://github.com/freebsd/freebsd-src) |
| OpenBSD | ISC | Clean NIC/storage drivers | [github.com/openbsd/src](https://github.com/openbsd/src) |
| ToaruOS | NCSA | AC97, e1000, VirtIO | [github.com/klange/toaruos](https://github.com/klange/toaruos) |
| ACPICA | BSD | Full ACPI implementation | [github.com/acpica/acpica](https://github.com/acpica/acpica) |

> [!WARNING]
> **NEVER** copy code from the **Linux kernel** (GPL-2.0). This would require relicensing the entire OS to GPL. For hardware where only Linux has a driver, do a clean-room implementation from public datasheets.

---

## Priority Order

| Priority | Section | Description |
|----------|---------|-------------|
| 🔴 P0 | 1.1 Kernel Symbol Table | Foundation — modules can't call kernel functions without it |
| 🔴 P0 | 1.2 ELF Module Loader | Foundation — load and relocate .kmod files |
| 🔴 P0 | 1.3 Module Build System | Foundation — compile drivers as .kmod |
| 🔴 P0 | 1.4 Driver Model | Foundation — PCI match, probe/remove lifecycle |
| 🔴 P0 | 1.5 Auto-Load at Boot | Foundation — scan C:\System\Drivers\ on boot |
| 🟠 P1 | 1.6 RTL8139 as Module | Proof of concept — validate entire pipeline |
| 🟠 P1 | 2.1 NVMe (built-in) | Real hardware SSD support |
| 🟠 P1 | 3.1 Intel e1000 Module | VirtualBox networking |
| 🟡 P2 | 4.1 VMSVGA Module | VirtualBox GPU + cursor + acceleration |
| 🟡 P2 | 4.2 VirtIO-GPU Module | QEMU GPU (restore reverted code) |
| 🟡 P2 | 3.2 VirtIO-net Module | Fast QEMU networking |
| 🟡 P2 | 2.2 APIC/IOAPIC (built-in) | Required for MSI, multi-core |
| 🟢 P3 | 6.1 VirtIO-input Module | Convert existing code |
| 🟢 P3 | 3.3 RTL8169 Module | Common real-world NIC |
| 🟢 P3 | 2.3 HPET Timer (built-in) | High-precision timing |
| 🟢 P3 | 7.1 License Tracking | Attribution compliance |
| 🔵 P4 | 4.3 Bochs/BGA Module | Simple fallback display |
| 🔵 P4 | 5.1 Audio as Modules | Convert Phase 08 drivers to modules |
