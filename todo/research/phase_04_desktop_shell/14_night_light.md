# 87 — Night Light / Blue Light Filter

## Overview
Reduce blue light emission in the evening by shifting display colors toward warm tones. Like Windows Night Light or f.lux.

## How it works
- Apply a color transform matrix to the compositor output
- Shift blue channel down, boost red/yellow
- Gradually transition over 30 minutes (sunset → full warm)

```c
void nightlight_apply(gfx_surface_t *surface, uint8_t intensity) {
    /* intensity: 0 = off, 100 = max warmth */
    float r_boost = 1.0f;
    float b_reduce = 1.0f - (intensity / 100.0f) * 0.5f; /* reduce blue by up to 50% */
    /* Apply per-pixel in compositor's final blit */
}
```

## Schedule: auto on/off based on time (e.g., 9 PM → 7 AM) or manual toggle
## Codex: `System\Display\NightLight = 1`, `...\NightLightIntensity = 50`, `...\NightLightStart = "21:00"`
## Files: `src/kernel/display/nightlight.c` (~80 lines) | 1-2 days
