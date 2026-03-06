; ============================================================================
; crt0.asm — C runtime entry point for user-mode programs
;
; This is the first code that runs in a user-mode ELF binary.
; It calls main() and passes the return value to sys_exit().
; ============================================================================

section .text
global _start
extern main

_start:
    ; Clear base pointer for stack traces
    xor rbp, rbp

    ; Call main()
    call main

    ; main() returned in RAX — pass it as exit code to sys_exit()
    mov rdi, rax        ; arg1 = return value from main
    mov rax, 3          ; SYS_EXIT = 3
    int 0x80            ; syscall

    ; Should never reach here
.hang:
    hlt
    jmp .hang
