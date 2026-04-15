# Copilot Instructions -- Impossible OS

> Impossible OS is a production-grade 64-bit OS for x86-64. Custom UEFI bootloader, kernel, compositing desktop, and Win32-compatible API surface.

## Critical Rules

### Build
```bash
bash scripts/build.sh           # incremental build
bash scripts/build.sh clean     # full clean build
bash scripts/build.sh run       # build + QEMU
```
- **Never use raw `make` for builds**
- Verify: `tail -1 build/build.log` must show `=== BUILD OK ===`

### Testing
```bash
bash scripts/test.sh              # all suites
bash scripts/test.sh SUITE=mm     # Memory Management only
bash scripts/test.sh SUITE=ob     # Object Manager only
bash scripts/test.sh QUIET=1      # summary only
make test-mm                      # shorthand for SUITE=mm
make test-fs
make test-ob
make test-security
make test-ipc
make test-sched
make test-boot
make test-abi
make test-storage
make test-exec
```
- Test categories: `mm`, `fs`, `sched`, `ob`, `security`, `ipc`, `boot`, `abi`, `storage`, `exec`
- Register tests with `test_suite_register_cat("name", fn, TEST_CAT_XX)`
- Use `TEST_ASSERT_EQ` for concrete expected values and `TEST_SKIP` for hardware-dependent skips
- WHPX, VirtualBox, and bare metal are primary runtime validation targets; WSL2 TCG is useful for fast test runs

### Kernel C Code
- `-nostdinc` is active -- no `<stdint.h>`, `<string.h>`, or any angle-bracket headers
- Use `#include "kernel/types.h"` for all integer types and `size_t`
- No `malloc()` → use `kmalloc()` (≤ 4 KB) or `pmm_alloc_contiguous()` (larger)
- No `printf()` → use `printk()` or `klog()`
- Any new mutable shared state must be SMP-safe from the start: use per-CPU data, atomics, or the correct spinlock variant
- Do not add platform-specific test workarounds like "accept both WHPX and correct values"; diagnose the root cause instead

### Assembly
- NASM syntax, x86-64 Long Mode, UEFI-era / APIC environment
- No BIOS interrupts, no VGA text mode, no PIC assumptions

### API Surface
- Win32 is the native API -- POSIX only via Linux compat layer
- Use Windows-style paths: `C:\Impossible\System32\` with backslashes

### Safety
- Never run real-device disk commands (`dd`, `mkfs`, `/dev/sd*`) without explicit approval
- Stop and ask before security changes, destructive operations, ABI changes, dependency additions, or large refactors

### Style
- ASCII only in source, comments, docs, and scripts; do not use Unicode en/em dashes
- Prefer rewriting prose over using `--` as a fake em dash

### Test Code Safety
- Tests under `src/kernel/test/test_*.c` must **not** call live boot infrastructure:
  - `boot_progress(`, `boot_post_write16(`, `boot_post_nvram_write16(`, `post_display16(`
  - `vpd_stage_*(`, `vpd_init(`
  - `boot_splash_*(`
  - `boot_halt(`, `panic(`, `KeBugCheckEx(`
  - subsystem re-init calls like `pmm_init(`, `vmm_init(`, `heap_init(`, `serial_init(`, `acpi_init(`, `lapic_init(`, `ioapic_init(`, `timer_hal_init(`, `gdt_init(`, `idt_init(`
- Allowed test patterns:
  - pure constant checks
  - save/restore wrappers around `kernel_subsystem_set_ready()` / `kernel_subsystem_ready()`
  - pure data helpers like `boot_timing_record_step()`
  - wrappers around `BOOT_REQUIRE` / `BOOT_STEP`
  - read-only oracle queries like `boot_timing_get_steps()`

### Include Style
```c
// CORRECT
#include "kernel/types.h"
#include "kernel/mm/pmm.h"

// WRONG -- stripped by -nostdinc
#include <stdint.h>
```

## Naming Conventions

| Element | Convention | Example |
|---------|-----------|---------|
| Functions | `snake_case` | `pmm_alloc_frame()` |
| Variables | `snake_case` | `frame_count` |
| Macros/Constants | `UPPER_CASE` | `PAGE_SIZE` |
| Types/Structs | `snake_case_t` | `task_t` |

## Toolchain

- Compiler: `clang-19 --target=x86_64-elf -ffreestanding -nostdinc -nostdlib`
- Assembler: `nasm`
- Linker: `ld.lld-19`
- Crash debug: `llvm-addr2line-19 -e build/kernel.exe -f <RIP>`

## Bare-Metal and ABI Rules

- Bare metal is the target; QEMU and WHPX are convenience platforms, not the acceptance criteria
- `struct boot_info` at `0x10000` is the handoff ABI between `BOOTX64.EFI` and `kernel.exe`
- If `BOOT_INFO_VERSION` changes, update both:
  - `include/kernel/boot_info.h`
  - `src/boot/uefi/bootx64.c`
- Rebuild both bootloader and kernel together after ABI changes with `bash scripts/build.sh`
- Use `vmm_map_mmio_uc()` for device MMIO and `vmm_map_mmio_wc()` for framebuffer VRAM
- `msr_try_read()` is a no-crash safeguard, not proof that an MSR exists; gate on CPUID/features first
- CR3 reloads on WHPX can reset per-vCPU MSRs like PAT; re-program critical per-CPU MSRs after TLB-flushing page-table changes
- Deferred init must run inline in Phase 3; do **not** move it to background threads
- The user ELF range `0x800000--0x900000` is a cross-file contract; keep `include/kernel/mm/user_range.h`, `vmm.c`, `pmm.c`, `task.c`, and `user/user.ld` in sync

## Copilot Workflow in This Repo

- Prefer the project skills under `.claude/skills/` when they match the task
- Mandatory domain quality checks:
  - `src/boot/` -> `boot-code-quality`
  - `src/kernel/`, `include/kernel/` -> `kernel-code-quality`
  - `src/desktop/` -> `desktop-code-quality`
  - `src/shell/` -> `shell-code-quality`
  - `user/`, `src/apps/` -> `userland-code-quality`
- When implementing a TODO section, use the repo workflow skill instead of doing an ad hoc implementation
- When a Codex/rubber-duck review returns findings, verify each one against the cited code before fixing anything; do not blindly trust the reviewer

## Documentation Sync

- When code or conventions change, update related docs in the same task:
  - `CLAUDE.md`
  - relevant files in `.claude/skills/`
  - affected TODO files under `todo/`
  - this `.github/copilot-instructions.md` when the guidance for Copilot changes

## Repository Structure

Key directories:
- `src/kernel/` -- kernel core
- `src/boot/uefi/` -- UEFI bootloader
- `include/` -- all headers
- `todo/` -- development roadmap
- `.cursor/` -- Cursor AI rules and skills
- `.claude/` -- Claude Code AI skills
