; ============================================================================
; kpti_trampoline.asm -- KPTI trampoline stubs
;
; These stubs are copied into the trampoline page at KPTI_TRAMPOLINE_VA.
; They execute under user_cr3 and swap to kernel_cr3 before jumping to
; the real kernel entry points.
;
; Layout within the trampoline page:
;   0x000: kpti_syscall_entry  (S4 -- SYSCALL from ring 3)
;   0x080: kpti_syscall_return (S4 -- SYSRET to ring 3)
;   0x100: kpti_isr_entry      (S5 -- IDT from ring 3)
;   0x180: kpti_isr_return     (S5 -- IRETQ to ring 3)
;
; Per-CPU struct offsets (must match smp.h + kpti.h):
;   gs:104 = kernel_cr3
;   gs:112 = user_cr3
;
; XREF: 02-kernel-core/TODO-17-kernel-security-hardening.md S3
; ============================================================================

[BITS 64]
section .text

; ---- Offsets matching kpti.h ----
%define PCPU_KERNEL_CR3  104
%define PCPU_USER_CR3    112

; ---- Labels exported for kpti_init() to measure sizes ----
global kpti_stub_syscall_entry
global kpti_stub_syscall_entry_end
global kpti_stub_syscall_return
global kpti_stub_syscall_return_end
global kpti_stub_isr_entry
global kpti_stub_isr_entry_end
global kpti_stub_isr_return
global kpti_stub_isr_return_end

; ============================================================================
; SYSCALL entry trampoline (offset 0x000 in trampoline page)
;
; CPU state at entry: ring 0, RCX=user RIP, R11=user RFLAGS, IF cleared.
; GS = user TEB, KERNEL_GS_BASE = per-CPU data.
; CR3 = user_cr3 (no kernel mappings except this page + stacks).
;
; Stub: SWAPGS, load kernel_cr3 from gs:104, write CR3, JMP to real
; syscall_entry (which is in kernel text, now accessible).
; ============================================================================

kpti_stub_syscall_entry:
    swapgs                              ; GS = per-CPU; KERNEL_GS = TEB
    mov rax, [gs:PCPU_KERNEL_CR3]       ; load kernel CR3
    mov cr3, rax                        ; switch to kernel page tables
    ; Now kernel text is mapped -- jump to the real entry.
    ; syscall_entry_inner is the existing syscall_entry minus its SWAPGS
    ; (we already did it). The address is patched by kpti_init().
    mov rax, 0xDEADBEEFDEADBEEF        ; placeholder -- patched at runtime
    jmp rax
kpti_stub_syscall_entry_end:

; ============================================================================
; SYSCALL return trampoline (offset 0x080 in trampoline page)
;
; Called from kernel space just before returning to ring 3.
; Registers: RCX=user RIP, R11=user RFLAGS, RSP=user RSP, RAX=return val.
; Must switch to user_cr3 and SWAPGS before SYSRETQ.
; ============================================================================

kpti_stub_syscall_return:
    mov rax, [gs:PCPU_USER_CR3]         ; load user CR3
    mov cr3, rax                        ; switch to user page tables
    swapgs                              ; GS = user TEB; KERNEL_GS = per-CPU
    o64 sysret                          ; RIP=RCX, RFLAGS=R11|2, ring 3
kpti_stub_syscall_return_end:

; ============================================================================
; ISR entry trampoline (offset 0x100 in trampoline page)
;
; IDT delivery pushed exception frame onto TSS RSP0/IST stack (mapped in
; user_cr3). If we came from ring 3 (CS & 3 != 0), swap to kernel_cr3.
; If from ring 0, CR3 is already kernel_cr3 -- skip.
;
; Stack at entry (pushed by CPU):
;   [rsp+0]  = RIP
;   [rsp+8]  = CS   <-- check RPL
;   [rsp+16] = RFLAGS
;   [rsp+24] = RSP  (if ring 3)
;   [rsp+32] = SS   (if ring 3)
; ============================================================================

kpti_stub_isr_entry:
    test byte [rsp+8], 3               ; check RPL of saved CS
    jz .from_kernel                     ; ring 0 -- skip SWAPGS + CR3 swap
    swapgs                              ; ring 3: GS = per-CPU
    mov rax, [gs:PCPU_KERNEL_CR3]
    mov cr3, rax                        ; switch to kernel CR3
.from_kernel:
    ; Now kernel text is mapped -- jump to isr_common_stub.
    ; Address patched by kpti_init().
    mov rax, 0xCAFEBABECAFEBABE        ; placeholder -- patched at runtime
    jmp rax
kpti_stub_isr_entry_end:

; ============================================================================
; ISR return trampoline (offset 0x180 in trampoline page)
;
; Called before IRETQ when returning to ring 3.
; Must switch to user_cr3 and SWAPGS.
; ============================================================================

kpti_stub_isr_return:
    mov rax, [gs:PCPU_USER_CR3]
    mov cr3, rax                        ; switch to user CR3
    swapgs                              ; GS = user TEB
    iretq                               ; return to ring 3
kpti_stub_isr_return_end:
