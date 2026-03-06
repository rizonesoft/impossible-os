; =============================================================================
; Kernel Entry Point — GRUB drops us here in 32-bit protected mode
; We set up a stack and call kernel_main (64-bit transition comes later)
; =============================================================================

section .bss
align 16
stack_bottom:
    resb 16384              ; 16 KiB kernel stack
stack_top:

section .text
global _start
extern kernel_main

_start:
    ; GRUB sets eax = 0x36D76289 (Multiboot2 magic)
    ; GRUB sets ebx = pointer to Multiboot2 info structure

    ; Set up the stack
    mov esp, stack_top

    ; Push Multiboot2 info pointer and magic (for kernel_main)
    push 0                  ; padding for alignment
    push ebx                ; multiboot2 info pointer
    push eax                ; multiboot2 magic

    ; Call the C kernel entry point
    call kernel_main

    ; If kernel_main returns, halt the CPU
.hang:
    cli
    hlt
    jmp .hang
