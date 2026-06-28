/* ============================================================================
 * Linker Script -- 64-bit kernel memory layout
 *
 * The UEFI bootloader (src/boot/uefi/bootx64.c) loads kernel.exe as an ELF
 * file, resolves the `kernel_main` symbol from the ELF symbol table, and
 * calls it directly in 64-bit Long Mode. The ELF entry point declared
 * here (ENTRY) is the conventional load address but is not the symbol the
 * loader actually transfers control to -- the symbol-table lookup is
 * authoritative.
 *
 * Alternate boot protocols (Multiboot2, GRUB, Limine, legacy BIOS, EFI
 * stub direct boot, kexec) are explicit non-goals per the alt-boot
 * policy doc (docs/boot/alt-boot.md). The 1 MiB load address remains for
 * historical UEFI loader compatibility; bootx64.c relocates the kernel
 * to wherever the ELF PT_LOAD headers request.
 * ============================================================================ */

ENTRY(kernel_main)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    /* Kernel loaded at 1 MiB (matches the historical entry-point address; the
     * UEFI loader places PT_LOAD segments per the ELF program headers). */
    . = 1M;
    __kernel_start = .;

    /* Code section */
    .text ALIGN(4K) :
    {
        __text_start = .;
        *(.text)
        __text_end = .;
    }

    /* Read-only data. `*(.rodata .rodata.*)` collects the orphan sections
     * Clang emits for string literals + constants (.rodata.str1.*,
     * .rodata.cst*) so the full rodata span -- not just *(.rodata) -- is
     * covered by the W^X read-only pass (kernel_rodata_protect). */
    .rodata ALIGN(4K) :
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
    }

    /* Boot-protocol descriptor (boot_proto.c). Placed in its own
     * output section so the bootloader can find it by name in the
     * ELF section header table. KEEP(*) prevents LTO / --gc-sections
     * from dropping it. Lives inside the kernel's PT_LOAD segment
     * (no separate program header needed -- bootloader walks the
     * section table, not program headers, to locate it). `kernel_boot_proto`
     * is `const` and only read (by the bootloader, before EBS), so it joins
     * the W^X read-only span below. */
    .bootproto ALIGN(8) :
    {
        KEEP(*(.bootproto))
    }

    /* Initialized data -- first WRITABLE section, on a fresh page.
     * __rodata_end is pinned to the .data start so the W^X read-only span
     * [__rodata_start, __rodata_end) brackets EVERY allocated non-writable
     * section the linker places between .rodata and .data: .bootproto, the
     * orphan .reloc, .firmware_capsule_refused (const refusal sentinels), and
     * any future read-only orphan (lld ranks RO orphans before .data). All
     * are const / never written at runtime, so bracketing by data-start --
     * not by enumerating section names -- is drift-proof. */
    .data ALIGN(4K) :
    {
        __rodata_end = .;
        /* `*(.data .data.*)` -- the `.data.*` glob mirrors the `.bss .bss.*`
         * rule below: without it, a named/retained `.data.*` subsection would
         * orphan past __kernel_end and overlap PMM bitmap territory (the exact
         * drift Codex caught for .bss). None exist today under -fno-pie. */
        *(.data .data.*)
    }

    /* Uninitialized data (BSS) -- zeroed by the UEFI bootloader when
     * copying PT_LOAD segments with p_memsz > p_filesz.
     *
     * `*(.bss .bss.*)` catches BOTH the catch-all .bss section AND any
     * retained-or-named subsections (e.g. `.bss.bsp_boot_stack` emitted
     * by clang when a C object has `__attribute__((retain))`). Without
     * the `.bss.*` glob, retained subsections land AFTER __kernel_end
     * and overlap PMM bitmap territory -- Codex caught a real boot-time
     * stack/bitmap overlap from exactly that drift. */
    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    /* End of kernel -- PMM bitmap is placed here */
    . = ALIGN(4K);
    __kernel_end = .;
}
