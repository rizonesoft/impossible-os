# Hardware

CPU architecture, bus protocols, firmware interfaces, and interrupt controllers.

*Implementation docs will be added here as hardware driver TODOs are completed.*

## Subdirectories

| Directory       | Scope                                        | Specs                                                              |
| --------------- | -------------------------------------------- | ------------------------------------------------------------------ |
| `cpu/`          | x86-64 Long Mode, AMD64, Intel SDM           | [Specs → CPU](../../specs/hardware/cpu/)                              |
| `bus/`          | PCI/PCIe enumeration, BAR mapping            | [Specs → Bus](../../specs/hardware/bus/)                              |
| `firmware/`     | ACPI tables, UEFI boot services              | [Specs → Firmware](../../specs/hardware/firmware/)                    |
| `interrupts/`   | APIC, I/O APIC, interrupt routing            | [Specs → Interrupts](../../specs/hardware/interrupts/)                |
