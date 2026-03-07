# QEMU & UEFI Testing

Impossible OS boots via **UEFI** (not legacy BIOS). QEMU emulates UEFI using the
**OVMF** firmware. GRUB is the bootloader, loading the kernel via **Multiboot2**.

> **QEMU Version:** 10.2.1 (built from source with GTK display support).
> Upgraded from system QEMU 8.2.2 to resolve VirtIO legacy PIO transport issues.
> Installed at `/usr/local/bin/qemu-system-x86_64`.

## UEFI Firmware (OVMF)

| Property | Value |
|----------|-------|
| Package | `ovmf` v2024.02 |
| Code (read-only) | `/usr/share/OVMF/OVMF_CODE_4M.fd` |
| Variables (writable) | `/usr/share/OVMF/OVMF_VARS_4M.fd` |
| Note | Ubuntu 24.04 uses `_4M` variants (4 MiB flash images) |

> The Makefile copies `OVMF_VARS_4M.fd` to `build/` before each run so that
> UEFI variable writes don't modify the system copy.

## Boot Chain

```
OVMF (UEFI firmware)
  → GRUB (from ISO's EFI partition)
    → Multiboot2 protocol
      → kernel.elf (loaded at 64-bit Long Mode entry)
```

## QEMU Configuration

QEMU is invoked via `make run` with the following flags:

```makefile
QEMU_FLAGS := -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
              -drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
              -cdrom $(ISO_FILE) \
              -drive file=$(BUILD_DIR)/disk.img,format=raw,if=none,id=disk0 \
              -device virtio-blk-pci,drive=disk0 \
              -m 2G \
              -serial stdio \
              -vga none \
              -device VGA,xres=1280,yres=720 \
              -device rtl8139,netdev=net0 \
              -netdev user,id=net0 \
              -device virtio-tablet-pci \
              -rtc base=localtime \
              -no-reboot
```

| Flag | Purpose |
|------|---------|
| `-drive if=pflash ...` | UEFI firmware (code + variables) |
| `-cdrom $(ISO_FILE)` | Boot from the ISO |
| `-drive file=disk.img,...,if=none,id=disk0` | 64 MiB raw disk image (VirtIO backend) |
| `-device virtio-blk-pci,drive=disk0` | VirtIO block device (modern 1.0 transport) |
| `-m 2G` | 2 GiB RAM |
| `-serial stdio` | Serial port (COM1) output to terminal |
| `-vga none -device VGA,xres=1280,yres=720` | Custom resolution framebuffer |
| `-device rtl8139` | RTL8139 NIC for networking |
| `-netdev user` | User-mode networking (NAT, DHCP) |
| `-device virtio-tablet-pci` | Absolute mouse coordinates (no capture) |
| `-rtc base=localtime` | RTC uses host's local timezone |
| `-no-reboot` | Exit QEMU on triple fault instead of rebooting |

## Make Targets

| Target | Description |
|--------|-------------|
| `make all` | Clean build + ISO (auto-increments build number) |
| `make run` | Launch QEMU with serial on stdio |
| `make run-debug` | Launch QEMU paused, waiting for GDB on port 1234 |
| `make run-log` | Launch QEMU with serial output to `serial.log` file |
| `make clean` | Remove all build artifacts |

## Serial Logging

Two modes of serial capture:

| Mode | Command | Serial output goes to |
|------|---------|----------------------|
| Interactive | `make run` | Terminal (stdio) |
| File capture | `make run-log` | `serial.log` in project root |

The kernel's `printk()` writes to both the framebuffer console and COM1 serial.
The `log_info()`/`log_warn()`/`log_error()` functions write to serial only
(no framebuffer output), making them safe to use when the desktop compositor is active.

## Debugging with GDB

```bash
# Terminal 1: Launch QEMU paused
make run-debug

# Terminal 2: Attach GDB
x86_64-elf-gdb build/kernel.elf
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue
```
