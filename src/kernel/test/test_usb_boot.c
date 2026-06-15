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

void test_register_usb_boot(void)
{
    test_suite_register_cat("usb: controller count + MSC geometry",
                            test_usb_count_and_geometry, TEST_CAT_STORAGE);
}

#endif /* KERNEL_TESTS */
