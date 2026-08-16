---
name: kernel-code-quality
description: Automatic code quality checklist for writing kernel C code. Enforces SMP safety, 5-layer defense for invariants, memory rules, bare-metal correctness, POST16 diagnostics, unit test coverage, and error handling. Auto-loads when writing or modifying kernel source files (.c, .h, .asm) in src/ or include/.
---

# Kernel Code Quality

> Auto-loads when writing kernel code. A pre-flight checklist, not a post-hoc
> review: apply these rules WHILE writing. Every rule here was paid for by a
> real incident -- histories, full code examples, and the self-update protocol
> live in [references/incidents.md](references/incidents.md) (read on demand,
> e.g. when challenging or updating a gate).

## When This Applies

Every create/modify of `.c`, `.h`, `.asm` under `src/kernel/` or
`include/kernel/` (auto-routed by the domain hook; also during
`/implement-todo-section`, `/implement-ssdt-range`, direct edits). NOT for:
`src/boot/` (boot-code-quality), `src/desktop/`, `src/shell/`,
`user/`/`src/apps/` (their own skills), docs/TODO/script changes.

## Pre-Write Checklist (each gate is pass/fail)

### Gate 1: Freestanding Kernel Rules

- [ ] **No stdlib headers.** `#include "kernel/types.h"` -- never `<stdint.h>`, `<string.h>`, `<stdlib.h>`, `<stdio.h>`.
- [ ] **No stdlib functions.** `kmalloc()`/`kfree()`, `klog()`/`printk()`, `memcpy()`/`memset()` from `kernel/types.h`.
- [ ] **Memory sizing.** `kmalloc()` <= 4 KB only; `pmm_alloc_contiguous()` for larger. Always NULL-check.
- [ ] **ASCII only.** No U+2013/U+2014, no UTF-8 in strings/comments; rewrite prose, never `--` as fake em dash (CLAUDE.md policy).

### Gate 2: SMP Safety (Every. Single. Time.)

> 2+ CPUs from early boot. Unsynchronized mutable shared state WILL corrupt on real hardware.

- [ ] **New global/static mutable variable?** Spinlock (`spin_lock_irqsave`/`spin_unlock_irqrestore` if ISR-reachable), atomic (`__atomic_fetch_add`/`__atomic_load_n`), or per-CPU (`smp_this_cpu()->field`).
- [ ] **Interrupt-safe?** ISR-reachable locks use `irqsave` variants; plain spinlocks in interrupt context deadlock.
- [ ] **Lock hold time.** Never hold a spinlock across disk I/O, serial output, or anything blocking/millisecond-scale. Pattern: lock -> snapshot -> unlock -> use snapshot.
- [ ] **No CPU-count assumptions.** Works on 1 CPU and 64. A COUNT is never a slot bound -- iterate `MAX_CPUS` with `smp_cpu_is_online()`, or bound by `smp_cpu_present_count()` when walking discovered slots. `smp_cpu_count()` is LIVE (it falls when a CPU parks), so use it ONLY when you mean active processors, never as an index ceiling (TODO-10 section 21: a count-bounded affinity check rejected a live high slot). Never hardcode.

### Gate 3: 5-Layer Defense for Invariants

> Cross-file constants, structs with assembly-visible offsets, and ABI contracts get multi-layer protection. Full code templates + the GDT SYSRET reference implementation: references/incidents.md.

- [ ] **Layer 1 -- `_Static_assert`** on offsets (`__builtin_offsetof` -- freestanding), sizes, values. Value asserts are NOT enough: packed areas with variable-size fields (GDT_PTR/IDT_PTR = 10 bytes) also need NON-OVERLAP asserts.
- [ ] **Layer 2 -- runtime verification at init** (`klog(LOG_FATAL)` + `boot_halt` on mismatch).
- [ ] **Layer 3 -- unit test** (`TEST_ASSERT_EQ`, registered via `test_suite_register_cat`).
- [ ] **Layer 4 -- canary/guard** (guard pages, magic values/CRC verified on use, readback verification).
- [ ] **Layer 5 -- documentation** (offset-table comment; CLAUDE.md gotcha/safety entry; "assembly depends on this" warnings).
- [ ] **Parallel arrays:** when an enum grows, every array indexed by it grows; enforce `_Static_assert(count == ENUM_COUNT)`.
- **All 5 layers when:** new assembly-referenced struct, cross-file ABI contract, boot-critical init, security-critical invariant. **1-2 layers:** internal constants, non-critical helpers.

### Gate 4: Boot-Path Code

> Pre-`sti` errors are silent triple faults.

- [ ] **POST16 codes -- BOOT-PATH ONLY.** Init called from `boot_phase0/1/2.c` gets `POST16()` entry/exit (`0xD000-0xDFFF` dev range, `0xDDNN` format; check `include/kernel/boot_init.h` before assigning). Do NOT add POST16 to scheduler/syscall/exec/ELF/file-I/O/IPC/network or anything post-Phase-3 -- klog works there; post-boot POST16 is cargo-culted noise.
- [ ] **klog entry + exit** so serial shows progress.
- [ ] **Fail with message, never silence.** `boot_halt("why")` for unrecoverable; `klog(LOG_WARN)` + degrade for non-critical; never a bare failed return.
- [ ] **No thread_create for boot-path work.** compositor_run() starves pre-existing kthreads; init between `desktop_init()` and `compositor_run()` runs inline.
- [ ] **Panic-path code: no locks, no alloc.** panic/fault/boot_halt paths: no spinlocks (faulting context may hold them), no `kmalloc` (heap may be corrupt), no interrupt assumptions; serial_write + direct physical access only.

### Gate 5: Error Handling

- [ ] **Check every allocation** (`kmalloc`, `pmm_alloc_frame`, `pmm_alloc_contiguous` return NULL/0).
- [ ] **Severity:** `LOG_FATAL`+`boot_halt` unrecoverable / `LOG_ERROR` degraded-continue / `LOG_WARN` handled-unexpected / `LOG_INFO` milestones / `LOG_DEBUG` tracing.
- [ ] **No silent failure.** Fallible functions return an error code or log; bare `return;` after a failed check is a bug.
- [ ] **Escape structured output.** JSON/config writers escape `\ " \n \r \t` + control chars in variable strings; never snprintf raw strings into JSON.

### Gate 6: Bare Metal Correctness

> "Works on QEMU" is necessary, not sufficient. Incident details: references/incidents.md.

- [ ] **MMIO via UC/WC pages.** Device registers -> `vmm_map_mmio_uc()`; framebuffer VRAM -> `vmm_map_mmio_wc()`. MMIO through WB pages fails on real hardware.
- [ ] **No CPUID-gated instructions unchecked.** `clac`/`stac`, `xsave`, AVX: gate on `cpu_has(CPU_FEATURE_XX)`.
- [ ] **`msr_try_read()` is a no-crash guarantee, NOT an existence probe** (WHPX absorbs unknown MSRs). Gate on `cpu_has()`/CPUID first; `msr_try_read()` as secondary net.
- [ ] **Per-CPU MSRs on every CPU.** PAT, XCR0, TSC_AUX, KERNEL_GS_BASE etc. written in `boot_phase0()` must also be written in `ap_entry()` (via `cpu_harden()`).
- [ ] **CR3 reloads on WHPX can reset per-vCPU MSRs.** Re-program PAT (etc.) after page-table changes that flush the TLB via CR3 reload.
- [ ] **Never compile SSE2 fallback code with `-mavx2`** (VEX encoding #UDs on non-AVX CPUs). Split translation units: AVX2 file vs `-msse2` fallback+dispatch file.
- [ ] **GS_BASE set before `gs:N` reads** -- per-CPU access only after `smp_early_bsp_init()`; if unsure check `smp_this_cpu() != NULL`.
- [ ] **No LAPIC TPR writes in ISR path.** IRQL tracking is software-only.
- [ ] **User-mode pages need User bit at all 4 levels** -- `vmm_set_user_page()` handles split+propagate; per-process pages currently share identity-mapped frames.
- [ ] **Framebuffer 5-rule checklist:** runtime pixel format; stride from `PixelsPerScanLine`; VRAM WC-remapped after page-table takeover; all GOP `SetMode()` before EBS; back-buffer.

### Gate 7: Architecture Neutrality

> ARM64 port is planned (domain 16). Code outside `arch/` compiles for both.

- [ ] **No arch headers in neutral code** (`fs/`, `ob/`, `ipc/`, `nt/`, `registry.c`, `klog.c`, desktop/, shell/): no `gdt.h`, `idt.h`, `msr.h`, `cpuid.h`, lapic/ioapic/pit/pic headers, `cpu_security.h`.
- [ ] **No inline x86 asm in neutral code** (no `cli`, `rdtsc`, `invlpg` in VFS/OB/IPC/registry); use existing wrappers or the future HAL.
- [ ] **No GDT selectors or x86 register names in neutral logic** (`GDT_*`, `0x08`, CR0/CR3/CR4/EFER/`MSR_IA32_*` stay in arch files).
- [ ] **Known arch-specific files (acceptable):** `gdt.c`, `idt.c`, `isr_stubs.asm`, `syscall_entry.asm`, `syscall_fast.c`, `lapic.c`, `ioapic.c`, `pit.c`, `cpuid.c`, `msr.c`, `cpu_security.c`, `smp.c`, `ap_trampoline.asm`.
- [ ] **Borderline files** (`task.c`, `vmm.c`, `panic.c`, `irql.c`, `spinlock.c`): keep new code neutral where possible; mark arch sections `/* ARCH: x86-64 -- will move to arch/ */`.

### Gate 8: Unit Tests

- [ ] **New public function?** >= 1 test assertion in the right `test_*.c`.
- [ ] **New invariant?** `TEST_ASSERT_EQ` (Layer 3).
- [ ] **Register with category:** `test_suite_register_cat("name", fn, TEST_CAT_XX)`.
- [ ] **Concrete assertions** -- numeric expected values, never "verify it works".
- [ ] **NEVER call live boot infrastructure from a test** (3 incidents; hook-enforced). Forbidden: `boot_progress`, `boot_post_write16`, `post_display16`, `vpd_*`, `boot_splash_*`, `boot_halt`, `panic`, `KeBugCheckEx`, any subsystem `_init(`. Allowed: pure constant checks, save/restore wrappers, PURE data helpers (`boot_timing_record_step` OK; `boot_progress` NOT), read-only oracle queries. If Codex recommends testing a forbidden function, REJECT with code evidence (test the pure helper or accept the gap with a `**Note:**`). Opt-out for controlled fault-recovery tests: `/* TEST-SIDE-EFFECT-ALLOWED: <reason> */`. Full table: docs/infrastructure/test-policy.md.

### Gate 9: Documentation Sync

- [ ] New bare-metal lesson -> CLAUDE.md "Bare Metal Gotchas". New safety invariant -> "Safety Gates". Changed convention -> CLAUDE.md + `.claude/skills/` + TODOs, same commit. Assembly-visible struct -> offset-table comment.

### Gate 10: Production Quality -- No Patches, No Workarounds

> Every line ships as final. No "fix it later" pass.

- [ ] **No TODO/FIXME/HACK in new code** -- do the work now, or walk the scope-gap protocol ([implement-todo-section/scope-gap-protocol.md](../implement-todo-section/scope-gap-protocol.md)) so the paper trail lands in the TODO, not a comment.
- [ ] **No magic numbers** -- named `#define` in a header.
- [ ] **No copy-paste** -- extract on the third occurrence (not the first).
- [ ] **No emulator workarounds** -- code to the hardware spec; never `if (running_on_qemu)`. Only `hv_supports_*()` where hardware genuinely differs.
- [ ] **No skip lists / suppression** -- fix code, not tests; no `#pragma GCC diagnostic ignored`.
- [ ] **Never widen assertions to accept per-platform values** -- diagnose WHY the platform differs; a test accepting two answers verifies nothing.
- [ ] **Complete error paths** -- every `if (error)` logs/cleans/returns meaningfully.
- [ ] **Correct the first time** -- read spec + existing code + integration surface BEFORE typing.
- [ ] **No backwards-compat shims** -- signature changed? Update all callers; delete unused symbols completely.

### Gate 11a: Win32 API Surface -- Fix, Never Remove

- [ ] **Never remove an SSDT registration, syscall entry, or Win32-shaped export to "fix" it.** Registered slots/exports are PROMISES to future callers (Product North Star); removal is worse than broken.
- [ ] **Wrong ABI/signature/behavior? Implement the correct one** (Windows Internals 7e / MSDN / UEFI 2.10 / the SSDT master tables) -- never silently drop the registration.
- [ ] **Corner-cutting handlers with the right ABI** are Gate 11 work: extend to full spec, same commit when scope fits.
- [ ] **Only legitimate removal:** the syscall NUMBER itself is wrong -- and that is a MOVE, not a delete.
- [ ] **When in doubt, ask the user before removing any Win32 surface** (30 seconds vs an hour + regression scare; incident 2026-04-27 in references/incidents.md).

### Gate 11: Spec Compliance -- No Sub-Standard Code

- [ ] **Full spec, not the subset your test platform tolerates** -- check every correctness-affecting field/subtype/flag.
- [ ] **No "works on QEMU/WHPX" shortcuts** -- real firmware enforces the spec strictly.
- [ ] **Fix sub-standard code immediately, same commit** -- never accept as "forward-reserve" / "low risk" (incident 2026-04-12 in references/incidents.md).
- [ ] **Validate all spec-defined fields** when parsing ACPI/UEFI/MSR/PCI/hardware layouts.

## Post-Write Verification

1. `bash scripts/build.sh` -> `=== BUILD OK ===`.
2. New `_Static_assert`? Temporarily break the invariant, verify the build fails with the message, revert.
3. New tests registered and runnable with `test=1`.
4. SMP hazard scan: any new `static` mutable without lock/atomic/per-CPU comment is a bug.

## Quick Reference Card

| Situation | Action |
|-----------|--------|
| New global mutable variable | Add spinlock or make atomic or per-CPU |
| Spinlock around I/O or blocking op | Don't -- snapshot under lock, release, then I/O |
| New struct used by assembly | 5-layer defense (static assert + runtime + test + canary + doc) |
| Enum grows (new entry added) | Update ALL parallel arrays; add `_Static_assert(count == ENUM)` |
| New init function in BOOT path (Phase 0/1/2) | POST16 entry/exit + klog + error handling |
| New non-boot function (sched/exec/syscall/io) | klog only -- NO POST16 |
| Moving boot-path work to a thread | Don't -- compositor starves threads; run inline |
| Allocating XSAVE/FXSAVE buffer | Zero + FCW at offset 0 = 0x037F + MXCSR at offset 24 = 0x1F80 (zeroed values unmask all FP exceptions -> #MF/#XM) |
| SSE2 fallback in SIMD dispatch | Compile SSE2 code with `-msse2` ONLY, never `-mavx2` |
| XSAVE/XRSTOR/FXSAVE/FXRSTOR | CLTS first -- ALL FPU instructions fault #NM when CR0.TS=1 |
| FPU save/restore in scheduler | In BOTH preemptive schedule() AND cooperative schedule_now() |
| Code called from panic/fault handler | No spinlocks, no kmalloc, no interrupt assumptions |
| Writing JSON or structured output | Escape `\ " \n \r \t` + control chars in strings |
| New MMIO access (device regs) | `vmm_map_mmio_uc()` |
| Framebuffer VRAM access | `vmm_map_mmio_wc()` |
| Allocation > 4 KB | `pmm_alloc_contiguous()`, not `kmalloc()` |
| Any allocation | Check for NULL return |
| Cross-file constant | `_Static_assert` in every file that uses it |
| New public function | At least one unit test assertion |
| Test code touching boot infrastructure | DON'T: pure helper or save/restore wrapper only |
| Bare-metal crash lesson | Add to CLAUDE.md Bare Metal Gotchas |
| Unicode in strings/comments | Remove U+2013/U+2014; rewrite prose |
| Tempted to write TODO/FIXME | Walk the scope-gap protocol -- Branches A/B/C/D |
| Magic number in source | Extract to `#define` in a header |
| Test failing | Fix the code, never skip the test |
| Emulator-specific behavior | Write to the hardware spec, not the emulator |
| Probing MSR existence | CPUID/cpu_has() first; msr_try_read() as safety net only |
| Programming a per-CPU MSR (PAT, XCR0, ...) | Write on BSP AND every AP; cpu_harden() is the AP hook |
| CR3 reload (vmm_flush_tlb_all) | Re-program PAT + critical MSRs after |
| Test fails on one platform only | Fix the root cause, never widen assertions |
| Arch header in fs/ob/ipc/nt code | Don't -- find an arch-neutral way |
| Inline asm in neutral code | Use a wrapper or HAL call |
| GDT/IDT concept in neutral logic | Keep in arch-specific files |
| Packed area with variable-size fields | Assert non-overlap, not just values |
