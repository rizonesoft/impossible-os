# Kernel

Core kernel internals — boot chain, memory management, scheduling, and inter-process communication.

## Documents

| Document | Topics |
|---|---|
| [Init Sequencing](init-sequencing.md) | 4-phase boot, dependency gates, readiness oracle, POST codes, recovery UI |

## Subdirectories

| Directory      | Scope                                            | Specs                                                  |
| -------------- | ------------------------------------------------ | ------------------------------------------------------ |
| `boot/`        | UEFI bootloader, boot chain, handoff to kernel   | [UEFI 2.10](../specs/hardware/firmware/uefi-2.10.md)   |
| `memory/`      | PMM, VMM, heap, swap, mmap                       | —                                                      |
| `scheduler/`   | Threads, synchronization, preemption             | —                                                      |
| `ipc/`         | Pipes, signals, shared memory                    | —                                                      |
