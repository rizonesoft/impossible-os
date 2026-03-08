# Bootloader — UEFI + GRUB + Multiboot2

> **Design Decision:** Impossible OS is **UEFI-only** — no legacy BIOS/MBR support.
> This aligns with modern hardware and Hyper-V Generation 2 VMs.

## Boot Chain

```
OVMF (UEFI firmware)
  → EFI System Partition (FAT32)
    → /EFI/BOOT/BOOTX64.EFI (GRUB standalone)
      → /boot/grub/grub.cfg
        → multiboot2 /boot/kernel.exe
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

## GRUB EFI Binary

A standalone GRUB EFI binary is built via `grub-mkimage` for direct disk boot
(no ISO or `grub-mkrescue` needed):

```bash
grub-mkimage -O x86_64-efi -o BOOTX64.EFI -p /boot/grub \
    part_gpt fat normal multiboot2 boot \
    all_video efi_gop gfxterm configfile echo \
    search test reboot halt font loadenv
```

| Property | Value |
|----------|-------|
| Format | PE32+ (EFI application) |
| Size | ~790 KiB |
| Target | x86_64-efi |
| Config prefix | `/boot/grub` |
| Makefile target | `make grub-efi` |
| Output | `build/tools/BOOTX64.EFI` |

Installed at `/EFI/BOOT/BOOTX64.EFI` on the EFI System Partition. UEFI firmware
discovers and loads it automatically per the UEFI specification fallback path.

## GRUB Configuration

```cfg
set timeout=3
set default=0

menuentry "Impossible OS" {
    multiboot2 /boot/kernel.exe
    boot
}
```

GRUB loads the kernel via **Multiboot2**. All system files live on the IXFS
partition (`C:\`), pre-populated at build time by `mkfs-ixfs --populate`.

## Multiboot2 Header

The header requests:
- **Framebuffer** — 1280×720 at 32 bpp (GOP mode, no VGA text)

GRUB provides a **Multiboot2 info structure** containing:
- Memory map (20+ entries from UEFI)
- Framebuffer address, pitch, width, height, bpp
- ACPI RSDP pointer (v1 or v2)

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
| Module (type 3) | GRUB module start/end addresses |
| Command line (type 1) | Boot parameters |

## Disk Layout (Target — Phase 00)

The system boots from a single GPT disk image (`build/system-disk.img`):

```
┌────────────────────────────────────────────┐
│         system-disk.img (GPT)              │
├──────────────┬─────────────────────────────┤
│ Part 1       │ Part 2                      │
│ FAT32 (EFI)  │ IXFS v2 (C:\)              │
│ 256 MiB      │ remainder                  │
│              │                             │
│ /EFI/BOOT/   │ C:\Impossible\System\      │
│  BOOTX64.EFI │ C:\Users\Default\          │
│ /boot/       │ C:\Programs\               │
│  kernel.exe  │ hello.exe, shell.exe       │
│  grub.cfg    │ wallpaper.raw              │
└──────────────┴─────────────────────────────┘
```
