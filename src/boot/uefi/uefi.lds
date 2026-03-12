/* ============================================================================
 * UEFI application linker script
 *
 * Produces a position-independent shared object that objcopy converts
 * to PE/COFF (efi-app-x86_64). Entry point: efi_main.
 * ============================================================================ */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_main)

SECTIONS
{
    . = 0;

    .hash : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }

    . = ALIGN(4096);

    .text : {
        *(.text .text.*)
    }

    . = ALIGN(4096);

    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);

    .data : {
        *(.got .got.plt)
        *(.sdata .sdata.*)
        *(.data .data.*)
    }

    . = ALIGN(4096);

    .dynamic : {
        *(.dynamic)
    }

    . = ALIGN(4096);

    .rel : {
        *(.rel .rel.*)
    }

    . = ALIGN(4096);

    .rela : {
        *(.rela .rela.*)
    }

    . = ALIGN(4096);

    .dynsym : {
        *(.dynsym)
    }
    .dynstr : {
        *(.dynstr)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.GNU-stack)
        *(.eh_frame)
    }
}
