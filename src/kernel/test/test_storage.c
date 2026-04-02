/* ============================================================================
 * test_storage.c — Storage driver unit tests (AHCI, VirtIO-blk)
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/drivers/ahci.h"
#include "kernel/drivers/virtio_blk.h"

/* ---- AHCI sector 0 read ---- */

static void test_ahci_read(void)
{
    if (!ahci_present()) {
        TEST_SKIP("no AHCI controller");
        return;
    }

    uint8_t buf[512];
    for (uint32_t i = 0; i < 512; i++) buf[i] = 0;

    int rc = ahci_read(0, 0, 1, buf);
    TEST_ASSERT(rc == 0, "AHCI sector 0 read succeeds");
}

/* ---- VirtIO-blk sector 0 read ---- */

static void test_virtio_read(void)
{
    if (!virtio_blk_present()) {
        TEST_SKIP("no VirtIO-blk device");
        return;
    }

    uint8_t buf[512];
    for (uint32_t i = 0; i < 512; i++) buf[i] = 0;

    int rc = virtio_blk_read(0, 1, buf);
    TEST_ASSERT(rc == 0, "VirtIO-blk sector 0 read succeeds");
}

/* ---- Registration ---- */

void test_register_storage(void)
{
    test_suite_register_cat("Storage: AHCI read", test_ahci_read, TEST_CAT_STORAGE);
    test_suite_register_cat("Storage: VirtIO read", test_virtio_read, TEST_CAT_STORAGE);
}

#endif /* KERNEL_TESTS */
