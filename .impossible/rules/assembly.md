# Bare-Metal Assembly Rules

> **Applies to:** `**/*.asm`
> **Canonical location:** `.impossible/rules/assembly.md`

## Environment Assumptions

- Write **NASM-style x86-64 assembly**. Assume a UEFI-era, Long Mode, APIC-oriented environment unless the file explicitly documents an earlier boot phase.
- Do not introduce **BIOS interrupt assumptions** (`int 10h`, `int 13h`, etc.).
- Do not introduce **VGA text-mode** assumptions.
- Do not add new PIC-routing paths. Use APIC/IOAPIC.

## Style

- Comment non-obvious register contracts, MMIO access patterns, interrupt setup, page-table manipulation, and hand-off conventions — later debugging must not depend on guesswork.
- Keep assembly minimal and explicit. Move policy and higher-level flow into C when possible.
- Match the existing bootloader and kernel calling conventions, structure layouts, and memory assumptions exactly.

## Example

```asm
; CORRECT: UEFI-era APIC model
; Do NOT rely on BIOS int 10h or VGA text mode here.
; Use the UEFI-provided GOP framebuffer and APIC-era interrupt model instead.

global apic_eoi
apic_eoi:
    mov rax, [apic_base]
    mov dword [rax + 0xB0], 0   ; Write to EOI register
    ret
```
