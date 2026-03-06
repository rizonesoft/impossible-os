; =============================================================================
; switch_context.asm — Cooperative context switch (x86-64)
;
; void switch_context(uint64_t *old_rsp, uint64_t new_rsp)
;   rdi = pointer to old task's saved RSP (write current RSP here)
;   rsi = new task's saved RSP (restore from here)
;
; Saves callee-saved registers (rbx, rbp, r12-r15) and return address
; on the old stack, switches RSP, restores from new stack, and returns
; into the new task's execution context.
; =============================================================================

[BITS 64]

section .text

global switch_context

switch_context:
    ; --- Save callee-saved registers on OLD stack ---
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Save current RSP into *old_rsp (rdi points to old task's rsp field)
    mov [rdi], rsp

    ; --- Switch to NEW stack ---
    mov rsp, rsi

    ; --- Restore callee-saved registers from NEW stack ---
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; The return address is now at the top of the new stack.
    ; 'ret' pops it and jumps there — resuming the new task.
    ret
