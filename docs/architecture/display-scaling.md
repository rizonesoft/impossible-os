# GOP Mode Enumeration & Display Scaling

Impossible OS uses UEFI's `EFI_GRAPHICS_OUTPUT_PROTOCOL` to auto-detect the native
display resolution at boot. No resolution is hardcoded.

## How It Works

`init_gop()` in `src/boot/uefi/bootx64.c`:

1. Calls `gBS->LocateProtocol()` to obtain the GOP handle
2. Iterates all modes via `gop->QueryMode()` (mode count is `gop->Mode->MaxMode`)
3. Filters to 32bpp packed-pixel formats only:
   - `PixelBlueGreenRedReserved` (BGRA — preferred, matches kernel byte order)
   - `PixelRedGreenBlueReserved` (RGBA — accepted, still produces correct output)
4. Selects the mode with the highest pixel count (`width × height`)
5. Sets the mode via `gop->SetMode()`
6. Logs `[GOP] Mode N: WxH 32bpp selected` to the UEFI console
7. Populates `g_boot_info_ptr->fb` with the actual resolution and framebuffer address

The kernel's `fb_init()` reads `g_boot_info.fb.{width,height,pitch,addr}` — it has no
hardcoded resolution — so it automatically adapts to whatever the bootloader selects.

## Boot Log Output

```
[GOP] Mode 12: 1920x1080 32bpp selected
```

## Resolution Selection Strategy

| Priority | Condition | Result |
|----------|-----------|--------|
| 1st | Highest pixel count among 32bpp modes | Selected |
| Fallback | No 32bpp mode found | Keep firmware's current mode |

On QEMU the firmware typically offers 1280×800 or 1024×768 as the highest mode.
On real hardware the highest mode is usually the panel's native resolution
(1920×1080, 2560×1440, 3840×2160).

## HiDPI Considerations

At 4K (3840×2160), UI elements drawn at 1-pixel sizes become invisible to the user.

**Current approach (1× scaling):**
- Works correctly at all resolutions
- UI elements may be tiny on HiDPI displays

**Future work (not yet implemented):**
- Detect DPI from EDID (accessible via `EFI_EDID_DISCOVERED_PROTOCOL`)
- Apply a UI scale factor (2× for HiDPI, 1× otherwise) in the window manager
- Scale font rendering via `gfx_text.c`

For now, developers testing on 4K should set QEMU to a lower GOP mode by modifying
the OVMF display resolution in the QEMU flags (`-vga std` or firmware settings).

## Pixel Format Note

UEFI firmware may expose modes with `PixelBitMask` or `PixelBltOnly` pixel formats.
These are intentionally excluded — the kernel writes raw 32bpp BGRA to the framebuffer
and requires a direct memory-mapped linear framebuffer.

## PageFlip Compatibility

The VBE page-flip path in `fb_swap()` probes for Bochs VGA (`VBE_DISPI_ID`).  On real
hardware this probe returns 0 and page flipping falls back to a direct `rep movsq` copy
— correct at all resolutions.
