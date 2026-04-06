---
name: kernel-code-quality
description: Automatic code quality checklist for writing kernel C code. Enforces SMP safety, 5-layer defense for invariants, memory rules, bare-metal correctness, POST16 diagnostics, unit test coverage, and error handling. Auto-loads when writing or modifying kernel source files (.c, .h, .asm) in src/ or include/.
---

# Kernel Code Quality

> This skill auto-loads when writing kernel code. It is a pre-flight checklist, not a post-hoc review. Apply these rules WHILE writing code so it comes out correct the first time.

## Use This Skill When

- Writing or modifying any `.c`, `.h`, or `.asm` file under `src/` or `include/`.
- This skill auto-loads -- you do not need to invoke it manually.
- It applies during `/implement-todo-section`, `/implement-ssdt-range`, and any direct code editing.
- Does NOT apply to documentation-only changes, TODO edits, or script changes.

## When This Applies

Every time you create or modify a file under `src/kernel/`, `include/kernel/`, `src/boot/`, `src/desktop/`, or `src/shell/`. Does NOT apply to documentation-only changes, TODO edits, or script changes.

## Pre-Write Checklist

Before writing any kernel code, mentally walk through these gates. Each gate is pass/fail -- if you hit a "fail" condition, fix it before committing.

---

### Gate 1: Freestanding Kernel Rules

- [ ] **No stdlib headers.** Use `#include "kernel/types.h"` -- never `<stdint.h>`, `<string.h>`, `<stdlib.h>`, `<stdio.h>`.
- [ ] **No stdlib functions.** Use `kmalloc()`/`kfree()` not `malloc()`/`free()`. Use `klog()`/`printk()` not `printf()`. Use `memcpy()`/`memset()` from `kernel/types.h`.
- [ ] **Memory sizing.** `kmalloc()` for <= 4 KB only. `pmm_alloc_contiguous()` for larger buffers. Always check the return value for NULL.
- [ ] **ASCII only.** No Unicode dashes, no UTF-8 in strings or comments. Use `--` not em dash. Serial output goes to Windows terminals that garble multi-byte UTF-8.

### Gate 2: SMP Safety (Every. Single. Time.)

> The current kernel runs on 2+ CPUs from early boot. Any mutable shared state without synchronization WILL corrupt on real hardware.

- [ ] **New global/static mutable variable?** It needs a spinlock, atomic, or must be per-CPU.
  - Spinlock: `spin_lock_irqsave()` / `spin_unlock_irqrestore()` for data accessed from interrupt context.
  - Atomic: `__atomic_fetch_add()` / `__atomic_load_n()` for simple counters.
  - Per-CPU: access via `smp_this_cpu()->field` for data that is CPU-local.
- [ ] **Interrupt-safe?** If the code runs in an ISR or is called from both thread and ISR context, use `irqsave` spinlock variants. Never use plain spinlocks in interrupt context -- deadlock.
- [ ] **Lock hold time.** Never hold a spinlock during disk I/O, serial output, or any operation that blocks or takes milliseconds. Pattern: lock -> snapshot data into local variable -> unlock -> use snapshot for I/O. Lesson: klog ring buffer lock must not be held during serial_write().
- [ ] **No assumptions about CPU count.** Code must work on 1 CPU (VBox single-core) and 64 CPUs (future bare metal). Use `smp_cpu_count()`, never hardcode.

<!-- Updated 2026-04-06: added lock hold time rule after Codex found klog ring written without lock, fix required snapshot pattern -->

### Gate 3: 5-Layer Defense for Invariants

> Every cross-file constant, struct layout referenced by assembly, and ABI contract MUST have multi-layer protection. Reference: GDT SYSRET ordering in `gdt.h` + `gdt.c` + `syscall_fast.c` + `test_nt_types.c` + `CLAUDE.md`. Full pattern in `TODO-22-kernel-bulletproofing.md`.

Apply these layers when you:
- Define a constant used by multiple files (especially if one is assembly)
- Create a struct whose field offsets matter to assembly or cross-boundary code
- Add a new subsystem with boot-critical initialization

**Layer 1 -- Static Assert (compile-time, zero cost):**
```c
_Static_assert(__builtin_offsetof(struct my_struct, field) == EXPECTED,
    "description of why this offset matters");
_Static_assert(sizeof(struct my_struct) == EXPECTED_SIZE,
    "description of size contract");
_Static_assert(MY_CONSTANT == EXPECTED_VALUE,
    "description of value contract");
/* For packed data areas with variable-size fields (GDT_PTR=10 bytes,
 * IDT_PTR=10 bytes), assert NON-OVERLAP between adjacent fields: */
_Static_assert(FIELD_B_OFFSET >= FIELD_A_OFFSET + FIELD_A_SIZE,
    "field B must not overlap field A");
```
Use `__builtin_offsetof` (not `offsetof`) because we're freestanding.

**CRITICAL: Value asserts are not enough.** Asserting `CANARY == 0x38` only verifies you typed `0x38` -- it does NOT verify that 0x38 is a safe offset. Always add **non-overlap asserts** for packed data areas where fields have variable sizes (e.g., IDTR is 10 bytes, not 8). Lesson learned 2026-04-03: canary at 0x38 stomped IDT_PTR (0x30 + 10 bytes = 0x3A), crashing AP boot.

<!-- Updated 2026-04-03: added non-overlap assert rule after AP trampoline canary stomped IDT_PTR -->

**Layer 2 -- Runtime Verification (at init, nanoseconds):**
```c
void my_subsystem_init(void) {
    /* Verify invariant at runtime -- catches corruption that static asserts can't */
    if (actual != expected) {
        klog(LOG_FATAL, "subsys", "invariant violated: expected %u, got %u",
             (uint64_t)expected, (uint64_t)actual);
        boot_halt("subsystem invariant violated");
    }
}
```

**Layer 3 -- Unit Test:**
```c
static void test_my_invariant(void) {
    TEST_ASSERT_EQ(actual, expected, "description");
}
/* Register with: test_suite_register_cat("name", fn, TEST_CAT_XX); */
```

**Layer 4 -- Canary / Guard:**
- Guard pages at memory boundaries (not-present PTE)
- Magic values verified on use (`0xDEADBEEF` headers, CRC32 checksums)
- Readback verification (write GS_BASE, read via `mov %%gs:0`, compare)

**Layer 5 -- Documentation:**
- Offset table comment in the header file
- CLAUDE.md "Bare Metal Gotchas" or "Safety Gates" entry if it's a hard-won lesson
- `/* WARNING: Assembly code depends on this -- do NOT reorder */` comments

**Parallel array/table sync:** When an enum grows (e.g., `SUBSYS_OB` added to `kernel_subsys_t`), every parallel array indexed by that enum must also grow. Enforce with:
```c
_Static_assert(sizeof(s_names) / sizeof(s_names[0]) == ENUM_COUNT,
    "name table must match enum count");
```
Lesson: `s_subsys_names[]` missed `SUBSYS_OB`, causing OOB read in `kernel_subsystem_dump()` during panic.

**When to apply all 5 layers:** New struct referenced by assembly, new cross-file ABI contract, new boot-critical init sequence, new security-critical invariant.

**When 1-2 layers suffice:** Internal-only constants, non-critical helper structs, cosmetic changes.

<!-- Updated 2026-04-06: added parallel array sync rule after Codex found s_subsys_names missing SUBSYS_OB -->

### Gate 4: Boot-Path Code

> Code that runs before `sti` (interrupts enabled) is the most dangerous -- errors cause silent triple faults with no diagnostic output.

- [ ] **POST16 diagnostic codes.** Every init function in the boot path gets `POST16()` entry/exit codes. Use the `0xD000-0xDFFF` debug range for development. Format: `POST16(0xDDNN)` where `DD` = section number, `NN` = step. Check `include/kernel/boot_init.h` for existing codes before assigning new ones.
- [ ] **klog on entry and exit.** `klog(LOG_INFO, "subsys", "Thing initialized: %u items", count)` so serial output shows progress.
- [ ] **Fail with message, not silence.** Use `boot_halt("what went wrong")` for unrecoverable failures. Use `klog(LOG_WARN, ...)` + degrade for non-critical failures. Never return silently from an init function that failed.
- [ ] **No thread_create for boot-path work.** `compositor_run()` is an infinite event loop that starves kernel threads created before it. All init between `desktop_init()` and `compositor_run()` must run inline. See CLAUDE.md "No thread_create for deferred init."
- [ ] **Panic-path code: no locks, no alloc.** Code called from `panic_screen()`, `boot_halt()`, or fault handlers must not acquire spinlocks (the faulting context may hold them -- deadlock). Must not call `kmalloc()` (heap may be corrupt). Must not assume interrupts are enabled. Use only serial_write() and direct physical memory access.

<!-- Updated 2026-04-06: added thread_create ban and panic-path rules after deferred init regression and Codex findings -->

### Gate 5: Error Handling

- [ ] **Check every allocation.** `kmalloc()`, `pmm_alloc_frame()`, `pmm_alloc_contiguous()` can all return NULL/0. Handle it.
- [ ] **Severity levels:**
  - `LOG_FATAL` + `boot_halt()` -- unrecoverable (wrong struct layout, missing hardware)
  - `LOG_ERROR` -- serious but system can continue degraded
  - `LOG_WARN` -- unexpected but handled (fallback path taken)
  - `LOG_INFO` -- normal operation milestones
  - `LOG_DEBUG` -- verbose tracing (stripped in release)
- [ ] **No silent failure.** If a function can fail, it must either return an error code or log the failure. `return;` after a failed check without logging = bug.
- [ ] **Escape structured output.** When writing JSON, config, or any structured format, escape special characters (`\` `"` `\n` `\r` `\t`, control chars < 0x20) in user-controlled or variable strings. Never `snprintf` raw strings into JSON templates. Lesson: `events.jsonl` had unescaped message fields that broke `jq` parsing.

<!-- Updated 2026-04-06: added serialization escaping rule after Codex found unescaped JSON in events.jsonl -->

### Gate 6: Bare Metal Correctness

> "Works on QEMU" is necessary but not sufficient. "Works on bare metal" is the acceptance criteria.

- [ ] **MMIO must use UC or WC pages.** Device register MMIO (HPET, NVMe, xHCI, ECAM) must go through `vmm_map_mmio_uc()`. Framebuffer VRAM uses `vmm_map_mmio_wc()` for Write-Combining. The bootloader maps everything WB; MMIO through WB pages causes stale reads or bus errors on real hardware.
- [ ] **No CPUID-gated instructions without checking.** Before using `clac`/`stac` (SMAP), `xsave`/`xrstor` (XSAVE), or AVX instructions, check `cpu_has(CPU_FEATURE_XX)`. TCG and some bare metal CPUs lack these.
- [ ] **GS_BASE is set.** Any code that reads `gs:N` (per-CPU data) must run AFTER `smp_early_bsp_init()`. If unsure, check `smp_this_cpu() != NULL`.
- [ ] **No LAPIC TPR writes in ISR path.** IRQL tracking is software-only. The LAPIC handles hardware priority via ISR/PPR.
- [ ] **User-mode pages need User bit at all 4 levels.** `vmm_set_user_page()` auto-splits 2 MiB huge pages on demand and sets User at PML4/PDPT/PD/PT. Works for any address. Note: per-process pages currently share physical frames (identity-mapped) -- true isolation requires unique physical backing per process (planned for Win32 PE loader).
- [ ] **Framebuffer 5-rule checklist (TODO-22 §15).** When touching framebuffer/GOP/display code: (1) Pixel format checked at runtime, never assumed. (2) Stride from `PixelsPerScanLine`, not width. (3) VRAM remapped WC via `vmm_map_mmio_wc()` after page table takeover. (4) All GOP `SetMode()` calls before `ExitBootServices()`. (5) Back-buffer for double buffering.

<!-- Updated 2026-04-04: added framebuffer 5-rule checklist after WC remap implementation -->

### Gate 7: Architecture Neutrality

> Impossible OS targets ARM64 in addition to x86-64 (domain 16). Code outside `arch/` must compile for both. When the HAL exists, use it. Until then, don't make things worse.

- [ ] **No arch headers in neutral code.** Files in `fs/`, `ob/`, `ipc/`, `nt/`, `registry.c`, `klog.c`, desktop/, shell/ must NOT include: `kernel/gdt.h`, `kernel/idt.h`, `kernel/msr.h`, `kernel/cpuid.h`, `kernel/drivers/lapic.h`, `kernel/drivers/ioapic.h`, `kernel/drivers/pit.h`, `kernel/drivers/pic.h`, `kernel/cpu_security.h`.
- [ ] **No inline x86 asm in neutral code.** Don't write `__asm__ volatile("cli")` or `rdtsc` or `invlpg` in filesystem, object manager, IPC, or registry code. If you need interrupt control, use the future `arch_interrupts_disable()` HAL, or for now, call an existing wrapper.
- [ ] **No GDT selectors in neutral logic.** `GDT_KERNEL_CODE`, `GDT_USER_CODE`, `0x08`, `0x20|3` are x86 segment concepts. They belong in `task.c` context frame setup (which will move to `arch/x86_64/`) -- never in VFS, OB, or IPC code.
- [ ] **No x86 register names in neutral code.** `CR0`, `CR3`, `CR4`, `EFER`, `MSR_IA32_*` are x86-only. ARM uses `TTBR0_EL1`, `SCTLR_EL1`, etc. Keep these in arch-specific files.
- [ ] **Known arch-specific files (acceptable).** These files ARE x86-specific and will move to `arch/x86_64/` during the port: `gdt.c`, `idt.c`, `isr_stubs.asm`, `syscall_entry.asm`, `syscall_fast.c`, `lapic.c`, `ioapic.c`, `pit.c`, `cpuid.c`, `msr.c`, `cpu_security.c`, `smp.c`, `ap_trampoline.asm`. Arch includes in these files are expected.
- [ ] **Borderline files.** `task.c`, `vmm.c`, `panic.c`, `irql.c`, `spinlock.c` mix neutral logic with arch-specific mechanics. When modifying these, keep new code arch-neutral where possible and mark arch-specific sections with `/* ARCH: x86-64 -- will move to arch/ */`.

<!-- Updated 2026-04-03: added Gate 10 for ARM64 port preparation -->

### Gate 8: Unit Tests

- [ ] **New public function?** Add at least one test assertion in the appropriate `test_*.c` file.
- [ ] **New invariant?** Add `TEST_ASSERT_EQ` for the expected value (Layer 3 of the 5-layer defense).
- [ ] **Register with category.** Use `test_suite_register_cat("name", fn, TEST_CAT_XX)` where XX matches the subsystem: `MM`, `FS`, `SCHED`, `OB`, `SECURITY`, `IPC`, `BOOT`, `ABI`, `STORAGE`.
- [ ] **Concrete assertions.** Every test must have an expected value. No `"verify it works"` -- specify what "works" means numerically.

### Gate 9: Documentation Sync

- [ ] **New bare-metal lesson?** Add to CLAUDE.md "Bare Metal Gotchas" section.
- [ ] **New safety invariant?** Add to CLAUDE.md "Safety Gates" section.
- [ ] **Changed a convention?** Update CLAUDE.md, `.claude/skills/`, and affected TODO files in the same commit.
- [ ] **New struct with assembly offsets?** Add an offset table comment above the struct definition.

### Gate 10: Production Quality -- No Patches, No Workarounds

> Every line of code ships as if it is the final version. There is no "fix it later" pass. Impossible OS is built to rival Windows and Linux -- that standard applies to every commit, not just milestone releases.

- [ ] **No TODO/FIXME/HACK in new code.** If something needs work, do the work now or don't write it. A `// TODO: handle error` comment is a shipped bug. Either handle the error or don't merge.
- [ ] **No hardcoded magic numbers.** Every constant gets a `#define` with a descriptive name in a header. Inline `0x8E00` or `470` in source code is a maintenance trap -- use `AP_DATA_BASE` or `SSDT_MAIN_COUNT`.
- [ ] **No copy-paste code.** If you write the same pattern three times, extract it. But don't extract prematurely for one use -- wait for the third occurrence.
- [ ] **No emulator workarounds.** Code must be correct per the hardware specification. If QEMU does something wrong, that's QEMU's bug -- never add `if (running_on_qemu)` branching. The only platform-specific code allowed is `hv_supports_*()` checks where hardware genuinely differs.
- [ ] **No skip lists or suppression.** If a test fails, fix the code, don't skip the test. If a warning fires, fix the root cause, don't suppress the warning. `#pragma GCC diagnostic ignored` is almost never correct.
- [ ] **Complete error paths.** Every `if (error)` branch must do something meaningful: log, clean up partial state, return an error code. An empty `if (err) {}` block or a bare `return;` is a silent failure waiting to become a crash.
- [ ] **Correct the first time.** Read the spec, read the existing code, understand the integration surface BEFORE writing. A function that works on the first `build.sh` run is the goal -- iterating through compile errors is wasted motion. Think, then type.
- [ ] **No backwards-compatibility shims.** If a function signature changes, update all callers. Don't add a wrapper that converts old arguments to new ones. Don't re-export removed symbols. If it's unused, delete it completely.

<!-- Updated 2026-04-03: added Gate 9 after user feedback -- never patch, always production-ready -->

---

## Post-Write Verification

After writing code, before committing:

1. **Build check:** `bash scripts/build.sh` -- must show `=== BUILD OK ===`
2. **Static assert check:** If you added `_Static_assert`, temporarily break the invariant and verify the build fails with the expected message. Revert.
3. **Test check:** If you added tests, verify they're registered and would run with `test=1`.
4. **SMP hazard scan:** Any new `static` mutable variable without a spinlock, atomic, or per-CPU comment is an SMP bug. The pre-commit linter catches stdlib includes.

## Self-Update Protocol

This skill should evolve as the kernel grows. Update this file when:

- A new "Bare Metal Gotcha" is discovered during debugging -- add it to Gate 6
- A new cross-file invariant pattern emerges -- add it to Gate 3 examples
- A new subsystem category is added to the test framework -- add it to Gate 7
- A Gate rule causes a false alarm repeatedly -- refine the rule to be more precise
- A bug slips through that a Gate should have caught -- add a check for that bug class

When updating, add a comment at the bottom of the relevant Gate section:
```
<!-- Updated YYYY-MM-DD: added X rule after Y incident -->
```

## Quick Reference Card

| Situation | Action |
|-----------|--------|
| New global mutable variable | Add spinlock or make atomic or per-CPU |
| Spinlock around I/O or blocking op | Don't -- snapshot under lock, release, then I/O |
| New struct used by assembly | 5-layer defense (static assert + runtime + test + canary + doc) |
| Enum grows (new entry added) | Update ALL parallel arrays; add `_Static_assert(count == ENUM)` |
| New init function in boot path | POST16 entry/exit + klog + error handling |
| Moving boot-path work to a thread | Don't -- compositor starves threads; run inline |
| Code called from panic/fault handler | No spinlocks, no kmalloc, no interrupt assumptions |
| Writing JSON or structured output | Escape `\ " \n \r \t` and control chars in strings |
| New MMIO access (device regs) | `vmm_map_mmio_uc()` with UC attributes |
| Framebuffer VRAM access | `vmm_map_mmio_wc()` with Write-Combining |
| Allocation > 4 KB | `pmm_alloc_contiguous()`, not `kmalloc()` |
| Any allocation | Check for NULL return |
| Cross-file constant | `_Static_assert` in every file that uses it |
| New public function | At least one unit test assertion |
| Bare-metal crash lesson | Add to CLAUDE.md Bare Metal Gotchas |
| Unicode in strings/comments | Replace with ASCII `--` |
| Tempted to write TODO/FIXME | Do the work now or don't write the code |
| Magic number in source | Extract to `#define` in a header |
| Test failing | Fix the code, never skip the test |
| Emulator-specific behavior | Write to the hardware spec, not the emulator |
| Arch header in fs/ob/ipc/nt code | Don't include it -- find an arch-neutral way |
| Inline asm in neutral code | Use a wrapper or HAL call, not raw asm |
| GDT/IDT concept in neutral logic | Keep in arch-specific files (task.c context setup) |
| Packed data area with variable-size fields | Assert non-overlap, not just value correctness |
