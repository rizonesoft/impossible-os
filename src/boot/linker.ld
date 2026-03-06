/* ============================================================================
 * Linker Script — 64-bit kernel memory layout
 *
 * GRUB loads the ELF at 1 MiB. The Multiboot2 header must come first.
 * BSS boundaries are exported for zeroing in entry.asm.
 * ============================================================================ */

ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    /* GRUB loads the kernel at 1 MiB */
    . = 1M;

    /* Multiboot2 header must come first (GRUB scans for it) */
    .multiboot2 ALIGN(8) :
    {
        *(.multiboot2)
    }

    /* Code section */
    .text ALIGN(4K) :
    {
        *(.text)
    }

    /* Read-only data */
    .rodata ALIGN(4K) :
    {
        *(.rodata)
    }

    /* Initialized data */
    .data ALIGN(4K) :
    {
        *(.data)
    }

    /* Uninitialized data (BSS) — zeroed by entry.asm */
    .bss ALIGN(4K) :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        __bss_end = .;
    }

    /* End of kernel — PMM bitmap is placed here */
    . = ALIGN(4K);
    __kernel_end = .;
}
