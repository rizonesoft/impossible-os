<!-- docs: covers=todo/09-desktop-shell/TODO-13-explorer-shell-host.md sources=src/kernel/main/boot_desktop.c,src/kernel/main/shell_loader.c,src/desktop/desktop.c,src/desktop/wm.c,src/kernel/main/compositor.c,user/cmd.c reviewed=2026-09-29 order=19 -->
# Explorer Shell Host

## What is it?

On Windows, `explorer.exe` is the shell host: the user-mode process that owns the desktop and taskbar and opens files when you double-click them. This roadmap plans a small Windows-named `explorer.exe` for Impossible OS that starts after the desktop is ready (or starts the installer instead in installer mode), hooks into the taskbar and desktop, opens files through `ShellExecute` and the Registry's file associations, and becomes the default shell with `cmd.exe` as the fallback. It does not replace the [File Manager](file-manager.md). Nothing is implemented yet.

## How does it work?

**Today.** There is no `explorer.exe`, no shell host process and no `ShellExecute`. The desktop and taskbar are part of the kernel, and the only user-mode shell is `cmd.exe`:

1. [`boot_desktop.c`](../../src/kernel/main/boot_desktop.c) initialises the window manager ([`wm.c`](../../src/desktop/wm.c)) and the desktop ([`desktop.c`](../../src/desktop/desktop.c)), which draws the wallpaper, taskbar, Start menu and desktop icons, and reports `DESKTOP_READY`.
2. It opens the terminal window and starts a kernel task that loads `C:\cmd.exe` ([`shell_loader.c`](../../src/kernel/main/shell_loader.c), [`cmd.c`](../../user/cmd.c)).
3. It enables the scheduler and enters `compositor_run()`, the loop that draws every frame and never returns ([`compositor.c`](../../src/kernel/main/compositor.c)).

```mermaid
flowchart LR
    B[boot_desktop] --> W[wm_init + desktop_init]
    W --> R[DESKTOP_READY]
    R --> T[terminal_open]
    T --> S[shell_loader: C:\cmd.exe]
    S --> C[compositor_run loop]
```

That split is the main open question for this roadmap: the plan has `explorer.exe` own desktop and taskbar integration, while today both are kernel code driven by the compositor. The plan also places the binary at `C:\Windows\explorer.exe`, while the system folder everywhere else is `C:\Impossible\System32`.

**Planned design.**

1. **Binary and boot.** `explorer.exe` on the system image, started once the compositor is ready unless installer mode selects the installer.
2. **Shell32 gate.** With a strict gate enabled, boot reports an error in the log if the `shell32` exports `explorer.exe` needs are still missing.
3. **Taskbar and desktop.** Register the shell host window class and receive taskbar events.
4. **Open verb.** `ShellExecute(..., "open", ...)` looks up the file's association in `HKCR` and starts the right program.
5. **Default shell.** The shell is chosen by the `Shell` value under the `WinLogon` Registry key, falling back to `cmd.exe`.

The first milestones avoid COM, `IShellWindows` and web views, using `shell32` stubs and USER32 window calls only.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `shell_loader_func()` starting `C:\cmd.exe` | Shipped |
| `HKU\Default` `Shell` value (`C:\cmd.exe`) | Shipped Registry default, informational |
| `explorer.exe`, shell host window class | Planned |
| `ShellExecute`, `SHGetFolderPath` and other `shell32` exports | Planned in the [Win32 API surface roadmap](../../todo/10-platform-services/TODO-08-win32-api-surface.md) |
| `WinLogon` `Shell` value | Planned |

## How do I use it?

It cannot be used yet. The system boots straight to the kernel desktop with the Command Prompt open.

## What is not implemented yet?

- [Binary layout and boot](../../todo/09-desktop-shell/TODO-13-explorer-shell-host.md#1-binary-layout-and-boot)
- [Shell32 dependency gate](../../todo/09-desktop-shell/TODO-13-explorer-shell-host.md#2-shell32-dependency-gate)
- [Taskbar and desktop integration](../../todo/09-desktop-shell/TODO-13-explorer-shell-host.md#3-taskbar-and-desktop-integration), whose window list comes from the [Taskbar](../graphics/taskbar.md) roadmap
- [ShellExecute open-verb wiring](../../todo/09-desktop-shell/TODO-13-explorer-shell-host.md#4-shellexecute-open-verb-wiring), on [File Associations](file-associations.md)
- [Boot-time explorer default](../../todo/09-desktop-shell/TODO-13-explorer-shell-host.md#5-boot-time-explorer-default)

## How does it compare with Windows 11 and Linux?

On Windows 11, `explorer.exe` is the shell host and the Desktop Window Manager composites the screen; the shell is chosen by the `WinLogon` `Shell` value. On Linux the equivalent depends on the desktop environment, for example `gnome-shell` or `plasmashell`, started by the display manager. Impossible OS has no shell host process: the kernel draws the desktop and starts `cmd.exe`. The plan adds a Windows-named `explorer.exe` gated on the `shell32` exports it needs.

## See also

- [Explorer Shell Host roadmap](../../todo/09-desktop-shell/TODO-13-explorer-shell-host.md)
- [Desktop Shell Today](desktop-shell.md)
- [Desktop Compositor](compositor.md)
- [Kernel Initialization Sequencing](../kernel/kernel-init-sequencing.md)
- [Taskbar](../graphics/taskbar.md)
