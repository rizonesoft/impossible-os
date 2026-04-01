# CLAUDE.md — Impossible OS

> Claude Code project instructions for the Impossible OS kernel. This file and `.claude/skills/` are the complete Claude Code system — self-contained, no shared layers.

## Build — Never use raw `make`

```bash
bash scripts/build.sh           # incremental
bash scripts/build.sh clean     # full clean
bash scripts/build.sh run       # build + QEMU
```
Check `tail -1 build/build.log` for result — must show `=== BUILD OK ===`.

## Freestanding Kernel — No stdlib

- No `<stdint.h>`, `<string.h>`, etc. — use `#include "kernel/types.h"`
- No `malloc()`/`printf()` — use `kmalloc()`, `pmm_alloc_contiguous()`, `printk()`
- `kmalloc()` for ≤ 4 KB only; `pmm_alloc_contiguous()` for everything larger

## Assembly — NASM x86-64 only

- UEFI-era, Long Mode, APIC environment — no BIOS/VGA/PIC assumptions

## API Surface — Win32 native

- Win32 is the native API; POSIX via Linux compat layer only
- Canonical paths use Windows style: `C:\Impossible\System32\`

## Development Strategy — Bare Metal First, SMP From Day One

**SMP-safe by default.** Every new feature must work correctly on multi-CPU systems. Never design single-CPU assumptions into the code — use per-CPU data, proper locking, and atomic operations from the start. Windows NT was SMP from day one; Linux added it later and paid for it with the BKL for 20 years.

Bare metal is the target platform. VMs (QEMU, VBox) are convenience tools for fast iteration, not validation. Every feature must work on real hardware before it's done. "Verified on QEMU" is necessary but not sufficient — "Verified on bare metal" is the acceptance criteria.

When writing hardware-touching code, ask: "does this work without a hypervisor?" Emulated hardware (Bochs VGA, forgiving LAPIC, trapped MMIO) hides bugs that crash on real CPUs.

## Bare Metal Gotchas

These are hard-won lessons from real hardware debugging. Violating any of these will crash on bare metal while appearing to work fine in VMs.

- **No LAPIC TPR writes in ISR path.** The IDT `isr_handler` must track IRQL in software only — no `lapic_write(LAPIC_REG_TPR, ...)` on interrupt entry/exit. The LAPIC hardware handles vector priority masking via ISR/PPR. TPR writes break emulated LAPIC on WHPX/VBox/TCG.
- **No SMEP/SMAP until per-process page tables.** The shared identity-mapped address space uses 2 MiB pages; user stacks are `kmalloc`'d from the kernel heap, so user and kernel data share the same 2 MiB pages. Clearing User bit from "kernel" pages also blocks user-mode stack access. `hv_supports_cr4_smep_smap()` returns 0 for `PLATFORM_BARE_METAL`.
- **No MMIO through WB-cached pages.** HPET, ECAM, NVMe BARs, and future GPU BARs must use `vmm_map_mmio_uc()` with UC (uncacheable) attributes. The bootloader identity-maps everything as WB. LAPIC/IOAPIC work only because MTRRs override those ranges to UC. HPET calibration uses `vmm_map_mmio_uc()` (fixed 2026-03-29).
- **No CLAC/STAC without SMAP CPUID.** `clac` and `stac` cause #UD on CPUs without SMAP in CPUID — including QEMU TCG and VirtualBox NEM. The ISR common stub must NOT use `clac`/`stac` until SMAP is actually enabled via per-process page tables. This was the root cause of the 2026-03-28 "hardware interrupts crash on bare metal/TCG" issue.
- **GS_BASE must be set before any interrupt fires.** `smp_early_bsp_init()` is called as the first thing in `boot_phase0()` — before serial init. On bare metal, `GS_BASE` defaults to 0; `smp_this_cpu()` reads garbage from the real-mode IVT at physical address 0 instead of NULL, crashing the IRQL tracking in `isr_handler`.
- **No Init Level De-Assert IPI.** The broadcast Init Level De-Assert (ICR: INIT | ALL | LEVEL_DEASSERT) was deprecated since Intel P6 (1995) and is a hardware no-op on all x86-64 CPUs. On WHPX with 2+ vCPUs it hangs because the hypervisor traps the broadcast and stalls waiting for the not-yet-booted AP. Removed entirely 2026-04-01. The per-AP INIT→de-assert→SIPI sequence in `lapic_send_init()` is unrelated and required.
- **User-mode ELF range (0x800000–0x900000).** Three files must stay in sync: `vmm.c` (any U/S policy), `pmm.c` (`pmm_mark_region_used`), `user/user.ld` (linker base).

## Safety Gates

Stop and ask before: security-sensitive changes, destructive operations, ABI changes, dependency additions, large refactors.

## Doc Sync

When you change code or conventions, update `CLAUDE.md`, `.claude/skills/`, `.cursor/rules/`, `.cursor/skills/`, and affected TODO files in the same task.

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
| `/create-todo` | Create a new TODO file |
| `/validate-todo-file` | Validate a TODO for structural gaps |
| `/verify-todo-section` | Verify a TODO section against code evidence |
| `/improve-implementation-order` | Audit and fix an Implementation Order table |
| `/sync-ai-system` | Sync AI guidance across Cursor and Claude Code |

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
.cursor/            Cursor AI system (rules + skills) — independent
.claude/            Claude Code AI system (skills) — independent
```
