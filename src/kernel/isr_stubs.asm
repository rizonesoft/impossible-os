; =============================================================================
; isr_stubs.asm -- ISR and IRQ entry stubs (x86-64)
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
; Common interrupt handler -- saves state, calls C, restores state
; =============================================================================
isr_common_stub:
    ; NOTE: clac (SMAP) removed -- causes #UD on CPUs without SMAP CPUID
    ; support (e.g. QEMU TCG). Re-add via alternatives patching when SMAP
    ; is actually enabled (requires per-process page tables).

    ; ---- swapgs on ring-3 → ring-0 entry ----
    ; If we came from ring 3 (CS & 3 != 0), swap GS so the kernel sees
    ; per-CPU data via GS, and KERNEL_GS_BASE holds the user TEB address.
    ;
    ; Stack at this point:
    ;   [rsp+0]  = int_no (pushed by stub)
    ;   [rsp+8]  = err_code (pushed by stub or CPU)
    ;   [rsp+16] = RIP  (CPU)
    ;   [rsp+24] = CS   (CPU)  ← check this
    ;   [rsp+32] = RFLAGS
    ;   [rsp+40] = RSP  (user, if ring-3 entry)
    ;   [rsp+48] = SS   (user, if ring-3 entry)
    ;
    ; SYMMETRY REQUIREMENT: every swapgs here MUST have a matching swapgs
    ; on the exit path. A missing or double swapgs corrupts GS for all
    ; subsequent kernel code and is extremely hard to debug. If you modify
    ; this code, verify both paths in lockstep.
    test byte [rsp+24], 3     ; check RPL bits of saved CS
    jz .no_swapgs_entry       ; came from ring 0 → skip
    swapgs                    ; ring 3 → ring 0: swap TEB ↔ per-CPU
.no_swapgs_entry:
    ; Spectre v1 swapgs (CVE-2019-1125, TODO-10 S8): the conditional swapgs above
    ; can be mis-speculated, leaving GS pointing at the wrong base while the CPU
    ; speculatively dereferences gs:-relative addresses. LFENCE here serializes
    ; both paths before any GS-relative access (Linux FENCE_SWAPGS_KERNEL_ENTRY).
    lfence

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
    ; isr_handler returns the frame pointer to restore in rax.
    ; Normally it returns the same frame, but the scheduler may return
    ; a different task's frame to perform a preemptive context switch.
    mov rsp, rax

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

    ; ---- swapgs on ring-0 → ring-3 exit ----
    ; If returning to ring 3 (CS & 3 != 0), swap GS back so user-mode
    ; sees TEB via GS. Must match the entry swapgs exactly.
    ;
    ; Stack at this point (iret frame):
    ;   [rsp+0]  = RIP
    ;   [rsp+8]  = CS   ← check this
    ;   [rsp+16] = RFLAGS
    ;   [rsp+24] = RSP  (user)
    ;   [rsp+32] = SS   (user)
    test byte [rsp+8], 3      ; check RPL bits of return CS
    jz .no_swapgs_exit        ; returning to ring 0 → skip
    swapgs                    ; ring 0 → ring 3: swap per-CPU ↔ TEB
.no_swapgs_exit:

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

; =============================================================================
; Software interrupt stubs
; =============================================================================
ISR_NOERRCODE 128     ; syscall -- INT 0x80 (user → kernel)
ISR_NOERRCODE 129     ; yield() -- cooperative task switch (INT 0x81)

; =============================================================================
; Dynamic / Synthetic interrupt stubs (vectors 48-255)
;
; Covers MSI/MSI-X, VMBus SINT, STIMER, LAPIC spurious, and any other
; vector Hyper-V or hardware may deliver.  Without these, any interrupt
; on an unpopulated IDT entry causes #GP (null descriptor).
;
; Vectors 128-129 are skipped -- already defined above.
; =============================================================================
%assign i 48
%rep (256 - 48)
  %if i != 128 && i != 129
    ISR_NOERRCODE i
  %endif
  %assign i i+1
%endrep
