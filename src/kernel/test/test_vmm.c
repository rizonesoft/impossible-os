/* ============================================================================
 * test_vmm.c -- VMM unit tests (map, read, unmap round-trip)
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"

static void test_vmm_map_roundtrip(void)
{
    uintptr_t test_phys = pmm_alloc_frame();
    if (!test_phys) {
        TEST_SKIP("pmm_alloc_frame failed");
        return;
    }

    /* Use 8 GiB -- above the bootloader's 4 GiB identity-map */
    uintptr_t test_virt = 0x0000000200000000ULL;

    if (vmm_map_page(test_virt, test_phys, VMM_KERNEL_RW) != 0) {
        pmm_free_frame(test_phys);
        TEST_ASSERT(0, "vmm_map_page succeeds");
        return;
    }

    volatile uint64_t *p = (volatile uint64_t *)test_virt;
    *p = 0xDEADBEEFCAFE1234ULL;
    TEST_ASSERT(*p == 0xDEADBEEFCAFE1234ULL, "mapped page is readable/writable");

    uintptr_t resolved = vmm_get_physical(test_virt);
    TEST_ASSERT(resolved == test_phys, "vmm_get_physical returns correct frame");

    vmm_unmap_page(test_virt, 1);
}

void test_register_vmm(void)
{
    test_suite_register_cat("VMM: map/read/unmap", test_vmm_map_roundtrip, TEST_CAT_MM);
}

#endif /* KERNEL_TESTS */
