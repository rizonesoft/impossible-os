# CLAUDE.md -- Impossible OS

> Claude Code project instructions for the Impossible OS kernel. This file and `.claude/skills/` are the complete Claude Code system -- self-contained, no shared layers.

## Build -- Never use raw `make`

```bash
bash scripts/build.sh           # incremental
bash scripts/build.sh clean     # full clean
bash scripts/build.sh run       # build + QEMU
```
Check `tail -1 build/build.log` for result -- must show `=== BUILD OK ===`.

Host bootstrap (deps, supported distros, required-tool sentinels, idempotence): [docs/infrastructure/development-tooling.md#host-bootstrap-contract](docs/infrastructure/development-tooling.md#host-bootstrap-contract). Host profiles (support tiers, min versions, devcontainer, unsupported-host policy): [docs/infrastructure/development-tooling.md#supported-host-profiles-and-reproducible-environments](docs/infrastructure/development-tooling.md#supported-host-profiles-and-reproducible-environments). Re-check with `bash scripts/setup.sh --verify`; version floors via `bash scripts/setup.sh --versions`.

## Testing -- Category-Based Test Infrastructure

> **TCG works in WSL2** (~10s for build + 1493 tests). A post-commit hook runs the full suite after every kernel/source commit. SMEP is disabled globally (boot PML4 has User bit on all pages). WHPX, VirtualBox, and bare metal remain the primary validation platforms.

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

Categories: `mm`, `fs`, `sched`, `ob`, `security`, `ipc`, `boot`, `abi`, `storage`, `exec`. Configured via `test_suite=` and `test_quiet=` in `boot.conf`. Windows: `scripts/debug/kernel/run-mm-tests.bat` etc.

Register new tests with `test_suite_register_cat("name", fn, TEST_CAT_XX)`. Use `TEST_ASSERT_EQ(a, b, msg)` for value comparisons and `TEST_SKIP(msg)` for hardware-dependent tests.

## Smoke Test -- End-to-End Boot Validation

```bash
bash scripts/test-smoke.sh        # builds, boots in QEMU, checks for Boot complete + C:\>
```

The smoke test complements unit tests: unit tests cover individual behaviors against in-memory fixtures, the smoke test proves **the image actually boots to userspace**. It auto-selects KVM if `/dev/kvm` is writable and falls back to TCG otherwise. 30-second timeout, fail-fast on missing markers.

**When to run:**

- **After boot-path changes** -- any edit to `src/boot/`, `src/kernel/main/boot_*`, `src/kernel/idt.c|gdt.c|msr.c`, `src/kernel/smp/`, `src/kernel/mm/pmm.c|vmm.c|heap.c`, `src/kernel/drivers/lapic.c|ioapic.c|acpi.c|timer.c`, or any Phase 0/1 init code. Built into `/implement-todo-section` step 16 for exactly these paths.
- **While debugging** a boot crash or init-order bug -- `/debug-session` uses it as the primary reproduction + verification tool. Inspect `build/smoke-test.stripped.log` (ANSI stripped) to grep klog/POST16 output.
- **Before handing the user a build to test on native Windows / bare metal** -- a green smoke test on KVM catches ~90% of the phase-0/1 regressions that used to be caught only by the user's boot.

**When NOT to run:**

- Every edit or every commit -- that's what the post-commit unit-test hook is for. Smoke test is advisory, not a gate.
- Non-kernel changes (user/, src/apps/, src/shell/ alone): unit tests already cover the testable surface.

**KVM vs TCG roles** (both work in WSL2 as of 2026-04-17):

| Platform | Speed | Catches | Misses |
|---|---|---|---|
| **KVM** (default when available) | ~2s boot | real-CPU MSR traps, SMP timing, cache semantics | emulated-device bugs (NVMe, USB) |
| **TCG** (fallback) | ~10s boot | device emulation bugs, portable x86 behavior | per-CPU state, real MSR behavior |
| **WHPX / VBox / bare metal** (user runs) | varies | the rest | (authoritative) |

Neither WSL runner replaces native Windows or bare metal for shipping. They filter regressions before the user has to boot the image.

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

## Comments -- No Bare Section Refs in Code

Do not write bare "section-sign + number" references in source code comments, headers, assembly, tests, or tooling scripts. A comment like `/* capability negotiation */` carries its meaning forever; `/* section 11 capability negotiation */` (with the section-sign glyph and just a number) goes stale the moment the owning TODO renumbers its sections, and even before then it names no TODO and no domain so a reader has no way to re-anchor it.

**Rule:** in any file that is NOT `.md`, NOT inside `todo/`, and NOT inside `.claude/`, the section-sign glyph followed by a digit is only legal when the same line carries an external-spec qualifier that points at a stable published standard: UEFI, Intel, SDM, AMD, RFC `<n>`, ACPI `<n>`, NTFS, FAT32/16, NVMe, PCI, PCIe, PE/COFF, PE32, USB `<n>`, xHCI / EHCI / OHCI / UHCI, VirtIO, SMBIOS, IEEE, NIST, TCG, WHEA, HPET, MP Spec, the literal word "spec " or "specification". Anything else is a bare reference and must be rewritten.

**Fix options** (pick one):
- Rewrite the comment to name the feature (`/* capability negotiation */`, `/* xHCI BIOS -> OS handover */`).
- Prefix with an external-spec qualifier (`/* UEFI 2.10 section 4.6 */`).
- Replace with a markdown-doc link (`/* see docs/boot/boot-info-fields.md#capability-negotiation */`).

**Enforcement:** `scripts/lint.sh` Check 5 + a PreToolUse hook in `.claude/settings.json`. The hook blocks edits at save-time so CI never rejects; the lint check is the full-repo backstop. Current legacy violations are warn-listed; any NEW file or any unlisted file must not introduce bare section refs. Legacy cleanup is tracked as a follow-up item in `todo/00-infrastructure/TODO-01-developer-tooling-stack.md` Tooling Doctor section.

## Freestanding Kernel -- No stdlib

- No `<stdint.h>`, `<string.h>`, etc. -- use `#include "kernel/types.h"`
- No `malloc()`/`printf()` -- use `kmalloc()`, `pmm_alloc_contiguous()`, `printk()`
- `kmalloc()` for ≤ 4 KB only; `pmm_alloc_contiguous()` for everything larger

## Assembly -- NASM x86-64 only

- UEFI-era, Long Mode, APIC environment -- no BIOS/VGA/PIC assumptions

## API Surface -- Win32 native

- Win32 is the native API; POSIX via Linux compat layer only
- Canonical paths use Windows style: `C:\Impossible\System32\`

## Product North Star -- Complete, Compatible, Better

Impossible OS is not trying to be a partial Windows clone or a Linux-alike with renamed APIs. The target is broader: Win11 compatibility where that is the right surface, Linux compatibility where that expands reach, and a cleaner, faster, more refined system than either one.

**Completion-first, not checklist-first.** Finishing the listed checklist items is not enough if the feature is still obviously incomplete, fragile, poorly wired, or missing adjacent work that a real user would immediately hit next. When implementation reveals that nearby work is required for credible completeness, the agent must either:

- implement it now if it fits the current subsystem and one-session scope, or
- file it immediately in the owning TODO section or TODO file with a concrete checklist item and reciprocal XREF.

Do not ship happy-path completion with hidden adjacent gaps. "Done for this section" is not the bar; "credible, working, and properly owned" is the bar.

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
- **`msr_try_read()` requires the kernel IDT to be loaded.** The probe installs a custom #GP handler in a C-level `handlers[]` table consulted only by our ISR stubs, which only dispatch after `idt_init()` has `lidt`-ed the kernel IDT. Before that, IDTR still points at the UEFI IDT, and any real #GP (KVM trapping IA32_MPERF, real hardware trapping an unavailable MSR) is caught by UEFI's handler and halts the system. `msr_try_read()` now gates on `idt_is_loaded()` and returns `-1` pre-IDT; callers must have a fallback path. `simd_enable_avx512()` was moved from boot_phase0 to boot_phase1 (after `idt_init()`) so its throttle guard keeps working on bare metal. Discovered 2026-04-17 when the KVM smoke test crashed at MPERF probe in phase 0. Under virtualization CPUID lies (KVM with `-cpu host` passes AVX512F through but traps MPERF); always pair CPUID gate with `msr_try_read()` fallback.
- **CR3 reloads on WHPX can reset per-vCPU MSRs.** `write_cr3(read_cr3())` (used by `vmm_flush_tlb_all`) triggers a VMEXIT on WHPX that can reset the PAT MSR (and potentially other per-vCPU MSRs) to Intel defaults. Any MSR that must persist across TLB flushes must be re-programmed after CR3 reloads. Discovered 2026-04-13: `cpu_configure_pat()` wrote WC to PAT entry 1, then `vmm_promote_to_1g()` flushed TLB via CR3 reload, and PAT readback showed WT (Intel default). Fix: re-program PAT after page table modifications that flush TLB.
- **Disk-sourced config files get dynamic buffers + hard-fail overflow.** `resources/boot/boot.conf` was originally read into a 4096-byte stack buffer with a silent `[WARN] truncating` on overflow. On 2026-04-21 doc-comment growth pushed the file to 4.5 KiB; the truncation dropped the patch-appended `test=1` line at EOF and every `test=1` boot ran zero tests without any visible error. Fixed by switching `bootx64.c boot_conf_load()` to `gBS->AllocatePool(FileSize + 1)` via a `GetInfo(&EFI_FILE_INFO_ID, ...)` size probe, with a 1 MiB sanity cap that `boot_fatal()`s on hit (never `[WARN]` + continue). Codified in `boot-code-quality` Gate 14. Any future parser reading disk/user input should follow the same pattern: dynamic-size first, sanity cap second, silent truncation never.

## Safety Gates

- **GDT user segment order is SYSRET-critical.** `GDT_USER_DATA` (0x18) MUST be before `GDT_USER_CODE` (0x20). SYSRET computes CS=STAR[63:48]+16 and SS=STAR[63:48]+8 with fixed offsets. Swapping these selectors corrupts every ring-3 return. Enforced by `_Static_assert` in `gdt.h`, runtime verification in `gdt_init()`, and unit test. Never reorder without understanding the SYSRET constraint.
- **User-mode pages need User bit at all 4 levels.** `vmm_set_user_page()` auto-splits 2 MiB huge pages on demand and propagates User at PML4, PDPT, PD, and PT levels. It works for ANY address now, not just the pre-split user ELF range. However, all per-process user pages currently share the kernel's physical frames (identity-mapped). True per-process isolation requires allocating unique physical pages per process -- planned for the full Win32 PE loader / VirtualAlloc implementation.
- **Guard pages protect all stack and heap boundaries.** `vmm_install_guard_page()` splits 2 MiB huge pages and clears the PTE, causing #PF on access. Page fault handler checks a 32-entry guard table and shows the label (e.g. "GUARD: kernel task stack overflow") instead of generic PAGE_FAULT. Guard pages installed at: kernel task stacks (bottom), AP stacks (bottom), IST stacks (DF/NMI/MCE, bottom), heap end, user ELF range end (0x900000). When allocating new stacks, always allocate N+1 pages and guard the bottom one.
- **Scheduler is flat-cyclic round-robin across ALL runnable threads.** `find_next_task()` walks every `(task, thread)` slot in a single global cyclic order starting one slot after the current one; the first-reached thread at the highest priority wins. Same-priority threads in different tasks are NOT favored over same-task siblings (or vice versa) -- everyone gets a fair turn. Code can rely on this: a `while (!cond) yield()` loop will not starve sibling kthreads. Counter-example fixed 2026-04-15: the old task-first nested scan starved a newly-created kthread when its caller cooperatively yielded (instead of `thread_join`-blocking) -- ALPC handshake tests timed out 5s waiting for a worker that the scheduler never picked.

Stop and ask before: security-sensitive changes, destructive operations, ABI changes, dependency additions, large refactors.

## Doc Sync

When you change code or conventions, update `CLAUDE.md`, `.claude/skills/`, and affected TODO files in the same task.

## Commits -- zero AI-attribution trailers

Impossible OS commits do NOT carry `Co-Authored-By: Claude`, `Assisted-by: TOOL:MODEL`, or similar AI-attribution trailers. AI assistance is implicit in the project's identity (Claude Code-orchestrated by design; see Authority Hierarchy in [docs/infrastructure/ai-system.md](docs/infrastructure/ai-system.md#authority-hierarchy-read-this-first)), not a per-commit concern. This is a deliberate divergence from the Linux kernel 2025-12 `Assisted-by:` convention ([Documentation/process/coding-assistants.rst](https://docs.kernel.org/process/coding-assistants.html)) and Fedora's October 2025 adoption; the stance-change condition and the AI-slop screening bar are documented in [CONTRIBUTING.md "AI-Assisted Commit Policy"](CONTRIBUTING.md#ai-assisted-commit-policy-zero-trailer). If a commit in this repo carries such a trailer, the trailer is the bug; amend or rebase before merge.

## Autonomous-agent boundary -- interactive only

Impossible OS does NOT accept autonomous coding-agent PRs (Copilot cloud-agent, Devin, Cognition, or equivalent tools that run tasks in sandboxes and open PRs without per-step human authorship). Every commit is authored by a human operator working interactively with Claude Code. The repo deliberately does NOT ship `.github/workflows/copilot-setup-steps.yml`, `.github/agents/`, `.github/chatmodes/`, or `.github/instructions/` -- their absence is policy, not omission. `.github/copilot-instructions.md`, `AGENTS.md`, and `CLAUDE.md` exist for reviewer-mode / cross-tool-pointer / doctrine-source purposes and are NOT autonomous-agent enablement. GitHub-side cloud-agent enablement (org/repo Settings -> Copilot -> Access policies) is a procedural guard; the regression pack cannot detect it. Full reasoning, MCP-server corollary, and stance-change condition: [docs/infrastructure/ai-system.md "Autonomous-Agent Boundary Policy"](docs/infrastructure/ai-system.md#autonomous-agent-boundary-policy).

## Git Hooks

One command configures all repo-tracked hooks:

```bash
bash scripts/install-hooks.sh              # always-on: pre-commit lint + post-commit COUNT.md
bash scripts/install-hooks.sh --with-pre-push  # same + opt-in build/test pre-push gate
bash scripts/install-hooks.sh --status     # what is active now
bash scripts/install-hooks.sh --remove     # undo
```

Canonical path is `.githooks/` (activated by `core.hooksPath`). Manual edits under `.git/hooks/` are inert once configured. Mandatory gates are the pre-commit lint (fast, staged-file only) and the post-commit `COUNT.md` refresh; the pre-push build+test gate is opt-in because CI already gates PRs. Full lifecycle in [docs/infrastructure/development-tooling.md#local-ci-hooks](docs/infrastructure/development-tooling.md#local-ci-hooks). Claude Code harness hooks (post-commit unit-test + boot-smoke advisories) live in [.claude/settings.json](.claude/settings.json) -- a separate system, not git hooks.

## Toolchain

- Compiler: `clang-19 --target=x86_64-elf`
- Assembler: `nasm`
- Linker: `ld.lld-19`
- Crash debug: `llvm-addr2line-19 -e build/kernel.exe -f <RIP>`

### Kernel Binary Format -- ELF, Permanently

The kernel image is ELF and stays ELF. This is a pinned decision, not a defer-till-later. Rationale: the kernel's file format is strictly internal (the UEFI bootloader at [src/boot/uefi/bootx64.c](src/boot/uefi/bootx64.c) is the only code that parses it); nothing user-visible or Win32-ABI-visible depends on it. The common intuition that "Windows compatibility requires a PE32+ kernel" is correlation, not causation -- Linux kernel + `.ko` modules are both ELF and Windows kernel + `.sys` drivers are both PE because each ecosystem standardized on one format, not because technical alignment was required. An ELF kernel can load PE32+ drivers, EIF native binaries, and ELF Linux binaries regardless; format choices are independent at every layer.

Switch condition (what would unpin this): ONLY if Impossible OS needs to run under a hypervisor or firmware that rejects ELF kernel images (niche -- QEMU KVM/TCG, WHPX, VirtualBox, and UEFI bare metal all accept ELF via our bootloader). Driver ecosystem reach, Win32 API compat, `.sys` loading, `ntoskrnl`-parity claims -- none of these require a PE32+ kernel image.

Driver and module formats are tracked separately in the [kernel module system TODO](todo/04-drivers-hardware/TODO-05-kernel-module-system.md) (kernel module loader) and the [EIF full implementation TODO](todo/02-kernel-core/TODO-20-eif-full-implementation.md) (EIF native binary format). Both accept multiple formats (EIF native + PE32+ for drivers) without influencing the kernel's own format.

## Model Roles

Use the strongest available Opus-class model for implementation and judgment-heavy work such as `implement-todo-section`, `review-todo-section`, `create-todo`, `gap-analysis-todo`, `quality-review-section`, and design-heavy roadmap shaping. Keep Codex and GPT-5.4 style external review passes for adversarial review, consistency audit, dead-code audit, and performance review.

External review does not replace self-review. The agent must always perform its own integration-level check for completeness, polish, regressions, parity gaps, and ownerless adjacent work.

## Skills

Claude Code skills live in `.claude/skills/`. They auto-load when Claude judges them relevant based on the `description` field. Each skill is self-contained. Impossible OS is Claude Code-only as of 2026-04-18; Cursor was removed because the parallel skill set created clutter without a corresponding productivity win. The full ownership matrix (Claude master, Codex/Copilot subordinate reviewers, edit-here-not-there rules) lives at [docs/infrastructure/ai-system.md](docs/infrastructure/ai-system.md).

| Skill | Description |
|---|---|
| `/implement-todo-section` | Implement one TODO section end-to-end |
| `/implement-ssdt-range` | Implement + wire a range of SSDT entries, mark Done [x] |
| `/complete-todo-file` | Finalize a TODO whose `## N.` sections are all shipped, or sweep an active TODO for loose ends: stale `Accepted:`/`Deferred:` XREFs annotated with RESOLVED-by-§N, drifted test counts refreshed against current runs, unfilled `<§N commit>`/`<TBD>` placeholders resolved, `[/]` items narrowed to actually-pending platforms (TCG runnable via `FORCE_TCG=1`), reversible `[ ]` manual items demonstrated and closed. Then runs `/implement-unit-tests`, executes Verification items, flags remaining manual-only, commits |
| `/create-todo` | Create a new TODO file |
| `/validate-todo-file` | Validate a TODO for structural gaps |
| `/verify-todo-section` | Audit-mode wrapper over `/review-todo-section` (downgrade-only; never promotes to `[x]`) |
| `/quality-review-section` | Deep quality review -- standards, optimization, Win11/Linux parity (is it done RIGHT?) |
| `/review-todo-section` | Post-implementation review -- evidence mapping + MANDATORY quality Codex (perf/consistency/dead-code) |
| `/implement-unit-tests` | Implement a TODO's Unit Tests section end-to-end |
| `/audit-ssdt` | Audit SSDT registration vs TODO-05/TODO-12 master tables; insert missing prerequisite items |
| `/diagnose-serial-log` | Full serial-log audit: crashes, bugs, races, leaks, perf, POLICY violations (POLICIES.md), ACCURACY drift, baseline REGRESSION, SCOPE_CREEP; every fix gets Codex adversarial review |
| `/debug-session` | Structured debug session with rubber-duck validation -- hypothesis, trace, rubber-duck review, fix, verify |
| `/todo-pipeline` | 3-stage TODO prep: validate -> gap analysis -> validate (before implementation) |
| `/gap-analysis-todo` | Deep gap analysis of a TODO vs Win11/Linux parity + cross-TODO overlap + code-truth audit |
| `/codex-design-review` | Codex pre-implementation design review -- catches plan flaws before coding |
| `/codex-review-todo` | Codex adversarial review of all implemented sections in a TODO |
| `/codex-adversarial-review-section` | Codex adversarial review loop for a single section (up to 3 rounds) |
| `/codex-fix-review` | Fix Codex findings, re-review until resolved (max 3 iterations) |
| `/codex-test-coverage` | Codex test coverage gap analysis -- finds untested paths and missing assertions |
| `/codex-impact-analysis` | Codex dependency impact analysis -- what breaks if you change X? |
| `/codex-consistency-audit` | Codex cross-file consistency audit -- struct offsets, constants, API contracts |
| `/codex-dead-code` | Codex dead code scanner -- unused functions, defines, types, declarations |
| `/codex-perf-review` | Codex performance hot-path review -- ISR paths, lock times, O(n^2), allocations |
| `/boot-code-quality` | Pre-flight checklist for UEFI boot code -- EBS boundary, table safety, fallbacks |
| `/kernel-code-quality` | Auto-load checklist for kernel C -- SMP safety, memory rules, bare-metal, POST16 boot-path-only |
| `/desktop-code-quality` | Pre-flight checklist for desktop compositor code (placeholder) |
| `/shell-code-quality` | Pre-flight checklist for command shell code (placeholder) |
| `/userland-code-quality` | Pre-flight checklist for user-mode applications (placeholder) |

> Adding, editing, or retiring a skill: see [docs/infrastructure/skill-authoring.md](docs/infrastructure/skill-authoring.md) for the lifecycle, template, and catalog hygiene rules. Every live skill has a row here AND in [.claude/skills/README.md](.claude/skills/README.md); a directory without matching rows is drift.

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
| Commit + push of a TODO section-ship (`git commit` flips `\| \[x\] \|` in Implementation Order) | `review-todo-section` | The NEXT tool call after `git push` MUST be `Skill(review-todo-section, "<path> §N <title>")`. A PreToolUse hook in `.claude/settings.json` checks `git log HEAD` for a `[x]` flip without a matching `**Verified:**` stamp and BLOCKS every Edit/Write/non-review-Skill/non-script-Bash (Read/Grep/Glob stay free so the review can gather info). The gate is the hook, not the skill prose -- the skill's step 20 describes the rule; the hook enforces it. Opt-out: `SKIP_REVIEW_HOOK=1` env var for legitimate false positives (revert commits, stamp-only edits). Pattern I kept breaking on 2026-04-24: "the adversarial Codex already ran during implementation, review is paperwork" -- that substitution IS the failure mode. Step 5 (adversarial) and step 8 (quality) in `review-todo-section` are separate dispatches with separate scopes. |
| Test file in `src/kernel/test/` | `implement-unit-tests` | Wire to `test_runner.c`, correct `TEST_CAT_*`, concrete `TEST_ASSERT_*` with expected values. |
| Codex skill returns findings | `superpowers:receiving-code-review` | Same rule -- every codex-* skill is upstream of receiving-code-review. |
| Test asserts a deferred-feature sentinel (`STATUS_NOT_IMPLEMENTED`, `STATUS_NOT_SUPPORTED`, `E_NOTIMPL`, `ENOSYS`) as the EXPECTED result | `implement-unit-tests` "When to use TEST_PENDING vs TEST_ASSERT" | Use `TEST_PENDING(cond, msg)` instead of `TEST_ASSERT`. The deferred-feature outcome counts in the `pending` bucket and renders as `[STUB]` in the boot log; the end-of-run summary surfaces total reserved-but-unimplemented features at a glance. NEVER pair a `TEST_PENDING` test with a runtime `klog(LOG_WARN, ...)` in the stub body -- that duplicates the signal. Hook fires PostToolUse on test_*.c edits to remind. |
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
.claude/            Claude Code AI system (skills + settings + hooks)
```
