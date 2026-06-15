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
#include "kernel/drivers/keyboard.h"

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

void test_register_usb_hid(void)
{
    test_suite_register_cat("usb-hid: EP interval encoding",
                            test_usb_hid_interval_encode, TEST_CAT_STORAGE);
    test_suite_register_cat("usb-hid: keyboard usage->ASCII inject",
                            test_usb_hid_keyboard_inject, TEST_CAT_STORAGE);
}

#endif /* KERNEL_TESTS */
