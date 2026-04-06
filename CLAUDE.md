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

## No Unicode Dashes -- ASCII Only

Never use em dashes (`--`, U+2014), en dashes (`--`, U+2013), or other Unicode punctuation in source files, comments, strings, docs, or scripts. Always use ASCII double-dash `--`. Serial output goes to Windows terminals (CMD/PowerShell) which render UTF-8 multi-byte characters as garbled `ΓÇö`. This applies to `.c`, `.h`, `.md`, `.sh`, `.ps1`, `.bat`, `.conf`, and all other tracked files.

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

| Skill | Description |
|---|---|
| `/implement-todo-section` | Implement one TODO section end-to-end |
| `/implement-ssdt-range` | Implement + wire a range of SSDT entries, mark Done [x] |
| `/create-todo` | Create a new TODO file |
| `/validate-todo-file` | Validate a TODO for structural gaps |
| `/verify-todo-section` | Verify implemented section -- full Codex pipeline without implementing |
| `/improve-implementation-order` | Audit and fix an Implementation Order table |
| `/implement-unit-tests` | Implement a TODO's Unit Tests section end-to-end |
| `/diagnose-serial-log` | Parse serial log, classify WARN/FAIL/crash, trace to source, fix real bugs |
| `/run-verification` | Verify a TODO against a serial log (pasted or file), mark PASS/FAIL, detect regressions |
| `/validate-doc` | Fact-check a doc against the codebase, check for charts/tables/OS Comparison |
| `/todo-pipeline` | 3-stage TODO prep: validate -> gap analysis -> validate (before implementation) |
| `/codex-design-review` | Codex pre-implementation design review -- catches plan flaws before coding |
| `/codex-review-todo` | Codex adversarial review of all implemented sections in a TODO |
| `/codex-fix-review` | Fix Codex findings, re-review until resolved (max 3 iterations) |
| `/codex-test-coverage` | Codex test coverage gap analysis -- finds untested paths and missing assertions |
| `/codex-impact-analysis` | Codex dependency impact analysis -- what breaks if you change X? |
| `/codex-consistency-audit` | Codex cross-file consistency audit -- struct offsets, constants, API contracts |
| `/codex-dead-code` | Codex dead code scanner -- unused functions, defines, types, declarations |
| `/codex-perf-review` | Codex performance hot-path review -- ISR paths, lock times, O(n^2), allocations |

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
.cursor/            Cursor AI system (rules + skills) -- independent
.claude/            Claude Code AI system (skills) -- independent
```
