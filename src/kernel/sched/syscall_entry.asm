; ============================================================================
; syscall_entry.asm -- SYSCALL/SYSRET fast path entry point
;
; On SYSCALL, CPU does:
;   RCX = user RIP, R11 = user RFLAGS
;   CS/SS loaded from MSR_IA32_STAR
;   RFLAGS &= ~FMASK (IF cleared)
;   RIP = LSTAR (this function)
;
; Windows x64 syscall ABI:
;   RAX = service number
;   R10 = arg1 (ntdll: mov r10, rcx before SYSCALL)
;   RDX = arg2, R8 = arg3, R9 = arg4
;
; On SYSRET:
;   RIP = RCX, RFLAGS = (R11 & 0x3C7FD7) | 2
;   CS = STAR[63:48]+16, SS = STAR[63:48]+8
; ============================================================================

section .text
bits 64

; Per-CPU struct offsets (must match struct per_cpu_data in smp.h exactly)
%define PCPU_SYSCALL_RSP0      24       ; gs:24 = kernel stack top
%define PCPU_USER_RSP_SCRATCH  32       ; gs:32 = scratch for user RSP

global syscall_entry
extern syscall_dispatch_fast

syscall_entry:
    ; ---- Entry: ring 3 -> ring 0 ----
    ; IF is clear (FMASK). No interrupts until we sti.
    ; GS = user TEB; KERNEL_GS_BASE = per-CPU data.
    swapgs                              ; GS = per-CPU; KERNEL_GS = TEB

    ; Save user RSP to per-CPU scratch area
    mov [gs:PCPU_USER_RSP_SCRATCH], rsp

    ; Load kernel RSP from per-CPU (set by context switch)
    mov rsp, [gs:PCPU_SYSCALL_RSP0]

    ; Build stack frame (push order determines pop order)
    push qword [gs:PCPU_USER_RSP_SCRATCH] ; [rsp+64] user RSP
    push rcx                            ; [rsp+56] user RIP (CPU saved)
    push r11                            ; [rsp+48] user RFLAGS (CPU saved)
    push rbp                            ; [rsp+40]
    push rbx                            ; [rsp+32]
    push r12                            ; [rsp+24]
    push r13                            ; [rsp+16]
    push r14                            ; [rsp+8]
    push r15                            ; [rsp+0]

    ; Safe to enable interrupts now (on kernel stack with saved state)
    sti

    ; Call C dispatcher
    ; syscall_dispatch_fast(number, a1, a2, a3, a4, a5)
    ; SysV x86-64: rdi, rsi, rdx, rcx, r8, r9
    mov rdi, rax                        ; service number
    mov rsi, r10                        ; arg1 (R10 = original RCX from ntdll)
    ; rdx = arg2 (already in place)
    mov rcx, r8                         ; arg3
    mov r8, r9                          ; arg4
    xor r9, r9                          ; arg5 = 0 (future: from user stack)

    call syscall_dispatch_fast

    ; RAX = NTSTATUS return value

    ; ---- Exit: ring 0 -> ring 3 ----
    cli                                 ; disable interrupts for sysret

    ; Restore callee-save registers (reverse push order)
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    pop r11                             ; user RFLAGS -> R11 for SYSRET
    pop rcx                             ; user RIP -> RCX for SYSRET
    pop rsp                             ; user RSP restored directly

    swapgs                              ; GS = user TEB; KERNEL_GS = per-CPU
    o64 sysret                          ; RIP=RCX, RFLAGS=R11|2, ring 3
