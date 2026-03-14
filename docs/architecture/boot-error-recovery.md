# Boot Error Recovery Screen

When a kernel panic occurs during the boot splash phase, Impossible OS renders a
styled blue error screen instead of hanging or showing garbled output.

## How It Works

`panic_screen()` (`src/kernel/panic.c`) immediately:

1. Calls `if (boot_splash_active()) boot_splash_finish()` — stops the splash animation
2. Disables interrupts (`cli`)
3. Unlocks the compositor (`fb_unlock_compositor()`)
4. Renders the error screen directly to the framebuffer

## Visual Layout

```
█████████████████████████████████████████████
█      [Impossible OS BSOD icon - 128px]    █
█                                           █
█  Your Impossible OS ran into a problem    █
█  and needs to restart.                    █
█                                           █
█  Stop code:    PAGE_FAULT                 █
█  Description:  Null page mapped at 0x10   █
█  Source:       kernel/mm/vmm.c:142        █
█                                           █
█  Error code: 0x000000000000000E           █
█  RIP:        0xFFFFFFFF80012345           █
█                                           █
█  --- Register Dump ---                    █
█  RAX=...  RBX=...  RCX=...               █
█  ...                                      █
█                                           █
█  --- Stack Trace ---                      █
█  #0  0xFFFFFFFF8001ABCD                   █
█  ...                                      █
█                                           █
█  Crash dump saved to                      █
█  C:\Impossible\System\crashdump.log       █
█                                           █
█  [=============================----] 22s  █  ← auto-restart progress bar
█████████████████████████████████████████████
```

**Background color:** `0x003380` (Impossible OS blue — same hue as Windows BSOD)

## Auto-Restart

A 30-second countdown with a progress bar appears at the bottom of the screen.
The countdowns seconds are configurable via the Codex Registry:

```
HKEY_LOCAL_MACHINE\SYSTEM\Recovery\AutoRestart  DWORD  30
```

Set to `0` to disable auto-restart (system halts until reset).

When the countdown expires, restart is attempted via:
1. ACPI reset (I/O port `0xCF9`, value `0x06`)
2. Triple fault fallback (load null IDT, execute `int $0`)

## Crash Dump

A text crash dump is written to `C:\Impossible\System\crashdump.log` if the
VFS partition `C:` is mounted. Contains: description, source file:line, and
a note to check serial output for the full register dump.

## Key Functions

| Function | Location | Purpose |
|---|---|---|
| `panic_screen()` | `panic.c:285` | Main entry point |
| `draw_bsod_icon()` | `panic.c:113` | Renders embedded icon bitmap |  
| `write_crash_dump()` | `panic.c:139` | Writes log to `C:\Impossible\System\crashdump.log` |

## Integration with Boot Splash

```c
/* Stop boot animation dots if still running */
if (boot_splash_active())
    boot_splash_finish();
```

This ensures the framebuffer is clean and the compositor is properly shut down
before the BSOD is drawn, even if the panic occurs mid-fade-in.
