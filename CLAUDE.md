# CLAUDE.md -- Impossible OS

> Claude Code project instructions for the Impossible OS kernel. This file and `.claude/skills/` are the complete Claude Code system -- self-contained, no shared layers.

## Build -- Never use raw `make`

```bash
bash scripts/build.sh           # incremental
bash scripts/build.sh clean     # full clean
bash scripts/build.sh run       # build + QEMU
```
Check `tail -1 build/build.log` for result -- must show `=== BUILD OK ===`.

## Testing -- Category-Based Test Infrastructure

> **TCG works in WSL2** (~10s for build + 328 tests). A pre-push hook runs the full suite before every push. SMEP is disabled globally (boot PML4 has User bit on all pages). WHPX, VirtualBox, and bare metal remain the primary validation platforms.

```bash
bash scripts/test.sh              # all suites (WSL2 TCG, native Windows, bare metal)
bash scripts/test.sh SUITE=mm     # Memory Management only
bash scripts/test.sh SUITE=ob     # Object Manager only
bash scripts/test.sh QUIET=1      # summary only (suppress PASS lines)
make test-mm                      # shorthand for SUITE=mm
make test-fs                      # Filesystem suites
make test-ob                      # Object Manager suites
make test-security                # Security suites
make test-ipc                     # IPC suites (pipe, shmem, semaphore)
make test-sched                   # Scheduler suites
make test-boot                    # Boot init + klog suites
make test-abi                     # PEB/TEB + Registry suites
make test-storage                 # AHCI, VirtIO suites
make test-exec                    # Binary system suites (exec, EIF, modules)
```

Categories: `mm`, `fs`, `sched`, `ob`, `security`, `ipc`, `boot`, `abi`, `storage`, `exec`. Configured via `test_suite=` and `test_quiet=` in `boot.conf`. Windows: `scripts/debug/run-mm-tests.bat` etc.

Register new tests with `test_suite_register_cat("name", fn, TEST_CAT_XX)`. Use `TEST_ASSERT_EQ(a, b, msg)` for value comparisons and `TEST_SKIP(msg)` for hardware-dependent tests.

## Test Code -- No Live Boot Infrastructure Calls

> WSL has no working QEMU (CLAUDE memory `feedback_no_qemu_wsl`). I cannot validate runtime behavior here -- the user has to boot on native Windows or bare metal. Tests that mutate live boot state can freeze the kernel between the time they're committed and the time the user notices. **3 incidents to date** (2026-04-07 was a unit test calling `boot_progress("VERIFY_TEST", 0xCAFE)` from `test_boot_init.c`; froze WHPX boot after `BOOT_STEP clears ready on BOOT_DEFERRED`).

**Tests under `src/kernel/test/test_*.c` MUST NOT call** any function in this list -- a pre-commit hook enforces this:

| Forbidden in tests | Why |
|---|---|
| `boot_progress(`, `boot_post_write16(`, `boot_post_nvram_write16(`, `post_display16(` | Live VPD / framebuffer / I/O port / NVRAM side effects |
| `vpd_stage_begin(`, `vpd_stage_done(`, `vpd_stage_fail(`, `vpd_init(` | VPD state machine -- mutates row counters and renders to fb |
| `boot_splash_init(`, `boot_splash_status(`, `boot_splash_finish(`, `boot_splash_start_animation(` | Splash screen state |
| `boot_halt(`, `panic(`, `KeBugCheckEx(` | Halts the running kernel |
| `serial_init(`, `pmm_init(`, `vmm_init(`, `heap_init(`, `klog_early_init(`, `klog_disk_enable(` | Re-initializes a live subsystem |
| `acpi_init(`, `lapic_init(`, `ioapic_init(`, `timer_hal_init(`, `gdt_init(`, `idt_init(` | Brings up real hardware -- crashes if called twice |

**Allowed test patterns:**
- Pure constant checks (`TEST_ASSERT_EQ(POST16_FOO, 0x1020, ...)`)
- Save/restore wrappers around `kernel_subsystem_set_ready` / `kernel_subsystem_ready` on a specific slot (`SUBSYS_PMM` is fine -- existing tests use it)
- Direct calls to pure data helpers (`boot_timing_record_step()` is OK -- it just appends to an in-memory buffer; `boot_progress()` is NOT OK because it ALSO updates VPD/framebuffer)
- BOOT_REQUIRE / BOOT_STEP via wrapper functions that return the expected `boot_result_t`
- Read-only oracle queries (`kernel_subsystem_ready()`, `boot_timing_get_steps()`)

**If a Codex test-coverage finding recommends testing a forbidden function**, REJECT it with code evidence and document the gap. Codex doesn't know about WSL constraints. Test the underlying pure helper instead, or accept the gap and add a `**Note:**` in the TODO's Unit Tests section.

**Opt-out:** for legitimate test cases (panic recovery testing in a controlled context, hardware fault simulation), add `/* TEST-SIDE-EFFECT-ALLOWED: <one-line reason> */` in the test function body.

## No Unicode Dashes (en/em): ASCII only

Do not use Unicode **en dash** (U+2013) or **em dash** (U+2014) in source files, comments, strings, docs, or scripts. Windows serial and CMD/PowerShell often garble those bytes as mojibake (for example `ΓÇö`).

**Prose and explanatory comments:** Do not paste two ASCII hyphens (`--`) where an em dash would go; that is not clearer. It looks like minus or `--flag` noise. **Rewrite** the sentence (colon, semicolon, parentheses, comma pair, or two short sentences). Example: prefer `/* Redzone: detect underflow */` over `/* ... -- detect underflow */`.

**Where ASCII `--` stays correct:** technical uses readers already parse as non-prose (markdown `---`, decrement operator in docs, long options in command examples, search patterns, ranges where house style already uses double hyphen).

This applies to `.c`, `.h`, `.md`, `.sh`, `.ps1`, `.bat`, `.conf`, and all other tracked files.

## Freestanding Kernel -- No stdlib

- No `<stdint.h>`, `<string.h>`, etc. -- use `#include "kernel/types.h"`
- No `malloc()`/`printf()` -- use `kmalloc()`, `pmm_alloc_contiguous()`, `printk()`
- `kmalloc()` for ≤ 4 KB only; `pmm_alloc_contiguous()` for everything larger

## Assembly -- NASM x86-64 only

- UEFI-era, Long Mode, APIC environment -- no BIOS/VGA/PIC assumptions

## API Surface -- Win32 native

- Win32 is the native API; POSIX via Linux compat layer only
- Canonical paths use Windows style: `C:\Impossible\System32\`

## Development Strategy -- Bare Metal First, SMP From Day One

**SMP-safe by default.** Every new feature must work correctly on multi-CPU systems. Never design single-CPU assumptions into the code -- use per-CPU data, proper locking, and atomic operations from the start. Windows NT was SMP from day one; Linux added it later and paid for it with the BKL for 20 years.

Bare metal is the target platform. VMs (QEMU, VBox) are convenience tools for fast iteration, not validation. Every feature must work on real hardware before it's done. "Verified on QEMU" is necessary but not sufficient -- "Verified on bare metal" is the acceptance criteria.

When writing hardware-touching code, ask: "does this work without a hypervisor?" Emulated hardware (Bochs VGA, forgiving LAPIC, trapped MMIO) hides bugs that crash on real CPUs.

**Never paper over test failures with platform workarounds.** When a test fails on one platform (WHPX, TCG, VBox), the instinct is to widen the assertion: `TEST_ASSERT(val == expected || val == whpx_default)`. This hides the root cause. Instead: diagnose WHY the value differs and fix the underlying code. A test that accepts two answers is a test that verifies nothing. Incident: 2026-04-13 PAT and MSR tests both initially got "accept WHPX default" workarounds; the real bug was per-CPU MSRs not being programmed on APs.

## boot_info ABI

`struct boot_info` at 0x10000 is the handoff contract between BOOTX64.EFI and kernel.exe. An 8-byte header at offset 0 contains magic (`0x49504F53` "IPOS"), version, and size. Phase 0 validates the handoff in two stages before touching it: `boot_info_validate_addr()` confirms the pointer is non-NULL, 8-byte aligned, and within the bootloader's 4 GiB identity map (`BOOT_INFO_EARLY_MAP_END`); `boot_info_validate_header()` then confirms magic, version, and size. Either failure halts with observed vs expected values on serial. **Never ship mismatched BOOTX64.EFI and kernel.exe after a `BOOT_INFO_VERSION` bump** -- the kernel will halt with "boot_info: bad header". Always rebuild both with `bash scripts/build.sh`. Bump `BOOT_INFO_VERSION` in both `include/kernel/boot_info.h` and `src/boot/uefi/bootx64.c` when adding/removing/reordering fields (not needed for new fields in `_reserved` regions). Five per-field offset asserts on the mirror structs catch same-size reorders between kernel and bootloader images.

## Bare Metal Gotchas

These are hard-won lessons from real hardware debugging. Violating any of these will crash on bare metal while appearing to work fine in VMs.

- **No LAPIC TPR writes in ISR path.** The IDT `isr_handler` must track IRQL in software only -- no `lapic_write(LAPIC_REG_TPR, ...)` on interrupt entry/exit. The LAPIC hardware handles vector priority masking via ISR/PPR. TPR writes break emulated LAPIC on WHPX/VBox/TCG.
- **No SMEP/SMAP until per-process page tables.** The shared identity-mapped address space uses 2 MiB pages; user stacks are `kmalloc`'d from the kernel heap, so user and kernel data share the same 2 MiB pages. Clearing User bit from "kernel" pages also blocks user-mode stack access. `hv_supports_cr4_smep_smap()` returns 0 for `PLATFORM_BARE_METAL`.
- **No MMIO through WB-cached pages.** HPET, ECAM, NVMe BARs, and future GPU BARs must use `vmm_map_mmio_uc()` with UC (uncacheable) attributes. The bootloader identity-maps everything as WB. LAPIC/IOAPIC work only because MTRRs override those ranges to UC. HPET calibration uses `vmm_map_mmio_uc()` (fixed 2026-03-29).
- **No CLAC/STAC without SMAP CPUID.** `clac` and `stac` cause #UD on CPUs without SMAP in CPUID -- including QEMU TCG and VirtualBox NEM. The ISR common stub must NOT use `clac`/`stac` until SMAP is actually enabled via per-process page tables. This was the root cause of the 2026-03-28 "hardware interrupts crash on bare metal/TCG" issue.
- **GS_BASE must be set before any interrupt fires.** `smp_early_bsp_init()` is called as the first thing in `boot_phase0()` -- before serial init. On bare metal, `GS_BASE` defaults to 0; `smp_this_cpu()` reads garbage from the real-mode IVT at physical address 0 instead of NULL, crashing the IRQL tracking in `isr_handler`.
- **NVMe I/O unreliable on QEMU WHPX.** QEMU's emulated NVMe controller processes doorbell MMIO writes asynchronously through its event loop under WHPX. The vCPU polls the CQ at native speed before the main thread processes the command, causing intermittent admin and I/O timeouts. Not a driver bug -- the NVMe driver has correct barriers (`wmb`/`rmb`/`clflush`). Real NVMe hardware handles PCIe DMA synchronously with cache snooping. NVMe test uses TCG; normal boot (SATA) uses WHPX fine.
- **No Init Level De-Assert IPI.** The broadcast Init Level De-Assert (ICR: INIT | ALL | LEVEL_DEASSERT) was deprecated since Intel P6 (1995) and is a hardware no-op on all x86-64 CPUs. On WHPX with 2+ vCPUs it hangs because the hypervisor traps the broadcast and stalls waiting for the not-yet-booted AP. Removed entirely 2026-04-01. The per-AP INIT→de-assert→SIPI sequence in `lapic_send_init()` is unrelated and required.
- **User-mode ELF range (0x800000--0x900000).** Constants defined in `include/kernel/mm/user_range.h` (single source of truth). Three C files include it: `vmm.c`, `pmm.c`, `task.c`. `user/user.ld` must be updated manually if the base changes. Static asserts, runtime PMM bitmap verify, guard page at 0x900000, and unit test enforce sync.
- **No thread_create for deferred init.** `boot_run_deferred()` MUST run inline in Phase 3, not on a background thread. `compositor_run()` is an infinite event loop on the BSP that starves any kernel thread created just before it. Moving deferred inits to a thread caused VirtIO input and VBox mouse drivers to never initialize, breaking absolute cursor positioning on QEMU/VBox (2026-04-05). Per-thread fault isolation requires SEH (TODO-10).
- **FXSAVE/XSAVE buffer: set BOTH FCW and MXCSR defaults.** `task_alloc_xsave()` zeroes the buffer, then must set FCW at offset 0 to `0x037F` (all x87 exceptions masked, Intel SDM reset value) and MXCSR at offset 24 to `0x1F80` (all SIMD exceptions masked). Zeroed FCW causes #MF (vector 16) on the first imprecise x87 FP op; zeroed MXCSR causes #XM (vector 19) on the first SSE instruction. On WHPX (has XSAVE), XRSTOR init optimization masked the FCW=0 bug. On TCG (uses FXRSTOR), FCW=0 was loaded directly, freezing boot at the font renderer (2026-04-13). MXCSR=0 caused WHPX BSOD the same day.
- **CR0.TS must be cleared before ALL FPU instructions.** XSAVE, XRSTOR, FXSAVE, and FXRSTOR all fault with #NM when CR0.TS=1 -- not just restores. Both the preemptive schedule() and cooperative schedule_now() must CLTS before any save or restore. Missing CLTS before XRSTOR caused the original WHPX freeze (2026-04-13).
- **Never compile SSE2 fallback code with `-mavx2`.** The compiler emits VEX-encoded instructions (`vmovdqu` instead of `movdqu`) for all SSE operations when `-mavx2` is active, even around inline asm and in scalar tails. On CPUs without AVX (TCG `qemu64`, pre-Sandy Bridge bare metal), VEX instructions cause #UD. Split SIMD files: AVX2 functions in a `-mavx2` translation unit, SSE2 fallbacks and dispatch in a `-msse2` translation unit. This crashed TCG boot when `memops_sse.c` was compiled as part of `memops.c` with `-mavx2` (2026-04-13).
- **`msr_try_read()` is a no-crash guarantee, not an existence check.** WHPX silently absorbs reads of unknown MSRs (returns 0, no #GP), so `msr_try_read()` returning 0 does NOT prove the MSR exists. Always gate on `cpu_has()` or CPUID first; use `msr_try_read()` only as a secondary safety net. Pattern: `if (!cpu_has(FEATURE)) return; if (msr_try_read(MSR, &val) != 0) { fallback; }`. Discovered 2026-04-13 when `msr_try_read(0xFFFFFFFF)` returned success on WHPX.
- **CR3 reloads on WHPX can reset per-vCPU MSRs.** `write_cr3(read_cr3())` (used by `vmm_flush_tlb_all`) triggers a VMEXIT on WHPX that can reset the PAT MSR (and potentially other per-vCPU MSRs) to Intel defaults. Any MSR that must persist across TLB flushes must be re-programmed after CR3 reloads. Discovered 2026-04-13: `cpu_configure_pat()` wrote WC to PAT entry 1, then `vmm_promote_to_1g()` flushed TLB via CR3 reload, and PAT readback showed WT (Intel default). Fix: re-program PAT after page table modifications that flush TLB.

## Safety Gates

- **GDT user segment order is SYSRET-critical.** `GDT_USER_DATA` (0x18) MUST be before `GDT_USER_CODE` (0x20). SYSRET computes CS=STAR[63:48]+16 and SS=STAR[63:48]+8 with fixed offsets. Swapping these selectors corrupts every ring-3 return. Enforced by `_Static_assert` in `gdt.h`, runtime verification in `gdt_init()`, and unit test. Never reorder without understanding the SYSRET constraint.
- **User-mode pages need User bit at all 4 levels.** `vmm_set_user_page()` auto-splits 2 MiB huge pages on demand and propagates User at PML4, PDPT, PD, and PT levels. It works for ANY address now, not just the pre-split user ELF range. However, all per-process user pages currently share the kernel's physical frames (identity-mapped). True per-process isolation requires allocating unique physical pages per process -- planned for the full Win32 PE loader / VirtualAlloc implementation.
- **Guard pages protect all stack and heap boundaries.** `vmm_install_guard_page()` splits 2 MiB huge pages and clears the PTE, causing #PF on access. Page fault handler checks a 32-entry guard table and shows the label (e.g. "GUARD: kernel task stack overflow") instead of generic PAGE_FAULT. Guard pages installed at: kernel task stacks (bottom), AP stacks (bottom), IST stacks (DF/NMI/MCE, bottom), heap end, user ELF range end (0x900000). When allocating new stacks, always allocate N+1 pages and guard the bottom one.

Stop and ask before: security-sensitive changes, destructive operations, ABI changes, dependency additions, large refactors.

## Doc Sync

When you change code or conventions, update `CLAUDE.md`, `.claude/skills/`, and affected TODO files in the same task. Each AI tool system (`.claude/`, `.cursor/`) is independent -- create skills directly for each tool, not synced from a shared layer.

## Toolchain

- Compiler: `clang-19 --target=x86_64-elf`
- Assembler: `nasm`
- Linker: `ld.lld-19`
- Crash debug: `llvm-addr2line-19 -e build/kernel.exe -f <RIP>`

## Skills

Claude Code skills live in `.claude/skills/`. They auto-load when Claude judges them relevant based on the `description` field. Each skill is self-contained.

Cursor keeps a **full mirror** of Claude Code skills under `.cursor/skills/` (same filenames and workflows as `.claude/skills/`). After adding or editing a skill in `.claude/skills/`, sync to Cursor with: `rsync -a --delete .claude/skills/ .cursor/skills/`, then re-apply any Cursor-only deltas (for example Copilot dispatch strings) if you maintain them separately. Rules: `.cursor/rules/todo-validate-gap-workflows.mdc`, `.cursor/rules/todo-workflows-always-pointer.mdc`, `.cursor/rules/claude-third-party-hooks.mdc`. Hooks: `.cursor/hooks.json` plus, when **Third-party skills** is enabled in Cursor, the merged hooks from `.claude/settings.json` (PreToolUse / PostToolUse quality gates). External reviews in Cursor use `bash scripts/copilot-review.sh` (GitHub Copilot CLI when installed). Hooks may not run in every agent session; `@`-reference the skill or rule when automations do not fire. Confirm hook delivery in Cursor **Output → Hooks** if needed.

| Skill | Description |
|---|---|
| `/implement-todo-section` | Implement one TODO section end-to-end |
| `/implement-ssdt-range` | Implement + wire a range of SSDT entries, mark Done [x] |
| `/create-todo` | Create a new TODO file |
| `/validate-todo-file` | Validate a TODO for structural gaps |
| `/verify-todo-section` | Verify implemented section -- compliance audit (is it done?) |
| `/quality-review-section` | Deep quality review -- standards, optimization, Win11/Linux parity (is it done RIGHT?) |
| `/review-todo-section` | Post-implementation review -- evidence mapping + MANDATORY quality Codex (perf/consistency/dead-code) |
| `/implement-unit-tests` | Implement a TODO's Unit Tests section end-to-end |
| `/diagnose-serial-log` | Parse serial log, classify WARN/FAIL/crash, trace to source, fix real bugs |
| `/todo-pipeline` | 3-stage TODO prep: validate -> gap analysis -> validate (before implementation) |
| `/codex-design-review` | Codex pre-implementation design review -- catches plan flaws before coding |
| `/codex-review-todo` | Codex adversarial review of all implemented sections in a TODO |
| `/codex-fix-review` | Fix Codex findings, re-review until resolved (max 3 iterations) |
| `/codex-test-coverage` | Codex test coverage gap analysis -- finds untested paths and missing assertions |
| `/codex-impact-analysis` | Codex dependency impact analysis -- what breaks if you change X? |
| `/codex-consistency-audit` | Codex cross-file consistency audit -- struct offsets, constants, API contracts |
| `/codex-dead-code` | Codex dead code scanner -- unused functions, defines, types, declarations |
| `/codex-perf-review` | Codex performance hot-path review -- ISR paths, lock times, O(n^2), allocations |
| `/boot-code-quality` | Pre-flight checklist for UEFI boot code -- EBS boundary, table safety, fallbacks |
| `/desktop-code-quality` | Pre-flight checklist for desktop compositor code (placeholder) |
| `/shell-code-quality` | Pre-flight checklist for command shell code (placeholder) |
| `/userland-code-quality` | Pre-flight checklist for user-mode applications (placeholder) |

## Mandatory Skill Triggers

These are non-negotiable. Auto-loading by description is unreliable -- the rules below apply regardless of whether the skill description "felt relevant." Hooks in `.claude/settings.json` enforce the first two by injecting reminders into the conversation.

| Trigger | Skill | Why |
|---|---|---|
| Edit/Write on `src/boot/` (`.c`/`.h`/`.asm`/`.S`) | `boot-code-quality` | UEFI error handling, EBS boundary, table safety, framebuffer guards, fallback chains. |
| Edit/Write on `src/kernel/`, `include/kernel/` (`.c`/`.h`/`.asm`/`.S`) | `kernel-code-quality` | SMP safety, memory rules, bare-metal correctness, POST16 boot-path only. |
| Edit/Write on `src/desktop/` (`.c`/`.h`) | `desktop-code-quality` | WC mapping, compositor loop, pixel format, back-buffer pattern. |
| Edit/Write on `src/shell/` (`.c`/`.h`) | `shell-code-quality` | Win32 console API, Windows path conventions. |
| Edit/Write on `user/`, `src/apps/` (`.c`/`.h`) | `userland-code-quality` | User libc, syscall interface, no kernel headers. |
| Codex `adversarial-review` invocation completed | `superpowers:receiving-code-review` | Codex is a reviewer, not an authority. Verify each finding against cited code, classify Fix/Reject/Accept with evidence. No blind implementation, no performative agreement. |
| Implementing a TODO section | `implement-todo-section` | Steps 13-18 (Codex review, fix loop, self-review, build, validate, tie up loose ends) are MANDATORY before commit. No exceptions for "simple" sections. |
| Test file in `src/kernel/test/` | `implement-unit-tests` | Wire to `test_runner.c`, correct `TEST_CAT_*`, concrete `TEST_ASSERT_*` with expected values. |
| Codex skill returns findings | `superpowers:receiving-code-review` | Same rule -- every codex-* skill is upstream of receiving-code-review. |
| Writing "Accepted: ... XREF: TODO-NN §N" in a TODO stamp | `review-todo-section` step 15 | Every Accepted/Deferred XREF must name a concrete `[ ]` checklist item in the target section (use `(item: "NAME" at line N)` or name a specific helper/retrofit). Bare `§N` references and paraphrase-only parentheticals (e.g. `(SeAccessCheck)`) are rejected. Hooks enforce: PostToolUse reminder on TODO edits, `git commit` BLOCKs (exit 2) if staged diff contains bare XREFs. If no concrete item exists in the target, CREATE one now before stamping. |

If a skill is listed above, "I forgot" is not a valid excuse. The hooks will remind you; act on the reminder.

## Repository Layout

```
src/
├── boot/uefi/      Custom UEFI bootloader
├── kernel/         Kernel core (PMM, VMM, scheduler, VFS, drivers)
├── desktop/        Compositing desktop shell
├── shell/          Command-line shell
└── libc/           Minimal kernel libc
include/            All headers (mirrors src/)
resources/          Fonts, icons, wallpapers
todo/               Development roadmap (14 domains, 86 TODO files)
.cursor/            Cursor AI system (rules + skills + hooks) -- independent
.claude/            Claude Code AI system (skills) -- independent
```
