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
  → GRUB (from EFI partition, gfxmode=1280x720x32)
    → Multiboot2 protocol (framebuffer tag: 1280×720×32bpp)
      → kernel.elf (loaded at 64-bit Long Mode entry)
```

## Resolution Architecture

The display resolution is controlled by a **three-layer chain**, not by QEMU alone:

| Layer | What it does | Where configured |
|-------|-------------|-----------------|
| **QEMU VGA device** | Allocates VRAM & advertises available modes | `-device VGA,xres=1280,yres=720` in Makefile |
| **GRUB bootloader** | Selects a GOP mode from what VGA offers | `set gfxmode=1280x720x32` in `src/boot/grub.cfg` |
| **Multiboot2 header** | Requests preferred resolution | `dd 1280` / `dd 720` in `src/boot/multiboot2_header.asm` |

The kernel **never sets the resolution** — it reads whatever GRUB configured via `g_boot_info.fb.width/height` in `fb_init()`. To change resolution, update all three locations.

> [!IMPORTANT]
> On high-DPI Windows displays, the QEMU window may appear scaled. This is
> **Windows DPI scaling**, not the OS resolution. Fix via: right-click
> `qemu-system-x86_64.exe` → Properties → Compatibility →
> Change high DPI settings → Override scaling behavior → **Application**.

## QEMU Configuration

QEMU is invoked via `make run` with the following flags:

```makefile
$(QEMU) \
    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
    -drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
    -drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
    -device ich9-ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
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
| `-drive id=disk0,...` | System disk image (GPT: EFI + IXFS partitions) |
| `-device ich9-ahci` + `ide-hd` | AHCI/SATA controller (matches real hardware) |
| `-m 2G` | 2 GiB RAM |
| `-serial stdio` | Serial port (COM1) output to terminal |
| `-vga none -device VGA,xres=1280,yres=720` | Custom resolution framebuffer (Bochs VGA) |
| `-device rtl8139` | RTL8139 NIC for networking |
| `-netdev user` | User-mode networking (NAT, DHCP) |
| `-device virtio-tablet-pci` | Absolute mouse coordinates (see Input section) |
| `-rtc base=localtime` | RTC uses host's local timezone |
| `-no-reboot` | Exit QEMU on triple fault instead of rebooting |

## Mouse Input & Cursor

### Input Device: `virtio-tablet-pci`

Provides **absolute mouse coordinates** — the host cursor position maps directly
to guest coordinates without needing to capture/grab the mouse.

**Why not PS/2 mouse?**
PS/2 provides relative deltas and requires QEMU to "grab" the mouse. Under WSL2's
Wayland compositor (WSLg), the grab does not function correctly — the host
compositor intercepts input, preventing proper mouse tracking inside the guest.

### Double Cursor Issue (WSL2)

When running QEMU inside WSL2, **two cursors are visible**: the host system cursor
and the Impossible OS Adwaita cursor. This is a WSLg limitation — the Wayland
compositor does not allow GTK apps to hide the system cursor.

**Workarounds tried (none work under WSLg):**

| Approach | Result |
|----------|--------|
| `-display gtk,show-cursor=off` | Accepted by QEMU 10.2.1, ignored by WSLg |
| `-display sdl` | Not compiled into this QEMU build |
| `XCURSOR_THEME=Blank` | WSLg ignores per-app cursor themes |
| `unclutter-xfixes` | Not installed; would require system package |

**Solution: Run QEMU natively on Windows** (see below).

### Partial Screen Swap (`fb_swap_rect`)

Cursor-only movements use `fb_swap_rect()` instead of full `fb_swap()` to update
only the cursor region. Key implementation detail:

> [!WARNING]
> `fb_swap_rect()` must write to the **currently displayed** VRAM page when VBE
> page flipping is active. After `fb_swap()` flips to page 1 (y_offset=fb_height),
> `fb_swap_rect()` must also write to page 1, not page 0. Failure to do this causes
> the cursor to appear "stuck" after clicks. See commit `9657db1`.

## Running on Windows (Recommended for Cursor Testing)

Windows-native QEMU properly hides the host cursor with `virtio-tablet-pci`,
eliminating the double cursor issue.

### Setup

1. **Install QEMU for Windows:** [qemu.weilnetz.de/w64/](https://qemu.weilnetz.de/w64/)
2. **Build in WSL2:** `make all` (or `bash scripts/build.sh clean`)
3. **Launch:** Double-click `scripts/run-windows.bat`

The script auto-detects QEMU at `C:\Program Files\qemu\` and copies OVMF
firmware from WSL if needed.

### DPI Fix for High-DPI Monitors

Right-click `C:\Program Files\qemu\qemu-system-x86_64.exe` → **Properties** →
**Compatibility** → **Change high DPI settings** → ✅ **Override high DPI scaling
behavior** → select **Application**.

## Make Targets

| Target | Description |
|--------|-------------|
| `make all` | Full build (kernel + userland + system disk) |
| `make run` | Launch QEMU in WSL2 with serial on stdio |
| `make run-debug` | Launch QEMU paused, waiting for GDB on port 1234 |
| `make run-log` | Launch QEMU with serial output to `serial.log` |
| `make run-test` | Launch with secondary test disk on AHCI port 1 |
| `make clean` | Remove all build artifacts |

## Serial Logging

| Mode | Command | Serial output goes to |
|------|---------|----------------------|
| Interactive | `make run` | Terminal (stdio) |
| File capture | `make run-log` | `serial.log` in project root |

The kernel's `printk()` writes to both the framebuffer console and COM1 serial.
`log_info()`/`log_warn()`/`log_error()` write to serial only (safe when desktop
compositor is active).

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

## Known Gotchas

- **PMM for pixel data.** Never use `kmalloc` for image/pixel buffers. The kernel
  heap is only 2 MiB. Use `pmm_alloc_contiguous()` for anything > a few KB.
  Cursor pixel data, framebuffer back buffers, icon data, and font bitmaps must
  all use PMM. Violating this causes silent heap exhaustion. (See commit `63ef074`)

- **VBE page flip + partial swap.** `fb_swap()` alternates between VRAM page 0
  (y=0) and page 1 (y=720). `fb_swap_rect()` must track `page_current` and write
  to the correct page. (See commit `9657db1`)

- **Cursor loader: single best-fit size.** The Xcur parser loads only ONE size
  variant per cursor (closest to `XCUR_TARGET_SIZE=24`), not all sizes. Loading
  all sizes (5 sizes × 11 cursors = 55 allocations) exhausted the heap.
  (See commit `63ef074`)
