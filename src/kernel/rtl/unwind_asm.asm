; =============================================================================
; unwind_asm.asm -- RtlRestoreContext terminal resume primitive (x86-64)
;
; void RtlRestoreContext(CONTEXT *ctx, EXCEPTION_RECORD *rec)
;   SysV entry: rdi = ctx, rsi = rec (rec is ignored -- ABI parity only)
;
; Loads the CONTROL + INTEGER groups (all 16 GPRs, RSP, RIP, RFLAGS) from ctx
; and transfers control to ctx->Rip with ctx->Rsp restored. Does NOT return.
; This is the same-CPL kernel resume used at the end of RtlUnwindEx; it never
; touches the FP/XMM/segment/debug groups (a kernel CONTEXT never carries them --
; context_from_frame / frame_from_context restore only CONTROL + INTEGER), so it
; is independent of the lazy-FPU CR0.TS protocol.
;
; ASSEMBLY-VISIBLE ABI: the CTX_* byte offsets below MUST match struct _CONTEXT
; in include/kernel/except.h. That header pins each with _Static_assert, and
; src/kernel/rtl/unwind.c re-pins the exact offsets this file hardcodes so a
; field move breaks the C build before this file can silently desync.
; =============================================================================

[BITS 64]

; --- struct _CONTEXT field offsets (include/kernel/except.h) ---
%define CTX_ContextFlags   0x30
%define CTX_EFlags         0x44
%define CTX_Rax            0x78
%define CTX_Rcx            0x80
%define CTX_Rdx            0x88
%define CTX_Rbx            0x90
%define CTX_Rsp            0x98
%define CTX_Rbp            0xA0
%define CTX_Rsi            0xA8
%define CTX_Rdi            0xB0
%define CTX_R8             0xB8
%define CTX_R9             0xC0
%define CTX_R10            0xC8
%define CTX_R11            0xD0
%define CTX_R12            0xD8
%define CTX_R13            0xE0
%define CTX_R14            0xE8
%define CTX_R15            0xF0
%define CTX_Rip            0xF8

section .text

global RtlRestoreContext

RtlRestoreContext:
    ; The kernel is built -mno-red-zone, so a staging area written below a stack
    ; pointer is not interrupt-safe; disable interrupts across the whole sequence.
    ; The terminal transfer is a same-CPL IRETQ so RIP + RFLAGS + RSP + SS all take
    ; effect ATOMICALLY, after every GPR is restored -- a target with IF/TF set can
    ; therefore never enter an interrupt or single-step inside this trampoline with
    ; a half-restored register file (a plain popfq-then-pop sequence could).
    cli
    ; rdi = ctx. Build the IRETQ frame just below the target RSP:
    ;   [rax-40]=RIP  [rax-32]=CS  [rax-24]=RFLAGS  [rax-16]=RSP  [rax-8]=SS
    ; (IRETQ in long mode always pops all five, even for a same-CPL return.)
    mov     rax, [rdi + CTX_Rsp]        ; rax = target rsp
    mov     rcx, [rdi + CTX_Rip]
    mov     [rax - 40], rcx             ; RIP
    xor     ecx, ecx
    mov     cx, cs
    mov     [rax - 32], rcx             ; CS (current kernel selector; same-CPL)
    mov     ecx, [rdi + CTX_EFlags]     ; 32-bit load zero-extends (RFLAGS hi=0)
    mov     [rax - 24], rcx             ; RFLAGS
    mov     [rax - 16], rax             ; RSP (target)
    xor     ecx, ecx
    mov     cx, ss
    mov     [rax - 8], rcx              ; SS (current kernel selector; same-CPL)

    ; Stage rdi/rax/rcx/rdx below the IRETQ frame (we need rdi as the ctx pointer
    ; and rax/rcx as scratch until the very end); popped just before IRETQ.
    lea     rax, [rax - 40]             ; rax = IRETQ frame base
    mov     rcx, [rdi + CTX_Rdi]
    mov     [rax - 8], rcx              ; staged rdi
    mov     rcx, [rdi + CTX_Rax]
    mov     [rax - 16], rcx             ; staged rax
    mov     rcx, [rdi + CTX_Rcx]
    mov     [rax - 24], rcx             ; staged rcx
    mov     rcx, [rdi + CTX_Rdx]
    mov     [rax - 32], rcx             ; staged rdx

    ; Directly restore the registers that are neither our pointer nor scratch.
    mov     rbx, [rdi + CTX_Rbx]
    mov     rbp, [rdi + CTX_Rbp]
    mov     rsi, [rdi + CTX_Rsi]
    mov     r8,  [rdi + CTX_R8]
    mov     r9,  [rdi + CTX_R9]
    mov     r10, [rdi + CTX_R10]
    mov     r11, [rdi + CTX_R11]
    mov     r12, [rdi + CTX_R12]
    mov     r13, [rdi + CTX_R13]
    mov     r14, [rdi + CTX_R14]
    mov     r15, [rdi + CTX_R15]

    ; Switch to the staging area, pop the 4 staged registers, then IRETQ -- which
    ; atomically loads RIP, CS, RFLAGS, RSP, SS. rsp ends at ctx->Rsp.
    lea     rsp, [rax - 32]
    pop     rdx
    pop     rcx
    pop     rax
    pop     rdi
    iretq                               ; -> ctx->Rip, rsp = ctx->Rsp, RFLAGS atomic

; =============================================================================
; rtl_restore_selftest -- unit-test round-trip harness for RtlRestoreContext.
;
;   int rtl_restore_selftest(CONTEXT *ctx, uint64_t out[3])
;     rdi = ctx (caller fills Rbx/R12 with sentinels + Rsp with a scratch top;
;               this routine OVERRIDES ctx->Rip to its own landing label),
;     rsi = out (landing writes out[0]=rbx, out[1]=r12, out[2]=rsp -- captured
;               after the resume, so the C caller inspects MEMORY, never a
;               register the resume clobbered).
;   Returns 1 after a clean round trip.
;
; A restore transfers ALL registers, so the C caller cannot observe the resumed
; state directly without the compiler's callee-saved assumptions being violated.
; This harness saves every callee-saved register + RSP first, drives the resume
; into its own landing label, records the resumed rbx/r12/rsp to the caller's
; buffer, then restores the saved state and returns normally. The rrst_* save
; slots are TEST-ONLY scratch, written and read within a single non-reentrant
; call on one CPU (the unit-test thread), so they need no locking.
; =============================================================================

section .bss
rrst_out:        resq 1
rrst_saved_rsp:  resq 1
rrst_saved_rbx:  resq 1
rrst_saved_rbp:  resq 1
rrst_saved_r12:  resq 1
rrst_saved_r13:  resq 1
rrst_saved_r14:  resq 1
rrst_saved_r15:  resq 1

section .text

global rtl_restore_selftest
extern RtlRestoreContext

rtl_restore_selftest:
    mov     [rel rrst_saved_rbx], rbx
    mov     [rel rrst_saved_rbp], rbp
    mov     [rel rrst_saved_r12], r12
    mov     [rel rrst_saved_r13], r13
    mov     [rel rrst_saved_r14], r14
    mov     [rel rrst_saved_r15], r15
    mov     [rel rrst_saved_rsp], rsp
    mov     [rel rrst_out], rsi
    ; Point ctx->Rip at our landing label so the resume returns into us.
    lea     rax, [rel .land]
    mov     [rdi + CTX_Rip], rax
    call    RtlRestoreContext           ; rdi = ctx; does not return here
    ud2                                 ; unreachable

.land:
    ; Resumed: rbx/r12/rsp carry ctx's values. Record them to the caller buffer.
    mov     rax, [rel rrst_out]
    mov     [rax + 0], rbx
    mov     [rax + 8], r12
    mov     [rax + 16], rsp
    ; Restore our own callee-saved state + rsp and return to the C caller.
    mov     rbx, [rel rrst_saved_rbx]
    mov     rbp, [rel rrst_saved_rbp]
    mov     r12, [rel rrst_saved_r12]
    mov     r13, [rel rrst_saved_r13]
    mov     r14, [rel rrst_saved_r14]
    mov     r15, [rel rrst_saved_r15]
    mov     rsp, [rel rrst_saved_rsp]
    mov     eax, 1
    ret
