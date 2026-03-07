# System Services & Hardware Drivers

This phase covers the system-level services and hardware drivers that polish
the OS: structured logging, real-time clock, ACPI power management, and PCI
bus enumeration.

## Serial Logging

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/log.c` | Structured serial logging |
| `include/kernel/log.h` | Logging API |

### Design

| Property | Value |
|----------|-------|
| Output | Serial port COM1 (`0x3F8`) only — no framebuffer |
| Format | `[LEVEL][SUBSYS] message` with timestamp |
| Levels | `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` |
| Filtering | Compile-time via `LOG_MIN_LEVEL` |

### API

| Function | Description |
|----------|-------------|
| `log_init()` | Initialize logging subsystem |
| `log_info(subsys, fmt, ...)` | Informational message |
| `log_warn(subsys, fmt, ...)` | Warning |
| `log_error(subsys, fmt, ...)` | Error |

### Host Capture

| Make Target | Serial Output |
|-------------|---------------|
| `make run` | Terminal (stdio) |
| `make run-log` | `serial.log` file (`-serial file:serial.log`) |

Logs go to serial only (not the framebuffer), keeping the graphical desktop
clean while still capturing debug output.

## Real-Time Clock (RTC)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/rtc.c` | CMOS RTC driver |
| `include/kernel/drivers/rtc.h` | RTC API |

### How It Works

The CMOS RTC chip is accessed via I/O ports:
- **Port `0x70`** — register select (index)
- **Port `0x71`** — data read/write

### Registers Read

| Register | Index | Content |
|----------|-------|---------|
| Seconds | `0x00` | 0–59 |
| Minutes | `0x02` | 0–59 |
| Hours | `0x04` | 0–23 (24h) or 1–12 (12h) |
| Day | `0x07` | 1–31 |
| Month | `0x08` | 1–12 |
| Year | `0x09` | 0–99 |
| Status A | `0x0A` | Update-in-progress flag (bit 7) |
| Status B | `0x0B` | BCD/binary mode, 12/24h mode |

### Handling

- **BCD → binary** conversion (most BIOSes use BCD encoding)
- **12h → 24h** conversion when in 12-hour mode
- **Update-in-progress** — waits for bit 7 of Status A to clear before reading
- QEMU runs with `-rtc base=localtime` to match host timezone

### API

| Function | Description |
|----------|-------------|
| `rtc_init()` | Initialize and print current time |
| `rtc_read(time)` | Fill `struct rtc_time` with current date/time |
| `rtc_hours()` / `rtc_minutes()` / `rtc_seconds()` | Quick accessors |

The taskbar clock displays **HH:MM** from `rtc_read()`.

## ACPI Power Management

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/acpi.c` | ACPI table parsing and power management |
| `include/kernel/acpi.h` | ACPI structs (RSDP, RSDT, XSDT, FADT, GAS) |

### Table Parsing Chain

```
RSDP (from Multiboot2 tag)
  │  Physical address in g_boot_info.acpi_rsdp_addr
  ▼
RSDT / XSDT (root table with pointers to other tables)
  │  Checksum validated
  ▼
FADT ("FACP" — Fixed ACPI Description Table)
  │  Contains PM1a_CNT port and DSDT pointer
  ▼
DSDT (Differentiated System Description Table)
     Contains \_S5 sleep type value for shutdown
```

### Shutdown (`acpi_shutdown()`)

1. Parse `\_S5` object from DSDT AML bytecode → extract `SLP_TYPa`
2. Write `(SLP_TYPa << 10) | SLP_EN` to `PM1a_CNT` port
3. Fallback: QEMU port `0x604` with value `0x2000`
4. Fallback: Bochs port `0xB004` with value `0x2000`

### Reboot (`acpi_reboot()`)

1. Try ACPI reset register (FADT 2.0+, I/O or MMIO)
2. Fallback: keyboard controller reset (`outb 0x64, 0xFE`)
3. Fallback: triple fault (null IDT + `int 3`)

## PCI Bus Enumeration

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/pci.c` | PCI bus driver (222 lines) |
| `include/kernel/drivers/pci.h` | PCI API and device struct |

### Configuration Space Access

| Port | Purpose |
|------|---------|
| `0xCF8` | Config address (bus/dev/func/offset) |
| `0xCFC` | Config data (read/write) |

Address format (32-bit):
```
Bit 31:     Enable (always 1)
Bits 23-16: Bus number (0–255)
Bits 15-11: Device number (0–31)
Bits 10-8:  Function number (0–7)
Bits 7-0:   Register offset (4-byte aligned)
```

### API

| Function | Description |
|----------|-------------|
| `pci_scan()` | Enumerate all buses, log found devices |
| `pci_find_device(vendor, device)` | Find device by vendor/device ID |
| `pci_read{8,16,32}(bus, dev, func, off)` | Read config space |
| `pci_write{16,32}(bus, dev, func, off, val)` | Write config space |
| `pci_enable_bus_mastering(dev)` | Enable DMA for a device |

### Device Struct

```c
struct pci_device {
    uint8_t  bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if;
    uint8_t  irq_line;
    uint32_t bar[6];    /* Base Address Registers */
    uint8_t  found;
};
```

### Class Names

The driver identifies common device classes: IDE/SATA controllers, Ethernet,
Display, USB, Host/ISA/PCI bridges, and more.
