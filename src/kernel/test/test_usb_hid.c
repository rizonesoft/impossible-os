/* Unit tests for the USB HID boot-protocol path -- TODO-18.
 *
 * HID device enumeration touches live xHCI MMIO (no QEMU in WSL), so the
 * device-detection path is validated via QEMU `-device usb-kbd`/`usb-mouse`
 * and bare metal. The pure, hardware-independent surface that CAN be unit
 * tested is the EP-context Interval encoding (xhci_hid_interval_encode) and
 * the HID interface classification constants. Report-parsing tests land with
 * the keyboard/mouse driver sections. See the TODO-18 Unit Tests section. */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/drivers/xhci_dev.h"
#include "kernel/drivers/xhci.h"      /* xhci_hid_probe_counts */
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/mouse.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/drivers/usb_input_diag.h"

/* Local dependency-free string compare (no kernel strcmp in the test layer). */
static int diag_streq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* The EP-context Interval field encodes the polling period as 125us*2^Interval.
 * The encoding is speed-dependent (xHCI 6.2.3.6) and is the bare-metal-critical
 * part of interrupt-endpoint setup -- a wrong value mis-times HID polling. */
static void test_usb_hid_interval_encode(void)
{
    /* High/SuperSpeed: bInterval is a microframe exponent -> field = bInterval-1 */
    TEST_ASSERT_EQ(xhci_hid_interval_encode(USB_SPEED_HIGH, 1), 0,
                   "HS bInterval=1 -> Interval 0");
    TEST_ASSERT_EQ(xhci_hid_interval_encode(USB_SPEED_HIGH, 4), 3,
                   "HS bInterval=4 -> Interval 3");
    TEST_ASSERT_EQ(xhci_hid_interval_encode(USB_SPEED_SUPER, 16), 15,
                   "SS bInterval=16 -> Interval 15");
    /* SuperSpeedPlus uses the same microframe-exponent path as HS/SS. */
    TEST_ASSERT_EQ(xhci_hid_interval_encode(USB_SPEED_SUPER_PLUS, 4), 3,
                   "SS+ bInterval=4 -> Interval 3 (exponent path)");

    /* Full/Low speed: bInterval is frames -> field = floor(log2(bInterval))+3 */
    TEST_ASSERT_EQ(xhci_hid_interval_encode(USB_SPEED_FULL, 1), 3,
                   "FS bInterval=1 -> Interval 3");
    TEST_ASSERT_EQ(xhci_hid_interval_encode(USB_SPEED_FULL, 8), 6,
                   "FS bInterval=8 -> Interval 6");
    TEST_ASSERT_EQ(xhci_hid_interval_encode(USB_SPEED_FULL, 10), 6,
                   "FS bInterval=10 -> Interval 6 (QEMU usb-kbd)");
    TEST_ASSERT_EQ(xhci_hid_interval_encode(USB_SPEED_FULL, 16), 7,
                   "FS bInterval=16 -> Interval 7");

    /* Robustness: a zero/out-of-range bInterval must not underflow or exceed 15 */
    TEST_ASSERT_EQ(xhci_hid_interval_encode(USB_SPEED_HIGH, 0), 0,
                   "HS bInterval=0 clamps to Interval 0");
    TEST_ASSERT(xhci_hid_interval_encode(USB_SPEED_FULL, 255) <= 15,
                "FS bInterval=255 stays within the 4-bit-ish field");
}

/* HID boot-protocol keyboard usage -> ASCII via keyboard_inject_hid_key, drained
 * through the shared keyboard buffer (the same path keyboard_trygetchar reads).
 * Runs before the desktop/terminal opens, so injected chars land in kb_buffer.
 * HID modifier byte: bit1 (0x02) = Left Shift. */
static void test_usb_hid_keyboard_inject(void)
{
    char c;
    int drained = 0;

    /* Drain any pending input so we observe only our injected keys. */
    while (keyboard_trygetchar() != 0 && drained < 512) drained++;

    keyboard_inject_hid_key(0x04, 0x00);          /* usage 'a' */
    TEST_ASSERT_EQ(keyboard_trygetchar(), 'a', "HID 0x04 -> 'a'");

    keyboard_inject_hid_key(0x04, 0x02);          /* Shift + 'a' */
    TEST_ASSERT_EQ(keyboard_trygetchar(), 'A', "HID 0x04 + shift -> 'A'");

    keyboard_inject_hid_key(0x05, 0x00);          /* usage 'b' */
    TEST_ASSERT_EQ(keyboard_trygetchar(), 'b', "HID 0x05 -> 'b'");

    keyboard_inject_hid_key(0x1E, 0x02);          /* Shift + '1' -> '!' */
    TEST_ASSERT_EQ(keyboard_trygetchar(), '!', "HID 0x1E + shift -> '!'");

    keyboard_inject_hid_key(0x28, 0x00);          /* Enter -> '\n' */
    TEST_ASSERT_EQ(keyboard_trygetchar(), '\n', "HID 0x28 -> newline");

    /* Out-of-table / error usages must not push a character. */
    keyboard_inject_hid_key(0x01, 0x00);          /* ErrorRollOver */
    keyboard_inject_hid_key(0xFF, 0x00);          /* out of table */
    c = keyboard_trygetchar();
    TEST_ASSERT_EQ(c, 0, "rollover/out-of-table usages push nothing");

    /* Caps Lock (0x39) toggles letter case and emits no character itself. */
    keyboard_inject_hid_key(0x39, 0x00);          /* Caps Lock ON */
    TEST_ASSERT_EQ(keyboard_trygetchar(), 0, "Caps Lock press emits nothing");
    keyboard_inject_hid_key(0x04, 0x00);          /* 'a' with caps -> 'A' */
    TEST_ASSERT_EQ(keyboard_trygetchar(), 'A', "Caps Lock on: 0x04 -> 'A'");
    keyboard_inject_hid_key(0x39, 0x00);          /* Caps Lock OFF */
    keyboard_inject_hid_key(0x04, 0x00);          /* 'a' again -> 'a' */
    TEST_ASSERT_EQ(keyboard_trygetchar(), 'a', "Caps Lock off: 0x04 -> 'a'");
}

/* Boot-protocol mouse report decode (xhci_hid_decode_mouse): [buttons, dx, dy]
 * with dx/dy signed 8-bit and buttons bit0=L/bit1=R/bit2=M (1:1 MOUSE_BTN_*).
 * Pure decode -- the screen clamp + Y orientation live in mouse_update_relative,
 * which needs a live framebuffer and is validated via QEMU usb-mouse. */
static void test_usb_hid_mouse_decode(void)
{
    int32_t dx, dy;
    uint8_t buttons;

    /* TODO-18 spec case: left button, dx=+10, dy=-10 (0xF6). */
    const uint8_t r1[3] = { 0x01, 0x0A, 0xF6 };
    xhci_hid_decode_mouse(r1, &dx, &dy, &buttons);
    TEST_ASSERT_EQ(dx, 10, "mouse dx=0x0A -> +10");
    TEST_ASSERT_EQ(dy, -10, "mouse dy=0xF6 -> -10 (sign-extended)");
    TEST_ASSERT_EQ(buttons, MOUSE_BTN_LEFT, "mouse buttons 0x01 -> left");

    /* Right + middle held, no movement. */
    const uint8_t r2[3] = { 0x06, 0x00, 0x00 };
    xhci_hid_decode_mouse(r2, &dx, &dy, &buttons);
    TEST_ASSERT_EQ(dx, 0, "mouse dx=0 -> 0");
    TEST_ASSERT_EQ(dy, 0, "mouse dy=0 -> 0");
    TEST_ASSERT_EQ(buttons, MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE,
                   "mouse buttons 0x06 -> right|middle");

    /* Extreme signed deltas + all three buttons; reserved/wheel bits masked. */
    const uint8_t r3[3] = { 0xFF, 0x7F, 0x80 };
    xhci_hid_decode_mouse(r3, &dx, &dy, &buttons);
    TEST_ASSERT_EQ(dx, 127, "mouse dx=0x7F -> +127");
    TEST_ASSERT_EQ(dy, -128, "mouse dy=0x80 -> -128");
    TEST_ASSERT_EQ(buttons, MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE,
                   "mouse buttons 0xFF masks to 3 boot buttons");

    /* Negative dx (leftward) + positive dy in one report -- proves dx is signed
     * (a uint8_t regression would break leftward motion but pass the cases
     * above, which only exercise positive dx). dx=-128 boundary. */
    const uint8_t r4[3] = { 0x00, 0x80, 0x7F };
    xhci_hid_decode_mouse(r4, &dx, &dy, &buttons);
    TEST_ASSERT_EQ(dx, -128, "mouse dx=0x80 -> -128 (leftward, signed)");
    TEST_ASSERT_EQ(dy, 127, "mouse dy=0x7F -> +127");
    TEST_ASSERT_EQ(buttons, 0, "mouse buttons 0x00 -> none");

    /* 4-byte report: wheel + any trailing byte must NOT affect dx/dy/buttons
     * (boot protocol is movement + buttons only). */
    const uint8_t r5[5] = { 0x05, 0xFE, 0x02, 0x7F, 0xA5 };
    xhci_hid_decode_mouse(r5, &dx, &dy, &buttons);
    TEST_ASSERT_EQ(dx, -2, "4-byte report dx=0xFE -> -2 (wheel ignored)");
    TEST_ASSERT_EQ(dy, 2, "4-byte report dy=0x02 -> +2");
    TEST_ASSERT_EQ(buttons, MOUSE_BTN_LEFT | MOUSE_BTN_MIDDLE,
                   "4-byte report buttons 0x05 -> left|middle");
}

/* High slots: a real device enumerates from the bottom of devices[]. */
#define TEST_USB_MOUSE_A (MOUSE_USB_SOURCE_COUNT - 1)
#define TEST_USB_MOUSE_B (MOUSE_USB_SOURCE_COUNT - 2)

/* Input source coexistence: a relative source (USB/PS2) and an absolute source
 * (VirtIO/VBox) share one cursor. mouse_merge_absolute is edge-triggered so an
 * unchanged absolute report does not clobber relative deltas, and buttons are
 * OR-merged per source so an idle absolute report cannot release a held button.
 * Runs in Phase 3 before the compositor thread, so mutating the shared cursor
 * state is safe; the real input source overwrites it once the desktop runs. */
static void test_usb_hid_mouse_coexist(void)
{
    struct mouse_state s;

    if (fb_get_width() < 300 || fb_get_height() < 300) {
        TEST_SKIP("framebuffer too small/not ready for cursor merge test");
        return;
    }

    /* These cases assert the published OR, so a button someone is holding on a
     * real device right now would fail them with correct code. */
    s = mouse_get_state();
    if (s.buttons != 0) {
        TEST_SKIP("a real pointer device is holding a button");
        return;
    }

    /* Absolute source positions the cursor. */
    mouse_merge_absolute(200, 150, 0);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.x, 200, "absolute merge sets x");
    TEST_ASSERT_EQ(s.y, 150, "absolute merge sets y");

    /* A relative USB delta applies on top of the absolute position. */
    mouse_update_relative(TEST_USB_MOUSE_A, 5, -5, 0);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.x, 205, "USB relative dx applies after absolute");
    TEST_ASSERT_EQ(s.y, 145, "USB relative dy applies after absolute");

    /* An UNCHANGED absolute report must NOT reset the USB-moved cursor. */
    mouse_merge_absolute(200, 150, 0);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.x, 205, "unchanged absolute does not shadow USB position");
    TEST_ASSERT_EQ(s.y, 145, "unchanged absolute does not shadow USB position");

    /* Button OR-merge: USB holds left; an idle absolute (buttons=0) report must
     * not release it. */
    mouse_update_relative(TEST_USB_MOUSE_A, 0, 0, MOUSE_BTN_LEFT);
    mouse_merge_absolute(200, 150, 0);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.buttons, MOUSE_BTN_LEFT,
                   "idle absolute does not release a USB-held button");

    /* A changed absolute report wins position; the held USB button survives. */
    mouse_merge_absolute(250, 175, 0);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.x, 250, "changed absolute moves the cursor");
    TEST_ASSERT_EQ(s.buttons, MOUSE_BTN_LEFT,
                   "USB-held button survives an absolute move");

    /* USB release clears the merged button. */
    mouse_update_relative(TEST_USB_MOUSE_A, 0, 0, 0);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.buttons, 0, "USB release clears the merged button state");

    /* Absolute (VirtIO) button-only: a same-coordinate press publishes a button
     * without moving the cursor; the matching release clears the absolute slot. */
    mouse_merge_absolute(250, 175, MOUSE_BTN_RIGHT);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.x, 250, "absolute button-only press does not move cursor");
    TEST_ASSERT_EQ(s.buttons, MOUSE_BTN_RIGHT, "absolute button-only press publishes");
    mouse_merge_absolute(250, 175, 0);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.buttons, 0, "absolute button-only release clears");

    /* VBox feedback guard: VBox is position-only (its buttons echo the PS/2
     * mouse), so a USB-held button must NOT latch into the absolute slot. */
    mouse_update_relative(TEST_USB_MOUSE_A, 0, 0, MOUSE_BTN_LEFT);
    mouse_merge_absolute_position(250, 175);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.buttons, MOUSE_BTN_LEFT, "VBox position-only keeps USB button");
    mouse_update_relative(TEST_USB_MOUSE_A, 0, 0, 0);
    mouse_merge_absolute_position(250, 175);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.buttons, 0, "VBox position-only does not latch a released button");

    /* PS/2 + USB cross-device button OR: independent holds; releasing one
     * preserves the other (the explicit PS/2+USB coexistence requirement). */
    mouse_test_set_ps2_buttons(MOUSE_BTN_LEFT);
    mouse_update_relative(TEST_USB_MOUSE_A, 0, 0, MOUSE_BTN_RIGHT);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.buttons, MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT,
                   "PS/2 left + USB right OR-merge");
    mouse_update_relative(TEST_USB_MOUSE_A, 0, 0, 0);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.buttons, MOUSE_BTN_LEFT, "USB release preserves PS/2 hold");
    mouse_test_set_ps2_buttons(0);
    s = mouse_get_state();
    TEST_ASSERT_EQ(s.buttons, 0, "PS/2 release clears merged buttons");
}

/* Two USB mice: each holds its own buttons, and a report from one releases only
 * what that one held. Runs on private slots because polling is already live at
 * test time, so the published state also carries whatever a real mouse holds. */
static void test_usb_hid_mouse_per_device(void)
{
    uint8_t slots[MOUSE_USB_SOURCE_COUNT];
    uint8_t merged = 0;
    int i;

    for (i = 0; i < MOUSE_USB_SOURCE_COUNT; i++)
        slots[i] = 0;

    merged = mouse_usb_slot_store(slots, TEST_USB_MOUSE_A, MOUSE_BTN_LEFT, merged);
    merged = mouse_usb_slot_store(slots, TEST_USB_MOUSE_B, 0, merged);
    TEST_ASSERT_EQ(merged, MOUSE_BTN_LEFT, "idle mouse B keeps mouse A's held left");

    merged = mouse_usb_slot_store(slots, TEST_USB_MOUSE_B, MOUSE_BTN_RIGHT, merged);
    TEST_ASSERT_EQ(merged, MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT, "two mice OR-merge");

    merged = mouse_usb_slot_store(slots, TEST_USB_MOUSE_A, 0, merged);
    TEST_ASSERT_EQ(merged, MOUSE_BTN_RIGHT, "mouse A release leaves mouse B's right");

    merged = mouse_usb_slot_store(slots, TEST_USB_MOUSE_B, MOUSE_BTN_MIDDLE, merged);
    TEST_ASSERT_EQ(merged, MOUSE_BTN_MIDDLE, "a report replaces that mouse's own slot");

    merged = mouse_usb_slot_store(slots, 0, MOUSE_BTN_LEFT, merged);
    TEST_ASSERT_EQ(merged, MOUSE_BTN_LEFT | MOUSE_BTN_MIDDLE, "the lowest slot is merged too");
    merged = mouse_usb_slot_store(slots, TEST_USB_MOUSE_B, 0, merged);
    merged = mouse_usb_slot_store(slots, TEST_USB_MOUSE_B, 0, merged);
    TEST_ASSERT_EQ(merged, MOUSE_BTN_LEFT, "a repeated release changes nothing");
    merged = mouse_usb_slot_store(slots, 0, 0, merged);
    TEST_ASSERT_EQ(merged, 0, "all mice released");
}

/* The live entry points reach the named slot and refuse a source with no slot. */
static void test_usb_hid_mouse_source_bounds(void)
{
    TEST_ASSERT_EQ(mouse_update_relative(TEST_USB_MOUSE_A, 0, 0, MOUSE_BTN_LEFT), 0,
                   "report accepted");
    TEST_ASSERT_EQ(mouse_test_usb_source_buttons(TEST_USB_MOUSE_A), MOUSE_BTN_LEFT,
                   "report fills the named slot");
    TEST_ASSERT_EQ(mouse_test_usb_source_buttons(TEST_USB_MOUSE_B), 0,
                   "report leaves another mouse's slot alone");

    TEST_ASSERT_EQ(mouse_update_relative(-1, 0, 0, MOUSE_BTN_RIGHT), -1, "source -1 refused");
    TEST_ASSERT_EQ(mouse_update_relative(MOUSE_USB_SOURCE_COUNT, 0, 0, MOUSE_BTN_RIGHT), -1,
                   "source past the last slot refused");
    TEST_ASSERT_EQ(mouse_usb_source_release(-1), -1, "release of source -1 refused");
    TEST_ASSERT_EQ(mouse_usb_source_release(MOUSE_USB_SOURCE_COUNT), -1,
                   "release past the last slot refused");
    TEST_ASSERT_EQ(mouse_test_usb_source_buttons(TEST_USB_MOUSE_A), MOUSE_BTN_LEFT,
                   "refused calls leave the slot alone");

    TEST_ASSERT_EQ(mouse_usb_source_release(TEST_USB_MOUSE_A), 0, "release accepted");
    TEST_ASSERT_EQ(mouse_test_usb_source_buttons(TEST_USB_MOUSE_A), 0, "release empties the slot");
    TEST_ASSERT_EQ(mouse_usb_source_release(TEST_USB_MOUSE_A), 0, "second release accepted");
}

/* A mouse whose endpoint fails sends no release report, so stopping its polling
 * must drop what it held. A stopped keyboard must not touch the mouse slot that
 * shares its device index. Uses the last devices[] entry while it is inactive:
 * the poller resolves events through active devices only. */
static void test_usb_hid_stop_polling_releases(void)
{
    struct xhci_device *dev = xhci_test_device_slot(XHCI_MAX_DEVICES - 1);
    uint8_t saved_polling, saved_proto;
    int source;

    if (!dev || dev->active) {
        TEST_SKIP("last xHCI device slot is in use");
        return;
    }
    source = xhci_device_index(dev);
    saved_polling = dev->int_in_polling;
    saved_proto = dev->hid_proto;

    (void)mouse_update_relative(source, 0, 0, MOUSE_BTN_LEFT);
    dev->hid_proto = USB_PROTO_HID_KEYBOARD;
    dev->int_in_polling = 1;
    xhci_hid_stop_polling(dev);
    TEST_ASSERT_EQ(dev->int_in_polling, 0, "stopped keyboard no longer polls");
    TEST_ASSERT_EQ(mouse_test_usb_source_buttons(source), MOUSE_BTN_LEFT,
                   "stopped keyboard releases no mouse button");

    dev->hid_proto = USB_PROTO_HID_MOUSE;
    dev->int_in_polling = 1;
    xhci_hid_stop_polling(dev);
    TEST_ASSERT_EQ(dev->int_in_polling, 0, "stopped mouse no longer polls");
    TEST_ASSERT_EQ(mouse_test_usb_source_buttons(source), 0,
                   "stopped mouse releases its held button");

    /* A counted device leaves the HID usable count exactly once when its
     * polling stops; marking twice counts once. Net zero on the live count. */
    {
        uint32_t att0 = 0, use0 = 0, att1 = 0, use1 = 0;
        uint8_t saved_counted = dev->hid_usable_counted;

        dev->hid_usable_counted = 0;
        xhci_hid_probe_counts(&att0, &use0);

        /* The enumeration race: polling was seen live, then stopped before
         * the mark. The mark must re-check and leave the device uncounted. */
        dev->int_in_polling = 1;
        xhci_hid_stop_polling(dev);
        xhci_hid_mark_usable(dev);
        xhci_hid_probe_counts(&att1, &use1);
        TEST_ASSERT_EQ(use1, use0, "mark after a stop does not count the device");
        TEST_ASSERT_EQ(dev->hid_usable_counted, 0, "stopped device stays uncounted");

        dev->int_in_polling = 1;
        xhci_hid_mark_usable(dev);
        xhci_hid_mark_usable(dev);
        xhci_hid_probe_counts(&att1, &use1);
        TEST_ASSERT_EQ(use1, use0 + 1u, "marking twice counts the device once");
        xhci_hid_stop_polling(dev);
        xhci_hid_probe_counts(&att1, &use1);
        TEST_ASSERT_EQ(use1, use0, "stopped device leaves the usable count");
        xhci_hid_stop_polling(dev);
        xhci_hid_probe_counts(&att1, &use1);
        TEST_ASSERT_EQ(use1, use0, "second stop does not decrement again");
        TEST_ASSERT_EQ(att1, att0, "attempts are cumulative, untouched");
        dev->hid_usable_counted = saved_counted;
    }

    (void)mouse_usb_source_release(source);
    dev->int_in_polling = saved_polling;
    dev->hid_proto = saved_proto;
}

/* Input-source diagnostic summary formatter (usb_input_diag_format): pure
 * yes/no rendering of the [INPUT] Sources line, plus bounded-buffer safety. */
static void test_usb_input_diag_format(void)
{
    char buf[96];
    char small[10];
    int n;

    n = usb_input_diag_format(buf, (int)sizeof(buf), 1, 0, 1, 0);
    TEST_ASSERT(diag_streq(buf,
                "[INPUT] Sources: PS/2=yes USB_KBD=no USB_MOUSE=yes VIRTIO=no"),
                "diag line renders PS/2+USB_MOUSE present");
    TEST_ASSERT_EQ(n, 60, "diag line length matches rendered string");

    TEST_ASSERT(diag_streq(
                (usb_input_diag_format(buf, (int)sizeof(buf), 0, 1, 0, 1), buf),
                "[INPUT] Sources: PS/2=no USB_KBD=yes USB_MOUSE=no VIRTIO=yes"),
                "diag line renders USB_KBD+VIRTIO present");

    /* Realistic no-device (all-no, shortest) and all-device (all-yes) renders. */
    TEST_ASSERT(diag_streq(
                (usb_input_diag_format(buf, (int)sizeof(buf), 0, 0, 0, 0), buf),
                "[INPUT] Sources: PS/2=no USB_KBD=no USB_MOUSE=no VIRTIO=no"),
                "diag line all sources absent");
    TEST_ASSERT(diag_streq(
                (usb_input_diag_format(buf, (int)sizeof(buf), 1, 1, 1, 1), buf),
                "[INPUT] Sources: PS/2=yes USB_KBD=yes USB_MOUSE=yes VIRTIO=yes"),
                "diag line all sources present");

    /* Bounded-buffer safety: a tiny cap must NUL-terminate within bounds. */
    n = usb_input_diag_format(small, (int)sizeof(small), 1, 1, 1, 1);
    TEST_ASSERT(n < (int)sizeof(small), "diag truncates within a small cap");
    TEST_ASSERT_EQ(small[sizeof(small) - 1], 0, "diag NUL-terminates small buffer");

    /* cap=1: nothing rendered, just the NUL, length 0. */
    buf[0] = 'Z';
    TEST_ASSERT_EQ(usb_input_diag_format(buf, 1, 1, 1, 1, 1), 0, "diag cap=1 length 0");
    TEST_ASSERT_EQ(buf[0], 0, "diag cap=1 writes only NUL");

    /* Exact-fit boundary: the 60-char line at cap=60 truncates one char and
     * NUL-terminates at index 59; cap=61 renders the full 60-char string. */
    n = usb_input_diag_format(buf, 60, 1, 0, 1, 0);
    TEST_ASSERT_EQ(n, 59, "diag cap=full truncates by one");
    TEST_ASSERT_EQ(buf[59], 0, "diag cap=full NUL at last byte");
    n = usb_input_diag_format(buf, 61, 1, 0, 1, 0);
    TEST_ASSERT_EQ(n, 60, "diag cap=full+1 renders full line");

    /* Degenerate caps are rejected without touching the buffer. */
    buf[0] = 'Z';
    TEST_ASSERT_EQ(usb_input_diag_format(buf, 0, 1, 1, 1, 1), 0, "diag cap=0 writes nothing");
    TEST_ASSERT_EQ(buf[0], 'Z', "diag cap=0 leaves buffer untouched");
    buf[0] = 'Z';
    TEST_ASSERT_EQ(usb_input_diag_format(buf, -5, 1, 1, 1, 1), 0, "diag negative cap rejected");
    TEST_ASSERT_EQ(buf[0], 'Z', "diag negative cap leaves buffer untouched");
}

/* The pre-SET_CONFIGURATION classifier: recognises a HID boot keyboard or
 * mouse interface, ignores non-boot HID and other classes, and stops on a
 * truncated or zero-length descriptor rather than reading past `len`. */
static void cfg_copy(uint8_t dst[18], const uint8_t src[18])
{
    for (uint32_t i = 0; i < 18u; i++)
        dst[i] = src[i];
}

/* Run the identify logic over a descriptor set and return how many HID
 * attempts it counted. It counts into a private counter, so the live boot
 * accounting (read later by the load-status record) is never touched, even if
 * a real hot-plug probe runs concurrently. */
static uint32_t hid_identify_delta(struct xhci_device *dev, const uint8_t *cfg,
                                   uint16_t len, int *rc)
{
    uint32_t att0 = 0, use0 = 0, att1 = 0, use1 = 0, mine = 0;

    for (uint32_t i = 0; i < (uint32_t)sizeof *dev; i++)
        ((uint8_t *)dev)[i] = 0;
    for (uint16_t i = 0; i < len; i++)
        dev->config_data[i] = cfg[i];
    dev->config_len = len;
    xhci_hid_probe_counts(&att0, &use0);
    *rc = xhci_hid_test_identify(dev, &mine);
    xhci_hid_probe_counts(&att1, &use1);
    TEST_ASSERT_EQ(use1, use0, "identify never changes the usable count");
    TEST_ASSERT(att1 >= att0, "live attempt count never decreases");
    return mine;
}

/* The attempt producer: a recognised HID boot interface counts as one
 * attempt even when its endpoint is missing or too small, and is_hid stays
 * clear; a non-HID interface is not an attempt. */
static void test_usb_hid_identify_attempts(void)
{
    static struct xhci_device dev;
    /* config(9) + boot keyboard iface(9) + interrupt-IN EP 0x81, MaxPkt 8 */
    uint8_t cfg[25] = {
        9, 0x02, 25, 0, 1, 1, 0, 0x80, 50,
        9, USB_DESC_INTERFACE, 0, 0, 1,
        USB_CLASS_HID, USB_SUBCLASS_HID_BOOT, USB_PROTO_HID_KEYBOARD, 0,
        7, USB_DESC_ENDPOINT, 0x81, USB_EP_ATTR_INTERRUPT, 8, 0, 10,
    };
    int rc = 0;

    TEST_ASSERT_EQ(hid_identify_delta(&dev, cfg, 18, &rc), 1u,
                   "HID iface without endpoint is one attempt");
    TEST_ASSERT(rc != 0, "HID iface without endpoint is rejected");
    TEST_ASSERT_EQ(dev.is_hid, 0, "rejected device is not is_hid");

    /* A complete boot keyboard, classified with no controller (this seam
     * passes none): counted as an attempt and refused before any endpoint
     * configuration, which would dereference the controller. */
    TEST_ASSERT_EQ(hid_identify_delta(&dev, cfg, sizeof cfg, &rc), 1u,
                   "well-formed keyboard without a controller is one attempt");
    TEST_ASSERT(rc != 0, "well-formed keyboard without a controller is refused");
    TEST_ASSERT_EQ(dev.is_hid, 0, "controller-less identify never sets is_hid");
    TEST_ASSERT_EQ(dev.int_in_ring.phys, 0u,
                   "controller-less identify allocates no transfer ring");

    cfg[22] = 4;                       /* keyboard endpoint below 8 bytes */
    TEST_ASSERT_EQ(hid_identify_delta(&dev, cfg, sizeof cfg, &rc), 1u,
                   "undersized keyboard EP is one attempt");
    TEST_ASSERT(rc != 0, "undersized keyboard EP is rejected");
    TEST_ASSERT_EQ(dev.is_hid, 0, "undersized keyboard is not is_hid");

    cfg[16] = USB_PROTO_HID_MOUSE;     /* mouse endpoint below 3 bytes */
    cfg[22] = 2;
    TEST_ASSERT_EQ(hid_identify_delta(&dev, cfg, sizeof cfg, &rc), 1u,
                   "undersized mouse EP is one attempt");
    TEST_ASSERT(rc != 0, "undersized mouse EP is rejected");

    cfg[14] = 0x08;                    /* mass storage: not HID at all */
    TEST_ASSERT_EQ(hid_identify_delta(&dev, cfg, sizeof cfg, &rc), 0u,
                   "non-HID interface is not an attempt");
    TEST_ASSERT(rc != 0, "non-HID interface is rejected");
}

static void test_usb_hid_config_classifier(void)
{
    /* config(9) + iface(9): class HID, subclass boot, protocol keyboard */
    uint8_t kbd[18] = {
        9, 0x02, 18, 0, 1, 1, 0, 0x80, 50,
        9, USB_DESC_INTERFACE, 0, 0, 1,
        USB_CLASS_HID, USB_SUBCLASS_HID_BOOT, USB_PROTO_HID_KEYBOARD, 0,
    };
    uint8_t probe[18];

    TEST_ASSERT_EQ(xhci_config_has_hid_boot(kbd, sizeof kbd), 1,
                   "boot keyboard interface recognised");
    cfg_copy(probe, kbd);
    probe[16] = USB_PROTO_HID_MOUSE;
    TEST_ASSERT_EQ(xhci_config_has_hid_boot(probe, sizeof probe), 1,
                   "boot mouse interface recognised");
    cfg_copy(probe, kbd);
    probe[15] = 0;                       /* HID, but not the boot subclass */
    TEST_ASSERT_EQ(xhci_config_has_hid_boot(probe, sizeof probe), 0,
                   "non-boot HID is not counted");
    cfg_copy(probe, kbd);
    probe[14] = 0x08;                    /* mass storage */
    TEST_ASSERT_EQ(xhci_config_has_hid_boot(probe, sizeof probe), 0,
                   "mass-storage interface is not HID");
    TEST_ASSERT_EQ(xhci_config_has_hid_boot(kbd, 17), 0,
                   "truncated interface descriptor stops the walk");
    cfg_copy(probe, kbd);
    probe[0] = 0;                        /* zero bLength must not loop */
    TEST_ASSERT_EQ(xhci_config_has_hid_boot(probe, sizeof probe), 0,
                   "zero-length descriptor stops the walk");
    TEST_ASSERT_EQ(xhci_config_has_hid_boot((const uint8_t *)0, 18), 0,
                   "NULL descriptor -> 0");
}

void test_register_usb_hid(void)
{
    test_suite_register_cat("usb-hid: config descriptor HID classifier",
                            test_usb_hid_config_classifier, TEST_CAT_STORAGE);
    test_suite_register_cat("usb-hid: identify attempt accounting",
                            test_usb_hid_identify_attempts, TEST_CAT_STORAGE);
    test_suite_register_cat("usb-hid: EP interval encoding",
                            test_usb_hid_interval_encode, TEST_CAT_STORAGE);
    test_suite_register_cat("usb-hid: keyboard usage->ASCII inject",
                            test_usb_hid_keyboard_inject, TEST_CAT_STORAGE);
    test_suite_register_cat("usb-hid: mouse report decode",
                            test_usb_hid_mouse_decode, TEST_CAT_STORAGE);
    test_suite_register_cat("usb-hid: input source coexistence merge",
                            test_usb_hid_mouse_coexist, TEST_CAT_STORAGE);
    test_suite_register_cat("usb-hid: per-device mouse buttons",
                            test_usb_hid_mouse_per_device, TEST_CAT_STORAGE);
    test_suite_register_cat("usb-hid: mouse source bounds",
                            test_usb_hid_mouse_source_bounds, TEST_CAT_STORAGE);
    test_suite_register_cat("usb-hid: stopped mouse releases buttons",
                            test_usb_hid_stop_polling_releases, TEST_CAT_STORAGE);
    test_suite_register_cat("usb-hid: input diag source summary",
                            test_usb_input_diag_format, TEST_CAT_STORAGE);
}

#endif /* KERNEL_TESTS */
