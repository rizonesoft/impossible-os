# 65 — Gamepad / Joystick Support

## Overview

Game controller input via USB HID. Supports Xbox-style gamepads, joysticks, and D-pads.

---

## Input events
```c
/* gamepad.h */
struct gamepad_state {
    /* Buttons (bitfield) */
    uint16_t buttons;  /* A, B, X, Y, LB, RB, Start, Select, L3, R3 */

    /* Analog sticks (-32768 to +32767) */
    int16_t  left_x, left_y;
    int16_t  right_x, right_y;

    /* Triggers (0-255) */
    uint8_t  left_trigger;
    uint8_t  right_trigger;

    /* D-pad */
    uint8_t  dpad;  /* DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT */
};

#define BTN_A       (1 << 0)
#define BTN_B       (1 << 1)
#define BTN_X       (1 << 2)
#define BTN_Y       (1 << 3)
#define BTN_LB      (1 << 4)
#define BTN_RB      (1 << 5)
#define BTN_START   (1 << 6)
#define BTN_SELECT  (1 << 7)

int gamepad_count(void);
int gamepad_poll(int index, struct gamepad_state *state);
```

## QEMU: No direct gamepad emulation. Use USB passthrough: `-device usb-host,vendorid=0x045e,productid=0x028e` (Xbox 360)
## Depends on: `25_usb_support.md` (USB HID driver)

## Files: `src/kernel/drivers/usb/usb_gamepad.c` (~200 lines)
## Implementation: 3-5 days (after USB HID works)
## Priority: 🔴 Long-term / Nice-to-have
