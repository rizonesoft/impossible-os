# Bootloader — UEFI + GRUB + Multiboot2

> **Design Decision:** Impossible OS is **UEFI-only** — no legacy BIOS/MBR support.
> This aligns with modern hardware and Hyper-V Generation 2 VMs.

## Boot Chain

```
OVMF (UEFI firmware)
  → EFI System Partition on ISO
    → GRUB 2.12 (EFI application)
      → Multiboot2 protocol
        → entry.asm (32-bit protected mode entry)
          → Page tables + Long Mode setup
            → kernel_main() (64-bit)
```

## Key Files

| File | Purpose |
|------|---------|
| `src/boot/grub.cfg` | GRUB menu — `menuentry "Impossible OS"` |
| `src/boot/multiboot2_header.asm` | Multiboot2 header with framebuffer tag (1280×720×32) |
| `src/boot/entry.asm` | Kernel entry: verify magic, setup Long Mode, call `kernel_main` |
| `src/boot/linker.ld` | Linker script — kernel loaded at high address |

## GRUB Configuration

```cfg
set timeout=3
set default=0

menuentry "Impossible OS" {
    multiboot2 /boot/kernel.elf
    module2 /boot/initrd.img
    boot
}
```

GRUB loads the kernel via **Multiboot2** and passes the initrd as a module.

## Multiboot2 Header

The header requests:
- **Framebuffer** — 1280×720 at 32 bpp (GOP mode, no VGA text)
- **Module alignment** — for the initrd image

GRUB provides a **Multiboot2 info structure** containing:
- Memory map (20+ entries from UEFI)
- Framebuffer address, pitch, width, height, bpp
- ACPI RSDP pointer (v1 or v2)
- Module locations (initrd)

## Entry Point (`entry.asm`)

GRUB drops us in **32-bit protected mode** with:
- `EAX` = Multiboot2 magic (`0x36D76289`)
- `EBX` = Physical address of Multiboot2 info structure

The entry stub:
1. Verifies the magic value
2. Saves `EBX` (Multiboot2 info pointer)
3. Sets up identity-mapped page tables
4. Transitions to 64-bit Long Mode
5. Calls `kernel_main()`

## Long Mode Transition

```
32-bit Protected Mode
  → Set up 4-level page tables (PML4 → PDPT → PD → PT)
    → Identity-map 4 GiB using 2 MiB pages
  → Enable PAE (CR4 bit 5)
  → Set Long Mode Enable in IA32_EFER MSR
  → Enable paging (CR0 bit 31)
  → Load 64-bit GDT
  → Far-jump to 64-bit code segment (CS64)
  → Set up 64-bit stack
  → Zero BSS section
  → Call kernel_main()
```

## Multiboot2 Info Parsing

`src/kernel/multiboot2_parse.c` walks the tagged info structure and populates
`g_boot_info` (defined in `include/kernel/boot_info.h`):

| Tag | Data Extracted |
|-----|---------------|
| Memory map (type 6) | Physical memory regions (available, reserved, ACPI) |
| Framebuffer (type 8) | Address, pitch, width, height, bpp |
| ACPI old RSDP (type 14) | RSDP v1 physical address |
| ACPI new RSDP (type 15) | RSDP v2 physical address |
| Module (type 3) | Initrd start/end addresses |
| Command line (type 1) | Boot parameters |

## ISO Creation

The ISO is built by `grub-mkrescue`:

```bash
grub-mkrescue -o build/os-build.iso build/isodir
```

The `isodir` layout:
```
isodir/
└── boot/
    ├── kernel.elf      # Kernel binary
    ├── initrd.img      # Initial RAM filesystem
    └── grub/
        └── grub.cfg    # GRUB configuration
```

`grub-mkrescue` automatically adds the EFI boot partition and GRUB EFI binary.
