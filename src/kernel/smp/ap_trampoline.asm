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
; Shared data area at physical 0x8E00 (trampoline + 0xE00):
;   +0x00  CR3         (uint64_t) BSP's PML4 physical address
;   +0x08  STACK       (uint64_t) per-AP kernel stack top
;   +0x10  GDT_PTR     (10 bytes) GDTR for BSP's GDT
;   +0x20  ENTRY       (uint64_t) 64-bit C entry point
;   +0x28  CPUID       (uint32_t) logical CPU index
;   +0x30  IDT_PTR     (10 bytes) IDTR
; =============================================================================

%define AP_DATA  0x8E00

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

    ; Reload CS with BSP's code selector (0x08) via far return
    push 0x08                       ; BSP's CS selector
    lea rax, [rel .bsp_cs_loaded]
    push rax
    retfq

.bsp_cs_loaded:
    ; Load IDT from shared data
    lidt [AP_DATA + 0x30]

    ; Set up per-AP stack
    mov rsp, [AP_DATA + 0x08]

    ; Load CPU index into rdi (first arg to C entry)
    xor rdi, rdi
    mov edi, [AP_DATA + 0x28]

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
