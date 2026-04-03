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

static void test_vmm_map_mmio_wc(void)
{
    /* Map a known physical address as WC and verify the mapping works.
     * Use a PMM-allocated frame so we have a real page to write to. */
    uintptr_t phys = pmm_alloc_frame();
    if (!phys) {
        TEST_SKIP("pmm_alloc_frame failed");
        return;
    }

    /* Zero via identity map first */
    volatile uint64_t *id = (volatile uint64_t *)phys;
    *id = 0;

    void *wc = vmm_map_mmio_wc(phys, VMM_PAGE_SIZE);
    TEST_ASSERT(wc != (void *)0, "vmm_map_mmio_wc returns non-NULL");
    if (!wc) {
        pmm_free_frame(phys);
        return;
    }

    /* Write through WC mapping, verify via identity map */
    volatile uint64_t *p = (volatile uint64_t *)wc;
    *p = 0xCAFEBABE12345678ULL;

    /* Force write out of WC buffer */
    __asm__ volatile ("sfence" ::: "memory");

    TEST_ASSERT(*id == 0xCAFEBABE12345678ULL, "WC write visible via identity map");

    vmm_unmap_mmio(wc, VMM_PAGE_SIZE);
    pmm_free_frame(phys);
}

static void test_vmm_split_huge_page(void)
{
    /* Verify that vmm_split_huge_page works on a known identity-mapped address.
     * Pick an address in the first 4 GiB that's definitely a huge page. */
    uintptr_t test_addr = 0x40000000ULL;  /* 1 GiB -- inside identity map */

    int rc = vmm_split_huge_page(test_addr);
    TEST_ASSERT(rc == 0, "vmm_split_huge_page succeeds on identity-mapped range");

    /* Idempotent -- second call should also succeed */
    rc = vmm_split_huge_page(test_addr);
    TEST_ASSERT(rc == 0, "vmm_split_huge_page is idempotent");

    /* After split, vmm_get_physical should still resolve correctly */
    uintptr_t phys = vmm_get_physical(test_addr);
    TEST_ASSERT(phys == test_addr, "identity mapping preserved after huge page split");
}

static void test_vmm_guard_page_install(void)
{
    /* Allocate 2 contiguous frames, install guard on the first */
    uintptr_t base = pmm_alloc_contiguous(2);
    if (!base) {
        TEST_SKIP("pmm_alloc_contiguous(2) failed");
        return;
    }

    int rc = vmm_install_guard_page(base, "TEST: guard page");
    TEST_ASSERT(rc == 0, "vmm_install_guard_page succeeds");

    /* The guarded page should resolve to 0 (not mapped) */
    uintptr_t phys = vmm_get_physical(base);
    TEST_ASSERT(phys == 0, "guard page resolves to 0 (not present)");

    /* The page after the guard should still be accessible */
    uintptr_t phys2 = vmm_get_physical(base + 4096);
    TEST_ASSERT(phys2 == base + 4096, "page after guard still mapped");
}

void test_register_vmm(void)
{
    test_suite_register_cat("VMM: map/read/unmap", test_vmm_map_roundtrip, TEST_CAT_MM);
    test_suite_register_cat("VMM: mmio_wc map/write", test_vmm_map_mmio_wc, TEST_CAT_MM);
    test_suite_register_cat("VMM: split huge page", test_vmm_split_huge_page, TEST_CAT_MM);
    test_suite_register_cat("VMM: guard page install", test_vmm_guard_page_install, TEST_CAT_MM);
}

#endif /* KERNEL_TESTS */
