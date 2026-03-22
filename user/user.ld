/* User-mode ELF linker script — loads at 0x800000 (8 MiB)
 * Must be above kernel BSS end (~0x4C0000) to avoid collision. */

ENTRY(_start)

SECTIONS
{
    . = 0x800000;

    .text ALIGN(4K) : {
        *(.text)
    }

    .rodata ALIGN(4K) : {
        *(.rodata*)
    }

    .data ALIGN(4K) : {
        *(.data)
    }

    .bss ALIGN(4K) : {
        *(.bss)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
