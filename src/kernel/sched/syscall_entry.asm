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
extern transition_ring_record; fast-path transition ring hook

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

    ; ---- transition ring: record SYSCALL entry ----
    ; transition_ring_record(direction=TO_KERNEL, user_rip, user_rsp)
    ;   SysV args:  rdi, rsi, rdx. Preserve rax/r10/rdx across the call
    ;   since the dispatcher call below consumes them as service/arg1/arg2.
    ;
    ; After the 9-push sequence above (lines 43-51), the kernel stack
    ; holds: [rsp+0x30]=r11(user RFLAGS), [rsp+0x38]=rcx(user RIP),
    ; [rsp+0x40]=user RSP. Our 3 extra pushes (rax,r10,rdx) shift all
    ; offsets by +0x18, so user RIP is at [rsp+0x50]. User RSP is
    ; read from the stable per-CPU scratch slot instead of the stack
    ; copy, which avoids the shift arithmetic entirely.
    push rax                            ; save service
    push r10                            ; save arg1
    push rdx                            ; save arg2
    xor edi, edi                        ; direction = TRANSITION_DIR_TO_KERNEL (0)
    mov rsi, [rsp+0x50]                 ; user RIP from stacked rcx
    mov rdx, [gs:PCPU_USER_RSP_SCRATCH] ; user RSP from per-CPU scratch
    call transition_ring_record
    pop rdx                             ; restore arg2
    pop r10                             ; restore arg1
    pop rax                             ; restore service

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

    ; ---- transition ring: record SYSCALL exit ----
    ; Stack is still the full 9-push frame (no extra pushes here).
    ; User RIP at [rsp+0x38], user RSP at [rsp+0x40]. RAX holds the
    ; NTSTATUS return; preserve it across the call.
    push rax                            ; save NTSTATUS for sysret caller
    mov edi, 1                          ; direction = TRANSITION_DIR_TO_USER (1)
    mov rsi, [rsp+0x40]                 ; user RIP (rcx slot, shifted by +8 for our rax push)
    mov rdx, [rsp+0x48]                 ; user RSP (user_rsp slot, shifted by +8)
    call transition_ring_record
    pop rax                             ; restore NTSTATUS

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
