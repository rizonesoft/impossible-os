---
schema_version: 1
id: aarch64-kernel-port
domain: 16-architecture-ports
status: active
title: "TODO-02 -- AArch64 Kernel Port"
---

# TODO-02 -- AArch64 Kernel Port

> **Goal:** Port the Impossible OS kernel to ARM64 (AArch64). Boot via UEFI AA64, bring up GICv3 interrupt controller, generic timer, PSCI SMP, TTBR page tables, SVC syscall entry, and NEON/SVE context save/restore. When complete, the same kernel source tree builds for both x86-64 and AArch64, boots on QEMU virt machine, and runs the test suite.

> [!IMPORTANT]
> Depends on TODO-01 (arch abstraction layer). The HAL interfaces must exist before implementing ARM64 equivalents.

---

## Inputs

- `include/kernel/arch/hal.h` -- HAL interface from TODO-01
- `src/kernel/arch/x86_64/` -- reference implementation from TODO-01
- -> XREF: `16-architecture-ports/TODO-01-arch-abstraction-layer.md §1-§5` -- prerequisite: HAL + arch split

---

## Outcome

- AArch64 kernel boots on QEMU `-machine virt` with GICv3 and generic timer
- Scheduler runs, context switches work, syscalls via SVC dispatch to SSDT
- Unit test suite passes on QEMU AArch64
- Same `Makefile` with `ARCH=aarch64` builds the ARM64 kernel

---

## Implementation Order

| Star | Order | Deliverable                               | Depends On    | Status |
| ---- | :---: | ----------------------------------------- | ------------- | :----: |
| 💎   |   1   | UEFI AA64 bootloader                      | TODO-01 §1-§5 |  [ ]   |
| 💎   |   2   | Exception vectors + EL1 entry             | §1            |  [ ]   |
| 💎   |   3   | GICv3 interrupt controller                | §2            |  [ ]   |
| 💎   |   4   | ARM generic timer                         | §3            |  [ ]   |
| 💎   |   5   | TTBR page tables (4 KiB granule, 4-level) | §2            |  [ ]   |
| 💎   |   6   | PSCI SMP bringup                          | §3, §5        |  [ ]   |
| 💎   |   7   | SVC syscall entry + SSDT dispatch         | §2            |  [ ]   |
| 💎   |   8   | NEON/SVE context switch                   | §6            |  [ ]   |
| ⭐   |   9   | ARM security: PAN + BTI + PAC + MTE       | §5, §8        |  [ ]   |
| 💎   |  10   | QEMU AArch64 test suite pass              | §1-§8         |  [ ]   |

---

## 1. UEFI AA64 Bootloader

Port `src/boot/uefi/bootx64.c` to produce a `BOOTAA64.EFI` for ARM UEFI systems.

- [ ] Create `src/boot/uefi/bootaa64.c` (or conditionalize bootx64.c with `#ifdef __aarch64__`)
- [ ] PE/COFF machine type: `IMAGE_FILE_MACHINE_ARM64` (0xAA64)
- [ ] Same GOP, boot.conf, memory map, ExitBootServices flow
- [ ] Jump to kernel at EL1 (most UEFI firmware drops to EL1 before boot services)
- [ ] Decide how the ARM kernel discovers hardware: ACPI (as the x86 path does) or the Devicetree QEMU `virt` can pass; record it in the boot_info handoff
- [ ] Commit: `"arch: AArch64 UEFI bootloader (BOOTAA64.EFI)"`

---

## 2. Exception Vectors + EL1 Entry

ARM64 exception handling via VBAR_EL1 vector table.

- [ ] Create `src/kernel/arch/aarch64/exception_vectors.S`
- [ ] 16 vectors (4 types x 4 exception levels): sync, IRQ, FIQ, SError
- [ ] Save x0-x30, SP_EL0, ELR_EL1, SPSR_EL1 on exception entry
- [ ] Route to C handler `void aarch64_exception_handler(struct exception_frame *)`
- [ ] Commit: `"arch: AArch64 exception vectors + EL1 entry"`

---

## 3. GICv3 Interrupt Controller

ARM Generic Interrupt Controller v3 (replacement for LAPIC+IOAPIC).

- [ ] `src/kernel/arch/aarch64/gicv3.c` -- GICD (distributor) + GICR (redistributor) + ICC system registers
- [ ] Configure SPIs (shared peripheral interrupts) for timer, UART, virtio
- [ ] Priority-based masking (equivalent to IRQL)
- [ ] IPI via SGI (software generated interrupt) -- replace LAPIC IPI
- [ ] Commit: `"arch: AArch64 GICv3 interrupt controller"`

---

## 4. ARM Generic Timer

Always-present, always-calibrated timer (no calibration dance like x86).

- [ ] Read `CNTFRQ_EL0` for frequency (firmware-provided, no calibration needed)
- [ ] Configure `CNTP_CTL_EL1` + `CNTP_TVAL_EL1` for periodic tick
- [ ] Route timer interrupt to GICv3 PPI #30
- [ ] Implement `arch_read_timestamp()` via `CNTVCT_EL0`
- [ ] Commit: `"arch: AArch64 generic timer (no calibration needed)"`

---

## 5. TTBR Page Tables

ARM64 4-level page tables with 4 KiB granule (48-bit VA).

- [ ] TTBR0_EL1 for user space, TTBR1_EL1 for kernel space
- [ ] Page table entry format: AP[2:1] (access permissions), UXN, PXN, AF, SH
- [ ] ASID support (16-bit, avoids TLB flush on context switch)
- [ ] `arch_mmu_map_page()` / `arch_mmu_unmap_page()` implementations
- [ ] Commit: `"arch: AArch64 TTBR page tables (4-level, 4 KiB granule, ASID)"`

---

## 6. PSCI SMP Bringup

Power State Coordination Interface -- firmware-assisted CPU bringup.

- [ ] `PSCI_CPU_ON` call via `HVC` or `SMC` instruction
- [ ] One call per AP -- no trampoline needed (firmware handles mode switch)
- [ ] AP entry at EL1 with MMU enabled (firmware sets it up)
- [ ] Much simpler than INIT/SIPI -- ~50 lines vs ~300 for x86
- [ ] Commit: `"arch: AArch64 PSCI SMP bringup"`

---

## 7. SVC Syscall Entry

ARM64 syscall via `SVC #0` instruction -> EL0 to EL1 exception.

- [ ] Synchronous exception vector routes SVC to syscall dispatcher
- [ ] Service number in x8 (ARM64 Linux convention) or x16 (Apple convention) -- decide
- [ ] Arguments in x0-x5 (6 args, matches SSDT handler signature)
- [ ] Return value in x0
- [ ] Commit: `"arch: AArch64 SVC syscall entry + SSDT dispatch"`

---

## 8. NEON/SVE Context Switch

Save/restore SIMD state on context switch.

- [ ] NEON: 32 x 128-bit V registers (v0-v31) + FPCR + FPSR
- [ ] SVE (optional): scalable vector registers (VL-dependent size)
- [ ] Lazy save: only save if task used FPU (trap via CPACR_EL1.FPEN)
- [ ] Commit: `"arch: AArch64 NEON/SVE context save/restore"`

---

## 9. ARM Security Features

> [!TIP]
> ARM64 has several security features that are BETTER than x86-64 equivalents.
> MTE (Memory Tagging Extension) is hardware-assisted KASAN at near-zero cost.
> PAC (Pointer Authentication) makes ROP attacks significantly harder.

- [ ] PAN (Privileged Access Never) -- ARM equivalent of SMAP, prevents kernel from accessing user memory
- [ ] BTI (Branch Target Identification) -- ARM equivalent of CET IBT
- [ ] PAC (Pointer Authentication Codes) -- sign return addresses, no x86 equivalent
- [ ] MTE (Memory Tagging Extension) -- hardware memory safety, no x86 equivalent
- [ ] Commit: `"arch: AArch64 security -- PAN + BTI + PAC + MTE"`

---

## 10. QEMU AArch64 Test Suite

- [ ] `scripts/machines/run-aarch64.ps1` -- QEMU `-machine virt -cpu cortex-a72 -m 2G`. Lives alongside `scripts/machines/run-qemu.ps1` (the x86-64 launcher), not under `scripts/debug/` which is reserved for test-category bat runners.
- [ ] `scripts/debug/kernel/run-all-kernel-tests-aarch64.bat` -- Windows-side wrapper that invokes `scripts/machines/run-aarch64.ps1` with `-TestOnly`, parallel to the existing x86-64 `run-all-kernel-tests.bat` / `run-all-kernel-tests-tcg.bat` / `run-all-kernel-tests-1cpu.bat` family.
- [ ] Boot to serial output, all unit tests pass
- [ ] `ARCH=aarch64 bash scripts/build.sh` -> `=== BUILD OK ===`
- [ ] Commit: `"arch: AArch64 QEMU test runner + full test suite pass"`

---

## OS Comparison

| ⭐  | Feature                     | 🪟 Win11               | 🐧 Linux                    | 🚀 Impossible OS               |
| --- | --------------------------- | ---------------------- | --------------------------- | ------------------------------ |
| 💎  | ARM64 kernel port           | ✅ Windows on ARM      | ✅ arch/arm64/              | ⬜ §1-§8 -- full AArch64 port  |
| 💎  | GICv3 support               | ✅ HAL abstraction     | ✅ irqchip/gic-v3           | ⬜ §3 -- GICv3 driver          |
| 💎  | PSCI SMP                    | ✅ Via firmware        | ✅ drivers/firmware/psci    | ⬜ §6 -- PSCI CPU_ON           |
| ⭐  | MTE integration             | ❌ Not in Windows      | ⚠️ Opt-in KASAN-HW          | ⬜ §9 -- production MTE        |
| ⭐  | PAC for kernel              | ❌ User-mode only      | ✅ Since 5.7                | ⬜ §9 -- kernel PAC            |
| ⭐  | Same test suite both arches | ⚠️ Separate test infra | ⚠️ kselftest varies by arch | ⬜ §10 -- identical test suite |

---

## Verification

- [ ] `ARCH=aarch64 bash scripts/build.sh` -> `=== BUILD OK ===`
- [ ] QEMU AArch64 virt: boot to serial, all tests pass
- [ ] `ARCH=x86_64 bash scripts/build.sh` -> still passes (no regression)
