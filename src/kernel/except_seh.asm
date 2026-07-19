; =============================================================================
; except_seh.asm -- ki_seh_setjmp: kernel-mode __try/__except landing capture
;
; int ki_seh_setjmp(KI_JMP_BUF *buf)      SysV entry: rdi = buf
;
; Saves the callee-saved registers (RBX, RBP, R12-R15), the caller RSP AS IT
; WILL BE AFTER this function returns (post-`ret`, i.e. RSP+8 so the return
; address is already popped), and the return RIP into KI_JMP_BUF, then returns
; 0 in EAX. The "second return" (value 1) is NEVER produced here:
; ki_raise_kernel_exception synthesizes it by rewriting the trap frame
; (RIP/RSP/RBX/RBP/R12-R15 <- buf, RAX <- 1, landing RFLAGS normalized) so the
; IRETQ at the end of the fault resumes at the captured RIP exactly as if this
; function had returned 1. This is a setjmp whose matching longjmp is a
; trap-frame rewrite rather than a stack switch.
;
; ASSEMBLY-VISIBLE ABI: the KI_JMP_BUF byte offsets below MUST match
; struct ki_jmp_buf in include/kernel/except.h, where each is pinned by a
; _Static_assert. A field move breaks the C build (the offset asserts) before
; this file can silently desync.
; =============================================================================

[BITS 64]

; --- struct ki_jmp_buf field offsets (include/kernel/except.h) ---
%define JB_Rbx  0x00
%define JB_Rbp  0x08
%define JB_R12  0x10
%define JB_R13  0x18
%define JB_R14  0x20
%define JB_R15  0x28
%define JB_Rsp  0x30
%define JB_Rip  0x38

section .text

global ki_seh_setjmp
ki_seh_setjmp:
    mov     [rdi + JB_Rbx], rbx
    mov     [rdi + JB_Rbp], rbp
    mov     [rdi + JB_R12], r12
    mov     [rdi + JB_R13], r13
    mov     [rdi + JB_R14], r14
    mov     [rdi + JB_R15], r15
    ; caller RSP after `ret` pops the 8-byte return address
    lea     rax, [rsp + 8]
    mov     [rdi + JB_Rsp], rax
    ; return address (currently at the top of the stack)
    mov     rax, [rsp]
    mov     [rdi + JB_Rip], rax
    xor     eax, eax            ; initial call returns 0
    ret
