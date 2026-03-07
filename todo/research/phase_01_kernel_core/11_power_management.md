# 02 — Power Management

## Overview

System shutdown, restart, sleep, and ACPI power control. Users need to be able to turn off the OS cleanly.

---

## ACPI Shutdown / Restart

### QEMU shortcut (immediate)
```c
/* QEMU-specific shutdown via I/O port */
void qemu_shutdown(void) { outw(0x604, 0x2000); }
void qemu_restart(void) { outb(0x64, 0xFE); }  /* keyboard controller reset */
```

### Proper ACPI shutdown
1. Parse ACPI tables (RSDP → RSDT → FADT)
2. Read PM1a/PM1b control block addresses from FADT
3. Read S5 sleep state values from DSDT AML
4. Write SLP_TYP | SLP_EN to PM1a control register

### Clean shutdown sequence
```
1. Notify all apps: "save your work" (WM_CLOSE messages)
2. Flush Codex to disk
3. Flush all open files
4. Stop network services
5. Unmount filesystems
6. ACPI shutdown / power off
```

## Power states

| State | Windows | Implementation |
|-------|---------|---------------|
| Shutdown | Power off | ACPI S5 / QEMU port |
| Restart | Reboot | Keyboard controller reset / ACPI |
| Sleep | Suspend to RAM | ACPI S3 (future) |
| Hibernate | Suspend to disk | Save RAM to disk (future) |
| Lock | Lock screen | Show lock screen (future) |

## Files: `src/kernel/acpi/power.c` (~200 lines), `include/power.h` (~30 lines)
## Implementation: 3-5 days
