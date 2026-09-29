<!-- docs: covers=todo/09-desktop-shell/TODO-12-utilities.md sources=user/cmd.c,user/sysinfo/sysinfo.c,src/kernel/sched/syscall.c,include/kernel/sched/task.h,include/kernel/drivers/pci.h,include/libc/math.h,include/kernel/image.h,src/desktop/desktop.c reviewed=2026-09-29 order=18 -->
# Task Manager, Device Manager and Core Utilities

## What is it?

This roadmap plans the everyday tools: more shell commands (`cd`, `mkdir`, `copy`-style file commands and output redirection), a Calculator, an Image Viewer, a Win+Shift+S screenshot tool, an Archive Manager for ZIP files, a Calendar, Task Manager, Device Manager and a System Information window. None of its nine sections has shipped and none of the nine programs exists. What runs today is the command shell, `cmd.exe`, with a fixed set of built-in commands.

## How does it work?

**Today: the shell.** [`cmd.c`](../../user/cmd.c) is a user-mode `cmd.exe` with a `C:\>` prompt, a 16-line command history and lines up to 256 characters. There is no tab completion, no output redirection, no pipes and no `&&`. Its built-in commands are:

| Command | What it does |
| --- | --- |
| `help`, `?` | Lists the commands |
| `echo`, `clear` or `cls`, `uname`, `version` or `ver` | Basic output |
| `ls` or `dir` | Lists the root of `C:` |
| `cat` or `type` | Prints a file, up to 4,095 bytes |
| `ps`, `kill <pid>` | Lists processes (ID, state and name) and ends one |
| `sysinfo` | Runs `sysinfo.exe` |
| `uptime`, `ping`, `ifconfig` or `ipconfig` | System uptime and basic networking |
| `reboot`, `shutdown`, `exit` | Power and exit |

There is no `cd`, `pwd`, `mkdir`, `rmdir`, `cp`, `mv`, `rm`, `touch`, `whoami`, `date` or `free`.

**Process list.** `ps` uses the `SYS_GETPROCS` syscall, which returns only an ID, a state and a 32-character name per task ([`syscall.c`](../../src/kernel/sched/syscall.c)). The kernel already keeps more per process: user and kernel time charged by the timer tick, I/O counts and context switches ([`task.h`](../../include/kernel/sched/task.h)). A Task Manager can show CPU use from those counters.

**Other building blocks.** PCI devices are found by `pci_scan()` and looked up with `pci_find_device()`, but there is no call that lists them all ([`pci.h`](../../include/kernel/drivers/pci.h)). Images load, scale and save through `image_load()`, `image_scale()` and `image_save_bmp()` ([`image.h`](../../include/kernel/image.h)). The trigonometric and log functions a scientific calculator needs are in the kernel C library ([`math.h`](../../include/libc/math.h)). `sysinfo.exe` today does only one thing, `sysinfo firmware-updates`, which prints the firmware update advisor report ([`sysinfo.c`](../../user/sysinfo/sysinfo.c)); it is not a system summary.

**Planned design.**

1. **Shell commands.** `cd`, `pwd`, `mkdir`, `rmdir`, `cp`, `mv`, `rm`, `touch`, `whoami`, `date`, `free`, `>` and `>>`, plus `irq list` and `boot-timeline` diagnostics; pipes, tab completion and `&&` as stretch goals.
2. **Calculator.** A 320 by 480 window with a 5 by 4 keypad and memory keys.
3. **Image Viewer.** Fit, zoom and pan, previous and next in the folder, slideshow, set as wallpaper.
4. **Screenshot.** Win+Shift+S dims the screen for a rubber-band selection, with a countdown option and a notification.
5. **Archive Manager.** Browse a `.zip`, Extract All, add files, create new.
6. **Calendar.** A month grid with events stored in the Registry.
7. **Task Manager.** Processes and Performance tabs, End Task, Ctrl+Shift+Esc.
8. **Device Manager.** A tree of PCI devices by category with a properties pane.
9. **System Information.** A three-tab summary that can be exported to text.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `cmd.exe` built-ins listed above | Shipped |
| `SYS_GETPROCS`, `SYS_KILL` | Shipped: ID, state and name only |
| `pci_scan()`, `pci_find_device()` | Shipped; no list-all call |
| `image_load()`, `image_scale()`, `image_save_bmp()` | Shipped |
| `sysinfo firmware-updates` | Shipped |
| New shell commands, the eight programs | Planned |

## How do I use it?

Open the terminal and type `help` to see the commands. `ps` lists running tasks; `kill <pid>` ends one. `sysinfo firmware-updates` shows the firmware advisor report.

## What is not implemented yet?

- [Shell Command Expansion](../../todo/09-desktop-shell/TODO-12-utilities.md#1-shell-command-expansion-sonnet)
- [Calculator](../../todo/09-desktop-shell/TODO-12-utilities.md#2-calculator-sonnet) and [Image Viewer](../../todo/09-desktop-shell/TODO-12-utilities.md#3-image-viewer-sonnet)
- [Screenshot Enhancements](../../todo/09-desktop-shell/TODO-12-utilities.md#4-screenshot-enhancements-sonnet), on the capture owned by [Desktop Shell Features](../graphics/desktop-shell-features.md)
- [Archive Manager](../../todo/09-desktop-shell/TODO-12-utilities.md#5-archive-manager-sonnet), on the ZIP API in [Recycle Bin, ZIP and Task Scheduler](recycle-bin-zip-scheduler.md)
- [Calendar App](../../todo/09-desktop-shell/TODO-12-utilities.md#6-calendar-app-sonnet)
- [Task Manager](../../todo/09-desktop-shell/TODO-12-utilities.md#7-task-manager-sonnet)
- [Device Manager](../../todo/09-desktop-shell/TODO-12-utilities.md#8-device-manager-sonnet)
- [System Info](../../todo/09-desktop-shell/TODO-12-utilities.md#9-system-info-sonnet)

## How does it compare with Windows 11 and Linux?

Windows 11 ships Task Manager, Device Manager, a Calculator with scientific and programmer modes, Photos, the Snipping Tool, ZIP support in File Explorer, Calendar and `msinfo32`, and its command prompt and PowerShell have the full set of file commands. Linux desktops have GNOME System Monitor or `htop`, `lspci` and `lshw`, GNOME Calculator, Eye of GNOME, Flameshot, File Roller or Ark, GNOME Calendar and `inxi`, and shells with full redirection and pipes. Impossible OS has a small shell today. The plan's differences are a screenshot overlay drawn directly by the in-kernel compositor and CPU use taken from the scheduler's own counters.

## See also

- [Task Manager, Device Manager and Core Utilities roadmap](../../todo/09-desktop-shell/TODO-12-utilities.md)
- [Terminal](terminal.md)
- [Process Model Extensions](../kernel/process-model-extensions.md)
- [Resource Accounting and Quotas](../kernel/kernel-resource-accounting-quotas.md)
- [Control Panel and Settings](control-panel.md)
