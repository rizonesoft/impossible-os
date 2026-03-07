# 64 — Touch & Gesture Support

## Overview

Touchscreen input: tap, swipe, pinch-to-zoom, long press. Required for tablet/convertible devices.

---

## Touch events
```c
/* touch.h */
struct touch_event {
    uint8_t  type;       /* TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP */
    uint8_t  finger_id;  /* 0-9, for multi-touch */
    int32_t  x, y;       /* screen coordinates */
    float    pressure;   /* 0.0-1.0 (if available) */
};

#define TOUCH_DOWN  0
#define TOUCH_MOVE  1
#define TOUCH_UP    2
```

## Gesture recognition
| Gesture | Fingers | Action |
|---------|---------|--------|
| Tap | 1 | Click |
| Double tap | 1 | Double-click |
| Long press | 1 | Right-click |
| Swipe left/right | 1 | Back/forward |
| Swipe from edge | 1 | Open action center |
| Pinch | 2 | Zoom in/out |
| Two-finger scroll | 2 | Scroll |
| Three-finger swipe | 3 | Switch virtual desktop |

## QEMU: `-device usb-tablet` already sends absolute coordinates (we handle this in `virtio_input.c`)
## Real hardware: USB HID touchscreen or I2C touch controller

## Files: `src/kernel/drivers/touch.c` (~200 lines), `src/kernel/gesture.c` (~300 lines)
## Implementation: 1-2 weeks
## Priority: 🟡 Medium-term
