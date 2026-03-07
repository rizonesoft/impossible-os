# Display — Resolution, DPI & Multi-Monitor Research

## Overview

Implement dynamic resolution switching, DPI-aware rendering for high-resolution (4K/HiDPI) screens, and eventually multi-monitor support.

---

## Current State

| Feature | Status | Details |
|---------|--------|---------|
| Resolution | ✅ 1280×720 fixed | Hardcoded in Multiboot2 header + QEMU |
| Framebuffer | ✅ Linear 32bpp BGRA | `fb_get_width()` / `fb_get_height()` |
| DPI | ❌ None | Everything is pixel-based, no scaling |
| Resolution switching | ❌ None | Cannot change at runtime |
| Multi-monitor | ❌ None | Single framebuffer only |

---

## Part 1: Dynamic Resolution

### How it works

At boot, GRUB/Multiboot2 sets the framebuffer resolution. To support other resolutions, we need:

1. **VESA VBE (BIOS)** — Query available modes, switch modes via BIOS calls
2. **UEFI GOP** — Graphics Output Protocol (modern replacement)
3. **virtio-gpu** — Paravirtual GPU for QEMU (supports dynamic resize)
4. **QEMU `-vga` flag** — Select resolution at launch

### Supported resolutions (common)

| Resolution | Aspect | Name | DPI at 24" |
|-----------|--------|------|-----------|
| 1280×720 | 16:9 | 720p (current) | 61 |
| 1366×768 | 16:9 | WXGA | 65 |
| 1440×900 | 16:10 | WXGA+ | 75 |
| 1600×900 | 16:9 | HD+ | 75 |
| 1920×1080 | 16:9 | 1080p / FHD | 92 |
| 1920×1200 | 16:10 | WUXGA | 94 |
| 2560×1440 | 16:9 | 1440p / QHD | 122 |
| 3840×2160 | 16:9 | 4K / UHD | 184 |

### Runtime resolution change

```c
/* display.h */

struct display_mode {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;        /* bits per pixel (32) */
    uint32_t refresh;    /* refresh rate (Hz) */
};

/* Query available modes */
int display_enum_modes(struct display_mode *modes, uint32_t max_modes);

/* Switch to a new mode */
int display_set_mode(uint32_t width, uint32_t height);

/* Get current mode */
struct display_mode display_get_mode(void);
```

### QEMU dynamic resize (via virtio-gpu)

```
# QEMU with virtio-gpu allows the guest to request resolution changes
qemu-system-x86_64 -device virtio-vga-gl -display sdl,gl=on
```

With virtio-gpu, the guest can send `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D` with any dimensions, and QEMU resizes its window. No BIOS calls needed.

### Fallback (Multiboot2)

Without virtio-gpu, we can request different resolutions at boot via the Multiboot2 header:

```nasm
; Multiboot2 framebuffer tag — request preferred resolution
framebuffer_tag:
    dd 5            ; type = framebuffer
    dd 20           ; size
    dd 0            ; flags
    dd 1920         ; preferred width
    dd 1080         ; preferred height
    dd 32           ; preferred bpp
```

The resolution is a *preference* — GRUB picks the closest available mode.

---

## Part 2: DPI Scaling

### The problem

At 1080p on a 24" screen, 96 DPI is fine — a 12px font is readable. At 4K on the same screen, 96 DPI makes everything **tiny** (4× smaller). We need to scale the UI.

### DPI scale factors

| Resolution | Screen size | Physical DPI | Scale factor | Effective DPI |
|-----------|------------|-------------|-------------|---------------|
| 1280×720 | 15" laptop | 98 | 100% (1.0×) | 96 |
| 1920×1080 | 24" monitor | 92 | 100% (1.0×) | 96 |
| 1920×1080 | 15" laptop | 147 | 125% (1.25×) | 120 |
| 2560×1440 | 27" monitor | 109 | 125% (1.25×) | 120 |
| 3840×2160 | 27" monitor | 163 | 150% (1.5×) | 144 |
| 3840×2160 | 15" laptop | 293 | 200% (2.0×) | 192 |

### Scale factor API

```c
/* dpi.h */

/* System scale factor (stored in Codex System\Display\Scale) */
typedef enum {
    DPI_SCALE_100 = 100,  /* 1.0× — 96 DPI */
    DPI_SCALE_125 = 125,  /* 1.25× — 120 DPI */
    DPI_SCALE_150 = 150,  /* 1.5× — 144 DPI */
    DPI_SCALE_175 = 175,  /* 1.75× — 168 DPI */
    DPI_SCALE_200 = 200,  /* 2.0× — 192 DPI */
    DPI_SCALE_250 = 250,  /* 2.5× — 240 DPI */
    DPI_SCALE_300 = 300,  /* 3.0× — 288 DPI */
} dpi_scale_t;

/* Get current DPI scale factor (e.g., 150 for 150%) */
uint32_t dpi_get_scale(void);

/* Scale a pixel value by current DPI */
static inline int dpi_scale(int pixels) {
    return (pixels * dpi_get_scale()) / 100;
}

/* Scale constants */
#define DPI(px)  dpi_scale(px)
```

### How everything uses DPI scaling

```c
/* Before DPI awareness (hardcoded at 96 DPI) */
#define TASKBAR_HEIGHT  48
#define FONT_SIZE       14
#define ICON_SIZE       32
#define CORNER_RADIUS   8

/* After DPI awareness (scales with display) */
#define TASKBAR_HEIGHT  DPI(48)    /* 48 at 100%, 72 at 150%, 96 at 200% */
#define FONT_SIZE       DPI(14)    /* 14 at 100%, 21 at 150%, 28 at 200% */
#define ICON_SIZE       DPI(32)    /* 32 at 100%, 48 at 150%, 64 at 200% */
#define CORNER_RADIUS   DPI(8)     /* 8 at 100%, 12 at 150%, 16 at 200% */
```

### What needs to scale

| Element | How it scales |
|---------|--------------|
| **Font size** | `font_get(FONT_UI, DPI(14))` — renders at scaled pixel size |
| **Taskbar height** | `DPI(48)` → 48px at 100%, 96px at 200% |
| **Window title bar** | `DPI(32)` height |
| **Window borders** | `DPI(1)` thickness |
| **Corner radius** | `DPI(8)` |
| **Icon size** | Load 32px icons, or 48/64px for higher DPI |
| **Cursor size** | 24px at 100%, 48px at 200% |
| **Button padding** | `DPI(8)` horizontal, `DPI(4)` vertical |
| **Scroll bar width** | `DPI(16)` |
| **Menu item height** | `DPI(28)` |
| **Margins / spacing** | All use `DPI(n)` |
| **Mouse hit targets** | Minimum `DPI(32)` for touch/click targets |

### Multi-size icons

At 200% DPI, a 32×32 icon scaled to 64×64 looks blurry. Provide multiple sizes:

```c
/* icon_store.c */
const icon_t *icon_get_scaled(system_icon_t id) {
    uint32_t scale = dpi_get_scale();
    if (scale >= 200) return icon_get_at_size(id, 64);  /* 64×64 */
    if (scale >= 150) return icon_get_at_size(id, 48);  /* 48×48 */
    return icon_get_at_size(id, 32);                     /* 32×32 */
}
```

---

## Part 3: Auto-Detection

### DPI auto-detect from resolution

Since we may not know the physical screen size, use heuristics:

```c
uint32_t dpi_auto_detect(void) {
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    if (w >= 3840)       return DPI_SCALE_200;  /* 4K */
    if (w >= 2560)       return DPI_SCALE_150;  /* 1440p */
    if (w >= 1920)       return DPI_SCALE_100;  /* 1080p */
    return DPI_SCALE_100;                        /* 720p and below */
}
```

User can override in Settings Panel → Display → Scale.

### EDID (real hardware)

On real hardware, the monitor sends **EDID** data (Extended Display Identification Data) containing:
- Physical screen size (cm)
- Supported resolutions
- Preferred resolution and refresh rate

We can read EDID via DDC/I2C from the VGA/HDMI/DP connector. This gives us *actual* physical DPI.

---

## Part 4: Layout System

### Coordinate spaces

```
Logical Pixels (LP)           Physical Pixels (PP)
────────────────────          ────────────────────
DPI-independent units         Actual framebuffer pixels
App thinks in LP              Rendered in PP

LP × (scale/100) = PP

Example at 150% scale:
  Button LP: 100 × 40        Button PP: 150 × 60
  Font LP: 14px               Font PP: 21px
```

### App-level API

Apps should NEVER use raw pixel values. Always use `DPI()`:

```c
/* Good — DPI aware */
gfx_fill_rounded_rect(surface, x, y, DPI(200), DPI(40), DPI(8), color);
font_draw_string(surface, font_get(FONT_UI, DPI(14)), x, y, "Hello", white);

/* Bad — hardcoded pixels, will be tiny at 4K */
gfx_fill_rounded_rect(surface, x, y, 200, 40, 8, color);
font_draw_string(surface, font_get(FONT_UI, 14), x, y, "Hello", white);
```

---

## Part 5: Multi-Monitor (Future)

### Architecture

```
┌──────────────────┐ ┌──────────────────┐
│   Monitor 1      │ │   Monitor 2      │
│   1920×1080      │ │   2560×1440      │
│   100% DPI       │ │   125% DPI       │
│   Primary        │ │   Extended       │
└──────────────────┘ └──────────────────┘
         │                     │
         ▼                     ▼
┌──────────────────────────────────────┐
│     Virtual Desktop (combined)       │
│     4480×1440                        │
│     Windows can span monitors        │
└──────────────────────────────────────┘
```

### Multi-monitor data structures

```c
#define MAX_MONITORS 4

struct monitor {
    uint32_t  id;
    uint32_t  width, height;
    int32_t   x, y;           /* position in virtual desktop */
    uint32_t  dpi_scale;      /* per-monitor DPI */
    uint32_t *framebuffer;    /* framebuffer address */
    uint8_t   primary;        /* is this the primary display? */
};

struct display_manager {
    struct monitor monitors[MAX_MONITORS];
    uint32_t       monitor_count;
    uint32_t       virtual_width;   /* combined width */
    uint32_t       virtual_height;  /* max height */
};
```

### Per-monitor DPI

Windows 10+ supports **per-monitor DPI** — each monitor can have a different scale. When a window moves from one monitor to another, the OS sends a DPI change notification and the app re-renders at the new scale.

```c
/* Notify app when window moves to a monitor with different DPI */
#define WM_DPI_CHANGED  0x02E0

/* App receives: */
void on_dpi_changed(uint32_t new_dpi) {
    /* Re-layout and re-render everything at new DPI */
    app_relayout(new_dpi);
}
```

### QEMU multi-monitor

```
# QEMU supports multiple displays with virtio-gpu
qemu-system-x86_64 \
    -device virtio-vga,max_outputs=2 \
    -display sdl
```

---

## Codex Settings

```ini
[Display]
Width = 1920
Height = 1080
RefreshRate = 60
Scale = 150              ; DPI scale percentage
AutoScale = 1            ; auto-detect from resolution

[Display\Monitor1]
Width = 1920
Height = 1080
X = 0
Y = 0
Scale = 100
Primary = 1

[Display\Monitor2]
Width = 2560
Height = 1440
X = 1920
Y = 0
Scale = 125
Primary = 0
```

---

## Files

| Action | File | Lines (est.) | Purpose |
|--------|------|-------------|---------|
| **[NEW]** | `include/dpi.h` | ~40 | DPI scale types, `DPI()` macro, API |
| **[NEW]** | `src/kernel/display/dpi.c` | ~80 | Scale factor management, auto-detect |
| **[NEW]** | `src/kernel/display/display_mgr.c` | ~200 | Resolution switching, mode enumeration |
| **[MODIFY]** | `include/desktop.h` | ~20 | Replace hardcoded sizes with `DPI()` |
| **[MODIFY]** | `src/desktop/desktop.c` | ~30 | Use `DPI()` for all layout dimensions |
| **[MODIFY]** | `src/kernel/wm.c` | ~20 | DPI-aware window decorations |
| **[MODIFY]** | `include/gfx.h` | ~5 | Add `dpi.h` include |

---

## Implementation Order

### Phase 1: DPI scaling system (3-5 days)
- [ ] `dpi.h` — scale types, `DPI()` macro
- [ ] `dpi.c` — `dpi_get_scale()`, `dpi_auto_detect()`
- [ ] Codex integration (`System\Display\Scale`)
- [ ] Replace all hardcoded sizes in `desktop.h` with `DPI()`
- [ ] Update `desktop.c` and `wm.c` to use `DPI()`

### Phase 2: Dynamic resolution at boot (2-3 days)
- [ ] Multiboot2 header supports resolution preference
- [ ] `display_enum_modes()` — query VESA/VBE modes
- [ ] Settings Panel applet to select resolution (applied on reboot)
- [ ] Test at 1080p, 1440p, 4K in QEMU

### Phase 3: Runtime resolution switch (1-2 weeks)
- [ ] virtio-gpu driver for dynamic resize
- [ ] `display_set_mode()` — change resolution without reboot
- [ ] Re-allocate framebuffer, re-layout desktop
- [ ] Notify all windows of resolution change

### Phase 4: Multi-monitor (future — months)
- [ ] Multi-framebuffer support
- [ ] Virtual desktop coordinate space
- [ ] Per-monitor DPI
- [ ] Window dragging across monitors
- [ ] Monitor arrangement in Settings
