/* ============================================================================
 * test_swap.c -- Swap subsystem unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/swap.h"

static void test_swap_roundtrip(void)
{
    swap_init(64);

    uintptr_t test_phys = pmm_alloc_frame();
    if (!test_phys) {
        TEST_SKIP("pmm_alloc_frame failed");
        return;
    }

    uintptr_t test_virt = 0x800000;
    vmm_map_page(test_virt, test_phys, VMM_KERNEL_RW);

    uint8_t *page = (uint8_t *)test_virt;
    for (uint32_t k = 0; k < 4096; k++)
        page[k] = (uint8_t)(k & 0xFF);

    swap_clock_register(test_virt);

    int slot_id = swap_out(test_virt);
    TEST_ASSERT(slot_id >= 0, "swap_out succeeds");

    if (slot_id >= 0) {
        int rc = swap_in((uint32_t)slot_id, test_virt);
        TEST_ASSERT(rc == 0, "swap_in succeeds");

        uint32_t ok = 1;
        page = (uint8_t *)test_virt;
        for (uint32_t k = 0; k < 4096; k++) {
            if (page[k] != (uint8_t)(k & 0xFF)) {
                ok = 0;
                break;
            }
        }
        TEST_ASSERT(ok, "swap round-trip data integrity");
    }

    vmm_unmap_page(test_virt, 1);
}

void test_register_swap(void)
{
    test_suite_register_cat("Swap: out/in roundtrip", test_swap_roundtrip, TEST_CAT_MM);
}

#endif /* KERNEL_TESTS */
