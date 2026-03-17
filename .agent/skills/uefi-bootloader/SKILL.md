---
name: uefi-bootloader
description: UEFI boot chain architecture — boot_info struct, GOP init, kernel loading
---

# UEFI Bootloader Architecture

## Boot Chain

```
UEFI firmware → \EFI\BOOT\BOOTX64.EFI → kernel_main(magic, boot_info*)
```

The bootloader is a **PE32+/COFF EFI application** (`src/boot/uefi/bootx64.c`).
It runs in **Long Mode** (64-bit) from the start — no 16→32→64 transition.

## Boot Sequence (bootx64.c)

1. **GOP init** — locate Graphics Output Protocol, set 32bpp mode (EDID → current → fallback)
2. **Clear framebuffer** — zero VRAM to black (prevents firmware residue flash)
3. **Load kernel ELF** — read `\boot\kernel.exe` from FAT32 via Simple File System Protocol
4. **Parse ELF** — load PT_LOAD segments to physical addresses, find `kernel_main` symbol
5. **Find ACPI RSDP** — scan UEFI ConfigurationTable for ACPI 2.0/1.0 GUIDs
6. **Get memory map** — call GetMemoryMap(), convert UEFI types to boot_info format
7. **ExitBootServices()** — NO UEFI calls after this point
8. **Setup page tables** — identity-map first 4 GiB with 2 MiB pages (PML4 at 0x70000)
9. **Jump to kernel** — call `kernel_main(0x55454649, &boot_info)` (magic = "UEFI")

## boot_info Struct (`include/kernel/boot_info.h`)

Placed at physical address **0x10000** (64 KiB). Must match layout in both files.

| Field | Type | Description |
|---|---|---|
| `mmap[64]` | `boot_mmap_entry` | Memory map (base, length, type: 1=avail, 2=rsvd, 3=ACPI) |
| `mmap_count` | `uint32_t` | Number of valid entries |
| `mem_lower_kb` | `uint32_t` | Conventional memory (always 640) |
| `mem_upper_kb` | `uint32_t` | Extended memory in KiB |
| `fb` | `boot_framebuffer` | addr, pitch, width, height, bpp (32), type (1=RGB) |
| `fb_available` | `uint8_t` | 1 if GOP found |
| `acpi_rsdp_addr` | `uintptr_t` | Physical address of RSDP |
| `acpi_version` | `uint8_t` | 1 or 2 |
| `acpi_available` | `uint8_t` | 1 if found |
| `module_start/end` | `uintptr_t` | Initial RAM disk (if loaded) |
| `module_available` | `uint8_t` | 1 if module present |

## Adding New Boot Parameters

1. Add field to `struct boot_info` in **both** `include/kernel/boot_info.h` AND `src/boot/uefi/bootx64.c`
2. Populate the field in `bootx64.c` before `ExitBootServices()`
3. Read the field in kernel code via `g_boot_info.your_field`

## DO NOT

- **No VGA text mode** (0xB8000) — framebuffer is UEFI GOP, always 32bpp linear
- **No BIOS INT calls** — UEFI has no real-mode INT 10h/13h/15h
- **No GRUB/Multiboot headers** — this is a custom UEFI bootloader, not GRUB
- **No 32-bit entry** — UEFI boots directly into Long Mode, skip `entry.asm`
- **No `#include <stdio.h>`** — freestanding only (`<stdint.h>`, `<stddef.h>`, etc.)

## Key Constants

- Boot info address: `0x10000` (BOOT_INFO_PHYS_ADDR)
- Page tables: PML4 at `0x70000`, PDPT at `0x71000`, PDs at `0x72000-0x75000`
- UEFI magic: `0x55454649` ("UEFI" in ASCII)
- Kernel path: `\boot\kernel.exe` on the FAT32 ESP

> → XREF: `TODO-010-Bootloader.md` — full bootloader implementation details
