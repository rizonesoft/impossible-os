# Platform Services

The shared services that sit above the kernel and below individual apps: running Windows and Linux programs, the developer SDK and compiler, audio, image editing, updates and packages, restore and recovery, screensavers and display settings, accessibility, the installer and ISO, and a set of long-term features such as debuggers and print. Most of this roadmap domain has not started, so these pages say plainly what runs today, which is often a kernel primitive or a vendored library the plan will build on, and link the roadmap section that owns each gap.

## Roadmap Overviews

One page per platform services roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document | Topics |
| --- | --- |
| [Audio System and Media Player](audio-system.md) | Nothing yet; planned audio API, mixer, WAV/MP3/OGG/FLAC decoders, media player, `mmsys.cpl` |
| [Paint](paint-app.md) | The shipped surface, image load and BMP/PNG save calls; planned canvas, tools, undo and file dialogs |
| [System Updates and IPKG Packages](updates-packages.md) | Vendored miniz, SHA-256 and A/B slots; planned update client, `.ipkg` format, installer and uninstaller |
| [System Restore, Recovery and Observability](restore-recovery.md) | Shipped `events.jsonl` log, crash reports and degraded-boot screen; planned restore points, OOBE, F8 recovery |
| [Screensaver, Widgets and Display](screensaver-widgets-display.md) | GOP mode list in `boot_info`; planned screensavers, desktop widgets, display modes, multi-monitor |
| [Accessibility Features](accessibility.md) | Cursor sprites, keyboard latches and pointer injection; planned high contrast, sticky keys, magnifier, `ease.cpl` |
| [Win32 PE Loader](win32-pe-loader.md) | Shipped PE32+ validation, mapping and import binding to SSDT slots; planned relocations, callable imports, user CRT |
| [Win32 API Surface](win32-api-surface.md) | The 14 kernel32 and 96 ntdll export rows and the five-call test shim; planned kernel32, msvcrt, user32, gdi32 |
| [Compiler and SDK](compiler-sdk.md) | The clang-19 ELF user build, `user/libc.a` and one SDK header; planned SDK headers, import libs, TCC on the OS |
| [Linux ELF Compatibility](linux-compat.md) | Shipped ELF loader and SYSV initial stack; planned Linux syscall table, path translation, fds, signals, busybox |
| [Installer and ISO Build](installer-iso.md) | Shipped release image and hybrid ISO scripts, GPT and format primitives; planned installer and OOBE hand-off |
| [Long-Term Features](long-term-features.md) | NT debug hooks and serial output; planned GDB stub, debugger, touch, gamepad, print, TTS, TinyGL, sessions |
| [user32 Export Master Table](user32-exports.md) | 1,114 rows, none callable; how the table maps exports to owning roadmaps |
| [comctl32 Export Master Table](comctl32-exports.md) | 164 rows, none callable; common controls tiers and owners |
| [shell32 Export Master Table](shell32-exports.md) | 482 rows, none callable; path, icon and `ShellExecute` tiers and owners |
