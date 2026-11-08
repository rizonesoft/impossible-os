; ============================================================================
; reloc.asm -- Provide a minimal PE .reloc section for UEFI applications
;
; OVMF requires a base relocation directory entry in the PE/COFF binary, and
; RelocateImage() needs the section to exist with at least one valid block
; header. The block is deliberately empty, so firmware patches nothing: code is
; RIP-relative, and a pointer stored in data (a string table, a switch lookup
; table) is an ELF RELATIVE relocation that loader_self_relocate() in bootx64.c
; applies from .rela on entry.
;
; This creates a minimal .reloc section with a single empty relocation block
; (page RVA 0, block size 12 bytes, two Type 0 entries = IMAGE_REL_BASED_ABSOLUTE).
; PE/COFF requires each block to be 32-bit aligned, so the second entry pads it.
; ============================================================================

section .reloc

; Relocation block header:
;   VirtualAddress (4 bytes): 0x00000000 -- page RVA of relocations
;   SizeOfBlock (4 bytes):    0x0000000C -- 12 bytes (header + 2 ABSOLUTE entries)
;   TypeOffset[0..1] (4 bytes): 0x0000   -- IMAGE_REL_BASED_ABSOLUTE (no-op padding)
dd 0           ; VirtualAddress = 0
dd 12          ; SizeOfBlock = 12 (8 header + 4 padding, 32-bit aligned)
dw 0           ; Type 0 = IMAGE_REL_BASED_ABSOLUTE (no-op)
dw 0           ; Type 0 = IMAGE_REL_BASED_ABSOLUTE (no-op)
