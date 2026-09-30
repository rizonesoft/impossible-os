# Architecture Ports

How the kernel is meant to grow beyond x86-64: an abstraction layer that separates architecture code from the rest, a 64-bit ARM port, and support for machines with more than 16 processors. The kernel runs on x86-64 with up to 16 CPUs today, and none of these roadmaps has started; each page says what exists now and which roadmap section owns the rest.

## Planned

One page per roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document | Topics |
| --- | --- |
| [Architecture Abstraction Layer](arch-abstraction-layer.md) | HAL header, `src/kernel/arch/` split, `ARCH=` build variable; where the x86-64 code lives today |
| [AArch64 Kernel Port](aarch64-kernel-port.md) | `BOOTAA64.EFI`, GICv3, generic timer, PSCI, `SVC`, NEON and SVE, PAN, BTI, PAC and MTE |
| [SMP Scaling and Processor Groups](smp-scaling-processor-groups.md) | The 16-CPU limit, MADT parsing, processor groups, `KeSetTargetProcessorDpcEx`, group affinity |

Longer-range research on the same subjects is in [Future Research](../research/index.md).
