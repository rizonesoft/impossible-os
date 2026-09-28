<!-- docs: covers=todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md sources=src/kernel/drivers/serial.c,include/kernel/drivers/serial.h,src/kernel/idt.c,include/kernel/idt.h,src/kernel/except.c,include/kernel/except.h reviewed=2026-09-28 order=29 -->
# Kernel Debugger (KD Protocol)

## What is it?

The kernel debugger (KD) is a planned WinDbg-compatible debug stub that would let a second machine attach over serial and control a live kernel: set breakpoints, read and write memory and registers, single-step, and inspect a crash without a reboot-and-guess cycle. The stub itself does not exist yet. What does exist is the serial driver it would talk through and an exception-dispatch hook shaped for it; this page describes both and links the open roadmap sections.

## How does it work?

[`serial.c`](../../src/kernel/drivers/serial.c) is a polling COM driver: `serial_putchar()`/`serial_write()` transmit, `serial_trygetchar()` reads one byte without blocking, and an abort-safe emergency write path underneath them serves panic-time output. There is no receive interrupt, no receive ring buffer and no packet framing. `serial_init()` programs the UART with divisor 0, which keeps whatever baud rate the firmware already set rather than forcing one, and `serial_adopt_boot_info()` later adopts the bootloader-reported I/O port but deliberately not a reported baud rate, leaving the line-rate decision to the roadmap's serial-line section.

The exception path already has the debugger slot NT puts there. [`except.h`](../../include/kernel/except.h) declares `KI_DEBUG_ROUTINE`, a callback called first-chance and second-chance for kernel-mode exceptions, and `ki_set_debug_routine()`, which publishes or detaches it with an atomic release store. In [`except.c`](../../src/kernel/except.c) the kernel-mode order is: debugger first chance, then the kernel `__try`/`__except` walk in `ki_raise_kernel_exception()`, then debugger second chance, then `KeBugCheckEx`. The callback defaults to NULL and only the exception unit tests register one, so today the notification is skipped and an unhandled kernel fault goes straight on to the bugcheck. `#DB` (vector 1) and `#BP` (vector 3) are already routed: `except_init()` installs `except_common_handler` on them through `idt_register_handler()` ([`idt.c`](../../src/kernel/idt.c)), mapping them to `STATUS_SINGLE_STEP` and `STATUS_BREAKPOINT`, so a kernel `INT3` enters exception dispatch and can be caught by kernel SEH. What is missing is a debugger behind them: with no callback registered, an unhandled breakpoint goes on to the bugcheck.

There is no `src/kernel/kd/` directory, no KD packet type, no connection state and no breakpoint table anywhere in the tree.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `serial_putchar()`, `serial_write()`, `serial_trygetchar()` | COM transmit and single-byte poll receive ([`serial.h`](../../include/kernel/drivers/serial.h)) |
| `serial_init()`, `serial_adopt_boot_info()` | Early UART bring-up and post-handoff port (not baud) adoption |
| `idt_register_handler()`, `idt_register_handler_quiet()` | Generic IDT vector hook installation ([`idt.h`](../../include/kernel/idt.h)) |
| `KI_DEBUG_ROUTINE`, `ki_set_debug_routine()` | The first-chance and second-chance kernel debugger callback slot, NULL by default ([`except.h`](../../include/kernel/except.h)) |
| `ki_dispatch_exception()`, `ki_raise_kernel_exception()` | The fault dispatcher that consults the slot, and the kernel SEH walk it falls through to |

## How do I use it?

There is nothing to attach to yet: no `kddebug=` boot argument, no `HKLM\SYSTEM\KernelDebugger` key and no `kd_init()` exist, so neither WinDbg nor any other debugger can connect. Debugging today means the serial log and the panic screen described in [Panic Screen and Crash Experience](panic-screen-crash-experience.md), plus unit tests of the dispatch path:

```bash
bash scripts/test.sh SUITE=except   # exception dispatch and kernel SEH suites
```

## What is not implemented yet?

- **A fixed 115200-baud line.** `serial_init()` keeps the firmware's divisor ([Serial Line Setup: 115200 Baud, 8N1, FIFO](../../todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md#1-serial-line-setup-115200-baud-8n1-fifo)).
- **Interrupt-driven receive and a KD port selector.** `serial_trygetchar()` is the only receive path ([IRQ-Driven RX Ring Buffer and KD COM Port Selection](../../todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md#2-irq-driven-rx-ring-buffer-and-kd-com-port-selection)).
- **KD packet framing and the connection handshake.** No packet type, checksum, ACK/RESEND or break-in detection exists ([KD Packet Framing: Send / Receive, Checksum, ACK/RESEND](../../todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md#3-kd-packet-framing-send--receive-checksum-ackresend), [KD Connection Handshake & Breakin Detection](../../todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md#4-kd-connection-handshake--breakin-detection)).
- **Handing `#DB`/`#BP` to a debugger, and AP freeze.** Both vectors reach exception dispatch, but only the unit tests call `ki_set_debug_routine()` and no code freezes the other CPUs while a debugger holds the machine ([`#DB` / `#BP` Exception Routing to KD, AP Freeze/Thaw](../../todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md#5-db--bp-exception-routing-to-kd-ap-freezethaw)).
- **The debugger commands.** Context get and set, memory access, breakpoints, single-step, the module list and I/O or MSR access over the wire are all unbuilt, starting with [Context Get / Set](../../todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md#6-context-get--set).
- **`kd_break()` and F12 break-in.** No target-side break-in exists ([`kd_break()`, Keyboard F12 Breakin & QEMU Guide](../../todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md#13-kd_break-keyboard-f12-breakin--qemu-guide)).
- **Debug syscalls.** No `NtCreateDebugObject` or related services are registered ([Debug Syscalls Wired to SSDT](../../todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md#14-debug-syscalls-wired-to-ssdt)).
- **Transport and coexistence policy.** Network and USB transports and the policy for the panic screen and idle loop are not decided ([KD Transport Roadmap, SMP Policy, and Coexistence](../../todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md#15-kd-transport-roadmap-smp-policy-and-coexistence)).

## How does it compare with Windows 11 and Linux?

Windows ships WinDbg over KD on serial, network (KDNET) and USB, with breakpoints, register and memory access and a module list; Linux offers the same through kgdb (a GDB remote stub) and the kdb shell. Impossible OS has neither today. It has already matched one structural piece of Windows: the kernel exception dispatcher gives a debugger first-chance and second-chance notification exactly where NT's `KiDebugRoutine` sits, so the stub has a defined place to plug in. The roadmap targets WinDbg's wire protocol rather than GDB's.

## See also

- [Kernel Debugger (KD Protocol) roadmap](../../todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md)
- [Exception Dispatch and SEH](exception-dispatch-seh.md)
- [Panic Screen and Crash Experience](panic-screen-crash-experience.md)
- [Native API and the SSDT](native-api-ssdt.md)
