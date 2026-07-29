/* ============================================================================
 * test_fastpath_hardening.c -- TODO-04 -18 unit tests
 *
 * Oracle-only checks against in-memory state -- no live boot calls.
 * Verifies the TWO invariants a runtime ABI handshake depends on:
 *
 *   (a) IMPOSSIBLE_OS_ABI_HASH is non-zero and readable via the same
 *       constant the user-mode libc includes. A zero hash would mean
 *       the generator produced empty input and every user binary would
 *       trivially "match" a kernel that had been renumbered in any
 *       direction -- the runtime gate would be useless.
 *
 *   (b) KUSD self-describing header is populated: AbiMagic == 'KUSD',
 *       AbiVersion == KUSD_ABI_VERSION, AbiStructSize == 0x340
 *       (size of the Windows-compatible portion), AbiLayoutHash ==
 *       IMPOSSIBLE_OS_ABI_HASH. These are pure reads of the kernel
 *       KUSD page after `kusd_init()` ran as part of normal boot.
 *
 * XREF: 00-infrastructure/TODO-04-usermode-test-framework.md -18 Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/abi_hash.h"
#include "kernel/nt/kusd.h"
#include "kernel/smp.h"           /* -19 transition ring fields */

/* g_kusd is declared in src/kernel/time/kusd_time.c. The test reads
 * from it without mutating any field -- safe per the "no live boot
 * calls" rule (read-only oracle query of state already initialized
 * during normal Phase 3 boot). */
extern volatile KUSER_SHARED_DATA *g_kusd;

static void test_abi_hash_nonzero(void)
{
    TEST_ASSERT_NEQ((uint64_t)IMPOSSIBLE_OS_ABI_HASH, (uint64_t)0,
                   "IMPOSSIBLE_OS_ABI_HASH must be non-zero");
}

static void test_abi_hash_generator_format(void)
{
    /* The hash is a 64-bit FNV-1a output. FNV-1a on non-empty input
     * never produces zero (offset basis is 0xCBF29CE484222325, and
     * the multiply-XOR mix does not converge to zero unless every
     * input byte happens to cancel the running state -- vanishingly
     * unlikely for the actual SYS_ + SSDT_ + TEB + KUSD tuple we hash).
     * We also check the top bit is not stuck clear/set to guard
     * against a generator bug that accidentally emits `| 0xFFFF...`
     * or truncates to 32 bits. */
    uint64_t h = IMPOSSIBLE_OS_ABI_HASH;
    TEST_ASSERT_NEQ(h, (uint64_t)0, "ABI hash is non-zero");
    TEST_ASSERT_NEQ(h, (uint64_t)-1, "ABI hash is not all-ones");
    TEST_ASSERT_NEQ(h & 0xFFFFFFFF00000000ULL, (uint64_t)0,
                   "ABI hash has non-zero upper 32 bits (not truncated)");
}

static void test_kusd_abi_header_present(void)
{
    /* SUBSYS_TIME readiness gates kusd_init completion. If this test
     * runs before kusd_init (kernel-side TEST_CAT sweep ordering bug),
     * g_kusd would be NULL and the read would fault. The oracle is
     * the NULL check; if g_kusd is non-NULL, kusd_init has finished. */
    TEST_ASSERT_NEQ((uint64_t)(uintptr_t)g_kusd, (uint64_t)0,
                   "g_kusd populated by kusd_init");
    if (!g_kusd)
        return;
    TEST_ASSERT_EQ((uint64_t)g_kusd->AbiMagic, (uint64_t)KUSD_ABI_MAGIC,
                   "KUSD AbiMagic == 'KUSD'");
    TEST_ASSERT_EQ((uint64_t)g_kusd->AbiVersion, (uint64_t)KUSD_ABI_VERSION,
                   "KUSD AbiVersion matches header constant");
    TEST_ASSERT_EQ((uint64_t)g_kusd->AbiStructSize, (uint64_t)0x340,
                   "KUSD AbiStructSize == 0x340 (Win-compat portion)");
}

static void test_kusd_abi_hash_matches_kernel(void)
{
    /* The same hash user-mode crt_init compares against must be
     * written into the KUSD page by the kernel's kusd_init. A
     * mismatch here means kernel build drift: both sides now read
     * the SAME constant out of abi/generated/abi_contract.h, so a
     * disagreement means kusd_init captured its value from a stale
     * object rather than that the two headers diverged. */
    if (!g_kusd)
        return;
    TEST_ASSERT_EQ(g_kusd->AbiLayoutHash, IMPOSSIBLE_OS_ABI_HASH,
                   "KUSD AbiLayoutHash == IMPOSSIBLE_OS_ABI_HASH");
}

static void test_transition_ring_initialized(void)
{
    /* -19 ring init oracle: BSP ran transition_ring_init_this_cpu()
     * during smp_early_bsp_init. Read-only check of the marker --
     * no live init call per CLAUDE.md "no live boot infrastructure
     * calls" rule. */
    struct per_cpu_data *pcpu = smp_this_cpu();
    TEST_ASSERT_NEQ((uint64_t)(uintptr_t)pcpu, (uint64_t)0,
                    "smp_this_cpu returns valid per-CPU struct");
    if (!pcpu)
        return;
    TEST_ASSERT_EQ((uint64_t)pcpu->transition_init_marker,
                   (uint64_t)TRANSITION_INIT_MARKER,
                   "-19 transition ring init marker set on BSP");
    TEST_ASSERT_EQ((uint64_t)TRANSITION_RING_SIZE, (uint64_t)64,
                   "-19 transition ring sized to 64 entries");
}

static void test_kusd_abi_header_offsets(void)
{
    /* Belt-and-suspenders: the user-side header relies on these
     * byte offsets for inline asm. Pin them here even though the
     * KUSD header itself has _Static_assert -- a TEST_CAT_EXEC
     * failure is actionable on a CI boot; a _Static_assert fires
     * at kernel build time which is also caught but earlier. */
    TEST_ASSERT_EQ(__builtin_offsetof(KUSER_SHARED_DATA, AbiMagic),
                   (uint64_t)0x340, "AbiMagic @ 0x340");
    TEST_ASSERT_EQ(__builtin_offsetof(KUSER_SHARED_DATA, AbiLayoutHash),
                   (uint64_t)0x348, "AbiLayoutHash @ 0x348");
    TEST_ASSERT_EQ(sizeof(KUSER_SHARED_DATA), (uint64_t)0x1000,
                   "KUSD still exactly one page");
}

void test_register_fastpath_hardening(void)
{
    test_suite_register_cat("ABI: hash is non-zero",
                            test_abi_hash_nonzero, TEST_CAT_EXEC);
    test_suite_register_cat("ABI: hash format sanity",
                            test_abi_hash_generator_format, TEST_CAT_EXEC);
    test_suite_register_cat("KUSD: -18 header populated",
                            test_kusd_abi_header_present, TEST_CAT_EXEC);
    test_suite_register_cat("KUSD: -18 header hash matches kernel",
                            test_kusd_abi_hash_matches_kernel, TEST_CAT_EXEC);
    test_suite_register_cat("KUSD: -18 header offsets pinned",
                            test_kusd_abi_header_offsets, TEST_CAT_EXEC);
    test_suite_register_cat("RING: -19 transition ring initialized",
                            test_transition_ring_initialized, TEST_CAT_EXEC);
}

#endif /* KERNEL_TESTS */
