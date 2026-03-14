# Boot Splash Fade-In Transition

The boot splash implements a 500ms brightness fade-in when the kernel first takes
over the display from UEFI firmware. This eliminates the abrupt "black screen snap"
that occurs when the firmware logo disappears.

## Implementation

**File:** `src/kernel/boot_splash.c`

### Key Functions

| Function | Purpose |
|---|---|
| `splash_draw_icon_faded(uint8_t fade)` | Renders the 48×48 icon with all RGB channels scaled by `fade/255`. Replaces the original `splash_draw_icon()`. |
| `splash_draw_dots_faded(uint8_t fade)` | Renders the 6 resting dots with brightness scaled by `fade/255`. |

### Fade Loop (inside `boot_splash_init()`)

```c
static const uint8_t fade_levels[5] = { 51, 102, 153, 204, 255 };
for (int fi = 0; fi < 5; fi++) {
    splash_fill_rect(0, 0, scr_w, scr_h, 0x000000);
    splash_draw_icon_faded(fade_levels[fi]);
    splash_draw_dots_faded(fade_levels[fi]);
    fb_swap();
    sleep_ms(100);
}
```

### Timing

| Frame | Brightness | Elapsed |
|---|---|---|
| 1 | 20% (51/255) | 100ms |
| 2 | 40% (102/255) | 200ms |
| 3 | 60% (153/255) | 300ms |
| 4 | 80% (204/255) | 400ms |
| 5 | 100% (255/255) | 500ms |

After the fade completes, `splash_draw_text("Starting...", ...)` is drawn and the
PIT callback is registered to begin the dot wave animation.

## Why This Works Seamlessly

The PIT callback (`splash_timer_callback`) is registered **after** `pit_init()` + `sti`
in `main.c`, which occurs after `boot_splash_init()` returns. The fade loop is therefore
purely synchronous with no IRQ contention — each `sleep_ms(100)` is a busy wait driven
by the PIT tick counter.

## Design Notes

- The dots render **at rest** (all at `intensity=0`) during the fade — they're not
  in the pulse wave yet. This prevents the animation from jumping mid-fade.
- `splash_draw_icon_faded(255)` is mathematically identical to the original
  `splash_draw_icon()` (removed), so there is no quality regression at full brightness.
- The 100ms inter-frame delay gives a smooth perceptual ramp without consuming
  significant boot time (500ms is barely noticeable during normal real-hardware boot).
