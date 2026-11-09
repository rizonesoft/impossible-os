; =============================================================================
; ap_trampoline.asm -- AP startup trampoline (assembled as flat binary)
;
; This code is assembled as a flat binary with -f bin, then converted to an
; ELF object via objcopy. The BSP copies it to physical address 0x8000
; at runtime before sending SIPI.
;
; APs wake in 16-bit real mode at 0x8000 (SIPI vector = 0x08).
; They transition through protected mode to long mode using the BSP's
; page tables, then jump to a 64-bit C entry point.
;
; IMPORTANT: The trampoline embeds its own temporary GDT for the
; real → protected → long mode transition. The BSP's GDT has L=1 on
; its code segment (selector 0x08), which is invalid for 32-bit
; protected mode. We use a local GDT with proper 32-bit segments,
; then switch to the BSP's GDT after entering long mode.
;
; Shared data area at physical 0x8E00 (trampoline + 0xE00), written once
; before the first SIPI and never rewritten:
;   +0x00  CR3         (uint64_t) BSP's PML4 physical address
;   +0x10  GDT_PTR     (10 bytes) GDTR for BSP's GDT
;   +0x20  ENTRY       (uint64_t) 64-bit C entry point
;   +0x30  IDT_PTR     (10 bytes) IDTR
;   +0x40  AP_REC[16]  per-target records {apic_id u32, state u32, stack u64}
;
; Per-AP values come ONLY from the AP's own record (TODO-09-boot S10). The AP
; reads its LIVE APIC ID without a stack, claims the record carrying that ID by
; lock cmpxchg ARMED -> TAKEN, and only then loads that record's stack. Until
; the claim it touches no stack and no per-AP data, so an AP that stalls at any
; instruction and wakes after the BSP moved on cannot run on, or corrupt,
; another AP's stack: a revoked record refuses its claim and the AP parks.
; The record index is the logical CPU index (handed to ap_entry in edi) and
; picks the TSS selector 0x28 + 16*index (GDT_TSS_SEL_FOR, gdt.h).
; =============================================================================

%define AP_DATA          0x8E00
%define AP_REC_BASE      (AP_DATA + 0x40)
%define AP_REC_SIZE      16
%define AP_REC_COUNT     16
%define AP_REC_OFF_APIC  0x00
%define AP_REC_OFF_STATE 0x04
%define AP_REC_OFF_STACK 0x08
%define AP_REC_ARMED     1
%define AP_REC_TAKEN     2
%define TSS_SEL_BASE     0x28
%define TSS_SEL_SHIFT    4              ; stride 16

%define MSR_APIC_BASE    0x1B
%define APIC_BASE_EN     (1 << 11)
%define APIC_BASE_EXTD   (1 << 10)
%define MSR_X2APIC_ID    0x802
%define XAPIC_ID_REG     0x20

[BITS 16]
[ORG 0x8000]

global_start:
    ; ---- 16-bit Real Mode ----
    cli
    cld

    ; Set up real-mode segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00              ; temporary real-mode stack

    ; ---- Enable A20 (fast A20 via port 0x92) ----
    in al, 0x92
    or al, 0x02
    and al, 0xFE                ; don't reset CPU
    out 0x92, al

    ; ---- Load temporary GDT (embedded in this trampoline) ----
    lgdt [tmp_gdtr]

    ; ---- Enter protected mode ----
    mov eax, cr0
    or eax, 1                   ; PE bit
    mov cr0, eax

    ; Far jump to 32-bit code using TEMPORARY selector 0x08
    ; (32-bit code segment: L=0, D=1 -- correct for protected mode)
    jmp 0x08:pm_entry

[BITS 32]
pm_entry:
    ; Load data segments with temporary selector 0x10
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- Live APIC ID into esi, stackless, before paging ----
    ; Paging is still off, so the xAPIC ID register is read at its physical
    ; address through the flat 4 GiB segment. In x2APIC mode the MMIO page is
    ; not decoded and the ID comes from MSR 0x802 instead. An APIC that is
    ; disabled, or based above 4 GiB, cannot be identified here: park.
    mov ecx, MSR_APIC_BASE
    rdmsr
    test eax, APIC_BASE_EN
    jz pm_park
    test eax, APIC_BASE_EXTD
    jnz .x2apic
    test edx, edx
    jnz pm_park
    and eax, 0xFFFFF000
    mov esi, [eax + XAPIC_ID_REG]
    shr esi, 24
    jmp .have_id
.x2apic:
    mov ecx, MSR_X2APIC_ID
    rdmsr
    mov esi, eax
.have_id:

    ; ---- Load BSP's CR3 ----
    mov eax, [AP_DATA + 0x00]
    mov cr3, eax

    ; ---- Enable PAE ----
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; ---- Enable Long Mode + NX + SYSCALL (IA32_EFER) ----
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 0)   ; SCE  -- SYSCALL/SYSRET enable
    or eax, (1 << 8)   ; LME  -- Long Mode Enable
    or eax, (1 << 11)  ; NXE  -- No-Execute Enable (must be set before paging)
    wrmsr

    ; ---- Enable paging ----
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)  ; PG + WP
    mov cr0, eax

    ; ---- Far jump to 64-bit using temporary selector 0x18 ----
    ; (64-bit code segment: L=1, D=0 -- correct for long mode)
    jmp 0x18:lm_entry

    ; Stackless park for an AP that could not identify itself.
pm_park:
    cli
    hlt
    jmp pm_park

[BITS 64]
lm_entry:
    ; Now in 64-bit long mode with temporary GDT.
    ; Switch to BSP's GDT and reload all segments.
    lgdt [AP_DATA + 0x10]

    ; Reload data segments with BSP's data selector (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- Claim this AP's own record (still stackless) ----
    ; edi = record index = logical CPU. A record matches only on the live ID
    ; read above AND only while ARMED; the cmpxchg is the single point where
    ; this AP and the BSP's revoke race, and exactly one of them wins.
    xor edi, edi
    mov ebx, AP_REC_BASE
.scan:
    cmp dword [rbx + AP_REC_OFF_APIC], esi
    jne .next
    mov eax, AP_REC_ARMED
    mov edx, AP_REC_TAKEN
    lock cmpxchg dword [rbx + AP_REC_OFF_STATE], edx
    je .claimed
.next:
    add ebx, AP_REC_SIZE
    inc edi
    cmp edi, AP_REC_COUNT
    jb .scan
    jmp .halt                       ; no armed record for this ID: park

.claimed:
    ; Private stack BEFORE the first push: nothing below this line shares a
    ; stack with any other AP.
    mov rsp, [rbx + AP_REC_OFF_STACK]
    test rsp, rsp
    jz .halt

    ; Reload CS with BSP's code selector (0x08) via far return
    push 0x08                       ; BSP's CS selector
    lea rax, [rel .bsp_cs_loaded]
    push rax
    retfq

.bsp_cs_loaded:
    ; Load IDT from shared data
    lidt [AP_DATA + 0x30]

    ; This CPU's own TSS (rsp0 + IST1-3), before any C code can take an NMI.
    mov eax, edi
    shl eax, TSS_SEL_SHIFT
    add eax, TSS_SEL_BASE
    ltr ax

    ; CPU index (the record index) stays in edi: first arg to ap_entry.
    ; The upper half is cleared by the 32-bit write above (xor edi, edi / inc).

    ; Jump to C entry point
    mov rax, [AP_DATA + 0x20]
    call rax

    ; Halt if ap_entry returns
.halt:
    cli
    hlt
    jmp .halt

; =============================================================================
; Temporary GDT -- used only during real → protected → long mode transition.
; After entering long mode, we switch to the BSP's GDT.
;
; Layout:
;   0x00: Null descriptor
;   0x08: 32-bit code (Ring 0) -- for protected mode transition
;   0x10: Data segment  (Ring 0) -- flat, used in both 32-bit and 64-bit
;   0x18: 64-bit code (Ring 0) -- for far jump into long mode
; =============================================================================

ALIGN 8
tmp_gdt:
    ; [0x00] Null descriptor
    dq 0

    ; [0x08] 32-bit code segment: base=0, limit=4GB, Execute/Read, Ring 0
    ;   Access = 0x9A (P=1, DPL=0, S=1, Type=1010b Execute/Read)
    ;   Flags  = G=1, D=1, L=0 → granularity byte = 0xCF
    dw 0xFFFF       ; limit [15:0]
    dw 0x0000       ; base  [15:0]
    db 0x00         ; base  [23:16]
    db 0x9A         ; access
    db 0xCF         ; G=1 D=1 L=0 limit[19:16]=0xF
    db 0x00         ; base  [31:24]

    ; [0x10] Data segment: base=0, limit=4GB, Read/Write, Ring 0
    ;   Access = 0x92, Flags = G=1, D=1, L=0 → 0xCF
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00

    ; [0x18] 64-bit code segment: base=0, limit=4GB, Execute/Read, Ring 0
    ;   Access = 0x9A, Flags = G=1, D=0, L=1 → 0xAF
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xAF         ; G=1 D=0 L=1 limit[19:16]=0xF
    db 0x00
tmp_gdt_end:

tmp_gdtr:
    dw tmp_gdt_end - tmp_gdt - 1    ; GDT limit
    dd tmp_gdt                       ; GDT base (32-bit, fine for real mode)
