; ============================================================================
; reloc.asm — Provide a minimal PE .reloc section for UEFI applications
;
; OVMF requires a base relocation directory entry in the PE/COFF binary.
; Since x86-64 PIC code (compiled with -fpic) uses RIP-relative addressing,
; we typically have zero actual relocations. But OVMF RelocateImage() still
; needs the section to exist with at least one valid block header.
;
; This creates a minimal .reloc section with a single empty relocation block
; (page RVA 0, block size 10 bytes, two Type 0 entries = IMAGE_REL_BASED_ABSOLUTE).
; ============================================================================

section .reloc

; Relocation block header:
;   VirtualAddress (4 bytes): 0x00000000 — page RVA of relocations
;   SizeOfBlock (4 bytes):    0x0000000A — 10 bytes (header + 1 ABSOLUTE entry)
;   TypeOffset[0] (2 bytes):  0x0000      — IMAGE_REL_BASED_ABSOLUTE (no-op padding)
dd 0           ; VirtualAddress = 0
dd 10          ; SizeOfBlock = 10 (8 header + 2 padding)
dw 0           ; Type 0 = IMAGE_REL_BASED_ABSOLUTE (no-op)
