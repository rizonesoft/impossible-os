; =============================================================================
; Kernel Entry Point — 32-bit → 64-bit Long Mode Transition
;
; On entry from GRUB (Multiboot2):
;   eax = 0x36D76289 (Multiboot2 magic)
;   ebx = physical address of Multiboot2 info structure
;   CPU in 32-bit protected mode, paging disabled, A20 enabled
;
; We must:
;   1. Verify Multiboot2 magic
;   2. Set up identity-mapped page tables (first 4 GiB using 2 MiB pages)
;   3. Enable PAE (CR4 bit 5)
;   4. Set Long Mode Enable in IA32_EFER MSR
;   5. Enable paging (CR0 bit 31)
;   6. Load 64-bit GDT and far-jump to 64-bit code
;   7. Set up 64-bit stack, zero BSS, call kernel_main()
; =============================================================================

[BITS 32]

section .bss
align 4096

; --- Page tables (identity map first 4 GiB with 2 MiB pages) ---
; PML4 → 1 PDPT → 4 PDs → 512 entries each = 4 GiB
pml4_table:     resb 4096       ; Page Map Level 4
pdpt_table:     resb 4096       ; Page Directory Pointer Table
pd_tables:      resb 4096 * 4   ; 4 Page Directories (1 GiB each)

; --- Kernel stack (16 KiB) ---
align 16
stack_bottom:
    resb 16384
stack_top:

section .data

; --- Error messages ---
err_no_multiboot: db "FATAL: Not loaded by Multiboot2!", 0
err_no_cpuid:     db "FATAL: CPUID not supported!", 0
err_no_long_mode: db "FATAL: Long Mode not supported!", 0

; --- 64-bit GDT ---
align 16
gdt64_start:
    ; Null descriptor (index 0)
    dq 0x0000000000000000

    ; Code segment descriptor (index 1) — 64-bit, execute/read
    ; Bits: Present=1, DPL=0, S=1, Type=Execute/Read, L=1 (Long Mode), D=0
    dq 0x00AF9A000000FFFF

    ; Data segment descriptor (index 2) — read/write
    ; Bits: Present=1, DPL=0, S=1, Type=Read/Write
    dq 0x00CF92000000FFFF
gdt64_end:

gdt64_pointer:
    dw gdt64_end - gdt64_start - 1     ; limit (2 bytes)
    dd gdt64_start                       ; base (4 bytes — works in 32-bit lgdt)
    dd 0                                 ; high 32 bits (for 64-bit lgdt reload)

; GDT segment selectors
CODE64_SEG equ 0x08     ; offset of code segment in GDT
DATA64_SEG equ 0x10     ; offset of data segment in GDT

; Multiboot2 magic
MULTIBOOT2_BOOTLOADER_MAGIC equ 0x36D76289

section .text
global _start
extern kernel_main

_start:
    ; Immediately disable interrupts (GRUB's IDT is stale)
    cli

    ; -----------------------------------------------------------------
    ; Step 0: Save Multiboot2 info to memory (before we clobber regs)
    ; -----------------------------------------------------------------
    mov [saved_magic], eax  ; save magic to memory
    mov [saved_mbi], ebx    ; save mbi pointer to memory

    ; Set up a temporary 32-bit stack
    mov esp, stack_top

    ; -----------------------------------------------------------------
    ; Step 1: Verify Multiboot2 magic
    ; -----------------------------------------------------------------
    cmp eax, MULTIBOOT2_BOOTLOADER_MAGIC
    jne .error_no_multiboot

    ; -----------------------------------------------------------------
    ; Step 1.5: Zero BSS (must do BEFORE page table setup since
    ;           page tables and stack live in BSS)
    ; -----------------------------------------------------------------
    extern __bss_start
    extern __bss_end

    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    shr ecx, 2              ; divide by 4 (zero in dwords)
    xor eax, eax
    rep stosd

    ; -----------------------------------------------------------------
    ; Step 2: Check for CPUID support (toggle ID flag in EFLAGS)
    ; -----------------------------------------------------------------
    pushfd
    pop eax
    mov ecx, eax            ; save original EFLAGS
    xor eax, 1 << 21        ; toggle ID bit (bit 21)
    push eax
    popfd
    pushfd
    pop eax
    push ecx                ; restore original EFLAGS
    popfd
    cmp eax, ecx            ; if unchanged, CPUID not supported
    je .error_no_cpuid

    ; -----------------------------------------------------------------
    ; Step 3: Check for Long Mode support (CPUID extended function)
    ; -----------------------------------------------------------------
    mov eax, 0x80000000     ; get highest extended function
    cpuid
    cmp eax, 0x80000001     ; need at least 0x80000001
    jb .error_no_long_mode

    mov eax, 0x80000001     ; extended processor info
    cpuid
    test edx, 1 << 29       ; LM bit (Long Mode)
    jz .error_no_long_mode

    ; -----------------------------------------------------------------
    ; Step 4: Set up identity-mapped page tables
    ;   PML4[0] → PDPT
    ;   PDPT[0..3] → PD[0..3]
    ;   PD[x][0..511] → 2 MiB pages (identity mapped)
    ; -----------------------------------------------------------------

    ; Zero all page table memory first
    mov edi, pml4_table
    mov ecx, (4096 * 6) / 4     ; 6 pages worth of DWORDs
    xor eax, eax
    rep stosd

    ; PML4[0] → PDPT (present + writable)
    mov eax, pdpt_table
    or eax, 0x03                 ; present + writable
    mov [pml4_table], eax

    ; PDPT[0..3] → PD[0..3]
    mov eax, pd_tables
    or eax, 0x03                 ; present + writable
    mov [pdpt_table + 0*8], eax
    add eax, 4096
    mov [pdpt_table + 1*8], eax
    add eax, 4096
    mov [pdpt_table + 2*8], eax
    add eax, 4096
    mov [pdpt_table + 3*8], eax

    ; Fill all 4 PDs with 2 MiB identity-mapped pages
    ; Each PD has 512 entries, each mapping 2 MiB = 0x200000
    ; PD entry flags: Present (0x01) + Writable (0x02) + Page Size 2MiB (0x80) = 0x83
    mov edi, pd_tables           ; start of first PD
    mov eax, 0x00000083          ; first 2 MiB page, flags: P+W+PS
    mov ecx, 512 * 4             ; 4 PDs × 512 entries = 2048 entries

.fill_pd:
    mov [edi], eax               ; low 32 bits of PD entry
    mov dword [edi + 4], 0       ; high 32 bits = 0 (addresses < 4 GiB)
    add eax, 0x00200000          ; next 2 MiB page
    add edi, 8                   ; next PD entry
    dec ecx
    jnz .fill_pd

    ; -----------------------------------------------------------------
    ; Step 5: Load PML4 into CR3
    ; -----------------------------------------------------------------
    mov eax, pml4_table
    mov cr3, eax

    ; -----------------------------------------------------------------
    ; Step 6: Enable PAE (CR4 bit 5)
    ; -----------------------------------------------------------------
    mov eax, cr4
    or eax, 1 << 5              ; PAE bit
    mov cr4, eax

    ; -----------------------------------------------------------------
    ; Step 7: Set Long Mode Enable in IA32_EFER MSR (bit 8)
    ; -----------------------------------------------------------------
    mov ecx, 0xC0000080          ; IA32_EFER MSR
    rdmsr
    or eax, 1 << 8              ; LME bit
    wrmsr

    ; -----------------------------------------------------------------
    ; Step 8: Enable paging (CR0 bit 31) + Write Protect (bit 16)
    ; -----------------------------------------------------------------
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)  ; PG + WP
    mov cr0, eax

    ; -----------------------------------------------------------------
    ; Step 9: Load 64-bit GDT and far-jump to 64-bit code
    ; -----------------------------------------------------------------
    lgdt [gdt64_pointer]

    ; Far jump to 64-bit code segment — this switches to Long Mode!
    jmp CODE64_SEG:.long_mode_entry

    ; =================================================================
    ; 64-BIT CODE STARTS HERE
    ; =================================================================

[BITS 64]
.long_mode_entry:
    ; -----------------------------------------------------------------
    ; Step 10: Set up 64-bit segment registers
    ; -----------------------------------------------------------------
    mov ax, DATA64_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; -----------------------------------------------------------------
    ; Step 11: Set up 64-bit stack
    ; -----------------------------------------------------------------
    mov rsp, stack_top

    ; (BSS was already zeroed in 32-bit Step 1.5)

    ; -----------------------------------------------------------------
    ; Step 13: Call kernel_main(magic, mbi_pointer)
    ;   System V AMD64 ABI: rdi = arg1, rsi = arg2
    ;   Reload from save area (written in 32-bit Step 0)
    ; -----------------------------------------------------------------
    xor rdi, rdi
    xor rsi, rsi
    mov edi, [saved_magic]       ; rdi = multiboot2 magic (zero-extended)
    mov esi, [saved_mbi]         ; rsi = multiboot2 info pointer (zero-extended)

    call kernel_main

    ; If kernel_main returns, halt
.halt64:
    cli
    hlt
    jmp .halt64

    ; =================================================================
    ; 32-BIT ERROR HANDLERS
    ; =================================================================

[BITS 32]

.error_no_multiboot:
    mov esi, err_no_multiboot
    jmp .serial_error

.error_no_cpuid:
    mov esi, err_no_cpuid
    jmp .serial_error

.error_no_long_mode:
    mov esi, err_no_long_mode
    ; fall through to .serial_error

; Print error to COM1 serial port and halt
.serial_error:
    lodsb
    test al, al
    jz .halt32
.wait_tx:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .wait_tx
    mov dx, 0x3F8
    mov al, [esi - 1]
    out dx, al
    jmp .serial_error

.halt32:
    cli
    hlt
    jmp .halt32

; --- Save area for multiboot info (written in 32-bit, read in 64-bit) ---
section .data
saved_magic: dd 0
saved_mbi:   dd 0
