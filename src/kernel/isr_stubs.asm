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
extern g_mds_verw_active      ; MDS VERW gate byte (TODO-10 S19)
extern g_mds_verw_sel         ; VERW 16-bit selector operand
extern g_nmi_depth            ; per-CPU NMI nesting depth (idt.c, TODO-10 S22)

; Scale of one g_nmi_depth entry. idt.c static-asserts the C element size
; against this value, and static-asserts CPU_PANIC_SAFE_ID_MASK == 0xFF, so the
; `shr ebx, 24` below cannot drift from the index the C helpers compute.
%define NMI_DEPTH_ENTRY_SIZE 4

; -----------------------------------------------------------------------------
; Macros: move this CPU's NMI nesting depth (TODO-10 S22)
;
; The index is derived the SAME way include/kernel/cpu_security.h's
; cpu_panic_safe_apic_id() derives it -- CPUID leaf 1, EBX[31:24] -- because
; every abort-context consumer must agree on one identity, and CPUID is the
; only source needing neither GS nor a memory operand. That matters here: the
; raise runs before the conditional swapgs, so GS may still hold the user base.
;
; CPUID clobbers EAX/EBX/ECX/EDX, so each macro saves exactly those four. On
; entry those four stack writes are the ONLY work preceding the marker; see the
; residual note on isr2.
; -----------------------------------------------------------------------------
; The two macros are deliberately NOT symmetric, because the window each has to
; minimise sits on the opposite side of the marker. RAISE minimises the writes
; that run BEFORE the depth goes up; LOWER minimises the reads that run AFTER it
; comes down. Folding them into one shared helper forces one to carry the
; other's ordering, which is exactly how the first version of this code left
; four register restores executing with the depth already clear.

%macro NMI_DEPTH_RAISE 0
    push rax                  ; the four writes that precede the marker
    push rbx
    push rcx
    push rdx
    mov eax, 1
    xor ecx, ecx
    cpuid                     ; EBX[31:24] = initial APIC id
    shr ebx, 24               ; rbx = 8-bit panic-safe CPU id (zero-extended)
    lea rcx, [rel g_nmi_depth]
    lock inc dword [rcx + rbx * NMI_DEPTH_ENTRY_SIZE]
    pop rdx                   ; past the marker, so these restores are covered
    pop rcx
    pop rbx
    pop rax
%endmacro

%macro NMI_DEPTH_LOWER 0
    ; rcx is pushed FIRST so it is restored LAST. The second LEA folds the index
    ; into rcx, so rcx ALONE has to be live when the counter is written, which
    ; lets rdx/rbx/rax be restored while the depth is still raised. Exactly one
    ; register restore then runs with the depth down. Restoring all four after
    ; the write -- the obvious shape, and the one this code shipped first --
    ; leaves four faultable stack reads inside the window this section closes.
    push rcx
    push rax
    push rbx
    push rdx
    mov eax, 1
    xor ecx, ecx
    cpuid
    shr ebx, 24
    lea rcx, [rel g_nmi_depth]
    lea rcx, [rcx + rbx * NMI_DEPTH_ENTRY_SIZE]   ; rcx = THIS CPU's entry
    pop rdx                   ; still covered -- the depth is not down yet
    pop rbx
    pop rax
    ; Saturate at zero, mirroring idt_nmi_exit(). An unbalanced lower would wrap
    ; to 0xFFFFFFFF and pin this CPU in "inside NMI" for the rest of the boot,
    ; disabling the guarded read on a CPU that is not in an NMI at all. Only this
    ; CPU writes its own entry, and no NMI can nest here (hardware NMI blocking
    ; holds until IRETQ), so the test needs no atomicity of its own.
    cmp dword [rcx], 0
    je %%depth_already_zero
    lock dec dword [rcx]
%%depth_already_zero:
    pop rcx                   ; the ONLY restore that runs with the depth down
%endmacro

; =============================================================================
; Macro: the common interrupt body -- saves state, calls C, restores state
;
; Instantiated TWICE so the ordinary and NMI paths cannot drift: %1 = 0 is the
; body every vector shares, %1 = 1 is the NMI-only copy whose epilogue lowers
; the nesting depth. This is build-time parameterization, not a runtime test:
; the shared path must not pay a per-interrupt branch for a counter only the NMI
; path reads, which is exactly why S18 could not close the epilogue window here.
; =============================================================================
%macro ISR_STUB_BODY 1
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
    ;   [rsp+40] = RSP  (ALWAYS present -- long mode pushes SS:RSP on every
    ;                    interrupt regardless of CPL; user RSP on ring-3 entry,
    ;                    interrupted kernel RSP on ring-0 entry)
    ;   [rsp+48] = SS   (ALWAYS present -- see RSP note; user SS vs kernel SS)
    ;
    ; SYMMETRY REQUIREMENT: every swapgs here MUST have a matching swapgs
    ; on the exit path. A missing or double swapgs corrupts GS for all
    ; subsequent kernel code and is extremely hard to debug. If you modify
    ; this code, verify both paths in lockstep.
    test byte [rsp+24], 3     ; check RPL bits of saved CS
    jz %%no_swapgs_entry      ; came from ring 0 → skip
    swapgs                    ; ring 3 → ring 0: swap TEB ↔ per-CPU
%%no_swapgs_entry:
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
    ; Stack at this point (iret frame). IRETQ ALWAYS pops all five in long mode
    ; (SS:RSP too), for a same-CPL ring-0 return as well as a ring-3 return:
    ;   [rsp+0]  = RIP
    ;   [rsp+8]  = CS   ← check this
    ;   [rsp+16] = RFLAGS
    ;   [rsp+24] = RSP  (user on ring-3 return, kernel on ring-0 return)
    ;   [rsp+32] = SS   (user on ring-3 return, kernel on ring-0 return)
    test byte [rsp+8], 3      ; check RPL bits of return CS
    jz %%no_swapgs_exit       ; returning to ring 0 → skip VERW + swapgs

    ; MDS (TODO-10 S19): clear CPU buffers before returning to ring 3 ONLY.
    ; Ring-0 returns jumped over this (no SMT-sibling leak boundary crossed, and
    ; it would run on every timer tick/IPI). Placed BEFORE swapgs so the
    ; post-swapgs user-GS CPL0 window stays minimal (NMI/MCE in that window runs
    ; with user GS). Operand is RIP-relative RO data; ZF clobber harmless (iretq
    ; reloads RFLAGS).
    cmp byte [rel g_mds_verw_active], 0
    jz %%skip_mds_verw
    verw word [rel g_mds_verw_sel]
%%skip_mds_verw:
    swapgs                    ; ring 0 → ring 3: swap per-CPU ↔ TEB
%%no_swapgs_exit:

%if %1
    ; ---- NMI epilogue: lower the depth as late as it can be lowered ----
    ; AFTER the register pops, the frame pop, the CS test, VERW and swapgs, so
    ; every one of them is still covered -- the epilogue window S18 named. The
    ; four saves below land under the IRET frame on the same IST stack this
    ; interrupt already owns, and no NMI can nest to overwrite them because
    ; hardware NMI blocking holds until IRETQ.
    ;
    ; RESIDUAL, stated exactly rather than claimed away: ONE register restore
    ; (`pop rcx`) and the IRETQ itself run with the depth already lowered, so a
    ; fault on either -- a corrupt CS or SS in the popped frame, or a debug
    ; watchpoint -- classifies as ordinary. It is one restore rather than four
    ; only because NMI_DEPTH_LOWER folds the index into rcx; see its comment.
    ;
    ; Zero is not reachable from here. Clearing the marker only after a
    ; successful architectural return is not expressible on x86-64 without a
    ; per-CPU return tail holding a hardcoded absolute operand -- 256 of them, to
    ; cover the id space, for a window on a path that no NMI reaches today
    ; because every NMI handler in this kernel is terminal. Never lowering at all
    ; would pin the CPU "inside NMI" for the rest of the boot. Lowering here is
    ; the tightest boundary that stays balanced.
    NMI_DEPTH_LOWER
%endif

    ; Return from interrupt
    iretq
%endmacro

; =============================================================================
; The two bodies. Ordinary vectors share the first; vector 2 uses the second.
; =============================================================================
; Each body exports an _end symbol. They exist so the unit tests can bound their
; scans by the SYMBOL rather than by a guessed byte count: the first version of
; those tests scanned a fixed 64-byte window over a 40-byte stub and read into
; the neighbouring stub, where an unrelated `push` byte satisfied the ordering
; assertion. A test that can pass by reading somebody else's code proves nothing.
global isr_common_stub
isr_common_stub:
    ISR_STUB_BODY 0
global isr_common_stub_end
isr_common_stub_end:

global isr_nmi_stub
isr_nmi_stub:
    ISR_STUB_BODY 1
global isr_nmi_stub_end
isr_nmi_stub_end:

; =============================================================================
; Dedicated NMI entry stub (vector 2) -- TODO-10 S22
;
; Vector 2 does NOT use ISR_NOERRCODE. S18 raised the nesting depth as
; isr_handler's first C statement, which left the whole asm prologue -- the
; error-code and vector pushes, 15 register pushes, the conditional swapgs and
; the LFENCE -- running with the depth still clear. A fault anywhere in that
; window classifies as an ordinary panic, which re-enables the fault-suppressed
; kernel read over a live IST2 frame: the nested-abort hang S18 removed, back at
; a lower rate. Closing it needs an NMI-specific asm site, which is this stub.
;
; The raise is FIRST, ahead of even the error-code and vector pushes.
;
; RESIDUAL, measured rather than claimed: four stack writes precede the marker
; -- the CPUID clobber set saved by NMI_DEPTH_INDEX. That is the floor, not an
; oversight. A per-CPU counter update needs either a scratch register (so, a
; save) or GS, and this counter is deliberately GS-independent because gs:0 is
; exactly what an abort cannot trust. RDPID would need one register instead of
; four, but it would introduce a SECOND derivation of CPU identity, which is the
; silent-drift failure cpu_panic_safe_apic_id() exists to prevent, and it needs
; IA32_TSC_AUX programmed per CPU, which this kernel does not do. Those four
; writes land on an IST2 stack the CPU has just written five qwords to, and on a
; real NMI nothing can nest into that window because NMI delivery stays blocked
; until IRETQ -- only a fault can enter it.
; =============================================================================
global isr2
isr2:
    NMI_DEPTH_RAISE           ; before the pushes below, and before every C line
    push qword 0              ; dummy error code
    push qword 2              ; interrupt number
    jmp isr_nmi_stub
global isr2_end
isr2_end:

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
; Vector 2 (NMI) is NOT emitted here -- it has a dedicated stub above that
; raises the NMI nesting depth before any of the shared prologue runs.
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
