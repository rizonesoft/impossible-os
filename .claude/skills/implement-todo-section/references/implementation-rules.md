# Implementation policies -- POST16, stubs, and stale TODO patterns

Read on demand from `implement-todo-section` step 6. The step text in SKILL.md carries the freestanding-kernel rules
that apply to every edit; this file carries the three policies you only need when the specific situation arises.

## POST16 codes are for BOOT-PATH code ONLY

POST16 was designed for pre-`sti` triple-fault diagnostics where klog isn't yet running.

**Use POST16 only when** the code might trip a triple fault before klog is initialized: Phase 0/1/2 boot, hardware init
(CPUID, GDT, IDT, page tables, APIC, ACPI, SMP AP startup), or any function called from `boot_init.c` before
`boot_phase3()` returns.

**Do NOT add POST16** to scheduler, syscall handlers, file I/O, ELF loading, exec, network, or any code that runs after
Phase 3 completes -- a single `klog(LOG_INFO, ...)` line is more useful there.

If `boot_init.h` defines a POST16 constant, check `boot_phase0/1/2.c` for the call site before assuming new code needs
one.

## STATUS_NOT_IMPLEMENTED completion-first policy

The aim is to write complete code so features do not get lost behind stubs. If a function would return
`STATUS_NOT_IMPLEMENTED`:

1. If it is **standalone** (self-contained, under 1000 lines, no deep dependency chain), implement it fully via
   scope-gap protocol Branch A.
2. If it requires **significant new infrastructure**, use Branch B/C/D to create a tracked TODO item with a
   domain-qualified XREF (e.g. `-> XREF: 02-kernel-core/TODO-17 <section-ref>`).

Never leave a `STATUS_NOT_IMPLEMENTED` stub without a tracked follow-up.

## Stale TODO patterns -- delete them, do not satisfy them

If the section's checklist explicitly demands `POST16(...)` codes for non-boot-path code (e.g. scheduler / exec /
syscall / auxv work), treat that as a stale guideline from before the POST16 rule above was added. SKIP the POST16 work
AND remove the stale checklist item from the TODO -- do not satisfy obsolete patterns.

The same applies to "test POST16 constants are 0xDDNN" items: those are tautological (the compiler enforces `#define`
values; the real protection is the boot-time uniqueness check). Document the removal in the commit message.
