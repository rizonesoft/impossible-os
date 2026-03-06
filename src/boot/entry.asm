; =============================================================================
; Kernel Entry Point — GRUB drops us here in 32-bit protected mode
;
; On entry from GRUB (Multiboot2):
;   eax = 0x36D76289 (Multiboot2 magic)
;   ebx = physical address of Multiboot2 info structure
;   CPU is in 32-bit protected mode, paging disabled, A20 enabled
; =============================================================================

section .bss
align 16
stack_bottom:
    resb 16384              ; 16 KiB kernel stack
stack_top:

section .data
    ; Error messages for early boot failures
    err_no_multiboot db "FATAL: Not loaded by a Multiboot2 bootloader!", 0

section .text
global _start
extern kernel_main

; Multiboot2 magic constant
MULTIBOOT2_BOOTLOADER_MAGIC equ 0x36D76289

_start:
    ; -----------------------------------------------------------------
    ; Step 1: Set up the stack
    ; -----------------------------------------------------------------
    mov esp, stack_top

    ; -----------------------------------------------------------------
    ; Step 2: Verify Multiboot2 magic in eax
    ;   If we weren't loaded by a Multiboot2 bootloader, halt.
    ; -----------------------------------------------------------------
    cmp eax, MULTIBOOT2_BOOTLOADER_MAGIC
    jne .no_multiboot

    ; -----------------------------------------------------------------
    ; Step 3: Save Multiboot2 info pointer (ebx) before we clobber it
    ; -----------------------------------------------------------------
    mov edi, ebx            ; edi = multiboot2 info pointer (arg2 for C)
    mov esi, eax            ; esi = multiboot2 magic (arg1 for C)

    ; -----------------------------------------------------------------
    ; Step 4: Call the C kernel entry point
    ;   kernel_main(uint32_t magic, uint32_t *mbi)
    ;   Per cdecl: push args right-to-left onto the stack
    ; -----------------------------------------------------------------
    push edi                ; arg2: multiboot2 info pointer
    push esi                ; arg1: multiboot2 magic
    call kernel_main
    add esp, 8              ; clean up stack (cdecl)

    ; -----------------------------------------------------------------
    ; If kernel_main returns, halt the CPU
    ; -----------------------------------------------------------------
.hang:
    cli
    hlt
    jmp .hang

    ; -----------------------------------------------------------------
    ; Error: not loaded by Multiboot2
    ; Write error message to serial port (COM1 = 0x3F8)
    ; -----------------------------------------------------------------
.no_multiboot:
    mov esi, err_no_multiboot
.serial_loop:
    lodsb                   ; al = next char
    test al, al
    jz .hang                ; null terminator → halt
.wait_serial:
    mov dx, 0x3FD           ; COM1 line status register
    in al, dx
    test al, 0x20           ; transmit buffer empty?
    jz .wait_serial
    mov dx, 0x3F8           ; COM1 data register
    mov al, [esi - 1]       ; reload character (lodsb incremented esi)
    out dx, al
    jmp .serial_loop
