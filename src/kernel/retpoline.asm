; ============================================================================
; retpoline.asm -- Spectre v2 retpoline thunks (TODO-10 S8)
;
; clang-19 with -mretpoline -mretpoline-external-thunk emits
;   call __x86_indirect_thunk_<reg>
; in place of every `call/jmp *<reg>` in compiler-generated code. Each thunk
; "returns" to the real target (held in <reg>) via the classic retpoline trick:
; the speculative path is trapped in an infinite pause/lfence capture loop, while
; the architectural path overwrites the pushed return address with the target and
; RETs to it. This denies the indirect-branch predictor any attacker-trainable
; target, mitigating Spectre v2 on CPUs without (Enhanced) IBRS.
;
; rsp is never an indirect-branch target register, so no thunk is emitted for it.
; Hand-written NASM indirect branches are NOT rewritten by the compiler flag and
; are inventoried separately (see TODO-10 S8 NASM inventory).
; ============================================================================

bits 64
section .text

%macro RETPOLINE_THUNK 1
global __x86_indirect_thunk_%1
__x86_indirect_thunk_%1:
    call %%set_target
%%capture:
    pause
    lfence
    jmp  %%capture
%%set_target:
    mov  [rsp], %1
    ret
%endmacro

RETPOLINE_THUNK rax
RETPOLINE_THUNK rbx
RETPOLINE_THUNK rcx
RETPOLINE_THUNK rdx
RETPOLINE_THUNK rsi
RETPOLINE_THUNK rdi
RETPOLINE_THUNK rbp
RETPOLINE_THUNK r8
RETPOLINE_THUNK r9
RETPOLINE_THUNK r10
RETPOLINE_THUNK r11
RETPOLINE_THUNK r12
RETPOLINE_THUNK r13
RETPOLINE_THUNK r14
RETPOLINE_THUNK r15
