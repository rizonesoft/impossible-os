; =============================================================================
; test_loader_eif.asm -- Binary-format loader coverage: EIF raw entry code
;
; Minimal entry point for the EIF test binary. Flat x86-64 NASM code
; that performs a single SYS_EXIT(0) via INT 0x80 and halts. Meant
; to be assembled with `nasm -f bin` (no ELF/PE wrapper) so the
; output is pure executable bytes that the EIF builder concatenates
; after the EIF header + segment table.
;
; The EIF format places the entry point at (load_base + entry_point)
; per include/kernel/eif.h, and load_base for this binary is set to
; USER_ELF_BASE (0x800000) so the segment lands in the same 2 MiB
; user ELF region every other test binary uses. entry_point is the
; offset within the segment where _start begins -- for this file,
; _start is the very first byte, so entry_point = 0.
;
; Same ABI reasoning as test_loader_pe.c: INT 0x80 is the proven
; syscall path. SYS_EXIT = 3 (decimal), RDI = 0 (exit status).
; =============================================================================

[bits 64]
[default rel]

; No section directive -- we are emitting a flat binary with
; `nasm -f bin`. The output is a single stream of bytes starting at
; _start. The builder prepends an EIF header + segment table, giving
; the loader a description of where to place these bytes in user
; memory and where the entry point sits within them.

global _start
_start:
    mov     rax, 3          ; SYS_EXIT
    xor     rdi, rdi        ; status = 0
    int     0x80
.halt_loop:
    pause                   ; sys_exit never returns; spin defensively
    jmp     .halt_loop
