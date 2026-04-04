# 16 Architecture Ports

This domain tracks porting the kernel to new CPU architectures. The x86-64 kernel is the reference implementation; ports extract architecture-specific code behind a HAL (Hardware Abstraction Layer) and provide equivalent implementations for each target.

## Belongs Here

- Architecture abstraction layer (HAL interfaces for interrupts, timers, page tables, syscall entry, SMP bringup, context switch).
- ARM64 (AArch64) port: UEFI AA64 boot, GICv3, generic timer, PSCI SMP, TTBR page tables, PAN/BTI/PAC/MTE.
- Build system multi-arch support (`ARCH=x86_64` / `ARCH=aarch64`).
- Per-architecture inline assembly, register definitions, and feature detection.

## Does Not Belong Here

- x86-64-specific features or fixes. Those stay in their current domains (02/03/04).
- RISC-V or other future architectures until a concrete port begins.

## Likely Source Areas

- `src/kernel/arch/x86_64/` (to be created from current arch-specific files)
- `src/kernel/arch/aarch64/` (new)
- `include/kernel/arch/` (HAL headers)
- `src/boot/uefi/` (AA64 bootloader variant)

## Epics

- None yet.

## Active TODOs

- [TODO-01 Architecture Abstraction Layer](./TODO-01-arch-abstraction-layer.md) -- Extract x86-64-specific code behind HAL interfaces; create `arch/x86_64/` and `arch/aarch64/` source trees; multi-arch build system.
- [TODO-02 AArch64 Kernel Port](./TODO-02-aarch64-kernel-port.md) -- UEFI AA64 boot, exception vectors, GICv3, generic timer, PSCI SMP, TTBR page tables, SVC syscall, NEON/SVE context switch.
- [TODO-03 SMP Scaling & Processor Groups](./TODO-03-smp-scaling-processor-groups.md) -- Scale beyond 16 CPUs: processor groups, PROCESSOR_NUMBER, KeSetTargetProcessorDpcEx, GROUP_AFFINITY, >64 LAPIC MADT parsing.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-arch-abstraction-layer.md` as the filename style for new leaf TODOs in this folder.
