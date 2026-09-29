# Desktop

The desktop foundation: the window manager, the compositor that paints the screen, keyboard and mouse routing, the basic control library, the desktop itself (wallpaper, icons, taskbar and Start menu), and the desktop shell: its services (clipboard, file associations, services, search, accounts, crypto) and its built-in programs. Most of this roadmap domain is superseded by the richer plans in [Graphics](../graphics/index.md), so these pages describe what runs today and point at the owning plan for everything else. The authoritative visual specification is the [design system](../design/index.md).

## Roadmap Overviews

One page per desktop foundation roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document | Topics |
| --- | --- |
| [Window Management Basics](window-management.md) | Window table, dragging, caption buttons and Alt+F4 today; minimize, resize, snap and hotkeys owned elsewhere |
| [Desktop Compositor](compositor.md) | The full-repaint compositor loop, partial presents, frame timing; planned damage tracking and VSync |
| [Keyboard and Mouse Input](input-system.md) | Today's direct key routing and merged pointer sources; planned modifiers, key events, Tab focus and hotkeys |
| [Control Library](control-library.md) | Buttons, labels, text boxes, scroll bars and the Control Gallery; new controls owned by the widget roadmaps |
| [Desktop Shell Today](desktop-shell.md) | Wallpaper, taskbar, clock and the current Start menu; Windows 11 shell owned by the graphics roadmaps |
| [Desktop Icons](desktop-icons.md) | Three static icons and the icon store today; planned grid, special folders, shortcuts and file-type icons |

## Desktop Shell Roadmaps

One page per desktop shell roadmap file: the shell services, the built-in programs and the desktop test harness.

| Document | Topics |
| --- | --- |
| [Clipboard](clipboard.md) | Nothing yet; planned kernel clipboard, Ctrl+C/X/V routing, Win32 API, Win+V history |
| [File Associations, Shortcuts and System Resources](file-associations.md) | File-type icons and the empty `HKCR` today; planned associations, Open With, `.lnk`, sounds, fonts |
| [Service Manager and Core Daemons](service-manager.md) | The default user in the environment today; planned services, restart, `sc`, autostart, sysinfo calls |
| [Recycle Bin, ZIP and Task Scheduler](recycle-bin-zip-scheduler.md) | Vendored but unbuilt miniz; planned Recycle Bin, ZIP API and commands, scheduler, `at` |
| [File Search and Indexing](file-search.md) | Directory listing only today; planned kernel index, ranked query, `find`, Start and Explorer search |
| [Security and User Accounts](security-accounts.md) | Shipped CSPRNG, Argon2 and tokens; planned accounts, sign-in, lock screen, permissions, elevation |
| [CNG Crypto and Certificate Store](cng-crypto.md) | Shipped hashes, Monocypher and Ed25519 verify; planned AES, RSA, X.509, BCrypt, NCrypt, signing, EFS |
| [Terminal](terminal.md) | The 80 by 20 Command Prompt today; planned cell grid, ANSI parser, scrollback, tabs, ConPTY |
| [File Manager](file-manager.md) | Nothing yet; planned Windows 11-style File Explorer |
| [Notepad](notepad.md) | Nothing yet; planned gap-buffer editor with undo, find and replace |
| [Control Panel and Settings](control-panel.md) | Unwired Settings button today; planned CPL host, applets and search |
| [Task Manager, Device Manager and Core Utilities](utilities.md) | The `cmd.exe` built-ins today; planned shell commands and eight utility programs |
| [Explorer Shell Host](explorer-shell-host.md) | The kernel desktop and `cmd.exe` today; planned `explorer.exe` and `ShellExecute` |
| [Desktop Test Late-Phase Harness](desktop-test-late-phase.md) | Shipped FrameStats file, monitor socket and shared-session regression; planned live-desktop tests |
