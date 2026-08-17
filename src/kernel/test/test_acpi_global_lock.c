/* test_acpi_global_lock.c -- ACPI 6.5 section 5.2.10.1 global lock arbitration.
 *
 * Covers the algorithm ACPICA's own acenv.h fallback gets wrong: that fallback
 * is `Acquired = 1` and never reads the lock word, so it always claims the
 * lock and never reports Pending. Every assertion below fails against that
 * behavior, which is the point of having them.
 *
 * These run in the DEFAULT build even though ACPICA itself is compiled out of
 * the test flavor, because the implementation deliberately lives in
 * src/kernel/acpi_global_lock.c rather than inside the OS Services Layer.
 *
 * No live boot infrastructure (test policy).
 *
 * XREF: 04-drivers-hardware/TODO-03-acpi-power-management.md section 1
 * (ACPICA AML interpreter integration).
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/acpi_global_lock.h"

/* LITERAL ACPI 6.5 table 5.20 wire values, deliberately NOT taken from the
 * implementation's own #defines. The first version of this suite reused the
 * implementation constants, so when acquire/release had the two bits reversed
 * every assertion still passed. A test that imports the value under test
 * proves only self-consistency. ACPICA's actbl.h agrees with these:
 *   ACPI_GLOCK_PENDING (1), ACPI_GLOCK_OWNED (1<<1). */
#define GLOCK_PENDING  0x1u   /* bit 0 */
#define GLOCK_OWNED    0x2u   /* bit 1 */

/* Uncontended acquire: word is free, we take it, Pending stays clear. */
static void test_acpi_glock_acquire_uncontended(void)
{
    volatile uint32_t word = 0u;

    TEST_ASSERT(acpi_global_lock_acquire(&word) != 0,
                "acquire on a free lock reports acquired");
    /* Exact wire value, not a masked check: acquiring 0 must produce
     * precisely 0x2 (Owned set, Pending clear). The reversed implementation
     * produced 0x1 here and a masked assertion would have accepted it. */
    TEST_ASSERT_EQ(word, 0x2u,
                   "acquiring a free lock writes exactly 0x2 (Owned)");
    TEST_ASSERT_EQ(word & GLOCK_PENDING, 0u,
                   "Pending stays clear on an uncontended acquire");
}

/* Contended acquire: firmware already owns it. We must NOT claim ownership,
 * and we must set Pending so the owner hands it over on release. */
static void test_acpi_glock_acquire_contended(void)
{
    volatile uint32_t word = 0x2u;   /* firmware holds it: Owned set */

    TEST_ASSERT(acpi_global_lock_acquire(&word) == 0,
                "acquire on a held lock reports NOT acquired");
    TEST_ASSERT_EQ(word, 0x3u,
                   "contended acquire writes exactly 0x3 (Owned|Pending)");
    TEST_ASSERT_EQ(word & GLOCK_PENDING, GLOCK_PENDING,
                   "Pending set so the owner knows to hand over");
    TEST_ASSERT_EQ(word & GLOCK_OWNED, GLOCK_OWNED,
                   "Owned remains set (the other side still owns it)");
}

/* Release with nobody waiting: both bits clear, no SMI required. */
static void test_acpi_glock_release_uncontended(void)
{
    volatile uint32_t word = 0x2u;   /* Owned, nobody waiting */

    TEST_ASSERT(acpi_global_lock_release(&word) == 0,
                "release with no waiter reports no SMI needed");
    TEST_ASSERT_EQ(word & (GLOCK_OWNED | GLOCK_PENDING), 0u,
                   "both Owned and Pending cleared on release");
}

/* Release with a waiter: caller must be told to raise the SMI. Getting this
 * wrong strands the firmware waiting forever. */
static void test_acpi_glock_release_contended(void)
{
    volatile uint32_t word = 0x3u;   /* Owned + Pending waiter */

    TEST_ASSERT(acpi_global_lock_release(&word) != 0,
                "release with a waiter reports SMI needed");
    TEST_ASSERT_EQ(word & (GLOCK_OWNED | GLOCK_PENDING), 0u,
                   "both bits cleared even when handing over");
}

/* The high 30 bits of the FACS word are not ours; arbitration must leave
 * them byte-exact or we corrupt whatever firmware keeps there. */
static void test_acpi_glock_preserves_upper_bits(void)
{
    volatile uint32_t word = 0xDEADBE00u;   /* low two bits clear */

    TEST_ASSERT(acpi_global_lock_acquire(&word) != 0, "acquire succeeds");
    TEST_ASSERT_EQ(word & 0xFFFFFF00u, 0xDEADBE00u,
                   "upper bits untouched by acquire");

    (void)acpi_global_lock_release(&word);
    TEST_ASSERT_EQ(word & 0xFFFFFF00u, 0xDEADBE00u,
                   "upper bits untouched by release");
}

/* A platform with no FACS global lock passes NULL. Failing closed there would
 * wedge every AML path that requests the lock, so acquire must succeed and
 * release must report no SMI. */
static void test_acpi_glock_null_is_not_a_fault(void)
{
    TEST_ASSERT(acpi_global_lock_acquire((volatile uint32_t *)0) != 0,
                "NULL lock acquires (no arbitration to lose)");
    TEST_ASSERT(acpi_global_lock_release((volatile uint32_t *)0) == 0,
                "NULL lock release reports no SMI needed");
}

/* Full round trip: acquire, release, re-acquire. The second acquire must be
 * uncontended, proving release actually returned the word to a free state. */
static void test_acpi_glock_round_trip(void)
{
    volatile uint32_t word = 0u;

    TEST_ASSERT(acpi_global_lock_acquire(&word) != 0, "first acquire");
    TEST_ASSERT(acpi_global_lock_release(&word) == 0, "release, no waiter");
    TEST_ASSERT(acpi_global_lock_acquire(&word) != 0,
                "re-acquire after release is uncontended");
    TEST_ASSERT_EQ(word & GLOCK_PENDING, 0u,
                   "no stale Pending bit survives the round trip");
}

void test_register_acpi_global_lock(void)
{
    test_suite_register_cat("acpi: global lock acquire uncontended",
                            test_acpi_glock_acquire_uncontended, TEST_CAT_X86);
    test_suite_register_cat("acpi: global lock acquire contended",
                            test_acpi_glock_acquire_contended, TEST_CAT_X86);
    test_suite_register_cat("acpi: global lock release uncontended",
                            test_acpi_glock_release_uncontended, TEST_CAT_X86);
    test_suite_register_cat("acpi: global lock release contended",
                            test_acpi_glock_release_contended, TEST_CAT_X86);
    test_suite_register_cat("acpi: global lock preserves upper bits",
                            test_acpi_glock_preserves_upper_bits, TEST_CAT_X86);
    test_suite_register_cat("acpi: global lock NULL is not a fault",
                            test_acpi_glock_null_is_not_a_fault, TEST_CAT_X86);
    test_suite_register_cat("acpi: global lock round trip",
                            test_acpi_glock_round_trip, TEST_CAT_X86);
}

#endif /* KERNEL_TESTS */
