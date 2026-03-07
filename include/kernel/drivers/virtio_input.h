/* ============================================================================
 * virtio_input.h — VirtIO Input device driver (tablet mode)
 *
 * Provides absolute mouse coordinates via VirtIO PCI, bypassing the
 * PS/2 mouse grab mechanism.  Used with QEMU's -device virtio-tablet-pci.
 *
 * References:
 *   - VirtIO spec 1.1 §5.8 (Input Device)
 *   - Linux evdev event types/codes
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Linux evdev event types ---- */
#define EV_SYN   0x00   /* Synchronization event */
#define EV_KEY   0x01   /* Key/button event */
#define EV_REL   0x02   /* Relative axis */
#define EV_ABS   0x03   /* Absolute axis */

/* ---- Absolute axis codes ---- */
#define ABS_X    0x00
#define ABS_Y    0x01

/* ---- Button codes ---- */
#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112

/* ---- Synchronization codes ---- */
#define SYN_REPORT  0x00

/* ---- VirtIO input event (8 bytes, matches Linux input_event) ---- */
struct virtio_input_event {
    uint16_t type;   /* EV_* */
    uint16_t code;   /* ABS_*, BTN_*, etc. */
    uint32_t value;  /* axis value or 0/1 for buttons */
} __attribute__((packed));

/* ---- VirtIO input state ---- */
struct virtio_input_state {
    int32_t  x;         /* Absolute X (scaled to screen coords) */
    int32_t  y;         /* Absolute Y (scaled to screen coords) */
    uint8_t  buttons;   /* Button state (same as mouse.h masks) */
};

/* Initialize the VirtIO input driver.
 * Returns 0 on success, -1 if device not found. */
int virtio_input_init(void);

/* Get the current input state (absolute coordinates + buttons).
 * Call from the compositor loop. */
struct virtio_input_state virtio_input_get_state(void);

/* Returns 1 if the VirtIO input device is active */
uint8_t virtio_input_available(void);
