/* ============================================================================
 * hv_input.h — Hyper-V Synthetic HID Input (Keyboard + Mouse)
 *
 * On Hyper-V Gen 2, PS/2 (i8042) is removed. Input is delivered via VMBus:
 *   - Keyboard VSP GUID: F912AD6D-2B17-48EA-BD65-F927A61C7684
 *   - Mouse VSP GUID:    CFA8B69E-5B4A-4CC0-B98B-8BA1A1F3F95A
 *
 * The synthetic HID protocol wraps input reports in HID descriptor frames.
 * Keyboard: sends PS/2-compatible scan codes (set 1) → keyboard_inject_scancode()
 * Mouse:    sends absolute coordinates (0-65535) → mouse_inject_state()
 *
 * Clean-room from Hyper-V TLFS (public spec).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Keyboard VSP GUID: F912AD6D-2B17-48EA-BD65-F927A61C7684 */
#define HV_GUID_KBD_VSP  { 0xF912AD6D, 0x2B17, 0x48EA, \
    { 0xBD, 0x65, 0xF9, 0x27, 0xA6, 0x1C, 0x76, 0x84 } }

/* Mouse VSP GUID: CFA8B69E-5B4A-4CC0-B98B-8BA1A1F3F95A */
#define HV_GUID_MOUSE_VSP  { 0xCFA8B69E, 0x5B4A, 0x4CC0, \
    { 0xB9, 0x8B, 0x8B, 0xA1, 0xA1, 0xF3, 0xF9, 0x5A } }

/* ---- Synthetic HID protocol messages ---- */

#define HV_HID_PROTOCOL_REQUEST     0
#define HV_HID_PROTOCOL_RESPONSE    1
#define HV_HID_INITIAL_DEVICE_INFO  2
#define HV_HID_INITIAL_DEVICE_INFO_ACK 3
#define HV_HID_INPUT_REPORT         4
#define HV_HID_FEATURE_REPORT       5

/* Synthetic HID protocol version */
#define HV_HID_VERSION_WIN10        0x00020000
#define HV_HID_VERSION_WIN8         0x00010000

/* Synthetic HID message header */
struct hv_hid_msg_header {
    uint32_t type;          /* HV_HID_* message type */
    uint32_t size;          /* total message size including header */
} __attribute__((packed));

/* Protocol version request/response */
struct hv_hid_protocol_request {
    struct hv_hid_msg_header header;
    uint32_t version;
} __attribute__((packed));

struct hv_hid_protocol_response {
    struct hv_hid_msg_header header;
    uint32_t version;
    uint32_t accepted;      /* 1 = accepted */
} __attribute__((packed));

/* Input report — contains raw HID data after the header */
struct hv_hid_input_report {
    struct hv_hid_msg_header header;
    uint8_t  data[];        /* variable-length HID report */
} __attribute__((packed));

/* ---- Public API ---- */

/* Initialize Hyper-V synthetic keyboard input.
 * Returns 0 on success, -1 on failure or if not on Hyper-V. */
int hv_kbd_init(void);

/* Initialize Hyper-V synthetic mouse input.
 * Returns 0 on success, -1 on failure or if not on Hyper-V. */
int hv_mouse_init(void);

/* Poll for keyboard input events. Call periodically from main loop.
 * Reads from VMBus ring buffer and injects scan codes. */
void hv_kbd_poll(void);

/* Poll for mouse input events. Call periodically from main loop.
 * Reads from VMBus ring buffer and injects absolute coordinates. */
void hv_mouse_poll(void);

/* Returns 1 if HV keyboard is active (PS/2 not available). */
int hv_kbd_available(void);

/* Returns 1 if HV mouse is active (PS/2 not available). */
int hv_mouse_available(void);
