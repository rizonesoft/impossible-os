/* SPDX-License-Identifier: MIT */
/* Unit tests for the interrupt/timer architecture (TODO-11).
 *
 * All tests are pure-helper / error-path calls -- no live boot
 * infrastructure, no live IRQ registration on real GSIs (a dummy handler
 * on a live line would either steal device interrupts or trip the
 * all-IRQ_NONE storm quarantine). Validation rules, vector translation,
 * MADT consolidated info, and timer state are read-only oracles here. */

#include "kernel/types.h"
#include "kernel/test/test.h"
#include "kernel/acpi.h"
#include "kernel/irq.h"
#include "kernel/timer.h"
#include "kernel/drivers/pic.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/hpet.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/vectors.h"
#include "kernel/time/mono_clock.h"
#include "kernel/drivers/pit.h"

/* ---- MADT consolidated info (S1) ---------------------------------------- */

static void test_madt_info_populated(void)
{
    const struct acpi_madt_info *mi = acpi_madt_info();
    TEST_ASSERT(mi != (const struct acpi_madt_info *)0,
                "acpi_madt_info() returns non-NULL");
    TEST_ASSERT(mi->cpu_count >= 1, "consolidated cpu_count >= 1");
    TEST_ASSERT(mi->lapic_base != 0, "consolidated lapic_base nonzero");
}

static void test_madt_info_matches_legacy(void)
{
    const struct acpi_madt_info *mi = acpi_madt_info();
    uint32_t i;
    TEST_ASSERT_EQ(mi->cpu_count, acpi_get_cpu_count(),
                   "consolidated cpu_count == acpi_get_cpu_count()");
    TEST_ASSERT_EQ((uint64_t)mi->lapic_base,
                   (uint64_t)acpi_get_lapic_base(),
                   "consolidated lapic_base == acpi_get_lapic_base()");
    TEST_ASSERT_EQ((uint64_t)(mi->flags & 1u),
                   (uint64_t)acpi_pcat_compat(),
                   "consolidated PCAT bit == acpi_pcat_compat()");
    TEST_ASSERT_EQ((uint64_t)mi->ioapic_base,
                   (uint64_t)acpi_get_ioapic_base(),
                   "consolidated ioapic_base == acpi_get_ioapic_base()");
    TEST_ASSERT_EQ(mi->override_count, acpi_get_override_count(),
                   "consolidated override_count matches legacy");
    for (i = 0; i < mi->override_count && i < 24; i++) {
        const struct madt_int_override *ovr = acpi_get_override(i);
        TEST_ASSERT(ovr != (const struct madt_int_override *)0,
                    "legacy override entry exists");
        if (!ovr) break;
        TEST_ASSERT_EQ((uint64_t)mi->overrides[i].bus_irq,
                       (uint64_t)ovr->source, "override bus_irq mirrors");
        TEST_ASSERT_EQ((uint64_t)mi->overrides[i].gsi,
                       (uint64_t)ovr->gsi, "override gsi mirrors");
        TEST_ASSERT_EQ((uint64_t)mi->overrides[i].flags,
                       (uint64_t)ovr->flags, "override flags mirror");
    }
}

static void test_pcat_compat_is_boolean(void)
{
    uint8_t v = acpi_pcat_compat();
    TEST_ASSERT(v == 0 || v == 1, "acpi_pcat_compat() returns 0 or 1");
}

/* ---- ISA vector translation (S4) ----------------------------------------- */

static void test_isa_irq_to_vector_mapping(void)
{
    TEST_ASSERT_EQ(isa_irq_to_vector(0),  PIC1_OFFSET,     "IRQ0 -> 0x20");
    TEST_ASSERT_EQ(isa_irq_to_vector(7),  PIC1_OFFSET + 7, "IRQ7 -> 0x27");
    TEST_ASSERT_EQ(isa_irq_to_vector(8),  PIC2_OFFSET,     "IRQ8 -> 0x70");
    TEST_ASSERT_EQ(isa_irq_to_vector(9),  PIC2_OFFSET + 1,
                   "IRQ9 (typical ACPI SCI) -> 0x71, NOT 0x29");
    TEST_ASSERT_EQ(isa_irq_to_vector(15), PIC2_OFFSET + 7, "IRQ15 -> 0x77");
    TEST_ASSERT_EQ(isa_irq_to_vector(16), 0, "IRQ16 -> 0 (invalid)");
    TEST_ASSERT_EQ(isa_irq_to_vector(0xFF), 0, "IRQ 0xFF -> 0 (invalid)");
}

static void test_vector_to_isa_mapping(void)
{
    TEST_ASSERT_EQ(irq_vector_to_isa(PIC1_OFFSET),     0,  "0x20 -> IRQ0");
    TEST_ASSERT_EQ(irq_vector_to_isa(PIC1_OFFSET + 7), 7,  "0x27 -> IRQ7");
    TEST_ASSERT_EQ(irq_vector_to_isa(PIC2_OFFSET),     8,  "0x70 -> IRQ8");
    TEST_ASSERT_EQ(irq_vector_to_isa(PIC2_OFFSET + 7), 15, "0x77 -> IRQ15");
    TEST_ASSERT_EQ(irq_vector_to_isa(0x50), 0xFF, "0x50 -> non-ISA");
    TEST_ASSERT_EQ(irq_vector_to_isa(0x2E), 0xFF,
                   "0x2E (NT syscall) is NOT an ISA vector");
}

/* ---- Dynamic allocator reservation (S4/S5) ------------------------------- */

static void test_alloc_never_returns_isa_window(void)
{
    /* Exhaust the dynamic allocator so the reserved 0x70-0x77 window,
     * the IRQ_DYNAMIC_BASE..IRQ_DYNAMIC_END bounds, AND the depleted
     * terminal (returns 0) are all asserted; free everything after and
     * confirm the allocator recovers. Pure table walk, no routing. */
    static uint8_t got[256];
    uint32_t i, n = 0;
    uint8_t probe;
    for (i = 0; i < 256; i++) {
        uint8_t v = irq_alloc_vector();
        if (!v)
            break;
        got[n++] = v;
        TEST_ASSERT(v < PIC2_OFFSET || v > PIC2_OFFSET + 7,
                    "irq_alloc_vector() avoids the 0x70-0x77 window");
        TEST_ASSERT(v >= IRQ_DYNAMIC_BASE && v <= IRQ_DYNAMIC_END,
                    "allocated vector inside dynamic range");
        TEST_ASSERT(v != VECTOR_LINUX_SYSCALL,
                    "allocator never returns INT 0x80 (Linux syscall)");
        TEST_ASSERT(v != VECTOR_YIELD,
                    "allocator never returns INT 0x81 (yield)");
    }
    TEST_ASSERT(n > 0, "dynamic allocator yields vectors");
    TEST_ASSERT_EQ(irq_alloc_vector(), 0, "depleted allocator returns 0");
    for (i = 0; i < n; i++)
        irq_free_vector(got[i]);
    probe = irq_alloc_vector();
    TEST_ASSERT(probe != 0, "allocator recovers after frees");
    irq_free_vector(probe);
}

/* ---- GSI request validation (S5) ----------------------------------------- */

static void test_request_gsi_invalid(void)
{
    /* NULL handlers are rejected BEFORE any allocation or routing, so
     * these calls are pure validation paths on every platform (no live
     * GSI is ever programmed). */
    TEST_ASSERT_EQ(irq_request_gsi(2, (irq_handler_t)0, (void *)0,
                                   "test-null"),
                   0, "irq_request_gsi(valid gsi, NULL handler) fails");
    TEST_ASSERT_EQ(irq_request_gsi(300, (irq_handler_t)0, (void *)0,
                                   "test-range"),
                   0, "irq_request_gsi(300) out of range fails");
    TEST_ASSERT_EQ(irq_gsi_count(99), 0, "unregistered GSI count is 0");
}

static void test_request_gsi_ex_validation(void)
{
    TEST_ASSERT_EQ(irq_request_gsi_ex(300, (irq_shared_handler_t)0,
                                      (void *)0, "t", 0, 1),
                   0, "irq_request_gsi_ex(300) out of range fails");
    TEST_ASSERT_EQ(irq_request_gsi_ex(5, (irq_shared_handler_t)0,
                                      (void *)0, "t", 0, 1),
                   0, "irq_request_gsi_ex NULL handler fails");
}

/* Dummy handlers for boundary-only calls: every invocation below fails
 * the GSI range / routing lookup BEFORE registration, so these are never
 * actually called -- they exist to keep the range guard isolated from
 * the NULL-handler guard (a regression dropping only the range check
 * must not stay green). */
static void test_dummy_irq(uint8_t vector, void *ctx)
{
    (void)vector; (void)ctx;
}

static int test_dummy_shared(uint8_t vector, void *ctx)
{
    (void)vector; (void)ctx;
    return IRQ_NONE;
}

static void test_reserve_vector_collisions(void)
{
    /* Refusal paths only -- no table mutation. Static owners (set in
     * irq_init) must reject a second reservation, e.g. a firmware FADT
     * SCI value that collides with the syscall/yield gates or the ISA
     * window. Out-of-dynamic-range vectors are no-op successes. */
    TEST_ASSERT_EQ(irq_reserve_vector(VECTOR_LINUX_SYSCALL, "test"), -1,
                   "reserve refuses INT 0x80 (already reserved)");
    TEST_ASSERT_EQ(irq_reserve_vector(VECTOR_YIELD, "test"), -1,
                   "reserve refuses INT 0x81 (already reserved)");
    TEST_ASSERT_EQ(irq_reserve_vector(PIC2_OFFSET, "test"), -1,
                   "reserve refuses ISA window vector 0x70");
    TEST_ASSERT_EQ(irq_reserve_vector(VECTOR_NT_SYSCALL, "test"), 0,
                   "reserve is a no-op below the dynamic range (0x2E)");
}

static void test_gsi_range_guards(void)
{
    TEST_ASSERT_EQ(irq_request_gsi(256, test_dummy_irq, (void *)0,
                                   "range-only"),
                   0, "irq_request_gsi(256, real handler) fails on range");
    TEST_ASSERT_EQ(irq_request_gsi_ex(256, test_dummy_shared, (void *)0,
                                      "range-only", 0, 1),
                   0, "irq_request_gsi_ex(256, real handler) fails on range");
    TEST_ASSERT_EQ(irq_release_gsi_shared(256, test_dummy_shared,
                                          (void *)0),
                   -1, "irq_release_gsi_shared(256) fails on range");
    TEST_ASSERT_EQ(irq_release_gsi_shared(99, test_dummy_shared,
                                          (void *)0),
                   -1, "irq_release_gsi_shared(unrouted GSI) fails");
    TEST_ASSERT_EQ(ioapic_set_destination(0xFFFFFFFFu, 0), -1,
                   "ioapic_set_destination(out-of-range GSI) fails");
}

static void test_set_affinity_validation(void)
{
    TEST_ASSERT_EQ(irq_set_affinity(99, 1), -1,
                   "affinity on unrouted GSI fails");
    TEST_ASSERT_EQ(irq_set_affinity(2, 0), -1,
                   "affinity with empty cpu mask fails");
    TEST_ASSERT_EQ(irq_set_affinity(300, 1), -1,
                   "affinity on out-of-range GSI fails");
}

static void test_gsi_reverse_map(void)
{
    /* Vectors that were never GSI-requested have no reverse mapping */
    TEST_ASSERT_EQ(irq_gsi_for_vector(0xEE), 0xFFFFFFFFu,
                   "unrequested vector has no GSI reverse map");
    TEST_ASSERT_EQ(irq_vector_quarantined(0xEE), 0,
                   "unrequested vector is not quarantined");
}

/* ---- Unified timer subsystem (S6/S7) ------------------------------------- */

static void test_system_timer_selected(void)
{
    TEST_ASSERT(g_system_timer != (timer_driver_t *)0,
                "g_system_timer selected");
    TEST_ASSERT(g_system_timer->name != (const char *)0,
                "timer driver has a name");
}

static void test_uptime_ns_monotonic(void)
{
    uint64_t a = uptime_ns();
    uint64_t b = uptime_ns();
    TEST_ASSERT(a > 0, "uptime_ns() > 0 after boot");
    TEST_ASSERT(b >= a, "uptime_ns() monotonic non-decreasing");
}

static void test_hpet_consistency(void)
{
    /* hpet_ns() returns 0 when the HPET is absent/disabled */
    if (!hpet_available())
        TEST_ASSERT_EQ(hpet_ns(), 0, "hpet_ns() is 0 when HPET absent");
    else
        TEST_ASSERT(hpet_frequency_hz() > 0,
                    "HPET present implies nonzero frequency");
}

static void test_uts_clocksource_contract(void)
{
    /* Single clocksource contract: both backends delegate read_ns to
     * mono_ns (pointer equality is pure and platform-independent) */
    TEST_ASSERT(pit_driver.read_ns == mono_ns,
                "pit_driver.read_ns delegates to mono_ns");
    TEST_ASSERT(lapic_driver.read_ns == mono_ns,
                "lapic_driver.read_ns delegates to mono_ns");
    /* When a mono source is live, uptime_ns() must be coherent with
     * mono_ns() (same clock, sampled close together) */
    if (mono_clock_source_id() != MONO_SRC_NONE) {
        uint64_t a = mono_ns();
        uint64_t b = uptime_ns();
        TEST_ASSERT(b >= a, "uptime_ns >= earlier mono_ns sample");
        TEST_ASSERT(b - a < 50000000ULL,
                    "uptime_ns within 50ms of mono_ns sample");
    }
}

static void test_uts_oneshot_surface(void)
{
    /* PIT has no one-shot; LAPIC provides the hook (vtable shape only --
     * arming the live timer from a test would perturb the system tick) */
    TEST_ASSERT(pit_driver.arm_oneshot == (int (*)(uint64_t))0,
                "PIT backend has no one-shot hook");
    TEST_ASSERT(lapic_driver.arm_oneshot != (int (*)(uint64_t))0,
                "LAPIC backend provides arm_oneshot");
    if (g_system_timer == &pit_driver)
        TEST_ASSERT_EQ(timer_arm_oneshot(0), -1,
                       "one-shot refused on the PIT backend");
}

static void test_oneshot_conversion_helpers(void)
{
    /* ns -> TSC: split multiply-divide must be exact and overflow-free */
    TEST_ASSERT_EQ(lapic_oneshot_ns_to_tsc(0, 4000000000ULL), 0,
                   "0 ns -> 0 TSC ticks");
    TEST_ASSERT_EQ(lapic_oneshot_ns_to_tsc(1000000000ULL, 4000000000ULL),
                   4000000000ULL, "1 s at 4 GHz = 4e9 TSC ticks");
    TEST_ASSERT_EQ(lapic_oneshot_ns_to_tsc(3600000000000ULL, 4000000000ULL),
                   14400000000000ULL, "1 hour at 4 GHz (would overflow u64 mul)");
    TEST_ASSERT_EQ(lapic_oneshot_ns_to_tsc(1, 1000000000ULL), 1,
                   "1 ns at 1 GHz = 1 tick");
    /* ns -> LAPIC initial-count ticks */
    TEST_ASSERT_EQ(lapic_oneshot_ns_to_ticks(0, 100000), 0,
                   "0 ns -> 0 LAPIC ticks");
    TEST_ASSERT_EQ(lapic_oneshot_ns_to_ticks(1000000000ULL, 100000),
                   100000000ULL, "1 s at 100000 ticks/ms = 1e8 ticks");
    TEST_ASSERT(lapic_oneshot_ns_to_ticks(60000000000ULL, 100000)
                    > 0xFFFFFFFFULL,
                "60 s at 100000 ticks/ms exceeds the 32-bit ICR range");
    TEST_ASSERT(lapic_oneshot_ns_to_ticks(42000000000ULL, 100000)
                    <= 0xFFFFFFFFULL,
                "42 s at 100000 ticks/ms still fits the 32-bit ICR range");
}

static void test_mono_lapic_scaling(void)
{
    TEST_ASSERT_EQ(mono_lapic_ticks_to_ns(100, 100), 1000000000ULL,
                   "100 ticks at 100 Hz = 1 s");
    TEST_ASSERT_EQ(mono_lapic_ticks_to_ns(1000, 1000), 1000000000ULL,
                   "1000 ticks at 1000 Hz = 1 s (resolution change)");
    TEST_ASSERT_EQ(mono_lapic_ticks_to_ns(123, 0), 0,
                   "freq 0 (no tick source) -> 0");
}

static int  s_fake_oneshot_rc;
static uint64_t s_fake_oneshot_deadline;
static int fake_arm_oneshot(uint64_t deadline_mono_ns)
{
    s_fake_oneshot_deadline = deadline_mono_ns;
    return s_fake_oneshot_rc;
}

static void test_oneshot_delegation(void)
{
    timer_driver_t fake = { .name = "fake", .arm_oneshot = fake_arm_oneshot };
    timer_driver_t no_hook = { .name = "nohook" };

    TEST_ASSERT_EQ(timer_arm_oneshot_on((timer_driver_t *)0, 5), -1,
                   "NULL driver refused");
    TEST_ASSERT_EQ(timer_arm_oneshot_on(&no_hook, 5), -1,
                   "driver without one-shot hook refused");
    s_fake_oneshot_rc = 0;
    TEST_ASSERT_EQ(timer_arm_oneshot_on(&fake, 0x123456789ABCDEFULL), 0,
                   "backend return value preserved (success)");
    TEST_ASSERT_EQ(s_fake_oneshot_deadline, 0x123456789ABCDEFULL,
                   "absolute deadline forwarded unmodified");
    s_fake_oneshot_rc = -1;
    TEST_ASSERT_EQ(timer_arm_oneshot_on(&fake, 7), -1,
                   "backend return value preserved (failure)");
}

static void test_lapic_calibration_state(void)
{
    /* When the LAPIC timer drives the system, calibration must have
     * been measured (the hardcoded estimate may never drive the tick) */
    if (g_system_timer && g_system_timer->name &&
        g_system_timer->name[0] == 'L') {
        TEST_ASSERT_EQ(lapic_timer_calibrated(), 1,
                       "LAPIC tick source implies measured calibration");
        TEST_ASSERT(lapic_timer_ticks_per_ms() > 0,
                    "LAPIC ticks/ms nonzero when selected");
    } else {
        TEST_SKIP("PIT tick source -- LAPIC calibration not asserted");
    }
}

void test_register_irq_timer(void)
{
    test_suite_register_cat("irq_timer: MADT info populated",
        test_madt_info_populated, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: MADT info matches legacy accessors",
        test_madt_info_matches_legacy, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: PCAT_COMPAT boolean",
        test_pcat_compat_is_boolean, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: ISA irq->vector mapping",
        test_isa_irq_to_vector_mapping, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: vector->ISA irq mapping",
        test_vector_to_isa_mapping, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: allocator avoids ISA window",
        test_alloc_never_returns_isa_window, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: invalid GSI requests fail",
        test_request_gsi_invalid, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: gsi_ex validation",
        test_request_gsi_ex_validation, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: GSI range guards",
        test_gsi_range_guards, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: reserve vector collisions",
        test_reserve_vector_collisions, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: affinity validation",
        test_set_affinity_validation, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: GSI reverse map",
        test_gsi_reverse_map, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: system timer selected",
        test_system_timer_selected, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: uptime_ns monotonic",
        test_uptime_ns_monotonic, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: HPET consistency",
        test_hpet_consistency, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: UTS clocksource contract",
        test_uts_clocksource_contract, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: UTS one-shot surface",
        test_uts_oneshot_surface, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: one-shot conversion helpers",
        test_oneshot_conversion_helpers, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: mono LAPIC tick scaling",
        test_mono_lapic_scaling, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: one-shot delegation",
        test_oneshot_delegation, TEST_CAT_BOOT);
    test_suite_register_cat("irq_timer: LAPIC calibration state",
        test_lapic_calibration_state, TEST_CAT_BOOT);
}
