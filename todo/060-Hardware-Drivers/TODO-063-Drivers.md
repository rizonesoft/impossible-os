# 080-Drivers — Driver System & Hardware Modules

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

**Prompt:** The kernel symbol table exports functions that loadable modules can call. Create a `struct ksym_entry { const char *name; void *addr; }` array populated at compile time. Use a `EXPORT_SYMBOL(func)` macro that adds an entry to a `.ksymtab` linker section. Export ~50 core functions: printk, kmalloc/kfree, pmm_alloc/free, pci_read/write_config, idt_register_handler, pic_unmask_irq, inb/outb/inw/outw/inl/outl, virtual memory helpers, blkdev_register, and ethernet_receive. The module loader resolves imports by searching this table. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: symbol table for loadable modules"`. Add notes, gotchas, and design decisions directly in this TODO section covering the symbol table, EXPORT_SYMBOL macro, and linker section layout.

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

**Prompt:** Loadable modules are compiled as ELF relocatable objects (`.o` files renamed to `.kmod`). The module loader reads a `.kmod` file from disk via VFS, parses its ELF header, loads `.text` (code) and `.data`/`.bss` sections into executable kernel memory, applies x86-64 relocations (R_X86_64_64, R_X86_64_PC32, R_X86_64_32S, R_X86_64_PLT32), and resolves undefined symbols against the kernel symbol table. PMM-allocated pages are identity-mapped and need execute permission. After loading, call the module's `module_init()` function. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: ELF module loader"`. Add notes, gotchas, and design decisions directly in this TODO section covering the ELF loader, relocation types, and symbol resolution.

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

**Prompt:** Each loadable driver compiles as a separate `.kmod` file. Add a `src/modules/` directory with per-driver subdirectories. Each module compiles with `-ffreestanding -nostdlib -nostdinc -c -fPIC -mcmodel=kernel` to produce a relocatable object. The Makefile installs `.kmod` files to the IXFS system partition at `C:\System\Drivers\`. A module header file `include/kernel/module.h` provides `MODULE_INIT(fn)`, `MODULE_CLEANUP(fn)`, `MODULE_NAME(str)`, and `MODULE_LICENSE(str)` macros. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"build: module compilation and installation"`. Add notes, gotchas, and design decisions directly in this TODO section covering the module build system, compiler flags, and Makefile integration.

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

### 1.4 Driver Model, HAL & PCI Match Tables

> *Merged from Phase 01 §5 (Hardware Abstraction Layer)*

**Prompt:** The driver model provides a standard lifecycle for device drivers and a Hardware Abstraction Layer (HAL) that allows the kernel to work with different hardware through abstract interfaces. `struct driver` has name, PCI match table (array of vendor/device ID pairs), probe function (called when matching device found), and remove function (called on unload). The HAL defines generic ops structs — `blk_ops` (read_sectors, write_sectors, capacity), `net_ops` (send_packet, get_mac), and `input_ops` (poll_event) — that each real driver fills in with function pointers. At boot, after PCI enumeration, the kernel walks all PCI devices and calls `driver_match_pci(dev)`. `driver_register(drv)` adds a driver to the global list and scans existing PCI devices for matches. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: driver model with HAL and PCI match tables"`. Add notes, gotchas, and design decisions directly in this TODO section covering the driver model, HAL interfaces, and PCI match tables.

- [ ] Create `src/kernel/driver_model.c` and `include/kernel/driver_model.h`
- [ ] Define `struct pci_match` (vendor_id, device_id — 0 = wildcard)
- [ ] Define `struct driver` (name, license, match_table, probe, remove)
- [ ] Define `blk_ops` interface: `read(dev, lba, count, buf)`, `write(...)`, `capacity(dev)`
- [ ] Define `net_ops` interface: `send(dev, data, len)`, `set_mac(dev, mac)`, `get_mac(dev)`
- [ ] Define `input_ops` interface: `poll(dev)`, `get_event(dev, event)`
- [ ] Implement `driver_register(drv)` — add to global list, probe matching PCI devices
- [ ] Implement `driver_unregister(drv)` — call remove for each matched device
- [ ] Implement `driver_match_pci(dev)` — walk all registered drivers for a PCI device
- [ ] Register current ATA/AHCI driver as a `blk_ops` implementation
- [ ] Register current RTL8139 driver as a `net_ops` implementation
- [ ] Register current keyboard/mouse as `input_ops` implementations
- [ ] After PCI scan in `main.c`, call `driver_probe_all()` for built-in drivers
- [ ] After module load, call `driver_register()` from `module_init()`
- [ ] Commit: `"kernel: driver model with HAL and PCI match tables"`

### 1.5 Auto-Load Modules at Boot

**Prompt:** After the IXFS root filesystem mounts at `C:\`, scan `C:\System\Drivers\` and load all `.kmod` files via `module_load()`. Log each module load: `[OK] Loaded module: e1000.kmod (Intel PRO/1000, BSD-2)`. If a module fails to load, log a warning and continue (non-fatal). The load order doesn't matter if drivers use the PCI match model — each driver's probe runs when a matching PCI device exists. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: auto-load modules at boot"`. Add notes, gotchas, and design decisions directly in this TODO section covering the boot-time module scan, logging, and error handling.

- [ ] After `vfs_mount()` in `main.c`, call `module_load_all("C:\\System\\Drivers")`
- [ ] Scan directory for `.kmod` files
- [ ] Load each module via `module_load(path)`
- [ ] Log: `[OK] Loaded module: name.kmod (description, license)`
- [ ] Log: `[WARN] Failed to load module: name.kmod (reason)` — continue boot
- [ ] Commit: `"kernel: auto-load modules at boot"`

### 1.6 Proof of Concept: RTL8139 as First Module

**Prompt:** Convert the existing built-in RTL8139 NIC driver to a loadable module as the first test case. Move `src/kernel/drivers/rtl8139.c` to `src/modules/rtl8139/rtl8139.c`. Add a PCI match table entry for vendor `0x10EC`, device `0x8139`. Implement `module_init` that calls `driver_register(&rtl8139_driver)`, and `module_cleanup` that calls `driver_unregister()`. The driver's `probe()` function contains the existing init code. Verify: remove RTL8139 from the kernel, boot, confirm it loads from disk and networking works. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"drivers: convert RTL8139 to loadable module"`. Add notes, gotchas, and design decisions directly in this TODO section covering the RTL8139 module conversion and verification steps.

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

| Driver              | File               | Status  | Source  |
| ------------------- | ------------------ | :-----: | ------- |
| PCI bus             | `pci.c`            | ✅ Done | Scratch |
| PIC (8259A)         | `pic.c`            | ✅ Done | Scratch |
| PIT timer           | `pit.c`            | ✅ Done | Scratch |
| Serial (UART)       | `serial.c`         | ✅ Done | Scratch |
| Framebuffer (VBE)   | `framebuffer.c`    | ✅ Done | Scratch |
| PS/2 keyboard       | `keyboard.c`       | ✅ Done | Scratch |
| PS/2 mouse          | `mouse.c`          | ✅ Done | Scratch |
| AHCI (SATA)         | `ahci.c`           | ✅ Done | Scratch |
| ATA/IDE             | `ata.c`            | ✅ Done | Scratch |
| VirtIO-blk          | `virtio_blk.c`     | ✅ Done | Scratch |
| RTC                 | `rtc.c`            | ✅ Done | Scratch |
| ACPI tables + power | `acpi.c`           | ✅ Done | Scratch |
| LAPIC (per-CPU)     | `lapic.c`          | ✅ Done | Scratch |
| IOAPIC (IRQ routing)| `ioapic.c`         | ✅ Done | Scratch |
| ACPI shutdown/reboot| `acpi.c`           | ✅ Done | Scratch |
| SMP boot (SIPI)     | `smp.c`            | ✅ Done | Scratch |

### 2.1 NVMe Storage Driver (Built-in)

**Prompt:** NVMe (Non-Volatile Memory Express) is the standard for modern SSDs. Detect via PCI class `0x01`, subclass `0x08`, prog_if `0x02`. Map BAR0 for MMIO registers. NVMe uses a simple queue-pair model: Admin Queue for control commands, I/O Queues for read/write. Initialize: reset controller, configure Admin Queue (submission + completion), create I/O queue pair. Identify namespace to get disk capacity. Implement read/write via I/O submission queue entries (SQEs). Register as a block device for filesystem mounting. Port from SerenityOS NVMe driver (BSD-2). This is **built-in** because the boot disk may be NVMe. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"drivers: NVMe storage (built-in)"`. Add notes, gotchas, and design decisions directly in this TODO section covering the NVMe driver initialization, queue-pair model, and QEMU test flags.

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

### 2.2 APIC / IOAPIC (Built-in) — ✅ Done

> [!NOTE]
> → **XREF:** Full APIC enhancement roadmap (x2APIC, timer calibration, TLB shootdown,
> MSI/MSI-X, NMI watchdog, interrupt profiler) is in
> [`TODO-063.09-APIC-Architecture.md`](TODO-063-Drivers/TODO-063.09-APIC-Architecture.md).

The core APIC subsystem is implemented across `lapic.c`, `ioapic.c`, `acpi.c`, and `pic.c`:

- [x] ACPI MADT parsing — LAPIC IDs, x2APIC (type 9), IOAPIC base, ISOs, LAPIC NMI
- [x] LAPIC init — SVR, TPR, ESR, EOI, IPI send (xAPIC MMIO mode)
- [x] IOAPIC init — routes 16 ISA IRQs with ISO polarity/trigger flags to BSP
- [x] PCAT_COMPAT check — `acpi_pcat_compat()` skips PIC when flag is 0
- [x] LAPIC timer — periodic mode, vector 32 (hardcoded ICR, calibration in §063.09 §2)
- [x] SMP boot — INIT-SIPI-SIPI sequence via `smp.c`
- [x] PIC conditional disable — PIC masked only when PCAT_COMPAT=1

### 2.3 HPET Timer (Built-in)

**Prompt:** The HPET (High Precision Event Timer) provides nanosecond-resolution timing, replacing the PIT for precise measurements. Find HPET via ACPI table (signature "HPET"). Map the MMIO registers. Read the period from the General Capabilities register, enable the main counter. Use comparator 0 for periodic timer interrupts (can replace PIT for scheduler). Provide `hpet_read_ns()` for high-resolution timestamps. This is optional but valuable for accurate profiling and timer precision. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: HPET high-precision timer"`. Add notes, gotchas, and design decisions directly in this TODO section covering the HPET initialization, nanosecond timestamps, and ACPI table parsing.

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

**Prompt:** The Intel e1000 (82540EM) is the default NIC in VirtualBox and is extremely common in real hardware. Detect via PCI vendor `0x8086`, device `0x100E` (82540EM), `0x100F` (82545EM), `0x153A` (I217-LM), `0x10D3` (82574L). Map MMIO BAR0. Initialize: reset device, read MAC from EEPROM or RAL/RAH registers, configure TX/RX descriptor rings (16 entries each), enable TX/RX. TX: build descriptor with buffer pointer + length + EOP flag, update TDT tail. RX: pre-populate descriptors with buffers, poll/IRQ on RDH advance, pass frames to `ethernet_receive()`. Port from SerenityOS `E1000NetworkAdapter` (BSD-2). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"drivers: Intel e1000 NIC module"`. Add notes, gotchas, and design decisions directly in this TODO section covering the e1000 driver, MMIO register map, and descriptor ring setup.

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

**Prompt:** VirtIO-net is the paravirtual NIC for QEMU/KVM — much faster than RTL8139. Detect via PCI vendor `0x1AF4`, device `0x1000`. Reuse the VirtIO MMIO transport from `virtio.c`. Initialize two virtqueues: RX (queue 0) and TX (queue 1). Pre-populate RX queue with empty buffers. TX: build Ethernet frame, submit via descriptor chain, kick queue. RX interrupt: process used buffers, pass to `ethernet_receive()`. Read MAC from device config space. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"drivers: VirtIO-net NIC module"`. Add notes, gotchas, and design decisions directly in this TODO section covering VirtIO-net initialization, virtqueue setup, and QEMU flags.

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

**Prompt:** The RTL8169 is a very common gigabit Ethernet controller found in budget motherboards and PCIe NICs. Detect via PCI vendor `0x10EC`, device `0x8169` (RTL8169), `0x8168` (RTL8111). Similar to RTL8139 but with DMA descriptor rings instead of a fixed buffer. Port from FreeBSD `re(4)` driver or SerenityOS RTL8168 driver (BSD-2). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"drivers: RTL8169 gigabit NIC module"`. Add notes, gotchas, and design decisions directly in this TODO section covering the RTL8169 driver, DMA descriptor rings, and porting notes.

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

**Prompt:** VMSVGA is VirtualBox's GPU adapter, providing mode setting, 2D acceleration, and hardware cursor. Detect via PCI vendor `0x15AD`, device `0x0405`. Map I/O ports (SVGA_INDEX_PORT, SVGA_VALUE_PORT) and framebuffer BAR. Initialize: read SVGA_REG_ID, negotiate version, enable SVGA mode, set width/height/bpp. The FIFO (command buffer) at BAR2 enables 2D acceleration commands (RECT_FILL, RECT_COPY, UPDATE). Hardware cursor via SVGA_REG_CURSOR_*. Port from SerenityOS VMWareBackend (BSD-2). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"drivers: VMSVGA display module"`. Add notes, gotchas, and design decisions directly in this TODO section covering the VMSVGA driver, FIFO commands, and hardware cursor.

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

**Prompt:** VirtIO-GPU provides hardware cursor and 2D resource management in QEMU. Detect via PCI vendor `0x1AF4`, device `0x1050`. Uses controlq (VQ0) for display commands and cursorq (VQ1) for cursor updates. Commands: GET_DISPLAY_INFO, RESOURCE_CREATE_2D, RESOURCE_ATTACH_BACKING, SET_SCANOUT, TRANSFER_TO_HOST_2D, RESOURCE_FLUSH, UPDATE_CURSOR, MOVE_CURSOR. The driver code already exists (reverted) — restore and convert to a loadable module. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"drivers: VirtIO-GPU display module"`. Add notes, gotchas, and design decisions directly in this TODO section covering VirtIO-GPU commands, virtqueue setup, and resource management.

- [ ] Create `src/modules/virtio_gpu/virtio_gpu.c`
- [ ] Restore reverted VirtIO-GPU code as module
- [ ] PCI match: `{ 0x1AF4, 0x1050 }`
- [ ] Controlq (VQ0): display commands (scanout, resources, flush)
- [ ] Cursorq (VQ1): hardware cursor (update, move, hide)
- [ ] Register as display device
- [ ] QEMU flag: `-device virtio-vga` or `-device virtio-gpu-pci`
- [ ] Commit: `"drivers: VirtIO-GPU display module"`

#### VirtIO-GPU 2D Acceleration (Detailed)

> *Incorporated from parking-lot P18*

- [ ] PCI enumeration: detect VirtIO GPU device (vendor 0x1AF4, device 0x1050)
- [ ] Map control/cursor virtqueues via VirtIO transport
- [ ] Implement `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D` — allocate GPU resources
- [ ] Implement `VIRTIO_GPU_CMD_SET_SCANOUT` — bind resource to display
- [ ] Implement `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D` — upload pixel data
- [ ] Implement `VIRTIO_GPU_CMD_RESOURCE_FLUSH` — present to screen (page flip)
- [ ] Replace `fb_swap()` with VirtIO-GPU scanout flip
- [ ] Replace `fb_fill_rect()` with GPU fill command for large rects
- [ ] Cursor: use hardware cursor plane (eliminates cursor-in-compositor overhead)
- [ ] Fallback: keep VBE framebuffer path for non-VirtIO environments

### 4.3 Bochs/BGA Display Driver

**Prompt:** The Bochs Graphics Adapter (BGA) is the simplest GPU — just VBE DISPI I/O port registers for mode setting. Already used by the built-in framebuffer for page flipping. Extract the BGA-specific code to a display module that can set arbitrary resolutions, enable LFB, and do page flipping via Y_OFFSET. This works in QEMU with `-device VGA` and `-device bochs-display`. Low priority since the built-in VBE fallback already handles this. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"drivers: Bochs/BGA display module"`. Add notes, gotchas, and design decisions directly in this TODO section covering the BGA driver, DISPI registers, and page flipping.

- [ ] *(Stretch)* Create `src/modules/bga/bga.c`
- [ ] *(Stretch)* Extract BGA DISPI register code from `framebuffer.c`
- [ ] *(Stretch)* Mode setting, page flipping, resolution change
- [ ] Commit: `"drivers: Bochs/BGA display module"`

---

## 5. Audio Drivers (Modules)

> **Audio subsystem** (abstraction layer, mixer, codec libraries, unified loader)
> is in **[TODO-380-Audio.md](../380-Multimedia/TODO-380-Audio.md)**. This section covers the
> **hardware drivers** that talk to the sound card.

### 5.1 AC97 Sound Card Driver

**Prompt:** AC97 is the simplest sound card to implement in QEMU. Detect the Intel ICH AC97 controller via PCI class 0x04/subclass 0x01. Map two I/O BARs: the Native Audio Mixer BAR (for codec registers like master volume, PCM out volume) and the Native Audio Bus Master BAR (for DMA control). The Bus Master uses a Buffer Descriptor List (BDL) — a ring of 32 entries, each pointing to a PCM data buffer with length and IOC (Interrupt On Completion) flags. Fill the BDL with PCM audio data, set the BDL base address register, and start playback by setting the run bit. Generate a test sine wave (440 Hz, 16-bit signed, 44100 Hz) to verify audio output. After completing all items,sh clean`, and commit as `"drivers: AC97 sound card driver"`.


- [ ] Create `src/kernel/drivers/ac97.c` and `include/ac97.h`
- [ ] Detect AC97 controller via PCI (class `0x04`, subclass `0x01`, or Intel ICH vendor/device)
- [ ] Map I/O BAR (Native Audio Mixer BAR + Native Audio Bus Master BAR)
- [ ] Initialize AC97 codec:
  - [ ] Cold reset via Bus Master control register
  - [ ] Read codec ready status
  - [ ] Set master volume, PCM out volume
- [ ] Configure Bus Master for PCM out:
  - [ ] Allocate DMA buffer (ring of Buffer Descriptor List entries)
  - [ ] Each BDL entry: pointer to PCM data + length + flags (IOC)
  - [ ] Set BDL base address register
- [ ] Implement `ac97_play(pcm_data, samples, sample_rate)` — fill DMA buffers, start playback
- [ ] Implement `ac97_stop()` — halt DMA playback
- [ ] Implement `ac97_set_volume(volume)` — write mixer register (0–100%)
- [ ] Handle AC97 IRQ: buffer completion → refill with next chunk
- [ ] PCI match table: `{ 0x8086, 0x2415 }` (ICH), `{ 0x8086, 0x2425 }`, etc.
- [ ] QEMU flag: `-device AC97` (or `-soundhw ac97`)
- [ ] Test: play a short PCM tone (sine wave 440 Hz) to verify audio output
- [ ] Commit: `"drivers: AC97 sound card driver"`

### 5.2 Intel HDA Sound Driver *(Stretch)*

**Prompt:** Intel HDA (High Definition Audio) is the modern audio standard, more complex than AC97. Detect via PCI class 0x04, subclass 0x03. Map MMIO registers, initialize CORB (Command Output Ring Buffer) and RIRB (Response Input Ring Buffer) for codec communication. Enumerate codecs on the HDA link, parse the widget tree (AFG → mixer → DAC → output pin) to find the audio output path. Set up a DMA stream descriptor for PCM playback. QEMU: `-device intel-hda -device hda-duplex`. This is a stretch goal since AC97 covers QEMU testing. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: Intel HDA audio"`.


- [ ] *(Stretch)* Create `src/kernel/drivers/hda.c` and `include/hda.h`
- [ ] *(Stretch)* Detect Intel HDA via PCI (class `0x04`, subclass `0x03`)
- [ ] *(Stretch)* Map MMIO registers, initialize CORB/RIRB (command/response buffers)
- [ ] *(Stretch)* Enumerate codecs, parse widget tree, configure DAC path
- [ ] *(Stretch)* DMA stream setup for PCM playback
- [ ] *(Stretch)* PCI match table for common devices
- [ ] *(Stretch)* QEMU: `-device intel-hda -device hda-duplex`
- [ ] Commit: `"drivers: Intel HDA audio"`

### 5.3 Convert to Loadable Modules

> After the module system (§1) is complete, convert these drivers to `.kmod` files.

- [ ] Convert AC97 driver to loadable module format
- [ ] Convert Intel HDA driver to loadable module format

---

## 6. Input Drivers (Modules)

### 6.1 VirtIO-input (Tablet Mode)

**Prompt:** Convert the existing built-in VirtIO-input driver to a loadable module. Move `src/kernel/drivers/virtio_input.c` to `src/modules/virtio_input/`. Add PCI match for vendor `0x1AF4`, device `0x1052`. Module init registers the driver, probe contains current init code. After converting, remove VirtIO-input from the static kernel build. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"drivers: convert VirtIO-input to module"`. Add notes, gotchas, and design decisions directly in this TODO section covering the VirtIO-input module conversion and PCI match.

- [ ] Move `src/kernel/drivers/virtio_input.c` to `src/modules/virtio_input/`
- [ ] PCI match: `{ 0x1AF4, 0x1052 }`
- [ ] Module init/cleanup lifecycle
- [ ] Remove from static kernel build
- [ ] Commit: `"drivers: convert VirtIO-input to module"`

---

## 7. USB Drivers (Modules)

> *Moved from [TODO-063-Drivers.md](TODO-063-Drivers.md) §3–4*

### 7.1 USB Core

**Prompt:** USB Core defines the data structures and enumeration logic shared by all USB host controllers. Define `struct usb_device` (address, speed, descriptors, endpoints, class driver), plus standard USB descriptor structs. Implement the USB enumeration sequence: reset device on port → assign address → read device descriptor → read configuration descriptor → set configuration. After enumeration, match the device's class/subclass/protocol to a registered class driver. After completing all items,sh clean`, and commit as `"drivers: USB core and enumeration"`.

- [ ] Create `src/kernel/drivers/usb/usb_core.c` and `include/usb.h`
- [ ] Define `struct usb_device`, `struct usb_device_descriptor`, config/interface/endpoint descriptors
- [ ] Implement USB control transfer (SETUP + DATA + STATUS)
- [ ] Implement USB device enumeration (reset → SET_ADDRESS → GET_DESCRIPTOR → SET_CONFIGURATION)
- [ ] Match device class/subclass → load appropriate class driver
- [ ] Commit: `"drivers: USB core and enumeration"`

### 7.2 xHCI Host Controller Driver

**Prompt:** xHCI (USB 3.0) is the modern USB host controller. Detect via PCI class 0x0C, subclass 0x03, prog_if 0x30. Map BAR0 for MMIO registers. Initialize: halt, reset, allocate DCBAA, command ring, event ring, set Max Slots, start. Handle port status change events. Test with QEMU `-device qemu-xhci -device usb-kbd`. After completing all items,sh clean`, and commit as `"drivers: xHCI USB 3.0 host controller"`.

- [ ] Create `src/kernel/drivers/usb/xhci.c` and `include/xhci.h`
- [ ] PCI match: class `0x0C`, subclass `0x03`, prog_if `0x30`
- [ ] Map MMIO BAR0, initialize controller (halt, reset, DCBAA, rings)
- [ ] Handle port status change events → device attach/detach
- [ ] Implement control, bulk, interrupt transfers via Transfer Rings
- [ ] QEMU flag: `-device qemu-xhci`
- [ ] Commit: `"drivers: xHCI USB 3.0 host controller"`

### 7.3 USB HID Driver (Keyboard + Mouse)

- [ ] Create `src/kernel/drivers/usb/usb_hid.c`
- [ ] Match HID class (class `0x03`), set up interrupt IN endpoint
- [ ] USB keyboard: parse 8-byte report, convert keycodes, inject into keyboard subsystem
- [ ] USB mouse: parse report, inject into mouse subsystem
- [ ] QEMU: `-device usb-kbd -device usb-mouse`
- [ ] Commit: `"drivers: USB HID keyboard and mouse"`

### 7.4 USB Mass Storage Driver

- [ ] Create `src/kernel/drivers/usb/usb_msc.c`
- [ ] Match mass storage class (class `0x08`, subclass `0x06`, protocol `0x50`)
- [ ] Implement Bulk-Only Transport: CBW → data → CSW
- [ ] SCSI commands: INQUIRY, READ CAPACITY, READ(10), WRITE(10)
- [ ] Register as block device → auto-mount with drive letter
- [ ] Commit: `"drivers: USB mass storage (flash drives)"`

### 7.5 USB Hot-Plug Event System

- [ ] Kernel notification on USB device attach/detach
- [ ] Desktop toast: "USB drive detected — D:\\ (8.0 GB, FAT32)"
- [ ] Safe removal: system tray icon → flush, unmount, notify
- [ ] Commit: `"kernel: USB hot-plug notifications"`

### 7.6 PS/2 ↔ USB Fallback

- [ ] If USB HID detected, prefer USB input over PS/2
- [ ] Seamlessly switch input source without application changes
- [ ] Commit: `"drivers: PS/2 ↔ USB input fallback"`

### 7.7 EHCI/UHCI Fallback (Legacy USB)

- [ ] *(Stretch)* EHCI (USB 2.0, PCI prog_if `0x20`)
- [ ] *(Stretch)* UHCI (USB 1.1, PCI prog_if `0x00`)
- [ ] Commit: `"drivers: EHCI/UHCI legacy USB host controllers"`

### 7.8 USB Hub Support

- [ ] *(Stretch)* Detect USB hub devices (class `0x09`)
- [ ] *(Stretch)* Enumerate downstream ports
- [ ] Commit: `"drivers: USB hub support"`

---

## 8. Licensing & Attribution

### 8.1 License Tracking

**Prompt:** Create a `LICENSES/` directory in the project root with the full text of each license used by ported drivers. Create a `NOTICE.md` file listing each ported file, its original source, and its license. Each ported source file retains its original copyright header with an additional line noting the Impossible OS adaptation. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, and commit as `"docs: license tracking for ported drivers"`. Add notes, gotchas, and design decisions directly in this TODO section covering the license tracking process, SPDX identifiers, and NOTICE.md format.

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

| Source     | License | Best Drivers                    | URL                                                                      |
| ---------- | ------- | ------------------------------- | ------------------------------------------------------------------------ |
| SerenityOS | BSD-2   | e1000, AC97, HDA, VMSVGA, NVMe  | [github.com/SerenityOS/serenity](https://github.com/SerenityOS/serenity) |
| tinyusb    | MIT     | USB host (xHCI, EHCI), HID, MSC | [github.com/hathach/tinyusb](https://github.com/hathach/tinyusb)         |
| FreeBSD    | BSD-2   | Intel NIC (em/igb), RTL8169 (re)| [github.com/freebsd/freebsd-src](https://github.com/freebsd/freebsd-src) |
| OpenBSD    | ISC     | Clean NIC/storage drivers       | [github.com/openbsd/src](https://github.com/openbsd/src)                 |
| ToaruOS    | NCSA    | AC97, e1000, VirtIO             | [github.com/klange/toaruos](https://github.com/klange/toaruos)           |
| ACPICA     | BSD     | Full ACPI implementation        | [github.com/acpica/acpica](https://github.com/acpica/acpica)             |

> [!WARNING]
> **NEVER** copy code from the **Linux kernel** (GPL-2.0). This would require relicensing the entire OS to GPL. For hardware where only Linux has a driver, do a clean-room implementation from public datasheets.

---

## 10. Hyper-V VMBus Paravirtualization (Stretch)

> **Priority: 🟢 P3** — Hyper-V Gen 2 support. QEMU and VirtualBox remain primary dev targets.
> The VMBus driver stack is the largest prerequisite for booting on Hyper-V Gen 2.
> Can be deferred until after the core driver model (§1) is complete.

> [!WARNING]
> → XREF: `TODO-010-Bootloader.md §1.6` — Hyper-V Gen 2 removes ALL legacy hardware.
> Without VMBus drivers, the OS has no storage, no input, and a frozen display.

### Source organization

All Hyper-V paravirtualization code lives in `src/kernel/drivers/hyperv/`:

```
src/kernel/drivers/hyperv/
├── vmbus.c        — VMBus core protocol, ring buffers, channel management
├── storvsc.c      — Synthetic SCSI storage (VHDX disk access)
├── hv_kbd.c       — Synthetic keyboard input
├── hv_mouse.c     — Synthetic mouse input
└── hv_video.c     — Synthetic video framebuffer
```

Headers: `include/kernel/drivers/hyperv/vmbus.h`, `storvsc.h`, etc.

### 10.1 VMBus Core Protocol

**Prompt:** VMBus is Microsoft's proprietary channel-based communication framework between the guest OS (VSC — Virtualization Service Client) and the host hypervisor (VSP — Virtualization Service Provider). This is the **foundation** — without VMBus, no synthetic device (disk, keyboard, mouse, video, network) can be accessed. Implement: discover Hyper-V via CPUID leaf `0x40000001`, set up the hypercall page via MSR `HV_X64_MSR_HYPERCALL`, negotiate VMBus protocol version, initiate the VMBus connection, enumerate offered channels, and manage ring buffer pairs (send/receive) for each channel. VMBus channels are identified by GUIDs. Clean-room implement from the public Hyper-V TLFS (Top-Level Functional Specification), NOT from Linux `hv_vmbus.c` (GPL). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: VMBus core protocol"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> **Legal:** Clean-room implement from the [Hyper-V TLFS](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/tlfs)
> (public spec). Do NOT reference Linux `hv_vmbus.c` (GPL contamination risk).

- [x] Create `src/kernel/drivers/hyperv/vmbus.c` and `include/kernel/drivers/hyperv/vmbus.h`
- [x] Detect Hyper-V via CPUID leaf `0x40000001` (signature `"Hv#1"`)
- [x] Read Hyper-V feature MSRs (guest OS ID, feature identification)
- [x] Set up hypercall page via `HV_X64_MSR_HYPERCALL`
- [x] Negotiate VMBus protocol version with host
- [x] Enumerate offered VMBus channels (GUID-based identification)
- [x] Implement ring buffer pair (send ring + receive ring) for each channel
  - [x] Allocate send/receive ring buffers per channel (PMM-backed, page-aligned)
  - [x] Read/write ring buffer with proper memory barriers
- [x] Implement VMBus message handler (interrupt-driven via SINT)
- [x] Implement `vmbus_open_channel(guid)` — open a specific VMBus channel
- [x] Clean-room from: Hyper-V TLFS (public spec), NOT Linux `hv_vmbus.c` (GPL)
- [x] Commit: `"drivers: VMBus core protocol"`

### 10.2 Synthetic SCSI Storage (storvsc)

**Prompt:** On Hyper-V Gen 2, virtual hard disks (VHDX) are attached to a Synthetic SCSI Controller accessible only through VMBus. The storvsc protocol sends SCSI commands (READ/WRITE/INQUIRY) over a VMBus channel identified by the Storage VSP GUID (`BA6163D9-04A1-4D29-B605-72E2FFB1DC7F`). Without this driver, the OS cannot read any disk — IXFS/FAT32 mount fails, and the graphical desktop cannot load assets. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: Hyper-V synthetic SCSI (storvsc)"`. Add notes directly in this TODO section.

- [ ] Create `src/kernel/drivers/hyperv/storvsc.c`
- [ ] Open VMBus channel for Storage VSP GUID
- [ ] Negotiate storvsc protocol version with host
- [ ] Implement SCSI commands over VMBus:
  - [ ] INQUIRY — identify virtual disk
  - [ ] READ CAPACITY — get disk size
  - [ ] READ(10/16) — read sectors
  - [ ] WRITE(10/16) — write sectors
- [ ] Register as block device (`blkdev_register`) for partition scanning
- [ ] Test: boot in Hyper-V Gen 2 → verify IXFS/FAT32 mount
- [ ] Commit: `"drivers: Hyper-V synthetic SCSI (storvsc)"`

### 10.3 Synthetic Keyboard & Mouse (hid-hyperv)

**Prompt:** On Hyper-V Gen 2, the PS/2 (i8042) controller is removed. Keyboard and mouse input is delivered via VMBus channels: Keyboard VSP GUID (`F912AD6D-2B17-48EA-BD65-F927A61C7684`) and Mouse VSP GUID. The synthetic HID protocol sends serialized input events (key scancodes, mouse coordinates) over the VMBus ring buffer. Without this driver, the desktop shell is completely unresponsive to user input. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: Hyper-V synthetic HID input"`. Add notes directly in this TODO section.

- [ ] Create `src/kernel/drivers/hyperv/hv_kbd.c` and `hv_mouse.c`
- [ ] Open VMBus channel for Keyboard VSP GUID
- [ ] Parse synthetic keyboard reports → inject into keyboard subsystem
- [ ] Open VMBus channel for Mouse VSP GUID
- [ ] Parse synthetic mouse reports → inject into mouse subsystem
- [ ] Graceful fallback: if VMBus channels not available, use PS/2 (legacy)
- [ ] Commit: `"drivers: Hyper-V synthetic HID input"`

### 10.4 Synthetic Video (hvfb)

**Prompt:** After `ExitBootServices()` in Hyper-V, the firmware-managed GOP framebuffer may freeze or become invalid if the synthetic video device is not properly acknowledged. The Hyper-V Synthetic Video driver communicates over VMBus (Video VSP GUID) to negotiate resolution and receive framebuffer updates. Note: the GOP framebuffer address from the bootloader typically remains accessible for basic pixel writes, but proper VMBus video integration enables resolution changes and avoids display freezes. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: Hyper-V synthetic video (hvfb)"`. Add notes directly in this TODO section.

- [ ] Create `src/kernel/drivers/hyperv/hv_video.c`
- [ ] Open VMBus channel for Video VSP GUID
- [ ] Negotiate screen resolution with host
- [ ] Map framebuffer via VMBus shared memory
- [ ] Implement dirty region notification (partial screen updates)
- [ ] Register as display device
- [ ] Fallback: if VMBus video unavailable, use bootloader GOP framebuffer as-is
- [ ] Commit: `"drivers: Hyper-V synthetic video (hvfb)"`

---

## 11. Device Manager GUI (🚀 Impossible OS Feature)

**Prompt:** Build a graphical Device Manager that shows all detected hardware with live status. The PCI bus scanner already discovers all devices — expose this data in a tree view organized by device class (Storage, Network, Display, Input, Audio). Each device shows vendor/device names (from PCI ID database), driver status (loaded/missing/error), current IRQ vector, and live interrupt rate (interrupts/sec). This gives users unprecedented hardware visibility. Neither Windows Device Manager nor Linux has a single integrated view with live interrupt data. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"shell: Device Manager GUI"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** Windows Device Manager is static — no live data, no interrupt rates,
> must right-click → Properties for each device. Linux has no built-in GUI equivalent (requires
> `lspci`, `lsusb`, `dmesg` CLI tools). Impossible OS can be the **first OS with a native,
> live Device Manager** showing real-time interrupt rates, driver health, and PCI topology.

- [ ] Enumerate devices: walk PCI scan results, USB device tree, platform devices
- [ ] Resolve PCI vendor/device names from embedded PCI ID database
- [ ] Tree view organized by class: Storage, Network, Display, Input, Audio, USB, Other
- [ ] Per-device info panel:
  - [ ] PCI BDF address (Bus:Device.Function)
  - [ ] Vendor name + device name
  - [ ] Driver status: ✅ Loaded (name.kmod) / ⚠️ Missing / ❌ Error
  - [ ] IRQ vector + live interrupt rate (interrupts/sec)
  - [ ] BAR addresses and memory/IO ranges
- [ ] Live update: refresh interrupt counters every 1 second
- [ ] Win32 API: `SetupDiGetClassDevs()`, `SetupDiEnumDeviceInfo()` for apps
- [ ] Commit: `"shell: Device Manager GUI"`

---

## Priority Order

| Priority | Section                       | Description                                                     |
| :------: | ----------------------------- | --------------------------------------------------------------- |
| ✅ Done   | 2.2 APIC/IOAPIC (built-in)   | `lapic.c` + `ioapic.c` — see TODO-063.09 for enhancements      |
| ✅ Done   | 9.1 ACPI Shutdown/Reboot      | `acpi_shutdown()` + `acpi_reboot()` with 3-method cascade       |
| 🔴 P0    | 1.1 Kernel Symbol Table       | Foundation — modules can't call kernel functions without it     |
| 🔴 P0    | 1.2 ELF Module Loader         | Foundation — load and relocate .kmod files                      |
| 🔴 P0    | 1.3 Module Build System       | Foundation — compile drivers as .kmod                           |
| 🔴 P0    | 1.4 Driver Model + HAL        | Foundation — PCI match, probe/remove lifecycle                  |
| 🔴 P0    | 1.5 Auto-Load at Boot         | Foundation — scan `C:\System\Drivers\` on boot                  |
| 🟠 P1    | 1.6 RTL8139 as Module         | Proof of concept — validate entire pipeline                     |
| 🟠 P1    | 2.1 NVMe (built-in)           | Real hardware SSD support                                       |
| 🟠 P1    | 3.1 Intel e1000 Module        | VirtualBox networking                                           |
| 🟠 P1    | 9.2 ACPI S3 Suspend           | Sleep/resume — **required for laptop support**                  |
| 🟡 P2    | 4.1 VMSVGA Module             | VirtualBox GPU + cursor + acceleration                          |
| 🟡 P2    | 4.2 VirtIO-GPU Module         | QEMU GPU (restore reverted code)                                |
| 🟡 P2    | 3.2 VirtIO-net Module         | Fast QEMU networking                                            |
| 🟡 P2    | 9.3 Battery Status            | Laptop support — system tray battery indicator                  |
| 🟡 P2    | 9.5 Power Button Handler      | Clean shutdown on physical button press                         |
| 🟢 P3    | 6.1 VirtIO-input Module       | Convert existing code                                           |
| 🟢 P3    | 3.3 RTL8169 Module            | Common real-world NIC                                           |
| 🟢 P3    | 2.3 HPET Timer (built-in)     | High-precision timing                                           |
| 🟢 P3    | 8.1 License Tracking          | Attribution compliance                                          |
| 🟢 P3    | 9.4 CPU Freq. Scaling         | Power efficiency on laptops                                     |
| 🟢 P3    | 9.6 Thermal Monitoring ⭐     | Per-core temp in Task Manager — no consumer OS has this         |
| 🟢 P3    | 10.1 VMBus Core               | Hyper-V Gen 2 — channel protocol foundation                     |
| 🟢 P3    | 10.2 Synthetic SCSI           | Hyper-V Gen 2 — VHDX disk access via VMBus                      |
| 🟢 P3    | 10.3 Synthetic HID            | Hyper-V Gen 2 — keyboard/mouse via VMBus                        |
| 🟢 P3    | 10.4 Synthetic Video          | Hyper-V Gen 2 — framebuffer via VMBus                           |
| 🔵 P4    | 4.3 Bochs/BGA Module          | Simple fallback display                                         |
| 🔵 P4    | 5.1 Audio as Modules          | Convert Phase 08 drivers to modules                             |
| 🟢 P3    | 11. Device Manager GUI ⭐     | Live PCI/USB tree with interrupt rates — no OS has this natively |

> [!NOTE]
> ⭐ = Feature where Impossible OS can be **superior** to both Windows and Linux.

---

## NIC Driver Porting Roadmap (~95% Device Coverage)

> **Target: 90–98% of all desktop/laptop wired Ethernet NICs.**
> The first 3 drivers alone cover ~75% of all devices.

### Wired Ethernet

| Priority | Driver                   | Chipsets                                  | Coverage  | Port From                    | License | Method  |
|----------|--------------------------|-------------------------------------------|-----------|------------------------------|---------|---------|
| 🟠 P1     | **Intel e1000/e1000e**   | 82540, 82574, I217, I218, I219            | ~35%      | FreeBSD `em(4)` / SerenityOS | BSD-2   | Port    |
| 🟠 P1     | **Realtek RTL8111**      | RTL8111B/C/D/E/F/G/H, RTL8168, RTL8169    | ~30%      | FreeBSD `re(4)`              | BSD-2   | Port    |
| 🟡 P2     | **Intel igc**            | I225-V, I226-V (2.5GbE)                   | ~10%      | FreeBSD `igc(4)`             | BSD-2   | Port    |
| 🟢 P3     | **Realtek RTL8125**      | RTL8125B/BG (2.5GbE)                      | ~5%       | FreeBSD `re(4)`              | BSD-2   | Port    |
| 🟢 P3     | **Broadcom tg3**         | BCM5751, BCM5754, BCM5761, BCM57765       | ~5%       | FreeBSD `bge(4)`             | BSD-2   | Port    |
| 🟢 P3     | **Qualcomm Atheros alx** | AR8161, AR8171, Killer E2200/E2400        | ~3%       | Datasheet (GPL in Linux)     | —       | Scratch |
| 🔵 P4     | **Intel ixgbe**          | X520, X540, X550 (10GbE)                  | ~2%       | FreeBSD `ix(4)`              | BSD-2   | Port    |
| 🔵 P4     | **Marvell/Aquantia AQC** | AQC107, AQC108, AQC113 (2.5G/5G/10G)      | ~1.5%     | FreeBSD `atlantic(4)`        | BSD-2   | Port    |
| 🔵 P4     | **Intel i210/i211**      | I210-AT, I211-AT (server/NAS boards)      | ~1%       | FreeBSD `igb(4)`             | BSD-2   | Port    |
| 🔵 P4     | **Broadcom bnxt**        | BCM57301, BCM57414 (NetXtreme-E)          | ~0.5%     | FreeBSD `bnxt(4)`            | BSD-2   | Port    |
| 🔵 P4     | **Marvell Yukon**        | 88E8040, 88E8056, 88E8058 (older laptops) | ~0.5%     | FreeBSD `msk(4)`             | BSD-2   | Port    |
| 🔵 P4     | **Qualcomm atl1c**       | AR8131, AR8132, AR8152 (older Atheros)    | ~0.5%     | OpenBSD `alc(4)`             | ISC     | Port    |
| 🔵 P4     | **Realtek RTL8153**      | RTL8153, RTL8156 (USB 3.0 GbE dongles)    | ~0.5%     | Scratch (USB CDC-ECM)        | —       | Scratch |
| 🔵 P4     | **ASIX AX88179**         | AX88179, AX88772 (USB Ethernet dongles)   | ~0.5%     | FreeBSD `axge(4)`            | BSD-2   | Port    |
| —        | **RTL8139**              | RTL8139C/D (100Mbps)                      | ~1%       | Already have                 | MIT     | Convert |
| —        | **VirtIO-net**           | QEMU/KVM paravirtual                      | VMs       | SerenityOS                   | BSD-2   | Port    |
|          |                          |                                           | **~98%+** |                              |         |         |

### WiFi (Stretch — requires 802.11 MAC + WPA supplicant infrastructure)

> All major WiFi drivers are **GPL-only** in Linux. Each requires clean-room implementation from datasheets.

| Priority | Driver                | Chipsets                                | Laptop Coverage | License in Linux | Method  |
|----------|-----------------------|-----------------------------------------|-----------------|------------------|---------|
| 🔵 P4     | **Intel iwlwifi**     | AX200, AX201, AX210, BE200              | ~35%            | GPL              | Scratch |
| 🔵 P4     | **Realtek rtw89**     | RTL8852AE/BE/CE (Wi-Fi 6/6E)            | ~15%            | GPL              | Scratch |
| 🔵 P4     | **Qualcomm ath11k**   | WCN6855, WCN7850 (Wi-Fi 6E/7)           | ~15%            | GPL              | Scratch |
| 🔵 P4     | **Broadcom brcmfmac** | BCM4350, BCM4356, BCM43602              | ~10%            | GPL              | Scratch |
| 🔵 P4     | **MediaTek mt76**     | MT7921, MT7922 (Wi-Fi 6/6E)             | ~10%            | GPL              | Scratch |
| 🔵 P4     | **Qualcomm ath10k**   | QCA6174, QCA9377, QCA9984 (Wi-Fi 5)     | ~4%             | GPL              | Scratch |
| 🔵 P4     | **Realtek rtw88**     | RTL8822BE/CE, RTL8821CE (Wi-Fi 5)       | ~3%             | GPL              | Scratch |
| 🔵 P4     | **Ralink rt2x00**     | RT3090, RT5390, RT5592 (older MediaTek) | ~2%             | GPL              | Scratch |
| 🔵 P4     | **Qualcomm ath9k**    | AR9285, AR9380, AR9462 (Wi-Fi 4)        | ~2%             | GPL              | Scratch |
| 🔵 P4     | **Marvell mwifiex**   | 88W8897, 88W8997 (Surface devices)      | ~1%             | GPL              | Scratch |
| 🔵 P4     | **Broadcom b43**      | BCM4311, BCM4312, BCM4318 (legacy)      | ~1%             | GPL              | Scratch |
|          |                       |                                         | **~98%**        |                  |         |

### USB Drivers (Depends on Phase 08 USB stack)

> USB host controllers provide the transport. Device class drivers provide the functionality.
> **tinyusb** (MIT) is the recommended source for the host controller + HID stack.

#### Host Controllers

| Priority | Driver        | Covers                           | Port From | License | Method |
|----------|---------------|----------------------------------|-----------|---------|--------|
| 🟠 P1     | **xHCI**      | USB 3.x — all modern PCs (2012+) | tinyusb   | MIT     | Port   |
| 🟢 P3     | **EHCI**      | USB 2.0 — older PCs (2001–2015)  | tinyusb   | MIT     | Port   |
| 🔵 P4     | **OHCI/UHCI** | USB 1.1 — legacy (pre-2001)      | tinyusb   | MIT     | Port   |

#### Device Class Drivers

| Priority | Driver            | USB Class | Devices                                   | Port From     | License | Method  |
|----------|-------------------|-----------|-------------------------------------------|---------------|---------|---------|
| 🟠 P1     | **HID**           | 0x03      | Keyboard, mouse, gamepad, touchscreen     | tinyusb       | MIT     | Port    |
| 🟠 P1     | **Mass Storage**  | 0x08      | Flash drives, external HDDs, card readers | tinyusb       | MIT     | Port    |
| 🟡 P2     | **Hub**           | 0x09      | USB hubs, cascaded devices                | tinyusb       | MIT     | Port    |
| 🟡 P2     | **Audio**         | 0x01      | USB headsets, DACs, microphones           | Scratch       | —       | Scratch |
| 🟢 P3     | **CDC-ECM**       | 0x02      | USB Ethernet dongles (RTL8153, AX88179)   | Scratch       | —       | Scratch |
| 🟢 P3     | **Video**         | 0x0E      | USB webcams (UVC)                         | Scratch       | —       | Scratch |
| 🟢 P3     | **Printer**       | 0x07      | USB printers                              | Scratch       | —       | Scratch |
| 🔵 P4     | **CDC-ACM**       | 0x02      | USB serial/modem (Arduino, debug)         | Scratch       | —       | Scratch |
| 🔵 P4     | **Bluetooth HCI** | 0xE0      | USB Bluetooth dongles                     | BTstack (MIT) | MIT     | Port    |
| 🔵 P4     | **Wireless**      | —         | USB WiFi dongles (Realtek, Atheros)       | Scratch       | —       | Scratch |

### GPU / Graphics Drivers

> GPU drivers are the most complex in any OS. A full 3D driver (OpenGL/Vulkan)
> is 100K–500K lines. For Impossible OS, the target is **modesetting + 2D acceleration
> + hardware cursor** — enough for a polished desktop without 3D gaming.

#### Virtual GPUs (for development)

| Priority | Driver         | Used By       | Coverage | Port From            | License | Method  |
|----------|----------------|---------------|----------|----------------------|---------|---------|
| 🟡 P2     | **VMSVGA**     | VirtualBox    | VBox     | SerenityOS           | BSD-2   | Port    |
| 🟡 P2     | **VirtIO-GPU** | QEMU          | QEMU     | Reverted code        | MIT     | Restore |
| 🔵 P4     | **Bochs/BGA**  | QEMU fallback | QEMU     | Scratch (DISPI regs) | —       | Scratch |

#### Integrated GPUs (~80% of all PCs)

| Priority | Driver                  | Chipsets                              | Coverage | Port From                    | License | Method  |
|----------|-------------------------|---------------------------------------|----------|------------------------------|---------|---------|
| 🟢 P3     | **Intel HD/UHD/Iris**   | Gen 9 (Skylake) – Gen 12 (Alder Lake) | ~55%     | Clean-room (Intel open docs) | —       | Scratch |
| 🟢 P3     | **Intel Xe (Arc iGPU)** | Meteor Lake, Lunar Lake, Arrow Lake   | ~10%     | Clean-room (Intel open docs) | —       | Scratch |
| 🔵 P4     | **AMD APU (Vega/RDNA)** | Ryzen 3000G–8000G, Radeon 680M/780M   | ~15%     | Clean-room (AMD open docs)   | —       | Scratch |
|          |                         |                                       | **~80%** |                              |         |         |

> [!TIP]
> Intel publishes **open GPU documentation** (PRM — Programmer's Reference Manual) for all
> their GPUs including Arc. AMD publishes open register guides for Radeon. This makes
> clean-room implementation feasible without studying GPL code.

#### Discrete GPUs (~20% of all PCs)

| Priority | Driver             | Chipsets                       | Coverage | Port From                    | License | Method  |
|----------|--------------------|--------------------------------|----------|------------------------------|---------|---------|
| 🔵 P4     | **NVIDIA GeForce** | GTX 1000, RTX 2000–5000 series | ~16%     | Clean-room (nouveau RE docs) | —       | Scratch |
| 🔵 P4     | **AMD Radeon**     | RX 5000–9000 (RDNA 1–4)        | ~3%      | Clean-room (AMD open docs)   | —       | Scratch |
| 🔵 P4     | **Intel Arc**      | A380, A580, A750, A770, B580   | ~1%      | Clean-room (Intel open docs) | —       | Scratch |
|          |                    |                                | **~20%** |                              |         |         |

> [!WARNING]
> **NVIDIA is the hardest.** They do NOT publish register documentation. The Linux `nouveau`
> driver (GPL) is reverse-engineered. Any NVIDIA driver must be clean-room from the
> nouveau RE docs (register dumps), NOT from nouveau source code.
> AMD and Intel both publish **official open documentation** — much easier.

#### GPU Driver Scope (what "driver" means for each level)

| Level                          | What it provides                            | Complexity | Target       |
|--------------------------------|---------------------------------------------|------------|--------------|
| **Level 1: VESA/VBE fallback** | Basic framebuffer from bootloader           | ✅ Done     | Already have |
| **Level 2: KMS (modesetting)** | Set resolution, refresh rate, multi-monitor | Medium     | 🟢 P3         |
| **Level 3: 2D acceleration**   | Rect fill, blit, cursor, page flip          | Medium     | 🟢 P3         |
| **Level 4: OpenGL (3D)**       | Mesa/Gallium driver, shader compiler        | Very Large | 🔵 Future     |
| **Level 5: Vulkan**            | Full GPU pipeline                           | Massive    | 🔵 Future     |

> For Impossible OS, **Level 3** (modesetting + 2D accel + hardware cursor) is the
> realistic target. This provides a polished desktop without requiring a full 3D stack.

### Input Devices

| Stage    | Input Method                 | Coverage                     | Status    |
|----------|------------------------------|------------------------------|-----------|
| Now      | PS/2 mouse/keyboard          | ~100% (via legacy emulation) | ✅ Working |
| Phase 08 | USB HID (xHCI + HID class)   | ~100% (native USB)           | TODO      |
| Future   | Touchscreen, gamepad, stylus | niche                        | TODO      |

---

## 9. Power Management (ACPI)

> **Priority: P2** — Required for battery-powered laptops and proper QEMU shutdown

### 9.1 ACPI Shutdown / Reboot — ✅ Done

Full ACPI power management is implemented in `acpi.c`:

- [x] Parse ACPI FADT: `PM1a_CNT_BLK` and `SLP_TYP_S5` value (from DSDT `\_S5` AML parsing)
- [x] Implement `acpi_shutdown()` — writes `(SLP_TYPa << 10) | SLP_EN` to PM1a CTL port
  - [x] Fallback: QEMU-specific port `0x604`, Bochs port `0xB004`
  - [x] PM1b support: writes to both PM1a and PM1b if available
- [x] Implement `acpi_reboot()` — 3-method cascade:
  - [x] Method 1: ACPI reset register (FADT 2.0+ GAS)
  - [x] Method 2: Keyboard controller reset (port `0x64`, command `0xFE`)
  - [x] Method 3: Triple fault (null IDT + `int $0x03`)
- [x] Wire to `shutdown` and `reboot` shell commands
- [x] Commit: `"kernel: ACPI shutdown and reboot"`

### 9.2 ACPI S3 Suspend (Sleep)

**Prompt:** S3 is the suspend-to-RAM state — all device state is saved in RAM, only memory is powered. Sequence: flush dirty buffers, save CPU state to wakeup trampoline page, write `SLP_TYP_S3` to PM1a control. On wake, re-enter long mode via trampoline, reinit devices. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: ACPI S3 suspend/resume"`. Add notes directly in this TODO section.

> **Production requirement:** Every laptop user expects sleep/resume. Windows and Linux both
> support S3 natively. Without this, Impossible OS cannot be used on mobile hardware.

- [ ] Write wakeup trampoline (real-mode stub at 1 MB) that re-enters long mode
- [ ] Save CPU state: registers, GDT, IDT, CR3, stack pointer to trampoline page
- [ ] Flush registry hives and dirty disk buffers before suspend
- [ ] Write `(SLP_TYP_S3 << 10) | SLP_EN` to PM1a control register
- [ ] `acpi_resume()`: restore CPU state, reinit APIC, disk controllers, NIC
- [ ] Test: `sleep` shell command → QEMU pauses; power button → resumes
- [ ] Commit: `"kernel: ACPI S3 suspend/resume"`

### 9.3 Battery Status (ACPI Control Method Battery)

**Prompt:** Read battery status via ACPI `_BST` (Battery Status) and `_BIF`/`_BIX` control methods in the DSDT using ACPICA (Apache-2.0). Show battery percentage in system tray. Gracefully degrade if no battery objects found. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: ACPI battery status"`. Add notes directly in this TODO section.

- [ ] Integrate ACPICA AML interpreter (Apache-2.0) for DSDT evaluation
- [ ] Evaluate `_BIF`/`_BIX` at boot: design capacity, full charge capacity
- [ ] Poll `_BST` every 30 seconds: state (charging/discharging), remaining capacity
- [ ] System tray: battery icon with % and estimated time remaining
- [ ] Registry: `HKLM\HARDWARE\Battery\Percentage`, `State`, `TimeRemaining`
- [ ] Graceful degradation: no battery ACPI objects → hide tray icon silently
- [ ] Commit: `"kernel: ACPI battery status"`

### 9.4 CPU Frequency Scaling (DVFS)

**Prompt:** Read P-states from ACPI `_PSS`. Switch via `IA32_PERF_CTL` MSR (Intel) or `PERF_CTL` (AMD). Policy: max frequency when scheduler queue non-empty, min when idle > 100ms. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: CPU frequency scaling (DVFS)"`. Add notes directly in this TODO section.

- [ ] Read `_PSS` from ACPI DSDT: list of (frequency, voltage, latency) P-states
- [ ] Detect Intel SpeedStep via CPUID (ECX bit 7 of leaf 0x01)
- [ ] Switch P-state: write to `IA32_PERF_CTL` MSR (Intel) or `PERF_CTL` MSR (AMD)
- [ ] Policy: busy scheduler → max P-state; idle > 100ms → min P-state
- [ ] Registry: `HKLM\HARDWARE\CPU\CurrentFrequencyMHz`
- [ ] Commit: `"kernel: CPU frequency scaling (DVFS)"`

### 9.5 ACPI Power Button Event Handler

**Prompt:** The ACPI power button generates an SCI (System Control Interrupt) routed via the IOAPIC. When the user presses the physical power button, the OS should receive an ACPI event and initiate a clean shutdown (flush buffers, save registry, power off) rather than an instant hard power-off. Parse the ACPI FADT `SCI_INT` field for the interrupt vector, register an SCI handler, and decode Fixed Events (PM1a Status register bit 8 = Power Button). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: ACPI power button handler"`. Add notes directly in this TODO section.

- [ ] Parse FADT `SCI_INT` field — get SCI interrupt vector
- [ ] Route SCI via IOAPIC to BSP (level-triggered, active-low)
- [ ] Register SCI ISR in IDT
- [ ] In SCI handler: read PM1a Status register, check bit 8 (PWRBTN_STS)
- [ ] On power button press: initiate clean shutdown sequence
  - [ ] Flush dirty filesystem buffers
  - [ ] Save registry hives
  - [ ] Call `acpi_shutdown()`
- [ ] Clear PWRBTN_STS by writing 1 to bit 8 of PM1a Status
- [ ] Commit: `"kernel: ACPI power button handler"`

### 9.6 Thermal Monitoring and Emergency Shutdown (🚀 Impossible OS Feature)

**Prompt:** Read CPU thermal status via LAPIC Thermal LVT and `IA32_THERM_STATUS` MSR. Display CPU temperature in Task Manager. If temperature exceeds critical threshold, perform an emergency clean shutdown. Neither Windows nor Linux shows per-core temperature natively in their built-in task manager — both require third-party tools (HWMonitor, lm-sensors). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: thermal monitoring and emergency shutdown"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** Windows Task Manager does NOT show CPU temperature. Linux
> requires `lm-sensors` + a separate GUI tool. Impossible OS can be the first OS to
> show per-core temperature natively in Task Manager — with color-coded bars and
> automatic throttling/shutdown at critical temps.

- [ ] Read `IA32_THERM_STATUS` MSR — extract temperature from Digital Readout field
- [ ] Read `IA32_TEMPERATURE_TARGET` MSR — get Tj,max (max junction temperature)
- [ ] Calculate: `current_temp = Tj_max - digital_readout`
- [ ] Configure LAPIC Thermal LVT (vector, unmasked) for threshold crossings
- [ ] Poll every 5 seconds: `QueryCpuTemperature(cpu_id)` syscall for Task Manager
- [ ] Task Manager panel: per-core temperature bars
  - [ ] Color-code: green (<60°C), yellow (60–80°C), orange (80–95°C), red (>95°C)
- [ ] Emergency shutdown at critical temp (>100°C): flush + `acpi_shutdown()`
- [ ] Registry: `HKLM\HARDWARE\CPU\Temperature\Core0`, `Core1`, etc.
- [ ] Commit: `"kernel: thermal monitoring and emergency shutdown"`

---

## OS Comparison

| Feature                              | 🪟 Windows 11                           | 🐧 Linux 6.x                          | 🚀 Impossible OS                                     |
| ------------------------------------ | --------------------------------------- | -------------------------------------- | ---------------------------------------------------- |
| Loadable kernel modules              | ✅ WDM drivers (`.sys`)                  | ✅ `.ko` modules (`insmod`)             | ⬜ §1 P0 — `.kmod` ELF objects                        |
| Kernel symbol table                  | ✅ HAL exports                           | ✅ `EXPORT_SYMBOL` / `.kallsyms`        | ⬜ §1.1 P0 — `.ksymtab` linker section                |
| Driver model (PCI match)             | ✅ PnP manager + INF files               | ✅ `struct pci_device_id` tables        | ⬜ §1.4 P0 — HAL + `struct driver`                    |
| Auto-load drivers at boot            | ✅ Service manager + registry            | ✅ `modprobe` + `modules.dep`           | ⬜ §1.5 P0 — scan `C:\System\Drivers\`                |
| NVMe storage                         | ✅ Stornvme.sys                          | ✅ `nvme` driver                        | ⬜ §2.1 P1 — port from SerenityOS BSD-2               |
| APIC / IOAPIC                        | ✅ HAL APIC driver                       | ✅ `arch/x86/kernel/apic/`              | ✅ `lapic.c` + `ioapic.c` — see TODO-063.09           |
| PCAT_COMPAT check                    | ✅ HAL checks MADT flags                 | ✅ `acpi_pic_sci_set_trigger()`         | ✅ `acpi_pcat_compat()` — PIC skipped when 0          |
| ACPI shutdown / reboot               | ✅ Native                                | ✅ Native                               | ✅ `acpi_shutdown()` + `acpi_reboot()` — 3 fallbacks  |
| SMP boot (SIPI)                      | ✅ `HalpStartProcessor()`                | ✅ `do_boot_cpu()`                      | ✅ `smp.c` — INIT-SIPI-SIPI                           |
| Intel e1000 NIC                      | ✅ e1i65x64.sys                          | ✅ `e1000` / `e1000e`                   | ⬜ §3.1 P1 — port from SerenityOS BSD-2               |
| VirtIO-net NIC                       | ✅ netkvm.sys                            | ✅ `virtio_net`                         | ⬜ §3.2 P2                                            |
| GPU modesetting                      | ✅ WDDM 3.x                             | ✅ DRM/KMS                              | ⬜ §4 P2 — VMSVGA + VirtIO-GPU                        |
| USB xHCI controller                  | ✅ USBXHCI.sys                           | ✅ `xhci_hcd`                           | ⬜ §7.2 — port from tinyusb MIT                       |
| USB HID (keyboard/mouse)             | ✅ HIDCLASS.sys                          | ✅ `usbhid`                             | ⬜ §7.3 — port from tinyusb MIT                       |
| ACPI S3 suspend/resume               | ✅ Native                                | ✅ pm-utils / systemd-suspend           | ⬜ §9.2 P1 — **required for laptops**                 |
| Battery status (laptops)             | ✅ Control Panel + tray                  | ✅ UPower + system tray                 | ⬜ §9.3 P2 — ACPICA for `_BST` / `_BIF`              |
| CPU frequency scaling (DVFS)         | ✅ Power plans + HWP                     | ✅ `cpufreq` + governors                | ⬜ §9.4 P3 — P-state via MSR                          |
| Power button clean shutdown          | ✅ SCI handler (hidden)                  | ✅ `acpi_power_off` (hidden)            | ⬜ §9.5 P2 — SCI event handler                        |
| VMBus paravirtualization             | ✅ Native (VSC built-in)                 | ✅ `hv_vmbus` + storvsc/hid-hyperv      | ⬜ §10 P3 — **missing entirely**                      |
| Hyper-V Gen 2 boot                   | ✅ Native                                | ✅ With hv_* drivers                    | ⬜ §10 P3 — **cannot boot without VMBus**             |
| Licensing compliance                 | ✅ Proprietary                           | ✅ GPL-2.0                              | ✅ **§8 — strict MIT/BSD-2/BSD-3 only**               |
| **CPU temperature in Task Manager** ⭐| ❌ Requires HWMonitor (third-party)      | ❌ Requires lm-sensors + GUI            | ⬜ §9.6 — per-core temp bars, color-coded              |
| **HPET ns-resolution timestamps** ⭐ | ✅ QPC (but not user-visible)            | ✅ `clock_gettime` (but not user-visible)| ⬜ §2.3 — exposed as `hpet_read_ns()` API             |
| **Live Device Manager GUI** ⭐       | ⚠️ Static tree (no live data)            | ❌ CLI only (`lspci`, `lsusb`)           | ⬜ §11 — live interrupt rates + driver health per device|

