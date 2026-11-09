/* ============================================================================
 * test_ap_bringup.c -- AP bringup protocol (TODO-09-boot S10)
 *
 * Two halves. The PURE half pins the helpers the protocol's safety rests on:
 * the record revoke race (ap_rec_revoke), duplicate-identity refusal
 * (ap_rec_apic_in_use), the microsecond-to-TSC conversion behind the INIT
 * settle and post-SIPI waits (smp_us_to_tsc), and the per-CPU TSS selector
 * (gdt_tss_selector). The LIVE half reads what bringup already published and
 * fails on a real boot where an AP shares a TSS, runs without IST stacks, or
 * left its record claimable. Read-only: nothing here starts, stops or re-arms
 * a CPU.
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/smp.h"
#include "kernel/gdt.h"
#include "kernel/time/mono_clock.h"   /* MONO_TSC_HZ_MIN/MAX */

static void rec_set(struct ap_rec *r, uint32_t apic, uint32_t state)
{
    r->apic_id = apic;
    r->state = state;
    r->stack = 0x1000;
}

/* The BSP wins the revoke only while the record is ARMED, and winning makes
 * it terminal. A TAKEN record must stay TAKEN: the AP is running on it. */
static void test_rec_revoke_race(void)
{
    struct ap_rec r;

    rec_set(&r, 3, AP_REC_ARMED);
    TEST_ASSERT_EQ(ap_rec_revoke(&r), 1, "ARMED record: BSP wins the revoke");
    TEST_ASSERT_EQ(r.state, AP_REC_REVOKED, "won revoke leaves REVOKED");
    TEST_ASSERT_EQ(ap_rec_revoke(&r), 0, "REVOKED is terminal: second revoke loses");

    rec_set(&r, 3, AP_REC_TAKEN);
    TEST_ASSERT_EQ(ap_rec_revoke(&r), 0, "TAKEN record: AP already claimed");
    TEST_ASSERT_EQ(r.state, AP_REC_TAKEN, "lost revoke leaves the AP's claim intact");

    rec_set(&r, 3, AP_REC_EMPTY);
    TEST_ASSERT_EQ(ap_rec_revoke(&r), 0, "EMPTY record was never armed");
    TEST_ASSERT_EQ(r.state, AP_REC_EMPTY, "EMPTY untouched");

    rec_set(&r, 3, AP_REC_PREPARED);
    TEST_ASSERT_EQ(ap_rec_revoke(&r), 0, "PREPARED was never armed: nothing to revoke");
    TEST_ASSERT_EQ(r.state, AP_REC_PREPARED, "PREPARED untouched");

    rec_set(&r, 11, AP_REC_ARMED);
    r.stack = 0xABC000;
    (void)ap_rec_revoke(&r);
    TEST_ASSERT_EQ(r.apic_id, 11u, "revoke leaves the identity intact");
    TEST_ASSERT_EQ(r.stack, 0xABC000ull, "revoke leaves the stack field intact");

    TEST_ASSERT_EQ(ap_rec_revoke((struct ap_rec *)0), 0, "NULL record refused");
}

/* The production sequence: pass 1 prepares one record per MADT entry and
 * checks the next entry's identity against what it already prepared, BEFORE
 * any record is armed. A second entry with the same APIC ID must be refused
 * at that point. */
static void test_rec_prepare_reserves_identity(void)
{
    struct ap_rec t[4];
    uint32_t i;

    for (i = 0; i < 4; i++)
        rec_set(&t[i], 0, AP_REC_EMPTY);

    TEST_ASSERT_EQ(ap_rec_apic_in_use(t, 4, 5), 0, "first entry for ID 5 is free");
    TEST_ASSERT_EQ(ap_rec_prepare(&t[1], 5, 0x200000), 0, "prepare slot 1 for ID 5");
    TEST_ASSERT_EQ(t[1].state, AP_REC_PREPARED, "prepared, not claimable");
    TEST_ASSERT_EQ(t[1].apic_id, 5u, "prepared identity");
    TEST_ASSERT_EQ(t[1].stack, 0x200000ull, "prepared stack");
    TEST_ASSERT_EQ(ap_rec_apic_in_use(t, 4, 5), 1,
                   "a duplicate MADT entry for ID 5 is refused before any record is armed");

    TEST_ASSERT_EQ(ap_rec_prepare(&t[1], 6, 0x300000), -1, "a prepared record is never re-prepared");
    TEST_ASSERT_EQ(t[1].apic_id, 5u, "refused re-prepare changes nothing");
    TEST_ASSERT_EQ(ap_rec_prepare(&t[2], 6, 0), -1, "a zero stack is refused");
    TEST_ASSERT_EQ(t[2].state, AP_REC_EMPTY, "refused prepare leaves the record EMPTY");
    TEST_ASSERT_EQ(ap_rec_prepare((struct ap_rec *)0, 6, 0x1000), -1, "NULL record refused");
}

static void test_rec_apic_in_use(void)
{
    struct ap_rec t[4];
    uint32_t i;

    for (i = 0; i < 4; i++)
        rec_set(&t[i], 0, AP_REC_EMPTY);
    t[1].apic_id = 7;   /* EMPTY slot carrying a stale ID: not in use */
    TEST_ASSERT_EQ(ap_rec_apic_in_use(t, 4, 7), 0, "an EMPTY record claims no identity");

    t[2].apic_id = 9;
    t[2].state = AP_REC_REVOKED;
    TEST_ASSERT_EQ(ap_rec_apic_in_use(t, 4, 9), 1, "a REVOKED record still owns its ID");
    t[2].state = AP_REC_TAKEN;
    TEST_ASSERT_EQ(ap_rec_apic_in_use(t, 4, 9), 1, "a TAKEN record owns its ID");
    TEST_ASSERT_EQ(ap_rec_apic_in_use(t, 4, 10), 0, "other IDs are free");
    TEST_ASSERT_EQ(ap_rec_apic_in_use(t, 2, 9), 0, "search bounded by n");
    TEST_ASSERT_EQ(ap_rec_apic_in_use(t, 0, 9), 0, "n = 0 searches nothing");
    t[0].apic_id = 12;
    t[0].state = AP_REC_TAKEN;
    TEST_ASSERT_EQ(ap_rec_apic_in_use(t, 1, 12), 1, "a match in the first slot is found");

    t[3].apic_id = 0;
    t[3].state = AP_REC_ARMED;
    TEST_ASSERT_EQ(ap_rec_apic_in_use(t, 4, 0), 1, "APIC ID 0 is a real identity when armed");
    TEST_ASSERT_EQ(ap_rec_apic_in_use((const struct ap_rec *)0, 4, 9), 0, "NULL table refused");
}

/* Rounded UP and overflow-free: the protocol delays are minimums. */
static void test_us_to_tsc(void)
{
    TEST_ASSERT_EQ(smp_us_to_tsc(200, 1000000000ull), 200000ull, "200 us at 1 GHz");
    TEST_ASSERT_EQ(smp_us_to_tsc(200, 2400000001ull), 480001ull,
                   "a fractional tick rounds up, never down");
    TEST_ASSERT_EQ(smp_us_to_tsc(1, 999999ull), 1ull, "sub-MHz rate still waits one tick");
    TEST_ASSERT_EQ(smp_us_to_tsc(0, 3000000000ull), 0ull, "zero delay is zero ticks");
    TEST_ASSERT_EQ(smp_us_to_tsc(500, 0), 0ull, "zero Hz converts to zero (callers substitute MONO_TSC_HZ_MAX)");
    TEST_ASSERT_EQ(smp_us_to_tsc(10000, 3000000000ull), 30000000ull, "exact divisibility does not round up");
    TEST_ASSERT_EQ(smp_us_to_tsc(0xFFFFFFFFu, (1ull << 44) - 1), 75557863708317843ull,
                   "max us just below 2^44 Hz: exact ceiling, no overflow");
    TEST_ASSERT_EQ(smp_us_to_tsc(0xFFFFFFFFu, 1ull << 43), 37778931854161069ull,
                   "max us at 2^43 Hz: exact ceiling, no 64-bit overflow");
}

/* The bringup delays run before the monotonic clock qualifies the TSC, so the
 * rate must fall back to the loader's calibration, not to the 100 GHz bound
 * (which turns a 10 ms settle into ~333 ms at 3 GHz). */
static void test_delay_hz(void)
{
    TEST_ASSERT_EQ(smp_delay_hz(2000000000ull, 3000000000ull), 2000000000ull, "a trusted rate wins");
    TEST_ASSERT_EQ(smp_delay_hz(0, 3000000000ull), 3000000000ull, "loader calibration before the clock is up");
    TEST_ASSERT_EQ(smp_delay_hz(0, MONO_TSC_HZ_MIN), MONO_TSC_HZ_MIN, "loader rate at the lower bound accepted");
    TEST_ASSERT_EQ(smp_delay_hz(0, MONO_TSC_HZ_MAX), MONO_TSC_HZ_MAX, "loader rate at the upper bound accepted");
    TEST_ASSERT_EQ(smp_delay_hz(0, MONO_TSC_HZ_MIN - 1), MONO_TSC_HZ_MAX, "implausibly slow loader rate rejected");
    TEST_ASSERT_EQ(smp_delay_hz(0, MONO_TSC_HZ_MAX + 1), MONO_TSC_HZ_MAX, "implausibly fast loader rate rejected");
    TEST_ASSERT_EQ(smp_delay_hz(0, 0), MONO_TSC_HZ_MAX, "no rate at all: pessimistic bound");
}

static void test_tss_selector(void)
{
    TEST_ASSERT_EQ(gdt_tss_selector(0), GDT_TSS_SEG, "BSP keeps the 0x28 TSS selector");
    TEST_ASSERT_EQ(gdt_tss_selector(1), 0x38u, "CPU 1 TSS follows the BSP's 16-byte descriptor");
    TEST_ASSERT_EQ(gdt_tss_selector(MAX_CPUS - 1), (uint16_t)(0x28u + 16u * (MAX_CPUS - 1)),
                   "last CPU's selector");
    TEST_ASSERT_EQ(gdt_tss_selector(MAX_CPUS), 0u, "past MAX_CPUS: no selector");
    TEST_ASSERT_EQ(gdt_tss_selector(0xFFFFFFFFu), 0u, "UINT32_MAX: no selector, no wrap");
    TEST_ASSERT((uint32_t)gdt_tss_selector(MAX_CPUS - 1) + 15u < GDT_NUM_ENTRIES * 8u,
                "the last TSS descriptor fits inside the GDT limit");
}

/* Live: every online CPU runs on its OWN TSS with its own guarded IST1-3. */
static void test_live_per_cpu_tss(void)
{
    uint32_t c, d;
    unsigned k;

    for (c = 0; c < MAX_CPUS; c++) {
        struct per_cpu_data *pc;

        if (!smp_cpu_is_online(c))
            continue;
        pc = smp_get_cpu(c);
        TEST_ASSERT(pc != (struct per_cpu_data *)0, "online CPU has a per-CPU block");
        if (!pc)
            continue;
        TEST_ASSERT_EQ(pc->tr_selector, gdt_tss_selector(c), "CPU loaded its own TSS selector");
        for (k = 1; k <= 3; k++) {
            uint64_t ist = gdt_get_cpu_ist(c, k);
            unsigned j;

            TEST_ASSERT(ist != 0, "online CPU has IST1-3 populated");
            TEST_ASSERT_EQ(ist & 0xFFFull, 0ull, "IST stack top is page aligned");
            /* Every (CPU, slot) pair against every earlier one, the same CPU's
             * other slots included: any alias lets a nested or concurrent
             * exception overwrite a live frame. */
            for (d = 0; d <= c; d++) {
                if (!smp_cpu_is_online(d))
                    continue;
                for (j = 1; j <= 3; j++) {
                    if (d == c && j >= k)
                        break;
                    TEST_ASSERT(ist != gdt_get_cpu_ist(d, j), "no two IST stacks alias");
                }
            }
        }
        TEST_ASSERT_EQ(gdt_get_cpu_ist(c, 0), 0ull, "IST index 0 means no IST");
        TEST_ASSERT_EQ(gdt_get_cpu_ist(c, 4), 0ull, "IST4 is not assigned");
        TEST_ASSERT_EQ(gdt_get_cpu_ist(c, 8), 0ull, "IST index past 7 refused");
        TEST_ASSERT_EQ(gdt_get_cpu_ist(c, 0xFFFFFFFFu), 0ull, "IST index UINT_MAX refused");
    }
    /* Slots past the discovered count never had a TSS published. */
    for (c = smp_cpu_present_count(); c < MAX_CPUS; c++)
        TEST_ASSERT_EQ(gdt_get_cpu_ist(c, 1), 0ull, "a CPU with no published TSS reports no IST");
    TEST_ASSERT_EQ(gdt_get_cpu_ist(0, 1), gdt_get_ist(1), "CPU 0 IST is the BSP TSS's");
    TEST_ASSERT_EQ(gdt_get_cpu_ist(MAX_CPUS, 1), 0ull, "no TSS past MAX_CPUS");
}

/* Live: the trampoline's GS_BASE source. An AP computes &cpu_data[index] from
 * these two words before it loads the kernel IDT, so they must name the real
 * per-CPU array, and every online CPU's block must be self-consistent. */
static void test_live_percpu_handoff(void)
{
    const volatile uint8_t *data = (const volatile uint8_t *)(uintptr_t)AP_DATA_BASE;
    uint64_t base = *(const volatile uint64_t *)(data + AP_OFF_PERCPU);
    uint32_t stride = *(const volatile uint32_t *)(data + AP_OFF_PCPU_STRIDE);
    uint32_t c, aps = 0;

    for (c = 1; c < MAX_CPUS; c++)
        aps += smp_cpu_is_online(c) ? 1u : 0u;
    if (aps == 0) {
        TEST_SKIP("no AP was started: smp_init writes no trampoline data on one CPU");
        return;
    }
    TEST_ASSERT_EQ(base, (uint64_t)(uintptr_t)smp_get_cpu(0), "AP_DATA names the per-CPU array");
    TEST_ASSERT_EQ(stride, (uint32_t)sizeof(struct per_cpu_data), "AP_DATA stride is one per-CPU block");
    for (c = 1; c < MAX_CPUS; c++) {
        struct per_cpu_data *pc;

        if (!smp_cpu_is_online(c))
            continue;
        pc = smp_get_cpu(c);
        TEST_ASSERT_EQ((uint64_t)(uintptr_t)pc, base + (uint64_t)c * stride,
                       "trampoline arithmetic lands on this CPU's block");
        TEST_ASSERT(pc->self == pc, "online AP block's gs:0 points at itself");
        TEST_ASSERT_EQ(pc->cpu_id, c, "online AP block carries its own index");
    }
}

/* Release undoes install on a slot no CPU uses: the lookup and IST stacks go,
 * and the slot can be installed again. Runs only on a free last slot. */
static void test_release_ap_tss(void)
{
    uint32_t slot = MAX_CPUS - 1u;
    unsigned k;

    if (smp_cpu_is_online(slot) || gdt_get_cpu_ist(slot, 1) != 0) {
        TEST_SKIP("last CPU slot is in use on this machine");
        return;
    }
    gdt_release_ap_tss(0);
    TEST_ASSERT(gdt_get_cpu_ist(0, 1) != 0, "release never touches the BSP TSS");
    gdt_release_ap_tss(slot);   /* nothing published: a no-op */

    TEST_ASSERT_EQ(gdt_install_ap_tss(slot, 0x1000ull), gdt_tss_selector(slot), "install on a free slot");
    for (k = 1; k <= 3; k++)
        TEST_ASSERT(gdt_get_cpu_ist(slot, k) != 0, "installed slot has IST1-3");
    TEST_ASSERT_EQ(gdt_install_ap_tss(slot, 0x1000ull), 0u, "a published slot refuses a second install");

    gdt_release_ap_tss(slot);
    for (k = 1; k <= 3; k++)
        TEST_ASSERT_EQ(gdt_get_cpu_ist(slot, k), 0ull, "released slot reports no IST");
    TEST_ASSERT_EQ(gdt_install_ap_tss(slot, 0x1000ull), gdt_tss_selector(slot),
                   "a released slot can be installed again");
    gdt_release_ap_tss(slot);
    TEST_ASSERT_EQ(gdt_get_cpu_ist(slot, 1), 0ull, "slot left free");
}

/* Live: after bringup no record is claimable, and every online AP's record is
 * TAKEN under its own APIC ID. */
static void test_live_records_terminal(void)
{
    const volatile struct ap_rec *recs = (const volatile struct ap_rec *)(uintptr_t)AP_REC_BASE;
    uint32_t i;

    for (i = 0; i < AP_REC_COUNT; i++)
        TEST_ASSERT(recs[i].state != AP_REC_ARMED, "no bringup record is left ARMED");
    for (i = 1; i < MAX_CPUS; i++) {
        struct per_cpu_data *pc;

        if (!smp_cpu_is_online(i))
            continue;
        pc = smp_get_cpu(i);
        if (!pc)
            continue;
        TEST_ASSERT_EQ(recs[i].state, AP_REC_TAKEN, "online AP claimed its own record");
        TEST_ASSERT_EQ(recs[i].apic_id, pc->lapic_id, "record identity matches the AP's LAPIC ID");
    }
}

void test_register_ap_bringup(void)
{
    test_suite_register_cat("ap_bringup: record revoke race", test_rec_revoke_race, TEST_CAT_X86);
    test_suite_register_cat("ap_bringup: duplicate APIC identity", test_rec_apic_in_use, TEST_CAT_X86);
    test_suite_register_cat("ap_bringup: prepare reserves identity", test_rec_prepare_reserves_identity,
                            TEST_CAT_X86);
    test_suite_register_cat("ap_bringup: us-to-TSC rounding", test_us_to_tsc, TEST_CAT_X86);
    test_suite_register_cat("ap_bringup: delay rate before the clock", test_delay_hz, TEST_CAT_X86);
    test_suite_register_cat("ap_bringup: per-CPU TSS selector", test_tss_selector, TEST_CAT_X86);
    test_suite_register_cat("ap_bringup: live per-CPU TSS + IST", test_live_per_cpu_tss, TEST_CAT_X86);
    test_suite_register_cat("ap_bringup: live records terminal", test_live_records_terminal, TEST_CAT_X86);
    test_suite_register_cat("ap_bringup: release AP TSS", test_release_ap_tss, TEST_CAT_X86);
    test_suite_register_cat("ap_bringup: live per-CPU handoff", test_live_percpu_handoff, TEST_CAT_X86);
}

#endif /* KERNEL_TESTS */
