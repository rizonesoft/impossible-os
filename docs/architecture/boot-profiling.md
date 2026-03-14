# Boot Profiling via Serial Log Timestamps

Every `klog()` call in Impossible OS automatically prefixes its serial output
with a high-resolution timestamp derived from the PIT tick counter.

## Format

```
[   1.234] [OK] boot: Boot complete in 1.234s (PIT uptime from interrupt init)
[   0.180] [..] ahci: Found 1 AHCI controller
[   0.000] [..] boot: --- Phase: interrupt controllers & timer ---
```

```
[<sec>.<ms>] <level-prefix> <subsystem>: <message>
```

| Field | Width | Description |
|---|---|---|
| `<sec>` | 4, space-padded | Seconds since `pit_init()` |
| `<ms>` | 3, zero-padded | Milliseconds within that second |
| `<level-prefix>` | 5 | `[..] ` DEBUG, `[OK] ` INFO, `[--] ` WARN, `[!!] ` ERROR, `[**] ` FATAL |

Timestamps show `[   0.000]` for all messages emitted before `pit_init()` (GDT,
IDT, PMM, etc.) since the PIT tick counter is 0 until the timer is started.

## Implementation

**File:** `src/kernel/klog.c` — inside `klog()`, the serial output section.

```c
uint64_t ms  = pit_get_ticks() * 10;  /* PIT @ 100Hz → 1 tick = 10ms */
uint32_t sec = (uint32_t)(ms / 1000);
uint32_t fms = (uint32_t)(ms % 1000);
/* emit '[' + space-padded sec + '.' + zero-padded ms + '] ' */
```

The timestamp is added to **serial output only**. The framebuffer display (which
is for the end-user UI) does not include timestamps.

## Boot Phase Markers

`main.c` emits `klog(LOG_DEBUG, "boot", "--- Phase: <name> ---")` at the start
of each major initialization stage:

| Phase marker | Covers |
|---|---|
| `storage & VFS` | VFS init |
| `interrupt controllers & timer` | GDT, IDT, PIC, PIT, RTC, keyboard, mouse |
| `display & splash` | `fb_init()`, `boot_splash_init()`, fade-in |
| `PCI & network hardware` | PCI scan, RTL8139, net stack, VirtIO, VBox mouse |
| `partition & filesystem mount` | `partition_scan_all()`, `partition_mount_filesystems()` |
| `network (DHCP)` | `dhcp_discover()` |
| `desktop & WM` | `wm_init()`, `desktop_init()`, icon/cursor/font loading |

## Boot Complete Marker

Logged at `LOG_INFO` just before the compositor loop starts:

```
[   X.XXX] [OK] boot: Boot complete in X.XXXs (PIT uptime from interrupt init)
```

## Interpreting the Timestamps

- The jump from `[   0.000]` to meaningful values happens when `pit_init()`
  runs (early in boot, after PMM/VMM/heap).
- A long gap between two lines = the code between them is slow.
- DHCP (`dhcp_discover()`) is typically the longest phase (~300ms) due to
  network round-trip latency.
- Font loading (`boot_font_init(16)`) is typically 20–60ms from disk.

## Viewing Boot Timestamps

Run QEMU with serial to stdio:

```bash
bash scripts/build.sh run
```

Serial output is mixed into stdout. For a clean file capture:

```bash
qemu-system-x86_64 ... -serial file:serial.log
cat serial.log | grep "^\["
```
