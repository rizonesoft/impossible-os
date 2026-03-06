; =============================================================================
; gdt_asm.asm — GDT and TSS loading helpers (x86-64)
;
; Called from gdt.c after populating the GDT in memory.
; =============================================================================

[BITS 64]

section .text

; void gdt_flush(uint64_t gdtr_addr)
;   rdi = pointer to GDTR structure
;   Loads the new GDT and reloads all segment registers
global gdt_flush
gdt_flush:
    lgdt [rdi]              ; Load the GDT from the pointer in rdi

    ; Reload CS by doing a far return
    ; Push the new code segment selector and the return address
    mov ax, 0x10            ; kernel data segment for other regs first
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far jump to reload CS with the kernel code segment (0x08)
    ; In 64-bit mode, we use a far return trick
    pop rdi                 ; save return address
    mov rax, 0x08           ; kernel code segment selector
    push rax                ; push CS
    push rdi                ; push return address
    retfq                   ; far return → loads CS:RIP

; void tss_flush(uint16_t selector)
;   di = TSS selector (e.g. 0x28)
;   Loads the Task Register with the TSS selector
global tss_flush
tss_flush:
    ltr di
    ret
