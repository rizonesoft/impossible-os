/* Unit tests for the NVMe driver -- TODO-16.
 *
 * NVMe is almost entirely live-MMIO + hardware-dependent: discovery, queues,
 * read/write, flush (NVM Flush command), and shutdown (CC.SHN) all touch the
 * controller and require a real (or emulated) NVMe device, which WSL has none
 * of. blkdev_shutdown_all() operates on the GLOBAL device list and would shut
 * down the live boot disk -- forbidden in tests. So the only test-safe surface
 * is the read-only controller-count / namespace-geometry query; everything
 * else is validated via scripts/test-smoke.sh on QEMU TCG (make run-nvme) and
 * manually on bare metal. See the TODO-16 Unit Tests section. */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/drivers/nvme.h"

/* Read-only query: safe with or without a controller. When a controller is
 * present, the lifecycle LBA-size validation guarantees an active namespace
 * has a PRP-encodable sector size (512 or 4096); unsupported sizes leave the
 * I/O queue inactive. */
static void test_nvme_count_and_geometry(void)
{
    TEST_ASSERT(nvme_controller_count() >= 0,
                "controller count is non-negative");

    if (nvme_controller_count() == 0) {
        TEST_SKIP("no NVMe controller -- flush/shutdown/LBA paths need hardware");
        return;
    }

    {
        struct nvme_controller *nc = nvme_get_controller(0);
        if (nc && nc->io_queue_active)
            TEST_ASSERT(nc->ns_sector_size == 512 || nc->ns_sector_size == 4096,
                        "active namespace has a PRP-encodable sector size");
    }
}

void test_register_nvme(void)
{
    test_suite_register_cat("nvme: controller count + namespace geometry",
                            test_nvme_count_and_geometry, TEST_CAT_STORAGE);
}

#endif /* KERNEL_TESTS */
