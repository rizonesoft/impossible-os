/* ============================================================================
 * lz4_merge.ld -- section-merge script for the scoped LZ4 dead-strip relink
 *
 * lz4.c is compiled with -ffunction-sections so the partial relink
 * (ld -r --gc-sections, rooted on the three block entry points) can drop the
 * ~50 KiB of unused streaming/dictionary/partial/fast LZ4 variants. That
 * leaves the survivors in per-function .text.LZ4_* / .rodata.* sections.
 *
 * The kernel linker script (src/boot/linker.ld) collects .text via *(.text)
 * only -- it does NOT glob .text.*. Without this merge the surviving LZ4
 * functions would land as orphan sections outside the executable .text mapping
 * and fault on instruction fetch (NX) when first called. Merging them back
 * into plain .text / .rodata here keeps the dead-strip fully scoped to the LZ4
 * object without touching the global kernel link.
 * ============================================================================ */
SECTIONS {
  .text   : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data   : { *(.data .data.*) }
  .bss    : { *(.bss .bss.* COMMON) }
}
