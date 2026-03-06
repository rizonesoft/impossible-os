/* ============================================================================
 * Linker Script — Controls memory layout of the kernel ELF
 * GRUB loads this ELF and jumps to _start
 * ============================================================================ */

ENTRY(_start)

SECTIONS
{
    /* GRUB loads the kernel at 1 MiB (standard for Multiboot) */
    . = 1M;

    /* Multiboot2 header must come first */
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

    /* Uninitialized data (BSS) */
    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss)
    }
}
