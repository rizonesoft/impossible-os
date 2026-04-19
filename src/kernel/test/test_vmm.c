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

/* --- vmm_map_user_page round-trip test ---
 *
 * Allocate a PMM frame, map it at a test VA via vmm_map_user_page using
 * the kernel PML4 (safe in single-threaded test context), write a pattern
 * through the identity map (phys == VA), read back through the mapped VA,
 * assert match.  Unmap and verify the PTE is cleared.
 *
 * No live boot infrastructure calls.  Uses pmm_alloc_frame / pmm_free_frame
 * which are pure allocator operations (not in the forbidden list).
 */
static void test_vmm_map_user_page_roundtrip(void)
{
    uintptr_t cr3 = vmm_get_kernel_cr3();
    uintptr_t test_va = 0x200000000ULL;  /* 8 GiB -- unmapped region */
    uintptr_t frame = pmm_alloc_frame();

    TEST_ASSERT(frame != 0, "pmm_alloc_frame returns non-zero");

    /* vmm_map_user_page zero-fills the frame internally */
    int rc = vmm_map_user_page(cr3, test_va, frame);
    TEST_ASSERT(rc == 0, "vmm_map_user_page succeeds");

    /* Write a pattern through the identity map (phys == kernel VA) */
    volatile uint64_t *id_ptr = (volatile uint64_t *)frame;
    *id_ptr = 0xDEADBEEF12345678ULL;

    /* Read back through the mapped user VA */
    volatile uint64_t *user_ptr = (volatile uint64_t *)test_va;
    TEST_ASSERT_EQ(*user_ptr, 0xDEADBEEF12345678ULL,
                   "user VA reads pattern written via identity map");

    /* Verify the mapping resolves to our frame */
    uintptr_t resolved = vmm_get_physical(test_va);
    TEST_ASSERT_EQ(resolved, frame,
                   "vmm_get_physical returns the mapped frame");

    /* Unmap and verify PTE is cleared */
    vmm_unmap_user_page(cr3, test_va);
    uintptr_t after = vmm_get_physical(test_va);
    TEST_ASSERT_EQ(after, 0,
                   "vmm_get_physical returns 0 after unmap");
}

/* §6 VMM map fault injection: arming vmm_map_fail_next() forces the
 * next vmm_map_page() to return -1 without touching page tables.
 * Subsequent calls succeed normally. */
static void test_vmm_map_fault_inject(void)
{
    uintptr_t frame = pmm_alloc_frame();
    if (!frame) {
        TEST_SKIP("pmm_alloc_frame failed");
        return;
    }

    uint64_t pre = vmm_map_fail_injections_triggered();
    uintptr_t test_virt = 0x0000000200010000ULL;  /* above 4 GiB ID map */

    vmm_map_fail_next();
    int r1 = vmm_map_page(test_virt, frame, VMM_KERNEL_RW);
    TEST_ASSERT_EQ(r1, -1,
                   "armed vmm_map_fail_next forces vmm_map_page to return -1");

    /* Second call (countdown now cleared) should succeed. */
    int r2 = vmm_map_page(test_virt, frame, VMM_KERNEL_RW);
    TEST_ASSERT_EQ(r2, 0, "auto-cleared after fire -- next vmm_map_page succeeds");

    TEST_ASSERT_EQ(vmm_map_fail_injections_triggered() - pre, 1u,
                   "vmm_map injection counter advanced by exactly 1");

    vmm_unmap_page(test_virt, 1);
}

/* Task-filter mirror test -- foreign-PID arm must skip without
 * consuming the countdown. */
static void test_vmm_map_fault_inject_task_filter(void)
{
    uintptr_t frame = pmm_alloc_frame();
    if (!frame) { TEST_SKIP("pmm_alloc_frame failed"); return; }

    uint64_t pre = vmm_map_fail_injections_triggered();
    uintptr_t va = 0x0000000200020000ULL;

    vmm_map_fail_next();
    vmm_map_fail_task_filter_set(0xFFFFFFFFu);
    int r1 = vmm_map_page(va, frame, VMM_KERNEL_RW);
    TEST_ASSERT_EQ(r1, 0, "task-filter skips non-matching task -- vmm_map_page succeeds");
    TEST_ASSERT_EQ(vmm_map_fail_injections_triggered(), pre,
                   "vmm_map injection counter unchanged when filter blocks");
    vmm_unmap_page(va, 1);

    vmm_map_fail_task_filter_clear();
    /* Re-allocate a frame since the first one was freed via unmap_page. */
    uintptr_t frame2 = pmm_alloc_frame();
    if (!frame2) return;
    int r2 = vmm_map_page(va, frame2, VMM_KERNEL_RW);
    TEST_ASSERT_EQ(r2, -1, "countdown preserved -- fires after filter clear");
    /* r2 == -1 means vmm_map_page rejected; free the frame we allocated. */
    pmm_free_frame(frame2);
    TEST_ASSERT_EQ(vmm_map_fail_injections_triggered() - pre, 1u,
                   "exactly 1 vmm_map fire after filter clear");
}

/* Max-injections auto-reload for vmm_map. */
static void test_vmm_map_fault_inject_max_cap(void)
{
    uint64_t pre = vmm_map_fail_injections_triggered();
    vmm_map_fail_max_injections_set(3);
    vmm_map_fail_next();

    /* Allocate 10 dummy frames and try to map each. First 3 fail,
     * last 7 succeed. */
    uintptr_t va_base = 0x0000000200030000ULL;
    int fail_hits = 0;
    int mapped_count = 0;
    uintptr_t mapped_vas[10] = {0};

    for (int i = 0; i < 10; i++) {
        uintptr_t frame = pmm_alloc_frame();
        if (!frame) continue;
        uintptr_t va = va_base + (uintptr_t)i * 0x1000;
        int r = vmm_map_page(va, frame, VMM_KERNEL_RW);
        if (r < 0) {
            fail_hits++;
            pmm_free_frame(frame);  /* vmm_map failed; free the frame */
        } else {
            mapped_vas[mapped_count++] = va;
        }
    }

    TEST_ASSERT_EQ(fail_hits, 3, "vmm_map max_injections=3 produces 3 fails");
    TEST_ASSERT_EQ(vmm_map_fail_fired_counter(), 3u,
                   "vmm_map fired_counter equals cap");
    TEST_ASSERT_EQ(vmm_map_fail_injections_triggered() - pre, 3u,
                   "vmm_map injection counter advanced by exactly 3");

    /* Cleanup: unmap + free-frame for each successful mapping. */
    for (int i = 0; i < mapped_count; i++)
        vmm_unmap_page(mapped_vas[i], 1);

    vmm_map_fail_max_injections_clear();
}

void test_register_vmm(void)
{
    test_suite_register_cat("VMM: map/read/unmap", test_vmm_map_roundtrip, TEST_CAT_MM);
    test_suite_register_cat("VMM: mmio_wc map/write", test_vmm_map_mmio_wc, TEST_CAT_MM);
    test_suite_register_cat("VMM: split huge page", test_vmm_split_huge_page, TEST_CAT_MM);
    test_suite_register_cat("VMM: guard page install", test_vmm_guard_page_install, TEST_CAT_MM);
    test_suite_register_cat("VMM: map_user_page roundtrip",
                            test_vmm_map_user_page_roundtrip, TEST_CAT_MM);
    test_suite_register_cat("VMM: fault-inject vmm_map_fail_next",
                            test_vmm_map_fault_inject, TEST_CAT_MM);
    test_suite_register_cat("VMM: fault-inject task-filter",
                            test_vmm_map_fault_inject_task_filter, TEST_CAT_MM);
    test_suite_register_cat("VMM: fault-inject max-injections cap",
                            test_vmm_map_fault_inject_max_cap, TEST_CAT_MM);
}

#endif /* KERNEL_TESTS */
