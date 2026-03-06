; =============================================================================
; isr_stubs.asm — ISR and IRQ entry stubs (x86-64)
;
; CPU exceptions (0-31): some push error codes, some don't.
; Hardware IRQs (32-47): remapped by PIC to interrupts 32-47.
;
; Each stub pushes the interrupt number (and a dummy error code if needed),
; then jumps to the common handler which saves all registers and calls
; the C dispatcher isr_handler().
; =============================================================================

[BITS 64]

section .text

; External C handler
extern isr_handler

; =============================================================================
; Common interrupt handler — saves state, calls C, restores state
; =============================================================================
isr_common_stub:
    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Pass pointer to interrupt_frame as first argument (rdi)
    mov rdi, rsp

    ; Align stack to 16 bytes (required by System V ABI)
    ; RSP might not be aligned after all the pushes
    mov rbp, rsp
    and rsp, ~0xF
    call isr_handler
    mov rsp, rbp

    ; Restore all general-purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Remove int_no and err_code from stack
    add rsp, 16

    ; Return from interrupt
    iretq

; =============================================================================
; Macro: ISR stub WITHOUT error code (push dummy 0)
; =============================================================================
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push qword 0            ; dummy error code
    push qword %1            ; interrupt number
    jmp isr_common_stub
%endmacro

; =============================================================================
; Macro: ISR stub WITH error code (CPU pushes it automatically)
; =============================================================================
%macro ISR_ERRCODE 1
global isr%1
isr%1:
    ; error code already pushed by CPU
    push qword %1            ; interrupt number
    jmp isr_common_stub
%endmacro

; =============================================================================
; Macro: IRQ stub (mapped to interrupt 32+n)
; =============================================================================
%macro IRQ 2
global irq%1
irq%1:
    push qword 0            ; dummy error code
    push qword %2            ; interrupt number (32 + irq_number)
    jmp isr_common_stub
%endmacro

; =============================================================================
; CPU Exceptions (ISR 0-31)
; =============================================================================
ISR_NOERRCODE 0      ; Division By Zero
ISR_NOERRCODE 1      ; Debug
ISR_NOERRCODE 2      ; Non-Maskable Interrupt
ISR_NOERRCODE 3      ; Breakpoint
ISR_NOERRCODE 4      ; Overflow
ISR_NOERRCODE 5      ; Bound Range Exceeded
ISR_NOERRCODE 6      ; Invalid Opcode
ISR_NOERRCODE 7      ; Device Not Available
ISR_ERRCODE   8      ; Double Fault
ISR_NOERRCODE 9      ; Coprocessor Segment Overrun
ISR_ERRCODE   10     ; Invalid TSS
ISR_ERRCODE   11     ; Segment Not Present
ISR_ERRCODE   12     ; Stack-Segment Fault
ISR_ERRCODE   13     ; General Protection Fault
ISR_ERRCODE   14     ; Page Fault
ISR_NOERRCODE 15     ; Reserved
ISR_NOERRCODE 16     ; x87 FP Exception
ISR_ERRCODE   17     ; Alignment Check
ISR_NOERRCODE 18     ; Machine Check
ISR_NOERRCODE 19     ; SIMD FP Exception
ISR_NOERRCODE 20     ; Virtualization Exception
ISR_ERRCODE   21     ; Control Protection Exception
ISR_NOERRCODE 22     ; Reserved
ISR_NOERRCODE 23     ; Reserved
ISR_NOERRCODE 24     ; Reserved
ISR_NOERRCODE 25     ; Reserved
ISR_NOERRCODE 26     ; Reserved
ISR_NOERRCODE 27     ; Reserved
ISR_NOERRCODE 28     ; Hypervisor Injection
ISR_ERRCODE   29     ; VMM Communication Exception
ISR_ERRCODE   30     ; Security Exception
ISR_NOERRCODE 31     ; Reserved

; =============================================================================
; Hardware IRQs (remapped to interrupts 32-47)
; =============================================================================
IRQ  0, 32           ; PIT Timer
IRQ  1, 33           ; Keyboard
IRQ  2, 34           ; Cascade
IRQ  3, 35           ; COM2
IRQ  4, 36           ; COM1
IRQ  5, 37           ; LPT2
IRQ  6, 38           ; Floppy
IRQ  7, 39           ; LPT1 / Spurious
IRQ  8, 40           ; CMOS RTC
IRQ  9, 41           ; Free
IRQ 10, 42           ; Free
IRQ 11, 43           ; Free
IRQ 12, 44           ; PS/2 Mouse
IRQ 13, 45           ; FPU
IRQ 14, 46           ; Primary ATA
IRQ 15, 47           ; Secondary ATA
