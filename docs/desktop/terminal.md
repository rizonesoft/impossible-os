<!-- docs: covers=todo/09-desktop-shell/TODO-08-terminal.md sources=src/desktop/terminal.c,include/desktop/terminal.h,src/desktop/desktop.c,src/kernel/main/shell_loader.c,src/kernel/drivers/keyboard.c,src/kernel/sched/syscall.c,src/kernel/main/compositor.c,src/kernel/main/boot_desktop.c reviewed=2026-09-29 order=14 -->
# Terminal

## What is it?

The terminal is the window that hosts the command shell. Today it is a fixed 80 by 20 character Command Prompt window drawn by the kernel. This roadmap replaces it with a real terminal emulator: a Unicode cell grid with 2,000 rows of scrollback, TrueType text with colours and attributes, an ANSI and VT100 escape parser, mouse selection with copy and paste, resizing, Registry settings, tabs and split panes, and a Windows pseudo-console (ConPTY) interface. None of its eight sections has shipped.

## How does it work?

**Today.** [`terminal.c`](../../src/desktop/terminal.c) keeps a static 80 by 20 array of ASCII characters with no colour or attributes ([`terminal.h`](../../include/desktop/terminal.h)).

- **Window.** `terminal_open()` creates a 656 by 328 pixel window titled "Command Prompt" at (50, 30). The window can be resized, but the grid never reflows. Opening it while it is already open does nothing.
- **Output.** User programs write through `sys_write`, which passes each byte to `terminal_putchar()` while the terminal is open ([`syscall.c`](../../src/kernel/sched/syscall.c)). Newline, carriage return, backspace and tab (to the next 4-column stop) are handled; only printable ASCII 32 to 126 is shown. Everything else, including the escape character, is dropped, so an ANSI colour sequence prints as literal text such as `[1;32m`. At the bottom the screen scrolls up and the top line is lost; there is no scrollback.
- **Drawing.** `terminal_render()` redraws every cell with the 8 by 16 bitmap font on light grey (`0xFFCCCCCC`) over near-black (`0xFF0C0C0C`), and draws the cursor as a solid block that hides the character beneath it. The compositor calls it once per full frame, when the terminal's contents changed and no window is being dragged ([`compositor.c`](../../src/kernel/main/compositor.c)).
- **Input.** The keyboard driver passes keys to `terminal_key_input()` whenever the terminal is open, without checking which window has focus ([`keyboard.c`](../../src/kernel/drivers/keyboard.c)). Keys go into a 256-byte ring guarded by a spinlock, and `sys_read` takes them from it. Ctrl+C is not queued: it sends SIGINT to the foreground program.
- **Launch.** Boot opens the terminal and starts `C:\cmd.exe` through `shell_loader_func()` ([`boot_desktop.c`](../../src/kernel/main/boot_desktop.c), [`shell_loader.c`](../../src/kernel/main/shell_loader.c)). Start > Terminal does the same ([`desktop.c`](../../src/desktop/desktop.c)); it starts another `cmd.exe` even when the terminal is already open, and both then read the same input ring.
- **Test hooks.** `terminal_get_buffer()`, `terminal_buffer_contains()` and a forced-open test mode let the desktop test suite inspect the screen ([Desktop and UI Test Framework](../infrastructure/desktop-ui-test-framework.md)). The rewrite has to keep them working.

**Planned design.**

1. **Cell grid.** 200 columns by 2,000 rows of `terminal_cell_t` (a Unicode code point, foreground, background and attribute flags) in page-allocated memory, with a pipe pair to the shell instead of the shared ring.
2. **Rendering.** TrueType monospace text, bold, italic, underline and inverse attributes, and a blinking cursor.
3. **ANSI parser.** SGR colours, cursor movement, erase and save and restore cursor.
4. **Scrollback and selection.** Mouse wheel and scroll bar, drag selection, Ctrl+Shift+C and Ctrl+Shift+V.
5. **Resize.** Reflow on window resize and SIGWINCH to the shell.
6. **Settings.** Font, cursor style, opacity and profiles under `HKCU\...\Terminal`.
7. **Advanced.** Tabs, acrylic background, split panes and clickable links.
8. **ConPTY.** `CreatePseudoConsole` and related calls for Win32 console programs.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `terminal_open()`, `terminal_close()`, `terminal_putchar()`, `terminal_puts()`, `terminal_render()` | Shipped: 80 by 20 ASCII |
| `terminal_key_input()`, `terminal_trygetchar()` | Shipped: shared 256-byte input ring |
| `terminal_get_buffer()`, `terminal_buffer_contains()` | Shipped test hooks |
| `terminal_resize()`, ANSI parser, scrollback, tabs, `CreatePseudoConsole` | Planned |

## How do I use it?

The terminal opens at boot with the `C:\>` prompt. Type shell commands such as `help`, `dir`, `ps` or `ipconfig`. Do not close the window: closing it does not reset the terminal's handle, so Start > Terminal cannot open a new one until the next boot.

## What is not implemented yet?

- [Terminal Cell Grid](../../todo/09-desktop-shell/TODO-08-terminal.md#1-terminal-cell-grid-opus), which also owns starting one shell per terminal instead of a new `cmd.exe` on every Start > Terminal click
- [Character Rendering](../../todo/09-desktop-shell/TODO-08-terminal.md#2-character-rendering-sonnet) and [ANSI Escape Parser](../../todo/09-desktop-shell/TODO-08-terminal.md#3-ansi-escape-parser-sonnet)
- [Scrollback and Text Selection](../../todo/09-desktop-shell/TODO-08-terminal.md#4-scrollback--text-selection-sonnet), on the [Clipboard](clipboard.md)
- [Resize Handling](../../todo/09-desktop-shell/TODO-08-terminal.md#5-resize-handling-sonnet) and [Registry Settings](../../todo/09-desktop-shell/TODO-08-terminal.md#6-registry-settings-sonnet)
- [Advanced Features](../../todo/09-desktop-shell/TODO-08-terminal.md#7-advanced-features-sonnet) and [Pseudo-Console (ConPTY)](../../todo/09-desktop-shell/TODO-08-terminal.md#8-pseudo-console-conpty)

Keeping the terminal handle valid after its window is closed, and routing keys only to the focused window, are owned by the [input system roadmap](../../todo/06-desktop-foundation/TODO-03-input-system.md); see [Keyboard and Mouse Input](input-system.md).

## How does it compare with Windows 11 and Linux?

Windows Terminal renders full Unicode with unlimited scrollback, VT sequences including Sixel images, ConPTY for console programs, a `settings.json` file, tabs, panes and acrylic. GNOME Terminal and Alacritty give near-complete VT220 with 256 colours, long scrollback, primary selection and PTY resize signals, with tmux for splits. Impossible OS has a plain 80 by 20 ASCII window today. The plan targets 16-colour SGR, 2,000 configurable rows, Registry settings that apply without a restart, and acrylic drawn by the kernel's own blur.

## See also

- [Terminal Emulator roadmap](../../todo/09-desktop-shell/TODO-08-terminal.md)
- [Keyboard and Mouse Input](input-system.md)
- [Window Management Basics](window-management.md)
- [Text and Fonts](../graphics/text-fonts.md)
- [Shell design: window chrome](../design/shell.md#window-chrome)
