# Kernel

Core kernel internals -- boot chain, memory management, scheduling, and inter-process communication.

*Implementation docs will be added here as kernel TODOs are completed.*

## Subdirectories

| Directory      | Scope                                            | Specs                                                  |
| -------------- | ------------------------------------------------ | ------------------------------------------------------ |
| `boot/`        | UEFI bootloader, boot chain, handoff to kernel   | [UEFI 2.10](../../specs/hardware/firmware/uefi-2.10.md)   |
| `memory/`      | PMM, VMM, heap, swap, mmap                       | --                                                      |
| `scheduler/`   | Threads, synchronization, preemption             | --                                                      |
| `ipc/`         | Pipes, signals, shared memory                    | --                                                      |
