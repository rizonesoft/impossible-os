# Bootloader — Custom UEFI Boot Application

> **Design Decision:** Impossible OS uses a hand-written UEFI boot application
> (`src/boot/uefi/bootx64.c`) — no GRUB, no Multiboot2. The bootloader runs
> directly as a PE/COFF EFI binary, initializes the framebuffer via GOP, loads
> the kernel ELF from disk, and jumps to `kernel_main()`.

## Boot Chain

```
OVMF (UEFI firmware)
  → EFI/BOOT/BOOTX64.EFI      ← shimx64.efi  (Secure Boot shim)
    └── verifies grubx64.efi against embedded MOK.cer
        → EFI/BOOT/grubx64.efi ← Our UEFI bootloader (signed with MOK.key)
            ├── GOP: set 1280×720 framebuffer
            ├── ELF loader: read /boot/kernel.exe
            ├── ACPI RSDP discovery
            ├── UEFI memory map + page tables
            └── ExitBootServices() → jump to kernel_main()
```

> For Secure Boot architecture, key management, and shim build details, see
> [secure-boot.md](secure-boot.md).

## Key Files

| File | Purpose |
|------|---------|
| `src/boot/uefi/bootx64.c` | UEFI boot application entry point (`efi_main`) |
| `src/boot/uefi/efi.h` | UEFI type definitions and protocol GUIDs |
| `src/boot/uefi/uefi.lds` | Linker script for PE/COFF output |
| `src/boot/linker.ld` | Kernel linker script |
| `shim/shimx64.efi` | Shim binary (installed as `EFI/BOOT/BOOTX64.EFI`) |
| `shim/mmx64.efi` | MokManager (installed as `EFI/BOOT/mmx64.efi`) |

## EFI System Partition Layout

```
\EFI\BOOT\
  ├── BOOTX64.EFI      ← shimx64.efi  (firmware loads this — Secure Boot path)
  ├── grubx64.efi      ← Our bootloader (signed with MOK.key)
  └── mmx64.efi        ← MokManager (first-boot key enrollment)
\boot\
  └── kernel.exe       ← Kernel ELF (loaded by our bootloader)
```

The `system-disk` Makefile target assembles this layout automatically.

## UEFI Boot Application (`bootx64.c`)

The boot application is compiled as a PE32+ EFI binary using the `ms_abi` calling
convention for UEFI compatibility.

### Entry Point: `efi_main`

```c
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table);
```

1. Initializes `EFI_SYSTEM_TABLE`, `EFI_BOOT_SERVICES`
2. Disables the UEFI watchdog timer (default 5-minute timeout)
3. Calls `init_gop()` — set 1280×720×32bpp framebuffer via GOP
4. Calls `fill_screen_black()` — clear screen for clean boot splash transition
5. Calls `load_kernel()` — read and parse `\boot\kernel.exe` (ELF64)
6. Calls `find_acpi_rsdp()` — locate ACPI 2.0 RSDP in UEFI config tables
7. Calls `get_memory_map()` / `fill_memory_map()` — build boot info memory map
8. Calls `setup_page_tables()` — identity-map 4 GiB using 2 MiB pages
9. Calls `ExitBootServices()` — hands off from UEFI
10. Jumps to kernel entry point via `jump_to_kernel()`

### GOP Framebuffer Initialization

`init_gop()` locates `EFI_GRAPHICS_OUTPUT_PROTOCOL` via `LocateProtocol` and
sets the video mode to **1280×720×32bpp** (`PixelBlueGreenRedReserved8BitPerColor`).
The framebuffer base address, width, height, and pitch are stored in `boot_info.fb`
and passed to the kernel.

### ELF64 Kernel Loader

`load_kernel()` reads `\boot\kernel.exe` via `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`:
1. Verifies ELF64 magic, class (`ELFCLASS64`), and machine (`EM_X86_64`)
2. Loads all `PT_LOAD` segments at their specified physical addresses
3. Zeroes BSS regions (where `p_filesz < p_memsz`)
4. Returns the ELF entry point

### ACPI RSDP Discovery

`find_acpi_rsdp()` searches the UEFI configuration table for:
- `EFI_ACPI_20_TABLE_GUID` (ACPI 2.0, preferred)
- `EFI_ACPI_TABLE_GUID` (ACPI 1.0, fallback)

The RSDP physical address and version are stored in `boot_info.acpi_rsdp_addr` / `acpi_version`.

### Memory Map

`get_memory_map()` calls UEFI `GetMemoryMap` (with retry on buffer resize).
`fill_memory_map()` converts UEFI memory types to Multiboot2-compatible types.

`ExitBootServices()` is called with the current map key — retried once on
`EFI_INVALID_PARAMETER` (map key went stale between calls).

### Page Tables

`setup_page_tables()` creates 4-level identity-mapped page tables:
- PML4 → PDPT → PD entries using **2 MiB pages**
- Identity-maps the bottom **4 GiB** of physical address space

## Disk Layout

The OS boots from a single GPT disk image (`build/system-disk.img`):

```
┌──────────────────────────────────────────────────┐
│              system-disk.img (GPT)               │
├──────────────────────┬───────────────────────────┤
│ Partition 1          │ Partition 2               │
│ FAT32 (EFI)          │ IXFS v2 (C:\)             │
│ 256 MiB              │ remainder                 │
│                      │                           │
│ EFI/BOOT/            │ C:\Impossible\System\     │
│   BOOTX64.EFI (shim) │ C:\Users\Default\         │
│   grubx64.efi        │ C:\Programs\              │
│   mmx64.efi          │ hello.exe, shell.exe      │
│ boot/                │ wallpaper, fonts, icons   │
│   kernel.exe         │                           │
└──────────────────────┴───────────────────────────┘
```

## Build

The UEFI bootloader is built by the `uefi-boot` Makefile target:

```bash
bash scripts/build.sh          # incremental
bash scripts/build.sh clean    # full clean build
```

The build signs the bootloader with `keys/MOK.key` (step `sign-efi`), then
packages it as `grubx64.efi` alongside the shim as `BOOTX64.EFI` on the EFI partition.
