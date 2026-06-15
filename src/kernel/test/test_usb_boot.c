/* Unit tests for the xHCI / USB-MSC boot path -- TODO-17.
 *
 * USB boot is almost entirely live-MMIO + hardware-dependent: controller
 * bring-up, 9-step enumeration, BOT SCSI read/write, and hot-plug all touch a
 * real (or emulated) xHCI controller, which WSL has none of. The only
 * test-safe surface is the read-only controller/device-count query and the
 * MSC geometry it exposes; everything else is validated via make run-usb-ci on
 * QEMU TCG and manually on bare metal. See the TODO-17 Unit Tests section. */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/drivers/xhci.h"
#include "kernel/drivers/xhci_dev.h"
#include "kernel/drivers/usb_msc.h"

/* Read-only query: safe with or without a controller. When a USB MSC device is
 * present, READ CAPACITY validation (usb_msc.c) guarantees an active device
 * exposes a supported sector size; unsupported sizes leave the device
 * unregistered. */
static void test_usb_count_and_geometry(void)
{
    TEST_ASSERT(xhci_controller_count() >= 0,
                "xHCI controller count is non-negative");
    TEST_ASSERT(xhci_msc_device_count() >= 0,
                "USB MSC device count is non-negative");

    if (xhci_controller_count() == 0) {
        TEST_SKIP("no xHCI controller -- enumeration/BOT paths need hardware");
        return;
    }

    if (xhci_msc_device_count() > 0) {
        struct xhci_device *dev = xhci_get_device(xhci_msc_device_index(0));
        const struct usb_msc_info *info = dev ? usb_msc_get_info(dev) : NULL;
        if (info && info->valid)
            TEST_ASSERT(info->sector_size == 512 || info->sector_size == 1024 ||
                        info->sector_size == 2048 || info->sector_size == 4096,
                        "MSC device has a supported sector size");
    }
}

/* Build a fixed-format sense buffer with a given sense key in byte 2. */
static void make_sense(uint8_t *sense, uint8_t key)
{
    int i;
    for (i = 0; i < SCSI_SENSE_LEN; i++)
        sense[i] = 0;
    sense[0] = 0x70;              /* fixed-format, current error */
    sense[2] = (uint8_t)(key & 0x0F);
}

/* msc_sense_classify maps a SCSI sense key to the MSC BOT retry policy class:
 * UNIT ATTENTION -> retry now, NOT READY -> wait + retry, MEDIUM/other ->
 * unrecoverable, NO SENSE / RECOVERED -> ok. Pure -- no hardware needed. */
static void test_usb_boot_sense_classify(void)
{
    uint8_t sense[SCSI_SENSE_LEN];

    make_sense(sense, SCSI_SK_UNIT_ATTENTION);
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_RETRY_NOW,
                   "UNIT ATTENTION -> retry now");

    make_sense(sense, SCSI_SK_NOT_READY);
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_WAIT_RETRY,
                   "NOT READY -> wait + retry");

    make_sense(sense, SCSI_SK_MEDIUM_ERROR);
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_UNRECOVERABLE,
                   "MEDIUM ERROR -> unrecoverable");

    make_sense(sense, SCSI_SK_HARDWARE_ERROR);
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_UNRECOVERABLE,
                   "HARDWARE ERROR -> unrecoverable");

    /* Every other named key + a reserved low key must NOT retry (default
     * branch): a future change that retried write-protected / illegal / blank /
     * aborted failures would otherwise pass the suite silently. */
    make_sense(sense, SCSI_SK_ILLEGAL_REQUEST);
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_UNRECOVERABLE,
                   "ILLEGAL REQUEST -> unrecoverable");
    make_sense(sense, SCSI_SK_DATA_PROTECT);
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_UNRECOVERABLE,
                   "DATA PROTECT -> unrecoverable");
    make_sense(sense, SCSI_SK_BLANK_CHECK);
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_UNRECOVERABLE,
                   "BLANK CHECK -> unrecoverable");
    make_sense(sense, SCSI_SK_ABORTED_COMMAND);
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_UNRECOVERABLE,
                   "ABORTED COMMAND -> unrecoverable");
    make_sense(sense, 0x0F);   /* reserved low key */
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_UNRECOVERABLE,
                   "reserved key 0x0F -> unrecoverable");

    make_sense(sense, SCSI_SK_NO_SENSE);
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_OK, "NO SENSE -> ok");

    make_sense(sense, SCSI_SK_RECOVERED);
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_OK, "RECOVERED -> ok");

    /* Only the low 4 bits are the sense key; high bits (FILEMARK/EOM/ILI) are
     * ignored, so UNIT ATTENTION with status bits set still classifies. */
    make_sense(sense, SCSI_SK_UNIT_ATTENTION);
    sense[2] |= 0xE0;
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_RETRY_NOW,
                   "sense-key high bits ignored");

    /* NULL sense -> unrecoverable (defensive). */
    TEST_ASSERT_EQ(msc_sense_classify((const uint8_t *)0), MSC_ERR_UNRECOVERABLE,
                   "NULL sense -> unrecoverable");

    /* Malformed/short reply: a zero-filled buffer (response code != 0x70/0x71)
     * must NOT misread as NO SENSE/OK -- the bulk path reports SHORT_PKT as
     * success without a byte count, so the format byte is the only guard. */
    make_sense(sense, SCSI_SK_NO_SENSE);
    sense[0] = 0x00;          /* invalid response code (zero-filled short reply) */
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_UNRECOVERABLE,
                   "invalid response code -> unrecoverable");
    /* Deferred-format sense (0x71) is still parsed. */
    make_sense(sense, SCSI_SK_UNIT_ATTENTION);
    sense[0] = 0x71;
    TEST_ASSERT_EQ(msc_sense_classify(sense), MSC_ERR_RETRY_NOW,
                   "deferred-format (0x71) sense parsed");
}

/* Tiny dependency-free string compare. */
static int sk_streq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* msc_sense_key_name names every defined sense key (no defined key falls to
 * "OTHER"); high bits are masked; an unknown key -> "OTHER". Pure. */
static void test_usb_boot_sense_key_name(void)
{
    TEST_ASSERT(sk_streq(msc_sense_key_name(SCSI_SK_NO_SENSE), "NO SENSE"), "0x0 name");
    TEST_ASSERT(sk_streq(msc_sense_key_name(SCSI_SK_RECOVERED), "RECOVERED"), "0x1 name");
    TEST_ASSERT(sk_streq(msc_sense_key_name(SCSI_SK_NOT_READY), "NOT READY"), "0x2 name");
    TEST_ASSERT(sk_streq(msc_sense_key_name(SCSI_SK_MEDIUM_ERROR), "MEDIUM ERROR"), "0x3 name");
    TEST_ASSERT(sk_streq(msc_sense_key_name(SCSI_SK_HARDWARE_ERROR), "HARDWARE ERROR"), "0x4 name");
    TEST_ASSERT(sk_streq(msc_sense_key_name(SCSI_SK_ILLEGAL_REQUEST), "ILLEGAL REQUEST"), "0x5 name");
    TEST_ASSERT(sk_streq(msc_sense_key_name(SCSI_SK_UNIT_ATTENTION), "UNIT ATTENTION"), "0x6 name");
    TEST_ASSERT(sk_streq(msc_sense_key_name(SCSI_SK_DATA_PROTECT), "DATA PROTECT"), "0x7 name");
    TEST_ASSERT(sk_streq(msc_sense_key_name(SCSI_SK_BLANK_CHECK), "BLANK CHECK"), "0x8 name");
    TEST_ASSERT(sk_streq(msc_sense_key_name(SCSI_SK_ABORTED_COMMAND), "ABORTED COMMAND"), "0xB name");
    /* High bits masked off (the sense byte carries FILEMARK/EOM/ILI flags). */
    TEST_ASSERT(sk_streq(msc_sense_key_name(0xE6), "UNIT ATTENTION"), "high bits masked");
    /* Unknown/reserved key. */
    TEST_ASSERT(sk_streq(msc_sense_key_name(0x0F), "OTHER"), "unknown key -> OTHER");
}

void test_register_usb_boot(void)
{
    test_suite_register_cat("usb: controller count + MSC geometry",
                            test_usb_count_and_geometry, TEST_CAT_STORAGE);
    test_suite_register_cat("usb-boot: SCSI sense classification",
                            test_usb_boot_sense_classify, TEST_CAT_STORAGE);
    test_suite_register_cat("usb-boot: SCSI sense key names",
                            test_usb_boot_sense_key_name, TEST_CAT_STORAGE);
}

#endif /* KERNEL_TESTS */
