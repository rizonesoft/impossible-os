/* ============================================================================
 * test-memmap-layout.c -- host-side behavioral gate for the kernel memory map
 *
 * include/kernel/mm/memmap.h pins the canonical address-space layout with
 * _Static_assert, which covers the CONSTANTS. It cannot cover the behavior of
 * the inline translation helpers: a static-inline call is not an integer
 * constant expression, so no assert can evaluate one. That blind spot is not
 * hypothetical -- the image-window range check originally used an exclusive
 * bound (BASE + SIZE) which wraps to 0 at the top of the address space, making
 * mm_virt_in_image() reject every valid kernel symbol while every assert in
 * the header still passed.
 *
 * This runs on the HOST (gcc, no kernel image, zero bytes of .text/.bss), so
 * it works while the kernel BSS ceiling blocks the in-kernel suite. The
 * in-kernel suite (src/kernel/test/test_highhalf.c) returns and supersedes the
 * runtime half of this once the higher-half relocation retires that ceiling;
 * this gate stays as the compile-host-side net.
 *
 * Usage: bash tools/memmap-check/check.sh
 * Dependencies: a 64-bit host C compiler. No network, no kernel build.
 * ============================================================================ */

#include <stdio.h>
#include "kernel/mm/memmap.h"

static int g_fails;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        g_fails++;
    }
}

/* ---- Image-window range checks (the exclusive-bound wrap) ---------------- */

static void test_image_window_bounds(void)
{
    printf("image window range checks:\n");

    /* The wrap itself: this is WHY the helpers use an inclusive last address.
     * If someone reintroduces `v < BASE + SIZE`, this documents what happens. */
    check(MM_KERNEL_VIRT_BASE + MM_KERNEL_IMAGE_SIZE == 0ULL,
        "exclusive image-window end wraps to 0 (inclusive bound is mandatory)");
    check(MM_KERNEL_IMAGE_LAST == 0xffffffffffffffffULL,
        "inclusive image-window end is UINT64_MAX");

    check(mm_virt_in_image(MM_KERNEL_IMAGE_BASE),
        "first valid image address is accepted");
    check(mm_virt_in_image(MM_KERNEL_IMAGE_LAST),
        "UINT64_MAX (last image address) is accepted");
    check(!mm_virt_in_image(MM_KERNEL_IMAGE_BASE - 1ULL),
        "address one below the image start is rejected");
    check(!mm_virt_in_image(0ULL),
        "null is not an image address");
}

/* ---- The two physical<->virtual relations -------------------------------- */

static void test_image_relation(void)
{
    printf("image relation:\n");

    check(mm_image_virt_to_phys((const void *)(uintptr_t)MM_KERNEL_IMAGE_BASE)
              == MM_KERNEL_PHYS_BASE,
        "image VA -> phys returns the LMA (not the failure sentinel)");
    check(mm_image_virt_to_phys((const void *)(uintptr_t)MM_KERNEL_IMAGE_LAST)
              == MM_KERNEL_PHYS_LAST,
        "last image VA -> last expressible phys");
    check(mm_image_phys_to_virt(MM_KERNEL_PHYS_BASE)
              == (void *)(uintptr_t)MM_KERNEL_IMAGE_BASE,
        "phys LMA -> image VA round-trips");

    /* The inverse must reject rather than wrap an out-of-range input into an
     * unrelated virtual address. */
    check(mm_image_phys_to_virt(MM_KERNEL_PHYS_LAST + 1ULL) == NULL,
        "phys above the image window is rejected (no wrap)");
    check(mm_image_phys_to_virt(0ULL) == NULL,
        "phys below the image LMA is rejected");
}

static void test_hhdm_relation(void)
{
    printf("HHDM relation:\n");

    /* Physical 0 is deliberately NOT in this set: it is not an allocatable
     * frame (pmm_init reserves the low 1 MiB) and admitting it would collide
     * with the 0 failure sentinel. It is asserted as REJECTED below -- an
     * earlier version of this test round-tripped 0 and thereby masked exactly
     * that ambiguity instead of detecting it. */
    const uint64_t sample[] = { 0x1000ULL, 0x200000ULL, 0xdeadb000ULL,
                                MM_HHDM_SIZE - 1ULL };
    unsigned i;

    for (i = 0; i < sizeof(sample) / sizeof(sample[0]); i++) {
        void *v = mm_phys_to_hhdm(sample[i]);
        check(v != NULL && mm_hhdm_to_phys(v) == sample[i],
            "phys -> HHDM -> phys round-trips");
        check(mm_virt_in_hhdm((uint64_t)(uintptr_t)v),
            "HHDM alias lands inside the direct-map window");
    }

    check(mm_phys_to_hhdm(MM_HHDM_SIZE) == NULL,
        "phys at the HHDM extent has no alias");
    check(mm_phys_in_hhdm(MM_HHDM_SIZE - 1ULL),
        "last aliasable phys is in range");
}

/* The 0-sentinel contract, stated as its own test because both relations
 * report failure by returning 0/NULL: no VALID input may ever produce 0, or a
 * caller cannot distinguish success from rejection. */
static void test_zero_sentinel_is_unambiguous(void)
{
    printf("zero-sentinel contract:\n");

    check(!mm_phys_in_hhdm(0ULL),
        "physical 0 is not an HHDM-aliasable frame");
    check(mm_phys_to_hhdm(0ULL) == NULL,
        "mm_phys_to_hhdm(0) is rejected, not translated");

    /* The forward and inverse domains must exclude exactly the same page.
     * Asserting only that mm_hhdm_to_phys(MM_HHDM_BASE) == 0 is NOT enough --
     * that passes whether the base was rejected or "successfully" translated
     * to 0, so it would codify the ambiguity instead of detecting it. Assert
     * the PREDICATE rejects it, which is what makes the 0 a rejection. */
    check(!mm_virt_in_hhdm(MM_HHDM_BASE),
        "HHDM base (alias of phys 0) is OUTSIDE the valid inverse domain");
    check(mm_hhdm_to_phys((const void *)(uintptr_t)MM_HHDM_BASE) == 0ULL,
        "HHDM base yields the sentinel because it was rejected");
    check(mm_virt_in_hhdm(MM_HHDM_BASE + 1ULL),
        "the address just above the HHDM base IS in the valid domain");
    check(mm_hhdm_to_phys((const void *)(uintptr_t)(MM_HHDM_BASE + 1ULL)) == 1ULL,
        "first valid direct-map address translates to phys 1, not the sentinel");
    check(!mm_phys_in_image(0ULL),
        "physical 0 is not an image address");
    check(mm_image_phys_to_virt(0ULL) == NULL,
        "mm_image_phys_to_virt(0) is rejected, not translated");

    /* The property that matters, swept from BOTH directions: no input that a
     * range predicate ACCEPTS may translate to the failure value. */
    uint64_t phys_probes[] = { 1ULL, 0x1000ULL, 0x200000ULL, MM_KERNEL_PHYS_BASE,
                               MM_KERNEL_PHYS_LAST, MM_HHDM_SIZE - 1ULL };
    uint64_t virt_probes[] = { MM_HHDM_BASE + 1ULL, MM_HHDM_BASE + 0x1000ULL,
                               MM_HHDM_END - 1ULL, MM_KERNEL_IMAGE_BASE,
                               MM_KERNEL_IMAGE_LAST };
    unsigned i;
    int collisions = 0;

    for (i = 0; i < sizeof(phys_probes) / sizeof(phys_probes[0]); i++) {
        void *h = mm_phys_to_hhdm(phys_probes[i]);
        if (h != NULL && mm_hhdm_to_phys(h) == 0ULL)
            collisions++;
        void *g = mm_image_phys_to_virt(phys_probes[i]);
        if (g != NULL && mm_image_virt_to_phys(g) == 0ULL)
            collisions++;
    }
    check(collisions == 0,
        "no accepted physical input in either relation produces the 0 sentinel");

    collisions = 0;
    for (i = 0; i < sizeof(virt_probes) / sizeof(virt_probes[0]); i++) {
        uint64_t v = virt_probes[i];
        if (mm_virt_in_hhdm(v) && mm_hhdm_to_phys((const void *)(uintptr_t)v) == 0ULL)
            collisions++;
        if (mm_virt_in_image(v) && mm_image_virt_to_phys((const void *)(uintptr_t)v) == 0ULL)
            collisions++;
    }
    check(collisions == 0,
        "no accepted virtual input in either relation produces the 0 sentinel");
}

static void test_relations_are_disjoint(void)
{
    printf("relation disjointness (the conflation bug class):\n");

    void *image_va = mm_image_phys_to_virt(MM_KERNEL_PHYS_BASE);
    void *hhdm_va  = mm_phys_to_hhdm(MM_KERNEL_PHYS_BASE);

    check(image_va != hhdm_va,
        "one physical page has different image and HHDM addresses");
    check(mm_hhdm_to_phys(image_va) == 0ULL,
        "mm_hhdm_to_phys REJECTS a kernel image address");
    check(mm_image_virt_to_phys(hhdm_va) == 0ULL,
        "mm_image_virt_to_phys REJECTS a direct-map address");
    check(!mm_virt_in_hhdm((uint64_t)(uintptr_t)image_va),
        "an image address is not inside the HHDM window");
    check(!mm_virt_in_image((uint64_t)(uintptr_t)hhdm_va),
        "an HHDM address is not inside the image window");
}

/* ---- Canonical reconstruction -------------------------------------------- */

static void test_canonical_reconstruction(void)
{
    printf("canonical reconstruction from page-table indices:\n");

    /* The trap: a naive OR of shifted indices is positive and non-canonical
     * for pml4i >= 256, so a walker comparing against high kernel symbols
     * silently never matches and skips the high half. */
    check(!MM_IS_CANONICAL_4LVL(511ULL << 39),
        "naive index reconstruction for PML4 511 is non-canonical (trap is real)");
    check(MM_IS_CANONICAL_4LVL(mm_canonical_from_indices(511, 0, 0, 0)),
        "mm_canonical_from_indices sign-extends PML4 511 to canonical");

    uint64_t b = MM_KERNEL_VIRT_BASE;
    check(mm_canonical_from_indices((b >> 39) & 0x1ffULL, (b >> 30) & 0x1ffULL,
                                    (b >> 21) & 0x1ffULL, (b >> 12) & 0x1ffULL) == b,
        "kernel image base round-trips through index reconstruction");

    check(mm_canonical_from_indices(0, 0, 0, 1) == 0x1000ULL,
        "lower-half reconstruction is unaffected by sign extension");
    check(MM_IS_CANONICAL_4LVL(mm_canonical_from_indices(255, 511, 511, 511)),
        "last lower-half address reconstructs canonically");
}

/* ---- Layout ------------------------------------------------------------- */

static void test_window_layout(void)
{
    printf("window layout:\n");

    check(MM_USER_END <= MM_HHDM_BASE, "user half ends before the HHDM");
    check(MM_HHDM_END <= MM_MMIO_BASE, "HHDM does not overlap MMIO");
    check(MM_MMIO_END <= MM_PERCPU_BASE, "MMIO does not overlap per-CPU");
    check(MM_PERCPU_END <= MM_KERNEL_VIRT_BASE,
        "per-CPU does not overlap the image window");

    check(MM_IS_CANONICAL_4LVL(MM_HHDM_BASE), "HHDM base is 4-level canonical");
    check(MM_IS_CANONICAL_4LVL(MM_KERNEL_VIRT_BASE),
        "image base is 4-level canonical");
    check(MM_IS_CANONICAL(MM_KERNEL_VIRT_BASE, MM_CANONICAL_SHIFT_5LVL),
        "image base stays canonical under LA57 (never moves)");
    check(!MM_IS_CANONICAL_4LVL(0xff11000000000000ULL),
        "a 5-level-only HHDM base is not 4-level canonical (must be re-based)");

    /* A PS=1 PDE needs a 2 MiB-aligned frame; 1 MiB would set reserved bit 20. */
    check((MM_KERNEL_PHYS_BASE & MM_MASK_2MIB) == 0ULL,
        "kernel LMA is 2 MiB-aligned (PS=1 PDE reserved-bit rule)");
    /* NOTE: this checks the IMAGE base, not the PML4 root. The actual CR3 root
     * is the bootloader's PT_PML4 constant, which this host gate cannot see;
     * binding PT_PML4 to MM_PML4_PHYS_LIMIT is owned by the bring-up section.
     * Labelled precisely so the gate does not overclaim what it verifies. */
    check(MM_KERNEL_PHYS_BASE < MM_PML4_PHYS_LIMIT,
        "kernel image LMA is below 4 GiB (NOT a check of the AP-loaded PML4 root)");
    check(MM_KERNEL_PHYS_BASE >= MM_AP_ENVELOPE_END,
        "kernel LMA does not overlap the AP bring-up envelope");
}

int main(void)
{
    printf("memmap layout gate (host)\n\n");

    test_image_window_bounds();
    test_image_relation();
    test_hhdm_relation();
    test_zero_sentinel_is_unambiguous();
    test_relations_are_disjoint();
    test_canonical_reconstruction();
    test_window_layout();

    if (g_fails) {
        printf("\nFAIL: %d memmap layout check(s) failed\n", g_fails);
        return 1;
    }
    printf("\nPASS: memmap layout gate -- all checks passed\n");
    return 0;
}
