<!-- docs: covers=todo/10-platform-services/TODO-12-long-term-features.md sources=include/kernel/drivers/serial.h,include/kernel/except.h,include/kernel/nt/service_numbers.h,src/kernel/panic.c,include/kernel/drivers/keyboard.h,include/kernel/drivers/mouse.h,src/libs/PROVENANCE.md reviewed=2026-09-29 order=12 -->
# Long-Term Features

## What is it?

This roadmap collects the power-user and ecosystem features that come after the core OS: a GDB kernel debug stub and a user-mode debugger, developer tools, touch gestures, gamepads with an XInput layer, printing to PDF, text to speech, software OpenGL through a TinyGL port, multi-user sessions with fast switching, opt-in telemetry and parental controls. All eleven sections are P2 to P4 and none has shipped. Each one is meant to be buildable on its own, and none blocks another roadmap.

## How does it work?

**Today.** None of these features exists. The code they would start from:

- **Debugging.** COM1 output through `serial_putchar()` and `serial_write()` ([`serial.h`](../../include/kernel/drivers/serial.h)); the panic path reads the debug registers `DR0` to `DR7` ([`panic.c`](../../src/kernel/panic.c)); and the NT-style hooks a debugger attaches to, `ki_set_debug_routine()` and `DbgkForwardException()` ([`except.h`](../../include/kernel/except.h)), with the service number for `NtDebugActiveProcess` reserved ([`service_numbers.h`](../../include/kernel/nt/service_numbers.h)). Those hooks belong to the WinDbg-compatible [Kernel Debugger (KD Protocol)](../kernel/kernel-debugger-kd.md) roadmap, which overlaps this roadmap's GDB stub and debugger sections.
- **Input.** `keyboard_inject_hid_key()` and `mouse_inject_state()` ([`keyboard.h`](../../include/kernel/drivers/keyboard.h), [`mouse.h`](../../include/kernel/drivers/mouse.h)) and the USB stack; there is no touch or gamepad driver.
- **Shell.** `cmd.exe` has `ps`, `reboot` and `shutdown`; there is no `memmap`, `netstat` or `strace` anywhere.
- **Libraries.** Mbed TLS is vendored and could carry telemetry uploads; SAM, eSpeak NG and TinyGL are not vendored ([`PROVENANCE.md`](../../src/libs/PROVENANCE.md)).

**Planned design.**

1. **GDB stub.** The GDB Remote Serial Protocol over COM1, so QEMU with `-gdb tcp::1234 -S` or a second machine can set breakpoints, read memory and step the kernel.
2. **User-mode debugger.** `debugger.exe` with hardware breakpoints through `DR0` to `DR7`, software breakpoints by `INT3` patching, and debug-register system calls.
3. **Developer tools.** An F12 debug console overlay, off by default, with kernel log, memory, network and system call tabs and `ps`, `memmap`, `netstat` and `strace <pid>` commands, plus an FPS overlay.
4. **Touch and gamepads.** Touch points and a gesture recogniser (tap, swipe, pinch) delivered as window messages; gamepad polling and XInput stubs.
5. **Print.** A PDF writer and a print dialog, with printer drivers left to the hardware roadmaps.
6. **Text to speech.** SAM first, then eSpeak NG, behind a `tts_speak()` call.
7. **Software OpenGL.** TinyGL as `opengl32.dll` for simple 3D programs.
8. **Sessions, telemetry, parental controls.** Per-user sessions with their own compositor surface and fast switching; telemetry that is off by default (`HKLM\SYSTEM\Privacy\Telemetry = 0`); and time and app limits in `parcon.cpl`.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `serial_write()`, debug register reads in the panic path | Shipped |
| `ki_set_debug_routine()`, `DbgkForwardException()` | Shipped hooks; the debugger behind them is planned |
| GDB stub, `debugger.exe`, `SYS_DEBUG_*` calls | Planned |
| Touch, gesture, gamepad and XInput calls | Planned |
| `pdf_*`, `tts_*`, `opengl32.dll`, sessions, `telemetry_*`, `parcon.cpl` | Planned |

## How do I use it?

Nothing here can be used yet. For kernel debugging today, read the serial log and the panic screen; see [Panic Screen and Crash Experience](../kernel/panic-screen-crash-experience.md).

## What is not implemented yet?

- [Kernel Debugger (GDB RSP Stub)](../../todo/10-platform-services/TODO-12-long-term-features.md#1-kernel-debugger-gdb-rsp-stub-opus) and [User-Mode Debugger](../../todo/10-platform-services/TODO-12-long-term-features.md#2-user-mode-debugger-debuggerexe-opus); settle the overlap with the [KD protocol roadmap](../../todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md) first
- [Developer Tools](../../todo/10-platform-services/TODO-12-long-term-features.md#3-developer-tools-sonnet)
- [Touch Input and Gestures](../../todo/10-platform-services/TODO-12-long-term-features.md#4-touch-input--gesture-recognizer-opus) and [Gamepad and XInput](../../todo/10-platform-services/TODO-12-long-term-features.md#5-gamepad--controller-input--xinput-stubs-sonnet), whose drivers belong to [Game Controllers and Haptics](../hardware/game-controllers.md) and [I2C and Precision Touchpad](../hardware/i2c-touchpad.md)
- [Print Support](../../todo/10-platform-services/TODO-12-long-term-features.md#6-print-support-sonnet), with printer drivers in [Printing and Scanning Device Path](../hardware/printing-scanning.md)
- [Text to Speech](../../todo/10-platform-services/TODO-12-long-term-features.md#7-text-to-speech-sam--espeak-ng-sonnet); the roadmap records eSpeak NG as LGPL, and its licence file must be checked before it is vendored
- [Software OpenGL](../../todo/10-platform-services/TODO-12-long-term-features.md#8-software-opengl-tinygl-port-opus)
- [Multi-User Sessions](../../todo/10-platform-services/TODO-12-long-term-features.md#9-multi-user-session-management--fast-switching-opus), which need user accounts from [Security and User Accounts](../desktop/security-accounts.md)
- [Telemetry](../../todo/10-platform-services/TODO-12-long-term-features.md#10-telemetry-opt-in-anonymous-sonnet) and [Parental Controls](../../todo/10-platform-services/TODO-12-long-term-features.md#11-parental-controls-parconcpl-sonnet)

## How does it compare with Windows 11 and Linux?

Windows 11 covers every row: WinDbg, touch and pen messages, XInput, Microsoft Print to PDF, Narrator, the WARP software renderer, fast user switching, diagnostic data settings and Family Safety. Linux has KGDB and GDB, libinput, evdev and SDL for gamepads, CUPS, eSpeak NG, Mesa's llvmpipe, multi-seat logins and distribution-specific telemetry. The Impossible OS plan's two distinctive items are a kernel GDB stub and user debugger that share one debug-register interface, and an in-kernel debug console. Nothing is built yet.

## See also

- [Long-Term Features roadmap](../../todo/10-platform-services/TODO-12-long-term-features.md)
- [Kernel Debugger (KD Protocol)](../kernel/kernel-debugger-kd.md)
- [Exception Dispatch and SEH](../kernel/exception-dispatch-seh.md)
- [Game Controllers](../hardware/game-controllers.md)
- [Printing and Scanning Device Path](../hardware/printing-scanning.md)
