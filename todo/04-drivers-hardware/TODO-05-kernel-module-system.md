---
schema_version: 1
id: kernel-module-system
domain: 04-drivers-hardware
status: active
title: "TODO-05 -- Kernel Module System"
---

# TODO-05 -- Kernel Module System

> [!IMPORTANT]
> **Decision pinned (2026-04-24):** The module loader parses ELF relocatable objects (`.kmod`) **independently of the kernel's own binary format**. The kernel image is ELF permanently (see `CLAUDE.md` -> Toolchain -> Kernel Binary Format); that has no bearing on which module formats this loader accepts. PE32+ `.sys` driver support is a legitimate future extension (tracked here when needed) and does NOT require re-targeting the kernel. Do not propose "align module format with kernel format" -- the layers are independently chosen.

> **Goal:** Build the loadable `.kmod` infrastructure every non-boot driver depends on: a kernel symbol table, an ELF relocatable object loader, a module build system, a driver model with PCI match tables and HAL vtables, boot-time auto-load from `C:\Impossible\System\Drivers\`, and RTL8139 converted to the first loadable module as a proof-of-concept.

> [!IMPORTANT]
> Static built-in drivers (AHCI, keyboard, framebuffer) remain built-in. Only hardware discovered post-boot or non-boot-critical devices use `.kmod`. The module loader allocates PMM-backed **executable** memory -- those pages must have the NX bit cleared in the VMM page tables. Every ported `.kmod` must carry an MIT/BSD-2/BSD-3/Apache-2.0 license header; GPL-2 code is never permitted.

## Inputs

- [`src/kernel/drivers/rtl8139.c`](../../src/kernel/drivers/rtl8139.c) -- RTL8139 driver being migrated to a module in §6
- [`src/kernel/main.c`](../../src/kernel/main.c) -- auto-load hook insertion point for §5
- [`scripts/build.sh`](../../scripts/build.sh), `Makefile` -- build system extended in §2
- → XREF: `05-storage-filesystems` domain -- VFS and IXFS must be mounted before `module_load_all()` in §5 can scan `C:\Impossible\System\Drivers\`
- → XREF: `TODO-01-pci-pcie-pnp-resource-manager.md` -- PCI enumeration scan completes before `driver_probe_all()` in §3; the driver model in §3 hooks into that scan callback
- → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §1` -- `vmm_map_exec(base, size)` call needed by §4 to mark module `.text` pages as executable (NX cleared)
- → XREF: `02-kernel-core/TODO-12-native-api-ssdt.md §4` -- SSDT indices 0x0270–0x0272 reserved for Plug and Play syscalls
- → XREF: `02-kernel-core/TODO-02-kernel-configuration-policy.md §5` -- Safe Mode policy decides which modules may auto-load during boot

## Outcome

- `EXPORT_SYMBOL(fn)` places ~60 core kernel symbols in a `.ksymtab` linker section; `ksym_lookup(name)` resolves them in O(N) for the loader.
- `module_load(path)` parses an `ET_REL`/`EM_X86_64` ELF object from IXFS, allocates executable PMM pages, applies all four x86-64 relocation types, resolves externals against `.ksymtab`, and calls `module_init()`.
- `src/modules/<driver>/` directories compile to `build/modules/*.kmod` with freestanding flags; the build system installs them to the IXFS partition at `C:\Impossible\System\Drivers\`.
- `struct driver` with `pci_match[]` table, `probe()`/`remove()` lifecycle, and `blk_ops`/`net_ops`/`input_ops` vtables forms the HAL; `driver_register()` triggers a PCI device scan immediately.
- After IXFS mounts, `module_load_all("C:\\Impossible\\System\\Drivers")` scans and loads every `.kmod`; failures are non-fatal with a `[WARN]` log entry.
- RTL8139 is removed from the static kernel build and rebuilt as `rtl8139.kmod`; networking verified end-to-end after loading from disk.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On                               | Status |
| --- | :---: | ---------------------------------------- | ---------------------------------------- | :----: |
| 💎  |   1   | §1 Kernel symbol table + `EXPORT_SYMBOL` | --                                       |  [ ]   |
| 💎  |   2   | §2 Module build system (`src/modules/`, Makefile) | §1 (headers needed for modules)          |  [ ]   |
| 💎  |   3   | §3 Driver model + HAL vtables + PCI match | §1 (exported HAL symbols), PCI scan      |  [ ]   |
| 💎  |   4   | §4 ELF relocatable module loader         | §1, §4 (test `.kmod` to load)            |  [ ]   |
| 💎  |   5   | §5 Auto-load modules at boot             | §3, VFS/IXFS mounted                     |  [ ]   |
| 💎  |   6   | §6 RTL8139 as first loadable module      | §3, §4, §2, §5                           |  [ ]   |
| 💎  |   7   | Plug and Play syscalls wired to SSDT     | §1, 02-kernel-core/TODO-12-native-api-ssdt.md §4 |  [ ]   |

> All six rows are 💎 parity: Windows NT has `.sys` driver loading with a symbol table (HAL.dll exports); Linux has `insmod`/`modprobe` with `.ko` ELF modules and `EXPORT_SYMBOL`. Impossible OS matches both with a leaner design -- no separate HAL.dll, no kernel version magic -- but achieves the same driver isolation and hot-load capability.

---

## 1. Kernel Symbol Table + `EXPORT_SYMBOL` `[Sonnet]`

Export ~60 core kernel functions into a `.ksymtab` linker section. The module loader resolves undefined ELF symbols against this table. A linear search by name is acceptable at load time -- modules load once and symbol resolution is not on the hot path.

**Files:** `src/kernel/main/ksymtab.c` (new), `include/kernel/ksymtab.h` (new), `src/kernel/kernel.ld` (linker script)

- [ ] Define `ksym_entry_t { const char *name; void *addr; }` in `include/kernel/ksymtab.h`
- [ ] `EXPORT_SYMBOL(fn)` macro: `static ksym_entry_t __ksym_##fn __attribute__((section(".ksymtab"), used)) = { #fn, (void *)&fn }`
- [ ] Add `.ksymtab` section to `kernel.ld`; align to 8 bytes; provide `__ksymtab_start` / `__ksymtab_end` symbols
- [ ] `ksym_lookup(name)` -- linear scan from `__ksymtab_start` to `__ksymtab_end`; return `addr` or `NULL`
- [ ] Export ~60 symbols across subsystems:
  - Printing: `klog`, `serial_write`
  - Memory: `kmalloc`, `kfree`, `krealloc`, `pmm_alloc_contiguous`, `pmm_free`
  - PCI: `pci_read_config8/16/32`, `pci_write_config8/16/32`, `pci_enable_bus_mastering`, `pci_find_device`
  - Interrupts: `idt_register_handler`, `pic_unmask_irq`, `pic_send_eoi`, `lapic_eoi`
  - I/O ports: `inb`, `outb`, `inw`, `outw`, `inl`, `outl`
  - Block: `blkdev_register`
  - Network: `ethernet_receive`, `net_register_nic`
  - Framebuffer: `fb_get_width`, `fb_get_height`, `fb_get_pitch`, `fb_get_base`
  - Scheduler: `thread_create`, `thread_yield`, `mutex_lock`, `mutex_unlock`, `sem_wait`, `sem_signal`
  - Timer: `uptime_ns`, `sleep_ms`
  - VMM: `vmm_map`, `vmm_unmap`, `vmm_map_exec`
- [ ] Boot log: `[OK] ksymtab: %u symbols exported`
- [ ] Commit: `"kernel: symbol table -- EXPORT_SYMBOL macro, .ksymtab section, ksym_lookup"`

## 2. Module Build System `[Sonnet]`

Add `src/modules/` to the build tree. Each subdirectory compiles to a freestanding relocatable `.kmod`. The build script installs `.kmod` files into the IXFS disk image at `C:\Impossible\System\Drivers\`.

**Files:** `src/modules/` (new directory), `include/kernel/module.h`, `Makefile`, `scripts/build.sh`

- [ ] Create `src/modules/` with `Makefile.module` template; each module subdir has its own `Makefile` that includes the template
- [ ] Module compile flags: `-ffreestanding -nostdlib -nostdinc -c -fPIC -mcmodel=kernel -I include/ -std=gnu11 -O2 -g`
- [ ] Output path: `build/modules/<name>.kmod`
- [ ] `include/kernel/module.h` macros:
  - `MODULE_INIT(fn)` -- `void module_init(void) __attribute__((alias(#fn)))`
  - `MODULE_CLEANUP(fn)` -- `void module_cleanup(void) __attribute__((alias(#fn)))`
  - `MODULE_NAME(str)` -- `static const char __module_name[] __attribute__((section(".modinfo"), used)) = "name=" str`
  - `MODULE_LICENSE(str)` -- `static const char __module_license[] __attribute__((section(".modinfo"), used)) = "license=" str`
- [ ] `scripts/build.sh`: after building kernel, build all `src/modules/*/`; `llvm-objcopy-19` strips debug from release `.kmod`
- [ ] Disk image creation: copy `build/modules/*.kmod` to `C:\Impossible\System\Drivers\` on the IXFS partition
- [ ] Commit: `"build: module build system -- src/modules/, MODULE_INIT/CLEANUP macros, IXFS install"`

## 3. Driver Model + HAL Vtables + PCI Match Tables `[Sonnet]`

Define `struct driver` with a PCI match table and `probe()`/`remove()` lifecycle. Three HAL vtable structs -- `blk_ops`, `net_ops`, `input_ops` -- form the Hardware Abstraction Layer. `driver_register()` adds to the global list and immediately probes all known PCI devices for a match.

**Files:** `src/kernel/main/driver_model.c` (new), `include/kernel/driver_model.h` (new)

- [ ] `pci_match_t { uint16_t vendor_id; uint16_t device_id; }` -- `0` in either field = wildcard
- [ ] `driver_t { const char *name; const char *license; const pci_match_t *match_table; int (*probe)(pci_device_t *dev); void (*remove)(pci_device_t *dev); }`
- [ ] `blk_ops_t { int (*read)(void *dev, uint64_t lba, uint32_t count, void *buf); int (*write)(...); uint64_t (*capacity)(void *dev); }`
- [ ] `net_ops_t { int (*send)(void *dev, const void *data, size_t len); void (*get_mac)(void *dev, uint8_t mac[6]); }`
- [ ] `input_ops_t { int (*poll)(void *dev); int (*get_event)(void *dev, input_event_t *ev); }`
- [ ] `driver_register(drv)` -- append to `g_drivers` list; for each already-enumerated PCI device, test match; call `probe()` if matched
- [ ] `driver_unregister(drv)` -- call `remove()` for each matched device; remove from list
- [ ] `driver_probe_all()` -- walk all PCI devices, call `driver_match_pci(dev)` against all registered drivers; called from `kernel_main()` after PCI scan
- [ ] Register existing AHCI as `blk_ops`, existing RTL8139 as `net_ops` (transitional; removed in §6), keyboard/mouse as `input_ops`
- [ ] Boot log per match: `[DRIVER] <name>: probe PCI %04x:%04x OK`
- [ ] Commit: `"kernel: driver model -- struct driver, pci_match, blk_ops/net_ops/input_ops HAL vtables"`

## 4. ELF Relocatable Module Loader `[Opus]`

Parse an `ET_REL`/`EM_X86_64` ELF file from disk, allocate PMM-backed executable memory, copy sections, apply the four standard x86-64 relocation types, resolve undefined externals via `ksym_lookup()`, and call `module_init()`. `module_unload()` calls `module_cleanup()` and frees the PMM pages.

**Files:** `src/kernel/main/module.c` (new), `include/kernel/module.h` (new)

> [!CAUTION]
> Module `.text` section pages must be allocated via `pmm_alloc_contiguous()` (never `kmalloc` -- modules exceed 4 KB) and mapped executable via `vmm_map_exec(base, size)` (NX bit cleared). Write permissions must be dropped after relocations are applied -- set `.text` pages to `PROT_READ | PROT_EXEC` before calling `module_init()`.
> `R_X86_64_32S` requires the module load address to be within ±2 GB of the kernel image -- allocate module memory in the kernel virtual address range (`0xFFFFFFFF80000000+`) to guarantee this.

- [ ] Define `loaded_module_t { char name[64]; void *base; size_t size; void (*init)(void); void (*cleanup)(void); loaded_module_t *next; }`
- [ ] `module_load(path)`:
  - [ ] User-requested load: `SeSinglePrivilegeCheck(&SeLoadDriverPrivilege, UserMode)` first, `STATUS_PRIVILEGE_NOT_HELD` if not held (KernelMode boot auto-load bypasses) -> XREF: 02-kernel-core/TODO-15 §8
  - [ ] VFS `read` into a temporary buffer
  - [ ] Verify ELF magic, `e_type == ET_REL`, `e_machine == EM_X86_64`
  - [ ] Enumerate section headers; sum sizes of `SHF_ALLOC` sections to compute `total_size`
  - [ ] `pmm_alloc_contiguous(total_size)` → `base`; `vmm_map_exec(base, total_size)`
  - [ ] Copy `.text`, `.rodata`, `.data` contents to `base`; zero-fill `.bss`
  - [ ] Process `.rela.text` and `.rela.data`: for each entry compute `S + A` (absolute) or `S + A - P` (PC-relative); handle `R_X86_64_64`, `R_X86_64_PC32`, `R_X86_64_32S`, `R_X86_64_PLT32`
  - [ ] Resolve `STB_GLOBAL` + `SHN_UNDEF` symbols: call `ksym_lookup(name)`; `STATUS_UNRESOLVED_SYMBOL` if not found
  - [ ] Remove write permission from `.text` pages: `vmm_mprotect(text_base, text_size, PROT_READ | PROT_EXEC)`
  - [ ] Locate `module_init` symbol; call it
  - [ ] Append `loaded_module_t` to `g_modules` list
- [ ] `module_unload(name)`: find module in list; call `cleanup()`; `vmm_unmap(base, size)`; `pmm_free(base)`
- [ ] Before `pmm_free(base)`, unregister + **drain** any Ob handle-op callbacks the module registered (pages hold the callback fn + `context`) -> XREF: `02-kernel-core/TODO-05-object-manager.md §13`
- [ ] Test: load a trivial `.kmod` that calls `klog(LOG_INFO, "TEST", "Hello from module")` -- verify serial output
- [ ] Commit: `"kernel: ELF module loader -- ET_REL parser, R_X86_64_* relocations, ksym resolution"`

## 5. Auto-Load Modules at Boot `[Sonnet]`

After IXFS mounts `C:\`, scan `C:\Impossible\System\Drivers\` and load every `.kmod` file. Each load is non-fatal -- a failed module logs `[WARN]` and boot continues. The `[modinfo]` `.modinfo` section data is parsed to produce the log line.

**Files:** `src/kernel/main.c`, `src/kernel/main/module.c`

- [ ] `module_load_all(dir)` -- VFS `opendir(dir)`, iterate entries; for each entry ending in `.kmod`, call `module_load(path)`
- [ ] Consult `kernel_config_get()->safe_mode` plus per-module autoload policy before loading; skip third-party, GUI-only, and network modules unless the current Safe Mode profile explicitly allows them
- [ ] On success: `klog(LOG_INFO, "MODULE", "Loaded %s (%s, %s)", name, module_name, module_license)` -- parsed from `.modinfo` section
- [ ] On failure: `klog(LOG_WARN, "MODULE", "Failed to load %s: %s", name, error_string)` -- continue loop
- [ ] In `kernel_main()`: insert `module_load_all("C:\\Impossible\\System\\Drivers")` immediately after `vfs_mount("C:", ...)` succeeds
- [ ] `module_list` shell command -- prints loaded modules: name, base address, size, license
- [ ] Boot log summary: `[OK] Modules: %u loaded, %u failed`
- [ ] Parse `module_param` declarations from `.modinfo`, accept `module.<name>.<param>=` boot-arg injection, and register each as a `module.<name>.` tunable. -> XREF: D02 T02 §6.
- [ ] Commit: `"kernel: auto-load modules at boot -- module_load_all, .modinfo parse, non-fatal scan"`

## 6. RTL8139 as First Loadable Module `[Sonnet]`

Convert the existing built-in RTL8139 driver to `src/modules/rtl8139/rtl8139.kmod`. This exercises the complete module pipeline end-to-end and validates that networking survives the transition from static to dynamic loading.

**Files:** `src/modules/rtl8139/rtl8139.c` (moved from `src/kernel/drivers/rtl8139.c`), `src/modules/rtl8139/Makefile` (new), `src/kernel/drivers/rtl8139.c` (deleted)

> [!IMPORTANT]
> Do not remove RTL8139 from the kernel until §4, §2, §3, and §5 are all verified working. Test the module loader with a trivial hello-world `.kmod` first before attempting the RTL8139 migration -- a broken loader is easier to debug without a critical driver involved.

- [ ] Move `src/kernel/drivers/rtl8139.c` → `src/modules/rtl8139/rtl8139.c`; add `src/modules/rtl8139/Makefile`
- [ ] Add `MODULE_NAME("RTL8139 NIC")`, `MODULE_LICENSE("BSD-2")`, PCI match table `{ 0x10EC, 0x8139, 0 }`
- [ ] `MODULE_INIT(rtl8139_module_init)` → calls `driver_register(&rtl8139_driver)`
- [ ] `MODULE_CLEANUP(rtl8139_module_cleanup)` → calls `driver_unregister(&rtl8139_driver)`
- [ ] Move RTL8139 hardware init code from `rtl8139_init()` into `rtl8139_driver.probe()`
- [ ] Remove RTL8139 from `src/kernel/drivers/` and from the kernel `Makefile` source list
- [ ] Boot verification: `bash scripts/build.sh clean run` -- serial log shows `[MODULE] Loaded rtl8139.kmod (RTL8139 NIC, BSD-2)` and `[DRIVER] RTL8139: probe PCI 10EC:8139 OK`; networking functions (ARP/ICMP) still work
- [ ] Commit: `"drivers: convert RTL8139 to loadable module -- first .kmod end-to-end test"`

---

## 7. Plug and Play syscalls wired to SSDT

PnP syscalls (`NtPlugPlayControl`, device-enumeration) are exposed through the native API SSDT so user-mode tools (Device Manager, `dmgr.exe`) can enumerate and control devices without a separate RPC layer. Depends on the §1 symbol export surface and `02-kernel-core/TODO-12-native-api-ssdt.md §4` SSDT dispatcher.

- [ ] Define the PnP control codes in `include/kernel/nt/pnp.h` and add them to the SSDT table.
- [ ] Wire enumerate / start / stop / query-id entries through `Iop*` helpers.
- [ ] Expose user-mode stubs in `ntdll.dll` (matches Windows ABI for dmgr.exe).
- [ ] Commit: `"drivers: wire PnP syscalls through SSDT"`

**Test checkpoint:** `dmgr.exe list` enumerates attached PCI devices via the SSDT entries; unit test calls `NtPlugPlayControl` directly and asserts the returned count matches `pci_scan()`'s in-kernel table.

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                                 | 🐧 Linux                                 | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎  | Kernel symbol export table               | ✅ `HAL.dll` + `ntoskrnl.exe` export tables | ✅ `EXPORT_SYMBOL` → `.kallsyms`; `ksym_lookup` via | ⬜ §1 -- `EXPORT_SYMBOL` → `.ksymtab` linker section, |
| 💎  | Loadable kernel module                   | ✅ `.sys` PE/COFF loaded by I/O          | ✅ `insmod`/`modprobe`; ELF `.ko` with relocations | ⬜ §4 -- `ET_REL` ELF loader, 4 reloc    |
| 💎  | Module build system + freestanding compiler flags | ✅ WDK/MSBuild; driver project templates | ✅ Kbuild `obj-m`; `-ffreestanding` per module | ⬜ §2 -- `src/modules/`, `Makefile.module`, IXFS install |
| 💎  | Driver model + PCI match tables + HAL vtables | ✅ WDM `DRIVER_OBJECT`; `IoCreateDevice`; miniport vtables | ✅ `struct bus_type`; `driver.probe()`; `platform_driver` | ⬜ §3 -- `struct driver`, `pci_match[]`, `blk_ops`/`net_ops`/`input_ops` |
| 💎  | Auto-load drivers at boot from filesystem | ✅ `HKLM\SYSTEM\CurrentControlSet\Services`; SCM loads | ✅ `initrd` + `depmod`; `modprobe` at    | ⬜ §5 -- scan `C:\Impossible\System\Drivers\`, non-fatal on failure |
| 💎  | First-party driver as loadable module    | ✅ All NDIS miniport NIC drivers         | ✅ Almost all NIC drivers are            | ⬜ §6 -- RTL8139 migrated to `rtl8139.kmod`; networking |

> **After §1–6:** Impossible OS reaches full parity with Windows NT and Linux for the foundational driver-loading infrastructure. The key design difference: no separate HAL binary (`HAL.dll`) -- the HAL vtables (`blk_ops`, `net_ops`) live directly in kernel address space and are accessed via standard C function-pointer dispatch, eliminating one indirection level and one binary boundary Windows drivers must cross.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Boot log shows `[OK] ksymtab: N symbols exported` (N ≥ 60)
- [ ] Trivial hello-world `.kmod` loads via `module_load()` and prints `Hello from module` to serial log
- [ ] `module list` shell command shows loaded modules with name, base address, and license
- [ ] `bash scripts/build.sh run`: boot log shows `[MODULE] Loaded rtl8139.kmod (RTL8139 NIC, BSD-2)` and `[DRIVER] RTL8139: probe PCI 10EC:8139 OK`
- [ ] RTL8139 is absent from the static kernel binary (`llvm-nm-19 build/kernel.exe | grep rtl8139` returns empty)
- [ ] Networking works post-migration: DHCP/ARP/ICMP function as before
- [ ] Failed `.kmod` (deliberate corrupt test file) → `[WARN]` in log; boot continues; other modules load
- [ ] Commit: `"drivers: kernel module system -- ksymtab, ELF loader, driver model, RTL8139 kmod"`
