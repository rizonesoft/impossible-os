; ============================================================================
; crt0.asm -- C runtime entry point for user-mode programs
;
; First code that runs in a user-mode ELF binary. Calls crt_init()
; (which performs the -18 ABI handshake + any future early setup),
; then main(), then sys_exit(main_return).
;
; The handshake lives in C (user/lib/crt_init.c) instead of here so
; it can `#include "abi_numbers.h"` directly -- no NASM macro
; injection, no CPP-vs-asm literal-syntax friction. crt_init() does
; not return if the handshake fails (it calls sys_exit internally);
; so the following `call main` is reached only for well-matched
; kernel+libc pairs.
; ============================================================================

section .text
global _start
extern main
extern crt_init

_start:
    ; Clear base pointer for stack traces
    xor rbp, rbp

    ; -18 ABI handshake + any future early init (crt_init never
    ; returns on mismatch; otherwise falls through to main).
    ; crt_init is call/ret balanced and only writes below RSP, so the
    ; SysV initial stack (argc at [rsp], argv[] at rsp+8) is preserved.
    call crt_init

    ; SysV entry hands main(argc, argv): argc = [rsp], argv = &argv[0] = rsp+8.
    ; crt_init clobbered the arg registers (caller-saved), so (re)load them
    ; from the preserved entry stack right before the call.
    mov rdi, [rsp]          ; arg1 = argc
    lea rsi, [rsp + 8]      ; arg2 = argv (pointer to argv[0])

    ; Call main(argc, argv)
    call main

    ; main() returned in RAX -- pass it as exit code to sys_exit()
    mov rdi, rax        ; arg1 = return value from main
    mov rax, 3          ; SYS_EXIT = 3
    int 0x80            ; syscall

    ; Should never reach here
.hang:
    hlt
    jmp .hang
