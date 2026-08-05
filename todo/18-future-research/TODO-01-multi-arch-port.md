---
schema_version: 1
id: multi-arch-port
domain: 18-future-research
status: active
title: "TODO-01 -- ARM64 & RISC-V Architecture Port"
---

# TODO-01 -- ARM64 & RISC-V Architecture Port

> **Goal:** Research spike to scope the effort of porting Impossible OS to AArch64
> (ARM64) and RISC-V (RV64GC) -- audit every x86-64-specific construct, write a minimal
> AArch64 proof-of-concept kernel, define the multi-arch layer, assess RISC-V delta,
> plan the build system changes, document ABI implications, and produce an effort
> estimate before committing to a full port.

> [!IMPORTANT]
> This is a **research and planning spike** only. §7 (research deliverables) is the
> primary output: a `docs/architecture/multi-arch-port-plan.md` document with effort
> estimates, dependency trees, and recommended port order. No changes to the main kernel
> tree are made during the research phase -- the §2 minimal AArch64 kernel prototype
> lives in a throwaway branch (`arch/arm64-spike`) only.
>
> **SIMD / SSE in the kernel**: the main build uses `-mno-sse -mno-sse2 -mno-mmx` so
> SIMD intrinsics must already be isolated to GFX paths only; the gap analysis (§1)
> should confirm or contradict this.
>
> **Subsequent implementation TODOs** (actual porting work) are NOT created here;
> §7 produces the plan that will seed those future TODOs once approved.

---

## Inputs

- `src/boot/entry.asm` + `src/kernel/smp/ap_trampoline.asm` -- x86-specific assembly entry, real-mode → long-mode trampoline, SIPI sequence
- `src/kernel/idt.c`, `src/kernel/irq.c` -- IDT, IRETQ, LAPIC/IOAPIC, `__asm__ volatile` interrupt handling
- `src/kernel/main.c` -- LSTAR/STAR/SFMASK MSR writes for syscall setup; `RDRAND`; `CPUID`
- `src/kernel/boot_timing.c` -- TSC reads via `RDTSC`
- `src/kernel/smp/smp.c` -- APIC-based SMP bringup (INIT-SIPI-SIPI sequence)
- `src/kernel/gfx/gfx_simd.c`, `src/kernel/gfx/gfx_text.c` -- SSE2/AVX paths (confirm scope with build flags)
- `src/kernel/panic.c`, `src/kernel/hw_dump.c` -- x86 register names (RAX, RBX, …, RIP, RFLAGS)
- `src/kernel/drivers/pit.c`, `src/kernel/drivers/pic.c` -- x86 port I/O (`inb`/`outb`), PIT, i8259 PIC
- `src/boot/linker.ld`, `scripts/build.sh` -- linker script and compiler flags (`-mcmodel=kernel`, `-mno-red-zone`, target triple `x86_64-elf`) -- §5 multi-arch build changes here
- `include/kernel/types.h`, `include/kernel/boot_info.h` -- check for `uint64_t` / struct layout assumptions
- `02-kernel-core/TODO-09-x86-64-architecture.md` (→ XREF) -- x86-64 hardware spec reference for the gap analysis
- `01-boot-platform/TODO-09-cpu-boot-sequencing.md §4` (→ XREF) -- hypervisor detection (`HV_TSC_ENLIGHTENMENT`); ARM64 hypervisors use a different SMCCC-based detection path
- `02-kernel-core/TODO-10-kernel-security-hardening.md` (→ XREF) -- SMEP/SMAP/CET are x86 features; AArch64 equivalents are PAN/UAO/BTI/PAC
- `TODO-06-android-app-compatibility.md` (→ XREF) -- TODO-06 section 6 native ARM APK stacks prefer AArch64 guest; Android compat research feeds ABI matrix in multi-arch plan

---

## Outcome

A `docs/architecture/multi-arch-port-plan.md` document that answers: how many lines
of code need to change, what the `src/arch/` abstraction layer looks like, how long the
ARM64 port would take, and whether RISC-V is worth pursuing in parallel. A minimal
AArch64 "Hello via PL011 UART" kernel boots under `qemu-system-aarch64` as proof that
the toolchain and UEFI boot path work.

---

## Implementation Order

| Step | Section                                  | 💎/⭐ | Dependency                               |
| ---- | ---------------------------------------- | --- | ---------------------------------------- |
| 1    | x86-64 gap analysis (audit + LOC delta estimate) | ⭐   | All arch-specific source files above     |
| 2    | Build system multi-arch (`ARCH=arm64/riscv64`) | ⭐   | §1 file inventory; `scripts/build.sh`; `linker.ld` |
| 3    | AArch64 boot path (minimal UART kernel prototype) | ⭐   | §2 build system; `clang-19 --target=aarch64-elf` |
| 4    | RISC-V gap analysis                      | ⭐   | §1 methodology; §3 ARM64 prototype for comparison |
| 5    | ABI considerations (AAPCS64 vs Win64 MSABI) | ⭐   | §3 prototype; Win32 ABI docs             |
| 6    | AArch64 port plan (`src/arch/arm64/` layer design) | ⭐   | §1 §2 §3 §5                              |
| 7    | Research deliverables (`multi-arch-port-plan.md`) | ⭐   | §1–§6 complete                           |

---

## 1. x86-64 Gap Analysis `[Opus]`

> Novel: first comprehensive ISA audit of the codebase. Requires mapping x86-64
> hardware constructs to AArch64/RISC-V equivalents across assembly, MSRs, interrupt
> architecture, and timer subsystems.

- [ ] **Audit methodology**: for each source file, scan for x86-specific constructs using:
  ```bash
  rg -n "(__asm__|asm volatile|LSTAR|SFMASK|STAR\b|wrmsr|rdmsr|rdrand|cpuid|iretq|sysret|swapgs|hlt\b|cli\b|sti\b|outb\b|inb\b|rdtsc|APIC|lapic|ioapic|HPET|TSC\b|PIT\b|i8259|_mm_|__m128|__m256)" src/ include/ --glob "*.{c,h,asm}"
  ```
  Record each hit in `docs/architecture/x86-arch-inventory.md` table: `File | Line | Construct | Category | AArch64 equivalent | RISC-V equivalent | Effort`

- [ ] **Give `copy_from_user`/`copy_to_user` an arch-neutral home.** `cpu_security.h` is arch-forbidden in `fs`/`ob`/`ipc`/`nt` per kernel-code-quality Gate 7, yet 8 neutral files include it purely for usercopy -- the gate is unenforceable as written
  - Current includers: `nt_syscall.c`, `nt_process.c`, `nt_unicode.c`, `nt_alpc.c`, `nt_misc.c`, `nt_env.c`, `nt_audit.c`, `ob/ob.c`. Operator decision: introduce a neutral `uaccess.h` behind which each arch supplies the primitive, OR amend Gate 7 to name usercopy as a sanctioned exception
  - Whichever way it lands, update the Gate 7 text in `.claude/skills/kernel-code-quality/SKILL.md` in the same change so doc and code agree

- [ ] **Construct categories and ARM64 equivalents**:

| x86-64 construct                         | AArch64 equivalent                       | RISC-V equivalent                        |
| ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| `LSTAR`/`STAR`/`SFMASK` MSRs (syscall)   | `VBAR_EL1` vector table + `svc #0`       | `stvec` CSR + `ecall`                    |
| `IDT` + `IRETQ` (interrupts)             | Exception vector table at `VBAR_EL1` (16 entries × 128 B) | `stvec` trap handler + `sret`            |
| LAPIC + IOAPIC (interrupt controller)    | GIC-400 / GICv3 (GICD + GICC MMIO registers) | PLIC (Platform-Level Interrupt Controller) |
| `INIT`-`SIPI`-`SIPI` (AP bringup)        | PSCI `CPU_ON` (SMC call `0xC4000003`)    | SBI `HSM` extension (`sbi_hart_start`)   |
| `RDTSC` (timer)                          | `CNTVCT_EL0` (counter-timer virtual count) | `rdtime` (CSR `time`)                    |
| HPET/PIT                                 | ARM Generic Timer (`CNTFRQ_EL0` frequency) | CLINT `mtime` register                   |
| `WRMSR`/`RDMSR`                          | `msr`/`mrs` system register instructions | `csrw`/`csrr` instructions               |
| `cli`/`sti`                              | `msr DAIFSet/DAIFClr, #0xF`              | `csrci/csrsi mstatus, MIE`               |
| `hlt`                                    | `wfi` (Wait For Interrupt)               | `wfi`                                    |
| `RDRAND`                                 | `RNDR` system register (ARMv8.5-RNG) or SMCCC TRNG | RISC-V entropy extension (`seed` CSR)    |
| `CPUID`                                  | `MIDR_EL1` + feature registers (`ID_AA64ISAR0_EL1`, etc.) | `misa` CSR + RISC-V priv spec            |
| Port I/O (`inb`/`outb`)                  | No port I/O -- everything MMIO           | No port I/O -- everything MMIO           |
| GDT/TSS                                  | No GDT -- exception levels replace privilege rings | No GDT -- CSR privilege modes            |
| `CR0`/`CR3`/`CR4` (page tables, paging control) | `TTBR0_EL1`/`TTBR1_EL1` + `TCR_EL1` + `SCTLR_EL1` | `satp` CSR (ASID + page table base)      |
| SMEP/SMAP/CET                            | PAN (Privileged Access Never) / UAO / BTI / PAC | not directly equivalent                  |
| SSE2 intrinsics (`_mm_*`)                | NEON (`vld1q_u8`, `vaddq_u8`, etc.)      | Vector extension (RVV 1.0)               |

- [ ] **LOC delta estimate**: count lines in all files containing x86-specific constructs; divide by total kernel LOC; target < 5% arch-specific:
  ```bash
  rg -l "(wrmsr|rdmsr|LSTAR|iretq|APIC|lapic|asm volatile)" src/ include/ | xargs wc -l | tail -1
  wc -l $(find src/ include/ -name "*.c" -o -name "*.h" -o -name "*.asm") | tail -1
  ```
- [ ] **Arch-specific file inventory** (expected high-isolation files -- confirm against audit):
  - **Fully arch-specific** (must be rewritten per-arch): `src/boot/entry.asm`, `src/kernel/smp/ap_trampoline.asm`, `src/kernel/idt.c`, `src/kernel/irq.c`, `src/kernel/drivers/pit.c`, `src/kernel/drivers/pic.c`
  - **Mostly arch-specific** (needs `#ifdef ARCH_X86_64` guards): `src/kernel/main.c` (syscall MSR setup), `src/kernel/smp/smp.c` (APIC SMP), `src/kernel/boot_timing.c` (TSC), `src/kernel/panic.c` (register dump), `src/kernel/hw_dump.c`
  - **Minor arch touch** (one or two `asm volatile` lines only): `src/boot/uefi/bootx64.c` (CPUID, RDRAND), `src/kernel/gfx/gfx_simd.c` (SSE2 -- confirm build flags)
  - **Arch-neutral** (should compile unchanged): `src/kernel/mm/`, `src/kernel/fs/`, `src/kernel/net/`, `src/kernel/ipc/`, `src/kernel/sched/` (except context switch), `src/desktop/`, `src/kernel/gfx/` (except `gfx_simd.c`)

---

## 2. Build System Multi-Arch `[Sonnet]`

**Source:** `scripts/build.sh`; arch-specific linker scripts; `include/arch/`

- [ ] **`ARCH` variable in `scripts/build.sh`**:
  ```bash
  ARCH=${ARCH:-x86_64}  # default; override: ARCH=arm64 bash scripts/build.sh
  case "$ARCH" in
    x86_64)  TARGET="x86_64-elf";  QEMU="qemu-system-x86_64";  ARCH_FLAGS="-mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2";;
    arm64)   TARGET="aarch64-elf"; QEMU="qemu-system-aarch64";  ARCH_FLAGS="-mcmodel=large -mno-red-zone";;
    riscv64) TARGET="riscv64-elf"; QEMU="qemu-system-riscv64";  ARCH_FLAGS="-mcmodel=medany -mabi=lp64d";;
  esac
  CC="clang-19 --target=$TARGET"
  ```
- [ ] **Arch-specific linker scripts**: `src/boot/linker-arm64.ld` and `src/boot/linker-riscv64.ld` -- kernel load address, section layout, arm64 `.text` alignment (4 B minimum, 4 KiB page-aligned sections)
- [ ] **`include/arch/` abstraction header** (research deliverable -- does not change kernel code):
  ```c
  // include/arch/x86_64/arch.h
  #define arch_wfi()           __asm__ volatile("hlt")
  #define arch_disable_irq()   __asm__ volatile("cli")
  #define arch_enable_irq()    __asm__ volatile("sti")
  #define arch_cpu_relax()     __asm__ volatile("pause")
  // include/arch/arm64/arch.h
  #define arch_wfi()           __asm__ volatile("wfi")
  #define arch_disable_irq()   __asm__ volatile("msr daifset, #0xF")
  #define arch_enable_irq()    __asm__ volatile("msr daifclr, #0xF")
  #define arch_cpu_relax()     __asm__ volatile("yield")
  ```
  `include/kernel/arch.h` selects the correct arch header via `#if defined(ARCH_X86_64) / ARCH_ARM64 / ARCH_RISCV64`
- [ ] **CI multi-arch compile matrix** (`.github/workflows/multi-arch-build.yml`): compile for all three arches on every push to `main`; x86_64 builds to bootable image; arm64/riscv64 compiles kernel only (no image -- boot test when port is functional); fail if any arch fails to compile

---

## 3. AArch64 Boot Path (Minimal Kernel Prototype) `[Opus]`

> Novel: first hardware-interface code targeting AArch64. Exception vector table at
> `VBAR_EL1`, `mrs`/`msr` system register access, EL1 setup -- no prior Impossible OS
> AArch64 code. Lives in branch `arch/arm64-spike` only; not merged to main.

**Source:** `arch/arm64-spike/` (throwaway branch, never merged to `main`)

- [ ] **Target**: `qemu-system-aarch64 -machine virt -cpu cortex-a72 -nographic -serial stdio -bios QEMU_EFI.fd -kernel kernel-arm64.elf`; prints `"Impossible OS ARM64 spike OK"` via PL011 UART, then `wfi` loop
- [ ] **UEFI boot path** (identical protocol to x86 -- PE/COFF `BOOTX64.EFI` → `BOOTAA64.EFI`):
  - Compile `src/boot/uefi/bootx64.c` with `--target=aarch64-elf` and AArch64-specific PE header; remove x86 inline asm (CPUID → use `MIDR_EL1` for CPU ID, `RNDR` for random); map same UEFI GOP + ELF loader
  - The bootloader already handles `ExitBootServices()` + page table setup; on ARM64: replace x86 PML4 setup with 4-level AArch64 tables (`TTBR1_EL1`, `TCR_EL1`, 4 KiB granule)
- [ ] **Minimal AArch64 kernel entry** (`arch/arm64-spike/entry.asm` in AArch64 assembly):
  ```asm
  .section .text.entry
  .global _start
  _start:
      ldr  x0, =vbar_table        // load exception vector table
      msr  vbar_el1, x0           // install exception vectors
      msr  spsel, #1              // use SP_EL1 stack pointer
      ldr  x1, =kernel_stack_top
      mov  sp, x1
      bl   kernel_main_arm64      // call C entry point
  1:  wfi
      b    1b
  ```
- [ ] **PL011 UART output** (`arch/arm64-spike/uart_pl011.c`):
  - UART base address `0x09000000` (QEMU `virt` machine PL011)
  - `pl011_putchar(char c)`: poll `*(volatile uint32_t*)(UART_BASE + 0x18) & (1<<5)` (TXFF flag); then `*(volatile uint32_t*)(UART_BASE) = c`; no interrupt-driven IO needed for prototype
- [ ] **Exception vector table** (`arch/arm64-spike/vectors.asm`): 4 groups × 4 entries = 16 entries; each 128 B aligned; all entries initially `b .` (spin) except `SError` which prints `"AArch64 exception!"` via PL011
- [ ] **Page tables** (identity-map first 1 GiB with 2 MiB blocks, same as x86 bootloader): `TTBR1_EL1` → PGD → PUD → PMD with 2 MiB block descriptors; `TCR_EL1`: T1SZ=16, TG1=4K, SH1=Inner Shareable, ORGN1/IRGN1=Write-Back Cacheable
- [ ] **Prototype verification**: `qemu-system-aarch64 -machine virt -cpu cortex-a72 -nographic -serial stdio -kernel kernel-arm64.elf` → serial shows `"Impossible OS ARM64 spike OK"`; QEMU exits cleanly via `wfi` (no crash)

---

## 4. RISC-V (RV64GC) Gap Analysis `[Sonnet]`

> Methodology established by §1; RISC-V has simpler privilege model than AArch64.
> Primary question: is RV64GC worth pursuing given the simpler ecosystem versus ARM64?

- [ ] **RISC-V privilege model mapping**:

| x86-64                | AArch64           | RISC-V RV64                              |
| --------------------- | ----------------- | ---------------------------------------- |
| Ring 0 (kernel)       | EL1               | S-mode (Supervisor)                      |
| Ring 3 (user)         | EL0               | U-mode (User)                            |
| SMM                   | EL3               | M-mode (Machine) -- usually firmware     |
| VM hypervisor         | EL2               | H-extension (Hypervisor)                 |
| `syscall` instruction | `svc #0`          | `ecall`                                  |
| `iret`                | `eret`            | `sret`                                   |
| MSR writes            | `msr` instruction | `csrw` instruction                       |
| APIC                  | GICv3             | PLIC (Platform-Level Interrupt Controller) |
| INIT-SIPI-SIPI        | PSCI `CPU_ON`     | SBI `HSM` `sbi_hart_start`               |
| RDTSC                 | `CNTVCT_EL0`      | `rdtime` pseudo-instruction              |

- [ ] **RISC-V-specific considerations**:
  - No UEFI standard on most RISC-V boards -- use **OpenSBI** (M-mode firmware) + **U-Boot** + kernel or TianoCore EDK2 on supported boards; QEMU `virt` machine supports UEFI via EDK2 RV64 port
  - Page table: Sv48 (4-level, identical concept to x86 PML4 but `satp` register); page size 4 KiB; 2 MiB `megapages` equivalent to x86 huge pages
  - Interrupt architecture: PLIC assigns priorities and enables; CLINT for timer interrupts (hart-local); no MMIO port I/O -- clean slate (easier than x86)
  - **`ecall` ABI** for SBI calls: set `a7` = SBI extension ID, `a6` = function ID, `a0–a5` = args; `ecall`; result in `a0`/`a1`
- [ ] **RISC-V prototype viability check**: can `clang-19 --target=riscv64-elf` compile a minimal kernel that boots under `qemu-system-riscv64 -machine virt -cpu rv64`? Run: `echo 'void _start() { for(;;); }' | clang-19 --target=riscv64-elf -O2 -ffreestanding -nostdlib -x c - -o test.elf && qemu-system-riscv64 -machine virt -cpu rv64 -kernel test.elf -nographic -serial stdio` -- verify no toolchain errors
- [ ] **RISC-V vs ARM64 strategic assessment** (for §7 deliverable):
  - ARM64: Apple Silicon, Snapdragon laptops, Raspberry Pi, AWS Graviton -- large install base
  - RISC-V: StarFive VisionFive2, Milk-V Pioneer, SiFive -- growing but niche; excellent for education
  - Recommendation: ARM64 first; RISC-V as second port sharing the arch abstraction layer

---

## 5. ABI Considerations `[Sonnet]`

> Win32 ABI is architecturally specific. Mapping to ARM64 and RISC-V requires defining
> ABI-conditional headers so the same Win32 API surface works on both.

- [ ] **x86-64 Win64 calling convention** (existing): integer args in `RCX, RDX, R8, R9`; 32 B shadow space; callee-saved `RBX, RBP, RDI, RSI, R12-R15`; `RAX` return; `__attribute__((ms_abi))` in Clang
- [ ] **AArch64 Microsoft ARM64 ABI** (Windows 11 on ARM, same as Impossible OS ARM64): integer args in `X0–X7`; no shadow space; callee-saved `X19–X28, X29 (FP), X30 (LR)`; `X0` return; this is the standard AAPCS64 -- **no `__attribute__` needed on ARM64** as AAPCS64 is already the system ABI
- [ ] **RISC-V LP64D ABI** (`-mabi=lp64d`): integer args in `a0–a7`; callee-saved `s0–s11`; `a0/a1` return
- [ ] **Win32 header macros** (`include/win32/types.h` additions):
  ```c
  #if defined(ARCH_X86_64)
  #  define WINAPI  __attribute__((ms_abi))
  #  define CALLBACK __attribute__((ms_abi))
  #elif defined(ARCH_ARM64)
  #  define WINAPI            // native AAPCS64
  #  define CALLBACK
  #elif defined(ARCH_RISCV64)
  #  define WINAPI            // native LP64D
  #  define CALLBACK
  #endif
  ```
- [ ] **Struct alignment audit**: Win32 structs use packed/aligned fields; check all `#pragma pack` and `__attribute__((packed))` uses in `include/win32/` for ARM64 alignment traps (unaligned 64-bit accesses fault on AArch64 by default unless `SCTLR_EL1.A = 0`)
- [ ] **ARM64 PE32+ binary format**: Microsoft ARM64 PE binaries use `IMAGE_FILE_MACHINE_ARM64 (0xAA64)` in PE header; PE loader (`TODO-07`) needs `#ifdef ARCH_ARM64` branch to set correct machine type and import table RVAs
- [ ] **Win32 varargs ABI**: `va_list` implementation differs between x86-64 (`va_list = char*`) and AArch64 (`va_list = __va_list` with stack/register spill areas); kernel `kprintf` and all variadic functions need `#include <stdarg.h>` equivalent -- audit all `...` function signatures in kernel

---

## 6. AArch64 Port Plan (`src/arch/arm64/` Layer Design) `[Opus]`

> Novel architectural design: defining the arch abstraction boundary and the ARM64
> hardware driver layer. Produces the design section of §7 deliverable, not actual code.

- [ ] **Proposed `src/arch/` directory layout**:
  ```
  src/arch/
  ├── x86_64/
  │   ├── entry.asm              (moved from src/boot/entry.asm)
  │   ├── idt.c                  (moved from src/kernel/idt.c)
  │   ├── irq.c                  (moved from src/kernel/irq.c)
  │   ├── apic.c                 (LAPIC + IOAPIC driver)
  │   ├── timer.c                (PIT + TSC)
  │   ├── smp.c                  (INIT-SIPI-SIPI)
  │   └── syscall.asm            (LSTAR/STAR/SFMASK + sysentry stub)
  ├── arm64/
  │   ├── entry.asm              (VBAR_EL1 install, EL1 stack setup)
  │   ├── vectors.asm            (16-entry exception vector table)
  │   ├── gic.c                  (GIC-400 / GICv3 GICD + GICC driver)
  │   ├── timer.c                (ARM Generic Timer CNTVCT_EL0)
  │   ├── smp.c                  (PSCI CPU_ON via SMC)
  │   └── syscall.asm            (VBAR_EL1 SVC handler stub)
  └── riscv64/
      ├── entry.asm              (stvec setup, S-mode entry)
      ├── plic.c                 (PLIC interrupt controller)
      ├── timer.c                (CLINT mtime + CNTVCT via SBI)
      ├── smp.c                  (SBI HSM sbi_hart_start)
      └── syscall.asm            (stvec trap handler, sret)
  ```
- [ ] **Kernel core refactoring required** (doc only -- no code changes in research phase):
  - `src/kernel/main.c`: extract syscall MSR setup into `arch_syscall_init()` (called by `arch/x86_64/syscall.asm`'s C companion); ARM64 version installs `VBAR_EL1` instead
  - `src/kernel/smp/smp.c`: extract `smp_send_ipi()` into arch function; ARM64 uses GIC SGI (Software Generated Interrupt) instead of APIC IPI
  - `src/kernel/sched/task.c`: context switch is fully arch-specific; `task_context_switch()` is already a naked ASM function -- add `src/arch/arm64/context_switch.asm` (save X19-X28 + LR + SP, restore)
  - `src/kernel/mm/vmm.c`: page table walking uses x86-specific 9-9-9-9-12 index layout; ARM64 is identical concept with `TTBR1_EL1` as root -- minimal changes needed (change `CR3` references to `TTBR1_EL1` write)
- [ ] **GIC (Generic Interrupt Controller) driver plan**: GICD (Distributor) at `0x08000000` + GICC (CPU Interface) at `0x08010000` on QEMU `virt`; init: enable GICD, set priority mask on GICC; routing: SGIs (0–15) for IPI, PPIs (16–31) for timer, SPIs (32–1019) for external IRQs; each arch function maps to `irq_controller_ops_t` vtable (`enable_irq`, `disable_irq`, `send_ipi`, `ack_irq`, `eoi_irq`)
- [ ] **PSCI SMP bringup plan**: `CPU_ON` PSCI call (`0xC4000003`): `x0 = PSCI_CPU_ON`, `x1 = MPIDR` (CPU affinity), `x2 = entry_point_address`, `x3 = context_id`; entry point must be in physical memory; AP starts at EL1 if `PSCI_SECONDARY_CPU_EL2_DISABLE` is set; replaces x86 INIT-SIPI-SIPI trampoline entirely

---

## 7. Research Deliverables `[Sonnet]`

> Output of the entire research spike -- no code merged to main.

- [ ] **`docs/architecture/x86-arch-inventory.md`**: complete table from §1 audit (`File | Line | Construct | Category | ARM64 equivalent | RISC-V equivalent | Effort`); total x86-specific LOC count; % of codebase
- [ ] **`docs/architecture/multi-arch-port-plan.md`** -- the primary deliverable; sections:
  - **Executive summary**: is the port feasible? LOC delta; estimated person-months for ARM64; for RISC-V
  - **Architecture abstraction layer**: proposed `src/arch/` layout from §6; which kernel files need guards vs. which are clean
  - **ARM64 port plan**: ordered work items (bootloader → entry + vectors → GIC driver → Generic Timer → PSCI SMP → context switch → syscall → vmm page tables → GFX NEON); estimated LOC per item; blocking issues
  - **RISC-V port plan**: same structure; note shared abstraction layer elements with ARM64
  - **ABI impact**: summary from §5; Win32 header changes; PE loader extension for `0xAA64`
  - **Build system changes**: from §2; CI matrix plan
  - **QEMU test strategy**: `virt` machine for both ARM64 and RISC-V; no real hardware required in Phase 1; target hardware for Phase 2 (Raspberry Pi 5 for ARM64, StarFive VisionFive2 for RISC-V)
  - **Recommended port order**: ARM64 first (larger market), RISC-V second (shared arch layer already built)
  - **Blocking issues**: list any design decisions that would require significant kernel restructuring (e.g., if context switch is deeply embedded in scheduler, if TSC calibration is baked into timer HAL with no HAL interface)
  - **Effort estimate**: ARM64 port: `[X person-weeks]`; RISC-V second port (incremental): `[Y person-weeks]`; total new file count; revised x86-specific LOC %
- [ ] **AArch64 spike results**: document the §3 prototype experiment -- did `BOOTAA64.EFI` load? did PL011 print? any unexpected blockers? include QEMU command that boots the prototype
- [ ] **Prototype branch archived**: push `arch/arm64-spike` branch to GitHub with a README explaining it is a research spike, not a stable port; open tracking issue "ARM64 port" in GitHub Issues linking to `multi-arch-port-plan.md`

---

## OS Comparison


| ⭐   | Feature                               | 🪟 Win11                              | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | ------------------------------------- | ------------------------------------ | ---------------------------------------- | ---------------------------------------- |
| 💎   | ARM64 (AArch64) port                  | ✅ Windows 11 ARM runs natively       | ✅ Linux ARM64 is tier-1; ships           | ⬜ §3 -- §6; research spike first; ARM64  |
| 💎   | RISC-V port                           | ✅ Windows on RISC-V: announced but   | ✅ Linux RISC-V is tier-2; mainline       | ⬜ §4 -- RV64GC second port; shared arch  |
| ⭐   | Formal arch gap analysis document     | ❌ Not public                         | ✅ Linux `Documentation/arch/` per-arch docs | ⬜ §7 -- `x86-arch-inventory.md` + `multi-arch-port-plan.md` |
| ⭐   | Multi-arch CI compile gate from day 1 | ❌ N/A (Windows is commercial)        | ✅ Linux CI builds on arm64,              | ⬜ §2 -- `multi-arch-build.yml`; compile-only initially, boot test |
| 💎   | Arch abstraction layer                | ✅ Windows HAL; arch-specific drivers | ✅ `arch/` directory per ISA in           | ⬜ §6 -- `src/arch/x86_64/` + `src/arch/arm64/` + `src/arch/riscv64/` |
| 💎   | PSCI / SBI SMP bringup                | ✅ Windows ARM64 uses PSCI            | ✅ Linux uses PSCI + SBI                  | ⬜ §6 -- PSCI `CPU_ON` replacing INIT-SIPI-SIPI; SBI |

Impossible OS's `⭐` advantage: the arch abstraction layer design starts from a greenfield
clean state -- all the x86 hardware cruft (i8259 PIC, PIT, real-mode trampoline) is
already isolated in a handful of files and can move cleanly into `src/arch/x86_64/`
without touching the scheduler, memory manager, filesystem, or networking. The public
gap analysis document (`x86-arch-inventory.md`) gives the community transparency into
exactly how much work remains, something neither Microsoft nor the Linux kernel project
publishes in structured machine-readable form.

---

## Verification

- [ ] **Gap analysis completeness**: `rg "(wrmsr|LSTAR|iretq|APIC|asm volatile)" src/ --glob "*.{c,h}" | wc -l` matches row count in `x86-arch-inventory.md`; no x86-specific construct in `src/kernel/mm/`, `src/kernel/fs/`, `src/kernel/net/` (these should be arch-neutral -- confirm)
- [ ] **Multi-arch compile**: `ARCH=arm64 bash scripts/build.sh` (compile-only, no boot) → `clang-19 --target=aarch64-elf` compiles all arch-neutral files without error; x86-specific files skipped via `#ifdef ARCH_X86_64` guards; `ARCH=riscv64 bash scripts/build.sh` same for RISC-V
- [ ] **AArch64 prototype boots**: `qemu-system-aarch64 -machine virt -cpu cortex-a72 -nographic -serial stdio -kernel arch/arm64-spike/kernel-arm64.elf` → serial output shows `"Impossible OS ARM64 spike OK"`; no QEMU crash
- [ ] **RISC-V toolchain validates**: `clang-19 --target=riscv64-elf -O2 -ffreestanding -nostdlib -x c /dev/null -o /dev/null` exits 0; confirms toolchain available for CI matrix
- [ ] **ABI header compiles**: `include/arch/x86_64/arch.h` + `include/arch/arm64/arch.h` both compile without errors under their respective `--target=` flags; `WINAPI` macro compiles a test Win32 function signature on both arches
- [ ] **Deliverable complete**: `docs/architecture/multi-arch-port-plan.md` exists; contains effort estimate in person-weeks; contains §6 `src/arch/` layout; contains ARM64 + RISC-V ordered work item lists; tracking GitHub Issue "ARM64 port" opened
- [ ] Commit: `"research: ARM64/RISC-V gap analysis, multi-arch build system, AArch64 spike prototype, ABI assessment, port plan doc"`
